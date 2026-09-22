#include "core/vault/fs_watcher.h"

#include "core/util/busy.h"
#include "core/vault/ignore_rules.h"

#include <QHash>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <thread>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcVaultWatch, "oc.vault.watch", QtInfoMsg)

namespace oc::vaultfs {

namespace {

constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO
                                | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;

struct BusyGuard {
    BusyGuard() { Busy::begin(); }
    ~BusyGuard() { Busy::end(); }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
};

void signalEventFd(int fd)
{
    const uint64_t one = 1;
    while (::write(fd, &one, sizeof one) < 0 && errno == EINTR) {
    }
}

void drainEventFd(int fd)
{
    uint64_t value = 0;
    while (::read(fd, &value, sizeof value) < 0 && errno == EINTR) {
    }
}

} // namespace

BusyToken makeBusyToken() { return std::make_shared<BusyGuard>(); }

QString watcherKindName(WatcherKind kind)
{
    switch (kind) {
    case WatcherKind::None: return QStringLiteral("none");
    case WatcherKind::Inotify: return QStringLiteral("inotify");
    case WatcherKind::Hybrid: return QStringLiteral("hybrid");
    case WatcherKind::Poll: return QStringLiteral("poll");
    }
    return QString();
}

std::optional<WatcherKind> watcherKindFromName(const QString& name)
{
    const QString n = name.trimmed().toLower();
    if (n == u"none" || n == u"off")
        return WatcherKind::None;
    if (n == u"inotify")
        return WatcherKind::Inotify;
    if (n == u"hybrid")
        return WatcherKind::Hybrid;
    if (n == u"poll" || n == u"polling")
        return WatcherKind::Poll;
    return std::nullopt;
}

std::optional<WatcherKind> autoWatcherKind(const QByteArray& absRoot)
{
    bool known = false;
    const bool remote = isFuseOrNetworkFs(absRoot, nullptr, &known);
    if (!known)
        return std::nullopt;
    return remote ? WatcherKind::Hybrid : WatcherKind::Inotify;
}

IFsWatcher::~IFsWatcher() = default;

// =============================================================================================
// WatchWorker: the watcher thread. All members below "worker state" are only touched by that
// thread once it runs (and by initialScan() before it starts).

class WatchWorker {
public:
    using Post = std::function<void(FsBatch&&)>;

    WatchWorker(QByteArray root, bool useInotify, int sweepMs, int pollFallbackMs, Post post)
        : m_root(std::move(root))
        , m_wantInotify(useInotify)
        , m_sweepMs(sweepMs)
        , m_pollFallbackMs(pollFallbackMs)
        , m_post(std::move(post))
        , m_wakeFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
    }

    ~WatchWorker()
    {
        stopThread();
        closeInotify();
        if (m_wakeFd >= 0)
            ::close(m_wakeFd);
    }

    // --- owner thread, before startThread()
    bool initialScan(const WalkVisitor& visit, const WalkOptions& opts, int* err)
    {
        if (m_wantInotify)
            openInotify();
        WalkOptions o = opts;
        const auto userOnDir = opts.onDir;
        o.onDir = [this, &userOnDir](const QString& rel, const QByteArray& abs, const DiskEntry& st) {
            if (m_fd >= 0 && !m_watchLimitHit && addWatch(rel, abs, st.isSymlink) == AddResult::Alias)
                return false;
            return userOnDir ? userOnDir(rel, abs, st) : true;
        };
        const bool ok = walkTree(m_root, QString(), visit, o, err);
        if (m_watchLimitHit) {
            qCWarning(lcVaultWatch) << "inotify watch limit reached; falling back to polling";
            closeInotify();
        }
        if (!ok) {
            closeInotify();
            m_rootLost = true;
        }
        return ok;
    }

    // --- owner thread, instead of initialScan(): nothing is watched until a restart command.
    void markRootLost() { m_rootLost = true; }

    bool startThread()
    {
        if (m_wakeFd < 0)
            return false;
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stopThread()
    {
        if (!m_thread.joinable())
            return;
        m_stopping.store(true);
        signalEventFd(m_wakeFd);
        m_thread.join();
    }

    // --- any thread
    void requestSweep(BusyToken token)
    {
        {
            std::lock_guard lock(m_cmdMutex);
            for (const Command& c : m_cmds)
                if (!c.restart)
                    return; // one queued sweep is enough (its token keeps Busy up)
            m_cmds.push_back(Command{false, std::move(token)});
        }
        signalEventFd(m_wakeFd);
    }

    void requestRestart(BusyToken token)
    {
        {
            std::lock_guard lock(m_cmdMutex);
            m_cmds.push_back(Command{true, std::move(token)});
        }
        signalEventFd(m_wakeFd);
    }

    bool usingInotify() const { return m_inotifyActive.load(); }

private:
    struct Command {
        bool restart = false;
        BusyToken busy;
    };
    enum class AddResult { Ok, Alias, Failed };
    // A directory seen leaving its path (IN_MOVED_FROM), waiting for the IN_MOVED_TO with its cookie.
    // Its watches are remembered by descriptor: by the time the batch ends, another directory may
    // have been created (and watched) under the old name.
    struct PendingDirMove {
        QString path;
        QSet<int> wds; // the directory's and its subdirectories' watches
    };

    void run()
    {
        pthread_setname_np(pthread_self(), "oc-vault-watch");
        if (m_fd < 0 && m_wantInotify && !m_rootLost)
            setSweepInterval(m_pollFallbackMs); // inotify wanted but unavailable
        if (m_sweepMs > 0)
            setSweepInterval(m_sweepMs);
        while (!m_stopping.load()) {
            int timeout = -1;
            if (m_sweepIntervalMs > 0)
                timeout = int(std::clamp<qint64>(m_nextSweepMs - monotonicMs(), 0, INT_MAX));
            pollfd fds[2] = {{m_wakeFd, POLLIN, 0}, {m_fd, POLLIN, 0}}; // a negative fd is ignored by poll()
            const int r = ::poll(fds, 2, timeout);
            if (m_stopping.load())
                break;
            if (r < 0) {
                if (errno != EINTR) {
                    qCWarning(lcVaultWatch) << "poll failed:" << errno;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            if (fds[0].revents & POLLIN)
                drainEventFd(m_wakeFd);
            std::vector<Command> cmds;
            {
                std::lock_guard lock(m_cmdMutex);
                cmds.swap(m_cmds);
            }
            for (Command& c : cmds) {
                if (m_stopping.load())
                    break;
                sweepNow(std::move(c.busy), c.restart);
            }
            if (m_fd >= 0 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP)))
                onReadable();
            if (m_sweepIntervalMs > 0 && monotonicMs() >= m_nextSweepMs) {
                m_nextSweepMs = monotonicMs() + m_sweepIntervalMs;
                if (!m_rootLost)
                    sweepNow(makeBusyToken(), false);
            }
        }
    }

    void setSweepInterval(int ms)
    {
        if (ms <= 0)
            return;
        if (m_sweepIntervalMs <= 0 || ms < m_sweepIntervalMs) {
            m_sweepIntervalMs = ms;
            m_nextSweepMs = monotonicMs() + ms;
        }
    }

    void sweepNow(BusyToken token, bool restart)
    {
        if (restart) {
            closeInotify();
            m_rootLost = false;
            m_watchLimitHit = false;
            if (m_wantInotify)
                openInotify();
        } else if (m_rootLost) {
            return; // the Vault is offline and probes the root itself; restart() re-arms us
        }
        bool cancelled = false;
        std::shared_ptr<FsSnapshot> snap = walkSnapshot(m_fd >= 0, &cancelled);
        if (cancelled)
            return;
        if (!snap->rootOk) {
            closeInotify(); // stop until the Vault sees the root again and calls restart()
            m_rootLost = true;
        } else if (m_watchLimitHit && m_fd >= 0) {
            fallbackToPolling();
        } else if (restart && m_fd < 0 && m_wantInotify) {
            setSweepInterval(m_pollFallbackMs);
        }
        FsBatch b;
        b.snapshot = std::move(snap);
        b.restart = restart;
        b.busy = std::move(token);
        m_post(std::move(b));
    }

    bool openInotify()
    {
        m_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_fd < 0) {
            qCWarning(lcVaultWatch) << "inotify_init1 failed:" << errno << "- using polling";
            m_inotifyActive.store(false);
            return false;
        }
        m_inotifyActive.store(true);
        return true;
    }

    void closeInotify()
    {
        if (m_fd >= 0)
            ::close(m_fd); // drops every watch
        m_fd = -1;
        m_rootWd = -1;
        m_wdToPath.clear();
        m_pathToWd.clear();
        m_inotifyActive.store(false);
    }

    void fallbackToPolling()
    {
        qCWarning(lcVaultWatch) << "inotify failed (watch limit?); falling back to polling every" << m_pollFallbackMs
                                << "ms";
        closeInotify();
        setSweepInterval(m_pollFallbackMs);
    }

    AddResult addWatch(const QString& rel, const QByteArray& abs, bool viaSymlink)
    {
        if (m_fd < 0)
            return AddResult::Failed;
        const int wd = ::inotify_add_watch(m_fd, abs.constData(), kWatchMask);
        if (wd < 0) {
            if (errno == ENOSPC || errno == ENOMEM)
                m_watchLimitHit = true;
            return AddResult::Failed;
        }
        const auto it = m_wdToPath.find(wd);
        if (it != m_wdToPath.end() && it.value() != rel) {
            if (viaSymlink)
                return AddResult::Alias; // this directory is already watched under another path
            // A directory we knew under another path (a move we couldn't pair): follow it.
            if (m_pathToWd.value(it.value(), -1) == wd)
                m_pathToWd.remove(it.value());
            it.value() = rel;
        } else {
            m_wdToPath.insert(wd, rel);
        }
        const auto pit = m_pathToWd.find(rel);
        if (pit != m_pathToWd.end() && pit.value() != wd) {
            // `rel` used to be another directory (replaced): forget the old watch.
            const int old = pit.value();
            ::inotify_rm_watch(m_fd, old);
            m_wdToPath.remove(old);
        }
        m_pathToWd.insert(rel, wd);
        if (rel.isEmpty())
            m_rootWd = wd;
        return AddResult::Ok;
    }

    void addWatchTree(const QString& rel)
    {
        if (m_fd < 0)
            return;
        WalkOptions o;
        o.cancel = &m_stopping;
        o.dirsOnly = true;
        o.onDir = [this](const QString& r, const QByteArray& abs, const DiskEntry& st) {
            return addWatch(r, abs, st.isSymlink) != AddResult::Alias;
        };
        int err = 0;
        walkTree(m_root, rel, [](const QString&, const DiskEntry&) {}, o, &err);
    }

    void dropMapping(int wd)
    {
        const auto it = m_wdToPath.find(wd);
        if (it == m_wdToPath.end())
            return;
        if (m_pathToWd.value(it.value(), -1) == wd)
            m_pathToWd.remove(it.value());
        m_wdToPath.erase(it);
        if (wd == m_rootWd)
            m_rootWd = -1;
    }

    QList<int> watchesUnder(const QString& rel) const
    {
        QList<int> wds;
        for (auto it = m_wdToPath.cbegin(); it != m_wdToPath.cend(); ++it)
            if (it.value() == rel || isStrictlyInside(it.value(), rel))
                wds.append(it.key());
        return wds;
    }

    void removeWatches(const QSet<int>& wds)
    {
        for (int wd : wds) {
            if (!m_wdToPath.contains(wd))
                continue; // already gone (directory deleted, or replaced by another watch)
            ::inotify_rm_watch(m_fd, wd);
            dropMapping(wd);
        }
    }

    // A directory (with the watches `moving`) moved from `from` to `to` inside the vault.
    void repathWatches(const QString& from, const QString& to, const QSet<int>& moving)
    {
        // Whatever was watched at the destination was replaced by this move.
        for (int wd : watchesUnder(to)) {
            if (!moving.contains(wd)) {
                ::inotify_rm_watch(m_fd, wd);
                dropMapping(wd);
            }
        }
        QList<QPair<int, QString>> updates;
        for (int wd : moving) {
            const auto it = m_wdToPath.constFind(wd);
            if (it == m_wdToPath.cend())
                continue; // deleted in the meantime
            const QString old = it.value();
            if (old != from && !isStrictlyInside(old, from))
                continue; // re-mapped in the meantime
            updates.append({wd, QString(to + QStringView(old).mid(from.size()))});
            if (m_pathToWd.value(old, -1) == wd)
                m_pathToWd.remove(old);
        }
        for (const auto& [wd, path] : std::as_const(updates)) {
            m_wdToPath.insert(wd, path);
            m_pathToWd.insert(path, wd);
        }
    }

    void rootGone(FsBatch& batch)
    {
        batch.rootLost = true;
        m_rootLost = true;
        closeInotify(); // no more events until restart(): nothing under the old root is ours now
    }

    void handleEvent(const inotify_event* ev, FsBatch& batch, QHash<quint32, PendingDirMove>& pendingDirMoves)
    {
        const uint32_t mask = ev->mask;
        if (mask & IN_Q_OVERFLOW) {
            batch.overflow = true;
            return;
        }
        const auto it = m_wdToPath.constFind(ev->wd);
        if (it == m_wdToPath.cend())
            return; // a watch we already dropped
        const QString dirPath = it.value();
        const bool isRoot = ev->wd == m_rootWd;
        if (mask & IN_IGNORED) { // watch gone: directory deleted, filesystem unmounted, or rm_watch
            dropMapping(ev->wd);
            if (isRoot)
                rootGone(batch);
            return;
        }
        if (mask & IN_UNMOUNT) {
            if (isRoot)
                rootGone(batch);
            else // a filesystem mounted below the root went away: its mount point shows something else
                batch.events.append(FsEvent{dirPath, FsEvent::Rescan | FsEvent::IsDir, 0});
            return;
        }
        if (mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            if (isRoot)
                rootGone(batch);
            return; // for other directories the parent reports the deletion/move
        }
        if (ev->len == 0)
            return; // e.g. IN_ATTRIB on the watched directory itself
        const std::optional<QString> name = decodeName(ev->name);
        if (!name || isIgnoredName(*name))
            return; // temp files, dot entries: never part of the vault
        const QString path = joinPath(dirPath, *name);
        const bool isDir = mask & IN_ISDIR;
        quint32 flags = isDir ? quint32(FsEvent::IsDir) : 0u;
        if (mask & IN_CREATE)
            flags |= FsEvent::Created;
        if (mask & IN_DELETE)
            flags |= FsEvent::Deleted;
        if (mask & IN_MODIFY)
            flags |= FsEvent::Modified;
        if (mask & IN_CLOSE_WRITE)
            flags |= FsEvent::CloseWrite;
        if (mask & IN_ATTRIB)
            flags |= FsEvent::Attrib;
        if (mask & IN_MOVED_FROM)
            flags |= FsEvent::MovedFrom;
        if (mask & IN_MOVED_TO)
            flags |= FsEvent::MovedTo;
        if (isDir) {
            if (mask & IN_MOVED_FROM) {
                const QList<int> wds = watchesUnder(path);
                pendingDirMoves.insert(ev->cookie, PendingDirMove{path, QSet<int>(wds.cbegin(), wds.cend())});
            } else if (mask & IN_MOVED_TO) {
                const auto pm = pendingDirMoves.find(ev->cookie);
                if (pm != pendingDirMoves.end()) {
                    repathWatches(pm->path, path, pm->wds);
                    pendingDirMoves.erase(pm);
                }
                if (!m_pathToWd.contains(path))
                    addWatchTree(path); // moved in from outside the vault (or from an ignored name)
            } else if (mask & IN_CREATE) {
                addWatchTree(path);
            }
        }
        batch.events.append(FsEvent{path, flags, ev->cookie});
    }

    void onReadable()
    {
        alignas(inotify_event) char buf[64 * 1024];
        FsBatch batch;
        QHash<quint32, PendingDirMove> pendingDirMoves;
        for (int reads = 0; reads < 16 && m_fd >= 0;) {
            const ssize_t n = ::read(m_fd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN) {
                    qCWarning(lcVaultWatch) << "inotify read failed:" << errno;
                    fallbackToPolling();
                    batch.overflow = true;
                }
                break;
            }
            if (n == 0)
                break;
            ++reads;
            for (const char* p = buf; p < buf + n && m_fd >= 0;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(p);
                handleEvent(ev, batch, pendingDirMoves);
                p += sizeof(inotify_event) + ev->len;
            }
        }
        // Directories moved away without a matching MOVED_TO left the vault (or their MOVED_TO is in
        // a later read; it then re-adds them): stop watching them.
        if (m_fd >= 0)
            for (const PendingDirMove& pm : std::as_const(pendingDirMoves))
                removeWatches(pm.wds);
        if (m_watchLimitHit && m_fd >= 0) {
            fallbackToPolling();
            batch.overflow = true; // some new directories are unwatched: have the vault sweep now
        }
        if (!batch.events.isEmpty() || batch.overflow || batch.rootLost) {
            batch.busy = makeBusyToken();
            m_post(std::move(batch));
        }
    }

    std::shared_ptr<FsSnapshot> walkSnapshot(bool addWatches, bool* cancelled)
    {
        auto snap = std::make_shared<FsSnapshot>();
        const RootProbe rp = probeRoot(m_root);
        snap->rootOk = rp.ok;
        snap->error = rp.err;
        snap->rootEntry = rp.st;
        snap->rootEntries = rp.entries;
        if (!rp.ok)
            return snap;
        bool incomplete = false;
        QSet<QString> seenDirs;
        WalkOptions o;
        o.cancel = &m_stopping;
        o.incomplete = &incomplete;
        o.onDir = [&](const QString& rel, const QByteArray& abs, const DiskEntry& st) {
            seenDirs.insert(rel);
            if (addWatches && m_fd >= 0 && !m_watchLimitHit && !m_pathToWd.contains(rel))
                return addWatch(rel, abs, st.isSymlink) != AddResult::Alias;
            return true;
        };
        int err = 0;
        const bool ok = walkTree(
            m_root, QString(), [&](const QString& rel, const DiskEntry& e) { snap->entries.insert(rel, e); }, o, &err);
        if (!ok) {
            if (err == ECANCELED) {
                *cancelled = true;
            } else {
                snap->rootOk = false;
                snap->error = err;
            }
            return snap;
        }
        snap->complete = !incomplete;
        if (addWatches && m_fd >= 0 && snap->complete) {
            // Watches whose path no longer is a directory of the vault (missed moves) are stale.
            QList<int> stale;
            for (auto it = m_pathToWd.cbegin(); it != m_pathToWd.cend(); ++it)
                if (!seenDirs.contains(it.key()))
                    stale.append(it.value());
            for (int wd : stale) {
                ::inotify_rm_watch(m_fd, wd);
                dropMapping(wd);
            }
        }
        return snap;
    }

    const QByteArray m_root;
    const bool m_wantInotify;
    const int m_sweepMs;
    const int m_pollFallbackMs;
    const Post m_post;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_inotifyActive{false};
    const int m_wakeFd;
    std::mutex m_cmdMutex;
    std::vector<Command> m_cmds; // guarded by m_cmdMutex
    std::thread m_thread;

    // worker state
    int m_fd = -1;
    int m_rootWd = -1;
    bool m_rootLost = false;
    bool m_watchLimitHit = false;
    int m_sweepIntervalMs = 0; // 0 = no periodic sweeps
    qint64 m_nextSweepMs = 0;
    QHash<int, QString> m_wdToPath;
    QHash<QString, int> m_pathToWd;
};

// =============================================================================================
// ThreadedFsWatcher (owner thread side)

ThreadedFsWatcher::ThreadedFsWatcher(WatcherKind kind, const QString& root, bool useInotify, int sweepMs,
                                     int pollFallbackMs)
    : m_kind(kind)
    , m_outbox(std::make_shared<Outbox>())
{
    m_notifyFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_notifyFd >= 0) {
        m_notifier = new QSocketNotifier(qintptr(m_notifyFd), QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
    } else {
        qCWarning(lcVaultWatch) << "eventfd failed:" << errno << "- external changes won't be noticed";
    }
    std::shared_ptr<Outbox> outbox = m_outbox;
    const int notifyFd = m_notifyFd;
    m_worker = std::make_unique<WatchWorker>(encodeName(root), useInotify, sweepMs, pollFallbackMs,
                                             [outbox, notifyFd](FsBatch&& b) {
                                                 {
                                                     std::lock_guard lock(outbox->mutex);
                                                     outbox->items.push_back(std::move(b));
                                                 }
                                                 if (notifyFd >= 0)
                                                     signalEventFd(notifyFd);
                                             });
}

ThreadedFsWatcher::~ThreadedFsWatcher()
{
    m_worker.reset(); // stops and joins the watcher thread
    delete m_notifier;
    m_notifier = nullptr;
    if (m_notifyFd >= 0)
        ::close(m_notifyFd);
}

bool ThreadedFsWatcher::usingInotify() const { return m_worker && m_worker->usingInotify(); }

bool ThreadedFsWatcher::start(const WalkVisitor& visit, const WalkOptions& walkOpts, int* err)
{
    const bool ok = m_worker->initialScan(visit, walkOpts, err);
    if (!m_worker->startThread())
        qCWarning(lcVaultWatch) << "cannot start the watcher thread";
    return ok;
}

void ThreadedFsWatcher::startDeferred()
{
    m_worker->markRootLost();
    if (!m_worker->startThread())
        qCWarning(lcVaultWatch) << "cannot start the watcher thread";
}

void ThreadedFsWatcher::requestSweep() { m_worker->requestSweep(makeBusyToken()); }

void ThreadedFsWatcher::restart() { m_worker->requestRestart(makeBusyToken()); }

void ThreadedFsWatcher::drain()
{
    drainEventFd(m_notifyFd);
    std::vector<FsBatch> items;
    {
        std::lock_guard lock(m_outbox->mutex);
        items.swap(m_outbox->items);
    }
    QPointer<ThreadedFsWatcher> self(this);
    for (FsBatch& b : items) {
        if (!self || !m_sink)
            break;
        m_sink(std::move(b));
    }
}

InotifyWatcher::InotifyWatcher(const QString& root, const WatcherOptions& opts)
    : ThreadedFsWatcher(WatcherKind::Inotify, root, true, 0, opts.pollIntervalMs)
{
}

HybridWatcher::HybridWatcher(const QString& root, const WatcherOptions& opts)
    : ThreadedFsWatcher(WatcherKind::Hybrid, root, true, opts.hybridSweepMs, opts.pollIntervalMs)
{
}

PollingWatcher::PollingWatcher(const QString& root, const WatcherOptions& opts)
    : ThreadedFsWatcher(WatcherKind::Poll, root, false, opts.pollIntervalMs, opts.pollIntervalMs)
{
}

std::unique_ptr<IFsWatcher> createFsWatcher(WatcherKind kind, const QString& root, const WatcherOptions& opts)
{
    switch (kind) {
    case WatcherKind::Inotify: return std::make_unique<InotifyWatcher>(root, opts);
    case WatcherKind::Hybrid: return std::make_unique<HybridWatcher>(root, opts);
    case WatcherKind::Poll: return std::make_unique<PollingWatcher>(root, opts);
    case WatcherKind::None: break;
    }
    return nullptr;
}

} // namespace oc::vaultfs
