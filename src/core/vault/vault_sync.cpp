// Vault <-> disk synchronisation: watcher intake and coalescing, reconciliation (stat is the
// source of truth), sweeps, offline handling and the mass-delete guard.

#include "core/vault/ignore_rules.h"
#include "core/vault/vault_impl.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cerrno>

Q_LOGGING_CATEGORY(lcVaultSync, "oc.vault.sync", QtInfoMsg)

namespace oc {

using namespace vaultfs;

namespace {

bool depthThenPath(const QString& a, const QString& b)
{
    const int da = pathDepth(a);
    const int db = pathDepth(b);
    return da != db ? da < db : a < b;
}

// What we know about the disk for one reconcile pass.
struct Scope {
    QHash<QString, DiskEntry> disk; // existing entries among the paths we looked at
    QSet<QString> checked;          // paths stat'ed individually (present in `disk` or known missing)
    QSet<QString> deepRoots;        // directories read recursively: `disk` has all their descendants
    QSet<QString> unknown;          // stat failed for a transient reason: leave the model alone there
    bool incomplete = false;        // a recursive read hit transient errors: don't infer deletions from it

    bool coveredByDeep(const QString& p) const
    {
        if (p.isEmpty())
            return false;
        if (deepRoots.contains(QString()))
            return true;
        for (qsizetype i = p.indexOf(u'/'); i >= 0; i = p.indexOf(u'/', i + 1))
            if (deepRoots.contains(p.left(i)))
                return true;
        return false;
    }
    // May we conclude that `path` is gone (or has another type) when it isn't in `disk`?
    bool knownMissing(const QString& path) const
    {
        if (unknown.contains(path))
            return false;
        if (checked.contains(path))
            return true;
        return !incomplete && coveredByDeep(path);
    }
};

struct Classification {
    QList<TAbstractFile*> goneTop;      // gone items without a gone ancestor
    QSet<TAbstractFile*> goneAll;       // gone items and all their descendants
    QList<QString> fresh;               // disk paths without a model item of the same type (depth order)
    QList<QPair<VaultFileNode*, QString>> changed;   // same path, different stat
    QList<QPair<VaultFileNode*, QString>> hashCheck; // same stat, content event, known hash
};

} // namespace

// =============================================================================================
// Startup

void Vault::Impl::startWatching()
{
    WatcherKind kind = autoWatcherKind(rootEnc);
    if (!cfg.watcher.trimmed().isEmpty()) {
        if (const std::optional<WatcherKind> k = watcherKindFromName(cfg.watcher))
            kind = *k;
        else
            qCWarning(lcVaultSync) << "unknown watcher backend" << cfg.watcher << "- using"
                                   << watcherKindName(kind);
    }
    watcherKind = kind;
    selfWrites.setTtl(cfg.selfWriteTtlMs);

    if (const std::optional<DiskEntry> st = statEntry(rootEnc))
        rootId = st->id;
    const WalkVisitor visit = [this](const QString& rel, const DiskEntry& e) {
        VaultFolderNode* parent = folderAt(parentPath(rel));
        if (parent && !byPath.contains(rel))
            addNode(parent, rel, e);
    };
    int err = 0;
    bool ok = false;
    if (kind != WatcherKind::None) {
        WatcherOptions wo;
        wo.pollIntervalMs = cfg.pollIntervalMs;
        wo.hybridSweepMs = cfg.hybridSweepMs;
        watcher = createFsWatcher(kind, root, wo);
        watcher->setSink([this](FsBatch b) { onBatch(std::move(b)); });
        ok = watcher->start(visit, WalkOptions{}, &err);
    } else {
        ok = walkTree(rootEnc, QString(), visit, WalkOptions{}, &err);
    }
    if (!ok) {
        qCWarning(lcVaultSync) << "vault folder not available:" << root << errnoString(err);
        online = false;
        offlineTimer.start(cfg.offlinePollMs);
    }
}

// =============================================================================================
// Intake + coalescing

void Vault::Impl::onBatch(FsBatch b)
{
    if (!loaded)
        return;
    if (b.snapshot) {
        pendingSnapshots.push_back(std::move(b)); // keeps its Busy token until processed
        scheduleProcessing(0);
        return;
    }
    // While offline, events are noise from a disappearing tree, except once restart() re-added the
    // watches: then they may describe changes made after the restart walk read a directory, and
    // are processed right after the restart snapshot brought the vault back online.
    if (dropWatcherEvents || (!online && !restartPending))
        return;
    if (b.busy)
        pendingBusy.push_back(std::move(b.busy));
    pendingEvents.append(b.events);
    pendingOverflow = pendingOverflow || b.overflow;
    pendingRootLost = pendingRootLost || b.rootLost;
    scheduleProcessing(b.rootLost || b.overflow ? 0 : cfg.coalesceMs);
}

void Vault::Impl::scheduleProcessing(int delayMs)
{
    const qint64 now = monotonicMs();
    if (!processTimer.isActive()) {
        pendingSinceMs = now;
        processUrgent = delayMs == 0;
        processTimer.start(delayMs);
        return;
    }
    if (delayMs == 0) {
        processUrgent = true;
        processTimer.start(0);
        return;
    }
    if (processUrgent)
        return; // already due right away
    // Debounce, but never later than coalesceMaxMs after the first pending event.
    const qint64 left = pendingSinceMs + cfg.coalesceMaxMs - now;
    processTimer.start(int(std::clamp<qint64>(left, 0, delayMs)));
}

void Vault::Impl::processPending()
{
    if (opDepth > 0) { // our own operation (or a KIO job's nested event loop) is in progress
        processTimer.start(std::max(10, cfg.coalesceMs));
        return;
    }
    processUrgent = false;
    const QList<FsEvent> events = std::exchange(pendingEvents, {});
    const bool overflow = std::exchange(pendingOverflow, false);
    const bool rootLost = std::exchange(pendingRootLost, false);
    const std::vector<FsBatch> snapshots = std::exchange(pendingSnapshots, {});
    const std::vector<BusyToken> busy = std::exchange(pendingBusy, {}); // released when this pass is done
    if (!loaded)
        return;
    OpScope op(this);
    selfWrites.expire();
    for (const FsBatch& b : snapshots)
        if (b.restart)
            onRestartSnapshot(*b.snapshot);
    if (!online) {
        if (restartPending) { // the restart snapshot isn't here yet: keep what arrived meanwhile
            pendingEvents = events + pendingEvents;
            pendingOverflow = pendingOverflow || overflow;
            pendingRootLost = pendingRootLost || rootLost;
            for (const BusyToken& t : busy)
                pendingBusy.push_back(t);
        }
        return;
    }
    if (!events.isEmpty() || overflow || rootLost) {
        if (rootLost || !rootHealthy()) {
            goOffline(); // unmounted / deleted / replaced: never report its files as deleted
            return;
        }
        if (overflow && watcher)
            watcher->requestSweep();
        if (!events.isEmpty())
            processEvents(events);
    }
    for (const FsBatch& b : snapshots)
        if (!b.restart && online)
            applySnapshot(*b.snapshot);
    ++passes;
}

void Vault::Impl::processEvents(const QList<FsEvent>& events)
{
    QSet<QString> touched;
    QSet<QString> hints;
    QSet<QString> deep;
    QHash<QString, QString> movedFrom; // MOVED_TO path -> MOVED_FROM path (same cookie)
    QHash<quint32, QString> fromByCookie;
    for (const FsEvent& ev : events) {
        if (ev.flags & FsEvent::Rescan) {
            deep.insert(ev.path);
            continue;
        }
        touched.insert(ev.path);
        if ((ev.flags & FsEvent::MovedFrom) && ev.cookie)
            fromByCookie.insert(ev.cookie, ev.path);
        if ((ev.flags & FsEvent::MovedTo) && ev.cookie) {
            const auto it = fromByCookie.constFind(ev.cookie);
            if (it != fromByCookie.cend())
                movedFrom.insert(ev.path, it.value());
        }
        if (ev.flags & (FsEvent::Modified | FsEvent::CloseWrite))
            hints.insert(ev.path);
    }
    reconcile(touched, movedFrom, hints, deep);
}

// =============================================================================================
// Sweeps

std::shared_ptr<FsSnapshot> Vault::Impl::syncSnapshot() const
{
    auto snap = std::make_shared<FsSnapshot>();
    const RootProbe rp = probeRoot(rootEnc);
    snap->rootOk = rp.ok;
    snap->error = rp.err;
    snap->rootEntry = rp.st;
    snap->rootEntries = rp.entries;
    if (!rp.ok)
        return snap;
    bool incomplete = false;
    WalkOptions o;
    o.incomplete = &incomplete;
    int err = 0;
    if (!walkTree(rootEnc, QString(), [&](const QString& rel, const DiskEntry& e) { snap->entries.insert(rel, e); }, o,
                  &err)) {
        snap->rootOk = false;
        snap->error = err;
    }
    snap->complete = !incomplete;
    return snap;
}

void Vault::Impl::applySnapshot(const FsSnapshot& snap)
{
    if (!snap.rootOk || (rootId.ino != 0 && !(snap.rootEntry.id == rootId))) {
        goOffline();
        return;
    }
    // The snapshot only nominates candidates; reconcile() re-stats them now (it may be stale).
    QSet<QString> touched;
    for (auto it = snap.entries.cbegin(); it != snap.entries.cend(); ++it) {
        TAbstractFile* m = byPath.value(it.key());
        if (!m || m->isFolder() != it->isDir) {
            touched.insert(it.key());
        } else if (!m->isFolder()) {
            const VaultFileNode* f = fnode(m);
            if (f->size() != it->size || f->info.mtimeNs != it->mtimeNs || !(f->info.id == it->id))
                touched.insert(it.key());
        }
    }
    if (snap.complete)
        for (auto it = byPath.cbegin(); it != byPath.cend(); ++it)
            if (!it.key().isEmpty() && !snap.entries.contains(it.key()))
                touched.insert(it.key());
    if (!touched.isEmpty())
        reconcile(touched, {}, {}, {});
}

void Vault::Impl::onRestartSnapshot(const FsSnapshot& snap)
{
    restartPending = false;
    // An empty directory where a vault with many files used to be is an unmounted mount point.
    const bool usable = snap.rootOk && (snap.rootEntries > 0 || nFiles <= cfg.massDeleteMinFiles);
    if (!usable) {
        if (online)
            goOffline();
        else
            offlineTimer.start(cfg.offlinePollMs);
        return;
    }
    rootId = snap.rootEntry.id;
    if (!online) {
        online = true;
        offlineTimer.stop();
        emit q->onlineChanged(true);
        if (!online)
            return;
    }
    applySnapshot(snap);
}

// =============================================================================================
// Offline handling

bool Vault::Impl::rootHealthy() const
{
    const RootProbe p = probeRoot(rootEnc);
    if (!p.ok)
        return false;
    if (rootId.ino != 0 && !(p.st.id == rootId))
        return false; // another directory now sits at the vault path (e.g. the bare mount point)
    if (p.entries == 0 && nFiles > cfg.massDeleteMinFiles)
        return false; // looks like an empty mount point
    return true;
}

void Vault::Impl::goOffline()
{
    if (!online)
        return;
    qCInfo(lcVaultSync) << "vault offline:" << root;
    online = false;
    pendingEvents.clear();
    pendingOverflow = false;
    pendingRootLost = false;
    pendingBusy.clear();
    std::erase_if(pendingSnapshots, [](const FsBatch& b) { return !b.restart; });
    restartPending = false;
    offlineTimer.start(cfg.offlinePollMs);
    emit q->onlineChanged(false);
}

void Vault::Impl::offlineTick()
{
    if (online) {
        offlineTimer.stop();
        return;
    }
    if (restartPending)
        return;
    const RootProbe p = probeRoot(rootEnc);
    if (!p.ok)
        return;
    if (p.entries == 0 && nFiles > cfg.massDeleteMinFiles)
        return; // an empty mount point: wait for the real thing
    offlineTimer.stop();
    qCInfo(lcVaultSync) << "vault folder is back:" << root;
    if (watcher) {
        restartPending = true;
        watcher->restart(); // re-adds watches, then delivers a snapshot -> onRestartSnapshot()
        return;
    }
    OpScope op(this);
    onRestartSnapshot(*syncSnapshot());
}

// =============================================================================================
// Reconciliation

void Vault::Impl::reconcile(const QSet<QString>& touched, const QHash<QString, QString>& movedFrom,
                            const QSet<QString>& contentHints, const QSet<QString>& deep)
{
    if (!online)
        return;
    Scope s;

    auto scanDeep = [&](const QString& dir) {
        s.deepRoots.insert(dir);
        WalkOptions o;
        o.incomplete = &s.incomplete;
        o.acceptSymlinkedDir = [this](const DevIno& id) { return !hasRealFolderWithId(id); };
        int err = 0;
        const bool ok = walkTree(
            rootEnc, dir, [&](const QString& rel, const DiskEntry& de) { s.disk.insert(rel, de); }, o, &err);
        if (!ok) {
            if (err == ENOENT || err == ENOTDIR)
                s.disk.remove(dir); // vanished in the meantime
            else
                s.incomplete = true;
        }
    };

    auto statInto = [&](const QString& p) -> std::optional<DiskEntry> {
        s.checked.insert(p);
        int err = 0;
        std::optional<DiskEntry> e = statEntry(absEnc(p), &err);
        if (!e) {
            if (isTransientErrno(err))
                s.unknown.insert(p);
            return std::nullopt;
        }
        if (e->isDir && e->isSymlink && hasRealFolderWithId(e->id))
            return std::nullopt; // a symlink to a folder we already have under its real path
        s.disk.insert(p, *e);
        return e;
    };

    // Directories whose whole content must be re-read (e.g. a mount below them went away).
    for (const QString& req : deep) {
        QString dir = req;
        while (!dir.isEmpty() && !folderAt(dir))
            dir = parentPath(dir);
        if (s.deepRoots.contains(dir) || s.coveredByDeep(dir))
            continue;
        if (!dir.isEmpty()) {
            const std::optional<DiskEntry> e = statInto(dir);
            if (!e || !e->isDir)
                continue;
        }
        scanDeep(dir);
    }

    QList<QString> order(touched.cbegin(), touched.cend());
    std::sort(order.begin(), order.end(), depthThenPath);
    for (const QString& p0 : std::as_const(order)) {
        if (p0.isEmpty())
            continue;
        // A path below a folder we don't have is represented by its first missing ancestor.
        QString p = p0;
        for (qsizetype i = p0.indexOf(u'/'); i >= 0; i = p0.indexOf(u'/', i + 1)) {
            const TAbstractFile* a = byPath.value(p0.left(i));
            if (!a || !a->isFolder()) {
                p = p0.left(i);
                break;
            }
        }
        if (s.checked.contains(p) || s.coveredByDeep(p))
            continue;
        const std::optional<DiskEntry> e = statInto(p);
        if (e && e->isDir) {
            const TAbstractFile* m = byPath.value(p);
            if (!m || !m->isFolder())
                scanDeep(p); // a new folder: its content may predate its watch
        }
    }

    auto classify = [&](Classification& c) {
        QList<TAbstractFile*> scoped;
        QSet<const TAbstractFile*> seen;
        auto add = [&](TAbstractFile* m) {
            if (m && m != rootNode && !seen.contains(m)) {
                seen.insert(m);
                scoped.append(m);
            }
        };
        for (const QString& p : std::as_const(s.checked))
            add(byPath.value(p));
        for (const QString& dir : std::as_const(s.deepRoots))
            if (VaultFolderNode* f = folderAt(dir))
                forEachDescendant(f, add);

        QHash<DevIno, QString> freshById;
        for (auto it = s.disk.cbegin(); it != s.disk.cend(); ++it) {
            const TAbstractFile* m = byPath.value(it.key());
            if (!m || m->isFolder() != it->isDir) {
                c.fresh.append(it.key());
                if (!freshById.contains(it->id))
                    freshById.insert(it->id, it.key());
            }
        }

        QList<TAbstractFile*> gone;
        for (TAbstractFile* m : std::as_const(scoped)) {
            const QString path = m->path();
            const auto it = s.disk.constFind(path);
            if (it == s.disk.cend() || it->isDir != m->isFolder()) {
                if (s.knownMissing(path))
                    gone.append(m);
                continue;
            }
            if (m->isFolder())
                continue;
            VaultFileNode* f = fnode(m);
            const DiskEntry& e = *it;
            if (!(e.id == f->info.id)) {
                // Replaced. If our old inode now lives at a fresh path, the file was moved away and a
                // new one created here: report a rename + a creation, not a modification.
                const QString movedTo = freshById.value(f->info.id);
                if (!movedTo.isEmpty()) {
                    const DiskEntry me = s.disk.value(movedTo);
                    if (!me.isDir
                        && (movedFrom.value(movedTo) == path || (me.size == f->size() && me.mtimeNs == f->info.mtimeNs))) {
                        gone.append(m);
                        c.fresh.append(path);
                        continue;
                    }
                }
            }
            if (e.size != f->size() || e.mtimeNs != f->info.mtimeNs || !(e.id == f->info.id))
                c.changed.append({f, path});
            else if (contentHints.contains(path) && !f->knownHash.isEmpty())
                c.hashCheck.append({f, path}); // same stat, but written to: coarse timestamps?
        }
        for (TAbstractFile* g : std::as_const(gone)) {
            c.goneAll.insert(g);
            if (g->isFolder())
                forEachDescendant(static_cast<TFolder*>(g), [&](TAbstractFile* x) { c.goneAll.insert(x); });
        }
        for (TAbstractFile* g : std::as_const(gone)) {
            bool top = true;
            for (TFolder* a = g->parent(); a && top; a = a->parent())
                top = !c.goneAll.contains(a);
            if (top)
                c.goneTop.append(g);
        }
        std::sort(c.fresh.begin(), c.fresh.end(), depthThenPath);
        c.fresh.erase(std::unique(c.fresh.begin(), c.fresh.end()), c.fresh.end());
    };

    auto looksSame = [&](const TAbstractFile* g, const QString& fp, const DiskEntry& de) {
        if (!g->isFolder()) {
            const VaultFileNode* f = static_cast<const VaultFileNode*>(g);
            return f->size() == de.size && f->info.mtimeNs == de.mtimeNs;
        }
        const QList<TAbstractFile*>& kids = static_cast<const TFolder*>(g)->children();
        if (kids.isEmpty())
            return true;
        for (const TAbstractFile* k : kids)
            if (s.disk.contains(joinPath(fp, k->name())))
                return true;
        return false;
    };

    auto matchRenames = [&](const Classification& c) {
        QList<QPair<TAbstractFile*, QString>> out;
        if (c.goneAll.isEmpty() || c.fresh.isEmpty())
            return out;
        QHash<DevIno, TAbstractFile*> goneById;
        for (TAbstractFile* g : c.goneAll)
            goneById.insert(info(g).id, g);
        QSet<const TAbstractFile*> claimed;
        QSet<QString> targets;
        for (const QString& fp : c.fresh) {
            bool insideTarget = false;
            for (qsizetype i = fp.indexOf(u'/'); i >= 0 && !insideTarget; i = fp.indexOf(u'/', i + 1))
                insideTarget = targets.contains(fp.left(i));
            if (insideTarget)
                continue; // content of a folder moved in this round: matched by path afterwards
            const DiskEntry de = s.disk.value(fp);
            TAbstractFile* g = nullptr;
            const QString from = movedFrom.value(fp);
            if (!from.isEmpty()) { // inotify paired this move: trust it even if the file changed since
                TAbstractFile* cand = byPath.value(from);
                if (cand && c.goneAll.contains(cand) && cand->isFolder() == de.isDir)
                    g = cand;
            }
            if (!g) {
                TAbstractFile* cand = goneById.value(de.id);
                if (cand && cand->isFolder() == de.isDir && looksSame(cand, fp, de))
                    g = cand;
            }
            if (!g || claimed.contains(g))
                continue;
            if (g->isFolder() && isStrictlyInside(fp, g->path()))
                continue;
            claimed.insert(g);
            targets.insert(fp);
            out.append({g, fp});
        }
        return out;
    };

    auto applyRename = [&](TAbstractFile* g, const QString& target) {
        if (!isLive(g) || g->path() == target)
            return;
        if (TAbstractFile* occupant = byPath.value(target)) {
            // e.g. a file replaced by a folder moved in from elsewhere: the occupant is gone
            if (occupant == g || (occupant->isFolder() && isStrictlyInside(g->path(), occupant->path())))
                return;
            removeSubtree(occupant, true);
        }
        VaultFolderNode* parent = rootNode;
        const QString parentP = parentPath(target);
        if (!parentP.isEmpty()) {
            QString prefix;
            for (const QString& comp : parentP.split(u'/')) {
                prefix = joinPath(prefix, comp);
                TAbstractFile* it = byPath.value(prefix);
                if (it && it->isFolder()) {
                    parent = dnode(it);
                    continue;
                }
                const auto de = s.disk.constFind(prefix);
                if (de == s.disk.cend() || !de->isDir)
                    return;
                if (it) {
                    if (it == g)
                        return;
                    removeSubtree(it, true); // a file where the disk now has a folder
                }
                auto* n = dnode(addNode(parent, prefix, *de));
                emit q->created(n, true);
                if (!isLive(n))
                    return;
                parent = n;
            }
        }
        if (!isLive(g) || !isLive(parent) || byPath.contains(target))
            return;
        emitRenamed(moveNode(g, parent, fileNameOf(target)), true);
    };

    auto applyChange = [&](VaultFileNode* f, const DiskEntry& e) {
        const QString path = f->path();
        if (e.size == f->size() && e.mtimeNs == f->info.mtimeNs && !f->knownHash.isEmpty()) {
            // Only the inode differs (replaced by an identical copy?): compare the content.
            if (diskHash(path) == f->knownHash) {
                setFileStat(f, e);
                return;
            }
        }
        std::optional<QByteArray> hash;
        const auto hashFn = [&] {
            if (!hash)
                hash = diskHash(path);
            return *hash;
        };
        if (selfWrites.isOwnWrite(path, e.size, e.mtimeNs, hashFn)) {
            // Our own write, seen with a different stat (FUSE attribute caching, coarse clocks...).
            const SelfWriteRegistry::Entry* own = selfWrites.find(path);
            setFileStat(f, e);
            f->knownHash = own ? own->hash : QByteArray();
            return;
        }
        setFileStat(f, e);
        f->knownHash.clear();
        emit q->modified(f, true);
    };

    Classification c;
    classify(c);

    // Mass-delete guard: before reporting a big share of the vault as deleted, make sure the vault
    // folder is really there (a flaky or unmounting drive must not look like a mass deletion).
    int goneFiles = 0;
    for (const TAbstractFile* g : std::as_const(c.goneAll))
        if (!g->isFolder())
            ++goneFiles;
    if (goneFiles > cfg.massDeleteMinFiles && goneFiles > cfg.massDeleteFraction * nFiles && !rootHealthy()) {
        qCWarning(lcVaultSync) << goneFiles << "of" << nFiles << "files seem gone and the vault folder looks unavailable";
        goOffline();
        return;
    }

    for (int round = 0; round < 8; ++round) {
        const QList<QPair<TAbstractFile*, QString>> renames = matchRenames(c);
        if (renames.isEmpty())
            break;
        for (const auto& [g, target] : renames)
            applyRename(g, target);
        c = Classification{};
        classify(c);
    }

    for (TAbstractFile* g : std::as_const(c.goneTop))
        if (isLive(g))
            removeSubtree(g, true);

    QList<QString> created = s.disk.keys();
    std::sort(created.begin(), created.end(), depthThenPath);
    for (const QString& fp : std::as_const(created)) {
        if (fp.isEmpty() || byPath.contains(fp))
            continue;
        VaultFolderNode* parent = folderAt(parentPath(fp));
        if (!parent)
            continue;
        TAbstractFile* n = addNode(parent, fp, s.disk.value(fp));
        emit q->created(n, true);
    }

    for (const auto& [f, path] : std::as_const(c.changed)) {
        if (!isLive(f) || f->path() != path)
            continue;
        const auto it = s.disk.constFind(path);
        if (it != s.disk.cend() && !it->isDir)
            applyChange(f, *it);
    }
    for (const auto& [f, path] : std::as_const(c.hashCheck)) {
        if (!isLive(f) || f->path() != path)
            continue;
        const QByteArray h = diskHash(path);
        if (!h.isEmpty() && h != f->knownHash) {
            f->knownHash = h;
            emit q->modified(f, true);
        }
    }
}

} // namespace oc
