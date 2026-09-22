#pragma once

// Filesystem watcher backends for the vault (private to src/core/vault).
//
// A backend runs its own thread and hands raw material to the thread that created it (the GUI
// thread): batches of raw events (paths already vault-relative, ignored names dropped) and/or
// full stat snapshots. It never interprets them against the vault model; the Vault coalesces,
// re-stats and diffs (stat is the source of truth).
//
//   InotifyWatcher  one inotify watch per non-ignored directory (IN_NONBLOCK | IN_CLOEXEC fd).
//                   Watches follow directory moves (cookie pairing), are added for new
//                   directories and removed for directories moved out. Falls back to polling if
//                   inotify is unavailable or the watch limit is hit.
//   HybridWatcher   inotify + a stat-only verification sweep every ~15 s (FUSE / network
//                   filesystems, where changes made outside this kernel are invisible to inotify).
//   PollingWatcher  sweeps only.
//
// Threading: the watcher thread is a std::thread running a poll() loop over the inotify fd and a
// command eventfd (no Qt event loop, no QObjects on it); results travel through a mutex-protected
// outbox and wake the owner thread through an eventfd watched by a QSocketNotifier. Everything is
// visible to ThreadSanitizer (QThread's futex-based synchronisation isn't) and shutdown is a
// plain join.

#include "core/vault/fs_scan.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QSocketNotifier;

namespace oc::vaultfs {

struct FsEvent {
    enum Flag : quint32 {
        Created = 1u << 0,
        Deleted = 1u << 1,
        Modified = 1u << 2,   // IN_MODIFY
        CloseWrite = 1u << 3, // IN_CLOSE_WRITE
        Attrib = 1u << 4,
        MovedFrom = 1u << 5,
        MovedTo = 1u << 6,
        IsDir = 1u << 7,
        Rescan = 1u << 8, // re-read the directory `path` recursively (e.g. a mount below it went away)
    };
    QString path; // vault-relative
    quint32 flags = 0;
    quint32 cookie = 0;
};

struct FsSnapshot {
    bool rootOk = false;
    int error = 0;          // errno when !rootOk
    bool complete = true;   // false: some directory couldn't be read; don't infer deletions from it
    DiskEntry rootEntry;
    int rootEntries = 0;    // entries directly in the root, dot files included (capped at 1)
    QHash<QString, DiskEntry> entries; // every non-ignored path below the root
};

// Shared token that keeps oc::Busy non-idle while the work it belongs to is in flight.
using BusyToken = std::shared_ptr<void>;
BusyToken makeBusyToken();

struct FsBatch {
    QList<FsEvent> events;
    bool overflow = false;  // events were lost (IN_Q_OVERFLOW, or the backend changed): rescan
    bool rootLost = false;  // the root was unmounted / deleted / moved away
    std::shared_ptr<const FsSnapshot> snapshot; // sweep result
    bool restart = false;   // snapshot produced by restart(): watches were re-established
    BusyToken busy;
};

enum class WatcherKind { None, Inotify, Hybrid, Poll };
QString watcherKindName(WatcherKind kind);
std::optional<WatcherKind> watcherKindFromName(const QString& name);
// FUSE / network filesystems -> Hybrid, else Inotify.
WatcherKind autoWatcherKind(const QByteArray& absRoot);

struct WatcherOptions {
    int pollIntervalMs = 3000;  // PollingWatcher (and the inotify -> polling fallback)
    int hybridSweepMs = 15000;  // HybridWatcher verification sweeps
};

class IFsWatcher : public QObject {
public:
    using Sink = std::function<void(FsBatch)>;
    ~IFsWatcher() override;

    virtual WatcherKind kind() const = 0;
    // Whether inotify is in use right now (false for polling, and after a fallback to polling).
    virtual bool usingInotify() const = 0;

    // Synchronous: establish the watches while walking the tree once; `visit` receives every
    // non-ignored entry (this walk is the vault's initial scan). Then starts the watcher thread.
    // Returns false (with *err) if the root can't be read; the thread is started anyway so that
    // restart() works later.
    virtual bool start(const WalkVisitor& visit, const WalkOptions& walkOpts, int* err) = 0;
    // Asynchronous full stat sweep -> an FsBatch carrying a snapshot.
    virtual void requestSweep() = 0;
    // Asynchronous: drop every watch, re-add them while walking, deliver the walk as a snapshot
    // (FsBatch::restart = true). Used when the root comes back after being unavailable.
    virtual void restart() = 0;

    // Batches are delivered on the thread that created the watcher (from its event loop).
    void setSink(Sink sink) { m_sink = std::move(sink); }

protected:
    Sink m_sink;
};

class WatchWorker;

// Common thread plumbing for all backends.
class ThreadedFsWatcher : public IFsWatcher {
public:
    ~ThreadedFsWatcher() override;

    WatcherKind kind() const override { return m_kind; }
    bool usingInotify() const override;
    bool start(const WalkVisitor& visit, const WalkOptions& walkOpts, int* err) override;
    void requestSweep() override;
    void restart() override;

protected:
    ThreadedFsWatcher(WatcherKind kind, const QString& root, bool useInotify, int sweepMs, int pollFallbackMs);

private:
    struct Outbox {
        std::mutex mutex;
        std::vector<FsBatch> items;
    };
    void drain();

    WatcherKind m_kind;
    int m_notifyFd = -1; // eventfd: the worker's "outbox has items" signal
    QSocketNotifier* m_notifier = nullptr;
    std::shared_ptr<Outbox> m_outbox;
    std::unique_ptr<WatchWorker> m_worker;
};

class InotifyWatcher final : public ThreadedFsWatcher {
public:
    InotifyWatcher(const QString& root, const WatcherOptions& opts);
};

class HybridWatcher final : public ThreadedFsWatcher {
public:
    HybridWatcher(const QString& root, const WatcherOptions& opts);
};

class PollingWatcher final : public ThreadedFsWatcher {
public:
    PollingWatcher(const QString& root, const WatcherOptions& opts);
};

std::unique_ptr<IFsWatcher> createFsWatcher(WatcherKind kind, const QString& root, const WatcherOptions& opts);

} // namespace oc::vaultfs
