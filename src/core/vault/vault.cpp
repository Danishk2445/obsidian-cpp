#include "core/vault/vault.h"

#include "core/util/busy.h"
#include "core/vault/ignore_rules.h"
#include "core/vault/trash.h"
#include "core/vault/vault_impl.h"
#include "text/codec.h"

#include <QDir>
#include <QFileInfo>

#include <cerrno>
#include <chrono>

#include <sys/stat.h>
#include <unistd.h>

namespace oc {

using namespace vaultfs;

VaultConfig VaultConfig::fromEnvironment()
{
    VaultConfig c;
    c.watcher = qEnvironmentVariable("OC_VAULT_WATCHER");
    bool ok = false;
    int v = qEnvironmentVariableIntValue("OC_VAULT_POLL_MS", &ok);
    if (ok && v > 0)
        c.pollIntervalMs = v;
    v = qEnvironmentVariableIntValue("OC_VAULT_SWEEP_MS", &ok);
    if (ok && v > 0)
        c.hybridSweepMs = v;
    return c;
}

// =============================================================================================
// Impl: construction, helpers, model mutation

Vault::Impl::Impl(Vault* q_, QString rootDir)
    : q(q_)
{
    root = QDir::cleanPath(QFileInfo(rootDir).absoluteFilePath());
    rootCanonical = QFileInfo(root).canonicalFilePath();
    if (rootCanonical.isEmpty())
        rootCanonical = root;
    rootEnc = encodeName(root);
    cfg = VaultConfig::fromEnvironment();

    rootNode = new VaultFolderNode;
    byPath.insert(QString(), rootNode);
    live.insert(rootNode);

    processTimer.setSingleShot(true);
    QObject::connect(&processTimer, &QTimer::timeout, q, [this] { processPending(); });
    QObject::connect(&offlineTimer, &QTimer::timeout, q, [this] { offlineTick(); });
}

Vault::Impl::~Impl()
{
    processTimer.stop();
    offlineTimer.stop();
    watcher.reset(); // joins the watcher thread
    pendingSnapshots.clear();
    pendingBusy.clear();
    for (TAbstractFile* item : std::as_const(byPath))
        delete item;
    byPath.clear();
    live.clear();
}

QString Vault::Impl::abs(const QString& vaultPath) const
{
    if (vaultPath.isEmpty())
        return root;
    if (root.endsWith(u'/'))
        return root + vaultPath;
    return root + u'/' + vaultPath;
}

VaultFolderNode* Vault::Impl::folderAt(const QString& path) const
{
    TAbstractFile* it = byPath.value(path);
    return it && it->isFolder() ? dnode(it) : nullptr;
}

bool Vault::Impl::checkOnline(QString* error) const
{
    if (online)
        return true;
    if (error)
        *error = QStringLiteral("The vault folder is not available (drive unmounted?).");
    return false;
}

bool Vault::Impl::validateNewPath(const QString& in, QString* out, QString* error) const
{
    if (!normalizePath(in, out)) {
        if (error)
            *error = QStringLiteral("Invalid path.");
        return false;
    }
    if (isIgnoredPath(*out)) {
        if (error)
            *error = QStringLiteral("Names starting with a dot or looking like temporary files are not allowed.");
        return false;
    }
    return true;
}

bool Vault::Impl::existsOnDisk(const QString& vaultPath) const
{
    struct stat st {};
    return ::lstat(absEnc(vaultPath).constData(), &st) == 0;
}

void Vault::Impl::setFileStat(VaultFileNode* f, const DiskEntry& e)
{
    f->m_size = e.size;
    f->m_mtimeMs = e.mtimeNs / 1'000'000;
    f->m_ctimeMs = e.btimeMs >= 0 ? e.btimeMs : f->m_mtimeMs;
    f->info = VaultNodeInfo{e.id, e.mtimeNs, e.isSymlink};
}

void Vault::Impl::setPathAndName(TAbstractFile* item, const QString& path)
{
    item->m_path = path;
    item->m_name = fileNameOf(path);
    if (!item->isFolder()) {
        auto* f = static_cast<TFile*>(item);
        splitName(item->m_name, &f->m_basename, &f->m_extension);
    }
}

TAbstractFile* Vault::Impl::addNode(VaultFolderNode* parent, const QString& path, const DiskEntry& e)
{
    TAbstractFile* n = nullptr;
    if (e.isDir) {
        auto* folder = new VaultFolderNode;
        folder->info = VaultNodeInfo{e.id, e.mtimeNs, e.isSymlink};
        n = folder;
    } else {
        auto* file = new VaultFileNode;
        setFileStat(file, e);
        n = file;
        ++nFiles;
    }
    setPathAndName(n, path);
    n->m_parent = parent;
    parent->m_children.append(n);
    byPath.insert(path, n);
    live.insert(n);
    return n;
}

void Vault::Impl::forEachDescendant(TFolder* folder, const std::function<void(TAbstractFile*)>& fn) const
{
    for (TAbstractFile* c : folder->children()) {
        fn(c);
        if (c->isFolder())
            forEachDescendant(static_cast<TFolder*>(c), fn);
    }
}

void Vault::Impl::removeSubtree(TAbstractFile* item, bool external)
{
    if (!isLive(item) || item == rootNode)
        return;
    QList<TAbstractFile*> postOrder;
    std::function<void(TAbstractFile*)> collect = [&](TAbstractFile* x) {
        if (x->isFolder())
            for (TAbstractFile* c : static_cast<TFolder*>(x)->children())
                collect(c);
        postOrder.append(x);
    };
    collect(item);
    if (TFolder* parent = item->parent())
        dnode(parent)->m_children.removeOne(item);
    for (TAbstractFile* x : std::as_const(postOrder)) {
        if (byPath.value(x->path()) == x)
            byPath.remove(x->path());
        live.remove(x);
        if (!x->isFolder())
            --nFiles;
    }
    for (TAbstractFile* x : std::as_const(postOrder))
        emit q->deleted(x, external);
    qDeleteAll(postOrder);
}

QList<QPair<TAbstractFile*, QString>> Vault::Impl::moveNode(TAbstractFile* item, VaultFolderNode* newParent,
                                                            const QString& newName)
{
    QList<QPair<TAbstractFile*, QString>> moved;
    const QString oldPath = item->path();
    const QString newPath = joinPath(newParent->path(), newName);
    moved.append({item, oldPath});
    if (item->isFolder())
        forEachDescendant(static_cast<TFolder*>(item), [&](TAbstractFile* x) { moved.append({x, x->path()}); });
    for (const auto& [x, old] : std::as_const(moved))
        if (byPath.value(old) == x)
            byPath.remove(old);
    if (TFolder* oldParent = item->parent())
        dnode(oldParent)->m_children.removeOne(item);
    item->m_parent = newParent;
    newParent->m_children.append(item);
    setPathAndName(item, newPath);
    for (qsizetype i = 1; i < moved.size(); ++i)
        setPathAndName(moved[i].first, QString(newPath + QStringView(moved[i].second).mid(oldPath.size())));
    for (const auto& [x, old] : std::as_const(moved))
        byPath.insert(x->path(), x);
    return moved;
}

void Vault::Impl::emitRenamed(const QList<QPair<TAbstractFile*, QString>>& moved, bool external)
{
    for (const auto& [x, old] : moved)
        if (isLive(x))
            emit q->renamed(x, old, external);
}

bool Vault::Impl::hasRealFolderWithId(const DevIno& id) const
{
    for (TAbstractFile* x : std::as_const(byPath)) {
        if (!x->isFolder())
            continue;
        const VaultNodeInfo& in = dnode(x)->info;
        if (!in.symlink && in.id == id)
            return true;
    }
    return false;
}

VaultFolderNode* Vault::Impl::adoptFolder(VaultFolderNode* parent, const QString& path, const DiskEntry& e,
                                          bool externalSelf)
{
    auto* node = dnode(addNode(parent, path, e));
    QList<QPair<QString, DiskEntry>> entries;
    WalkOptions o;
    o.acceptSymlinkedDir = [this](const DevIno& id) { return !hasRealFolderWithId(id); };
    walkTree(rootEnc, path, [&](const QString& rel, const DiskEntry& de) { entries.append({rel, de}); }, o);
    emit q->created(node, externalSelf);
    for (const auto& [rel, de] : std::as_const(entries)) {
        if (byPath.contains(rel))
            continue;
        VaultFolderNode* par = folderAt(parentPath(rel));
        if (!par)
            continue;
        TAbstractFile* n = addNode(par, rel, de);
        emit q->created(n, true);
    }
    return isLive(node) ? node : nullptr;
}

VaultFolderNode* Vault::Impl::ensureFolderChain(const QString& folderPath, QString* error)
{
    VaultFolderNode* cur = rootNode;
    if (folderPath.isEmpty())
        return cur;
    // Folders we know may have just been deleted on disk (the watcher hasn't told us yet): then
    // they are re-created along the way.
    const bool leafMissingOnDisk = !existsOnDisk(folderPath);
    QString prefix;
    for (const QString& comp : folderPath.split(u'/')) {
        prefix = joinPath(prefix, comp);
        if (TAbstractFile* it = byPath.value(prefix)) {
            if (!it->isFolder()) {
                if (error)
                    *error = QStringLiteral("\"%1\" is a file, not a folder.").arg(prefix);
                return nullptr;
            }
            if (leafMissingOnDisk && ::mkdir(absEnc(prefix).constData(), 0777) != 0 && errno != EEXIST) {
                if (error)
                    *error = QStringLiteral("Cannot create folder \"%1\": %2").arg(prefix, errnoString(errno));
                return nullptr;
            }
            cur = dnode(it);
            continue;
        }
        const QByteArray a = absEnc(prefix);
        if (::mkdir(a.constData(), 0777) != 0) {
            const int e = errno;
            std::optional<DiskEntry> st = statEntry(a);
            if (e != EEXIST || !st || !st->isDir) {
                if (error)
                    *error = QStringLiteral("Cannot create folder \"%1\": %2")
                                 .arg(prefix, e == EEXIST ? QStringLiteral("a file with that name exists") : errnoString(e));
                return nullptr;
            }
            // Exists on disk but not (yet) in the model: take it over with whatever it contains.
            cur = adoptFolder(cur, prefix, *st, false);
        } else {
            std::optional<DiskEntry> st = statEntry(a);
            if (!st) {
                if (error)
                    *error = QStringLiteral("Folder \"%1\" vanished after creating it.").arg(prefix);
                return nullptr;
            }
            auto* node = dnode(addNode(cur, prefix, *st));
            emit q->created(node, false);
            cur = isLive(node) ? node : nullptr;
        }
        if (!cur) {
            if (error)
                *error = QStringLiteral("Folder \"%1\" was removed while being created.").arg(prefix);
            return nullptr;
        }
    }
    return cur;
}

QByteArray Vault::Impl::diskHash(const QString& vaultPath) const
{
    QByteArray bytes;
    DiskEntry st;
    int e = 0;
    if (!readWholeFile(absEnc(vaultPath), &bytes, &st, &e))
        return QByteArray();
    return contentHash(bytes);
}

// =============================================================================================
// Vault: model access

Vault::Vault(QString rootDir, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>(this, std::move(rootDir)))
{
}

Vault::~Vault() = default;

const QString& Vault::root() const { return d->root; }

QString Vault::name() const { return QFileInfo(d->root).fileName(); }

QString Vault::absolutePath(const QString& vaultPath) const { return d->abs(vaultPath); }

std::optional<QString> Vault::vaultPath(const QString& absolutePath) const
{
    const QString p = QDir::cleanPath(absolutePath);
    for (const QString* r : {&d->root, &d->rootCanonical}) {
        if (p == *r)
            return QString();
        const QString prefix = r->endsWith(u'/') ? *r : QString(*r + u'/');
        if (p.startsWith(prefix))
            return p.mid(prefix.size());
    }
    return std::nullopt;
}

void Vault::load()
{
    if (d->loaded) {
        rescan();
        return;
    }
    Busy::Scope busy;
    d->startWatching();
    d->loaded = true;
    emit loaded();
    if (!d->online)
        emit onlineChanged(false);
}

bool Vault::isLoaded() const { return d->loaded; }

bool Vault::isOnline() const { return d->online; }

TFolder* Vault::rootFolder() const { return d->rootNode; }

TAbstractFile* Vault::item(const QString& path) const
{
    if (path == u"/")
        return d->rootNode;
    return d->byPath.value(path);
}

TFile* Vault::file(const QString& path) const
{
    TAbstractFile* it = item(path);
    return it && !it->isFolder() ? static_cast<TFile*>(it) : nullptr;
}

TFolder* Vault::folder(const QString& path) const
{
    TAbstractFile* it = item(path);
    return it && it->isFolder() ? static_cast<TFolder*>(it) : nullptr;
}

QList<TFile*> Vault::files() const
{
    QList<TFile*> out;
    out.reserve(d->nFiles);
    for (TAbstractFile* x : std::as_const(d->byPath))
        if (!x->isFolder())
            out.append(static_cast<TFile*>(x));
    return out;
}

QList<TFile*> Vault::markdownFiles() const
{
    QList<TFile*> out;
    for (TAbstractFile* x : std::as_const(d->byPath))
        if (!x->isFolder() && static_cast<TFile*>(x)->isMarkdown())
            out.append(static_cast<TFile*>(x));
    return out;
}

QList<TFolder*> Vault::folders() const
{
    QList<TFolder*> out;
    for (TAbstractFile* x : std::as_const(d->byPath))
        if (x->isFolder() && x != d->rootNode)
            out.append(static_cast<TFolder*>(x));
    return out;
}

int Vault::fileCount() const { return d->nFiles; }

bool Vault::isIgnoredName(const QString& name) { return vaultfs::isIgnoredName(name); }

// =============================================================================================
// DocumentStorage

std::optional<QByteArray> Vault::read(const QString& path, DiskState* state, QString* error)
{
    if (state)
        *state = DiskState{};
    QString p;
    if (!normalizePath(path, &p) || isIgnoredPath(p)) {
        if (error)
            *error = QStringLiteral("Invalid path.");
        return std::nullopt;
    }
    QByteArray bytes;
    DiskEntry st;
    int e = 0;
    if (!readWholeFile(d->absEnc(p), &bytes, &st, &e)) {
        if (error)
            *error = errnoString(e);
        return std::nullopt;
    }
    const QByteArray hash = contentHash(bytes);
    if (state)
        *state = DiskState{st.size, st.mtimeNs, hash};
    if (TFile* f = file(p)) {
        VaultFileNode* n = Impl::fnode(f);
        if (n->size() == st.size && n->info.mtimeNs == st.mtimeNs)
            n->knownHash = hash;
    }
    return bytes;
}

DiskState Vault::stat(const QString& path)
{
    QString p;
    if (!normalizePath(path, &p) || isIgnoredPath(p))
        return DiskState{};
    if (!d->online) {
        // Keep reporting what we last saw: an unmounted drive must not look like deleted files.
        if (TFile* f = file(p)) {
            const VaultFileNode* n = Impl::fnode(f);
            return DiskState{n->size(), n->info.mtimeNs, n->knownHash};
        }
        return DiskState{};
    }
    std::optional<DiskEntry> st = statEntry(d->absEnc(p));
    if (!st || st->isDir)
        return DiskState{};
    return DiskState{st->size, st->mtimeNs, QByteArray()};
}

namespace {

qint64 wallClockNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Vault::WriteResult Vault::write(const QString& path, const QByteArray& bytes, const DiskState& expected,
                                DiskState* newState, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return WriteResult::Error;
    };
    if (!d->checkOnline(error))
        return WriteResult::Error;
    QString p;
    if (!d->validateNewPath(path, &p, error))
        return WriteResult::Error;
    if (TAbstractFile* it = item(p); it && it->isFolder())
        return fail(QStringLiteral("\"%1\" is a folder.").arg(p));

    Impl::OpScope op(d.get());
    const QByteArray abs = d->absEnc(p);
    int e = 0;
    std::optional<DiskEntry> cur = statEntry(abs, &e);
    if (!cur && e != ENOENT && e != 0)
        return fail(errnoString(e));
    if (cur && cur->isDir)
        return fail(QStringLiteral("\"%1\" is a folder.").arg(p));
    const QByteArray newHash = contentHash(bytes);

    // Current disk content, read only when needed (the read also refreshes size/mtime consistently).
    std::optional<QByteArray> diskBytes;
    auto loadDisk = [&]() -> bool {
        QByteArray b;
        DiskEntry st;
        int re = 0;
        if (!readWholeFile(abs, &b, &st, &re))
            return false;
        diskBytes = std::move(b);
        cur->size = st.size;
        cur->mtimeNs = st.mtimeNs;
        return true;
    };

    const bool mustNotExist = expected.size < 0;
    if (mustNotExist && cur)
        return WriteResult::Conflict;
    if (!mustNotExist && !cur)
        return WriteResult::Conflict; // deleted (or renamed away) behind our back
    if (cur) {
        const bool statMatch = cur->size == expected.size && cur->mtimeNs == expected.mtimeNs;
        // Timestamps without sub-millisecond precision (FAT, exFAT, HFS+, some FUSE) or very recent
        // ones can hide a same-size change made within the same tick: then trust only the hash.
        const bool coarse = cur->mtimeNs % 1'000'000 == 0 || wallClockNs() - cur->mtimeNs < 2'000'000'000;
        const bool needDisk = !statMatch || (!expected.hash.isEmpty() && coarse) || cur->size == bytes.size();
        if (needDisk && !loadDisk())
            return fail(QStringLiteral("Cannot read \"%1\" before writing it.").arg(p));
        if (diskBytes && *diskBytes == bytes) {
            // Nothing to do: the file already holds exactly these bytes. Never rewrite it.
            if (newState)
                *newState = DiskState{cur->size, cur->mtimeNs, newHash};
            if (TFile* f = file(p)) {
                VaultFileNode* n = Impl::fnode(f);
                if (n->size() == cur->size && n->info.mtimeNs == cur->mtimeNs)
                    n->knownHash = newHash;
            }
            return WriteResult::Ok;
        }
        const bool statMatchNow = cur->size == expected.size && cur->mtimeNs == expected.mtimeNs;
        if (!statMatchNow) {
            // Changed on disk; only fine if the content is still what the caller based its edit on
            // (e.g. a touch, or a checkout that rewrote identical bytes).
            if (expected.hash.isEmpty() || !diskBytes || contentHash(*diskBytes) != expected.hash)
                return WriteResult::Conflict;
        } else if (!expected.hash.isEmpty() && diskBytes && contentHash(*diskBytes) != expected.hash) {
            return WriteResult::Conflict;
        }
    }

    VaultFolderNode* parent = d->ensureFolderChain(parentPath(p), error);
    if (!parent)
        return WriteResult::Error;
    QString werr;
    const IoResult r = writeFileAtomic(abs, bytes, cur ? WriteMode::Replace : WriteMode::CreateNew, &werr);
    if (r == IoResult::Exists)
        return WriteResult::Conflict;
    if (r == IoResult::Error)
        return fail(werr);
    std::optional<DiskEntry> st = statEntry(abs, &e);
    if (!st || st->isDir)
        return fail(QStringLiteral("\"%1\" vanished right after being written.").arg(p));
    d->selfWrites.recordWrite(p, st->size, st->mtimeNs, newHash, st->id);
    if (newState)
        *newState = DiskState{st->size, st->mtimeNs, newHash};

    TAbstractFile* existing = d->byPath.value(p);
    if (existing && !existing->isFolder()) {
        VaultFileNode* f = Impl::fnode(existing);
        Impl::setFileStat(f, *st);
        f->knownHash = newHash;
        emit modified(f, false);
    } else if (!existing && d->isLive(parent)) {
        VaultFileNode* f = Impl::fnode(d->addNode(parent, p, *st));
        f->knownHash = newHash;
        emit created(f, false);
    }
    return WriteResult::Ok;
}

// =============================================================================================
// File operations

TFile* Vault::createFile(const QString& path, const QByteArray& bytes, QString* error)
{
    if (!d->checkOnline(error))
        return nullptr;
    QString p;
    if (!d->validateNewPath(path, &p, error))
        return nullptr;
    if (d->byPath.contains(p) || d->existsOnDisk(p)) {
        if (error)
            *error = QStringLiteral("\"%1\" already exists.").arg(p);
        return nullptr;
    }
    Impl::OpScope op(d.get());
    VaultFolderNode* parent = d->ensureFolderChain(parentPath(p), error);
    if (!parent)
        return nullptr;
    const QByteArray abs = d->absEnc(p);
    QString werr;
    const IoResult r = writeFileAtomic(abs, bytes, WriteMode::CreateNew, &werr);
    if (r != IoResult::Ok) {
        if (error)
            *error = r == IoResult::Exists ? QStringLiteral("\"%1\" already exists.").arg(p) : werr;
        return nullptr;
    }
    std::optional<DiskEntry> st = statEntry(abs);
    if (!st || st->isDir || !d->isLive(parent) || d->byPath.contains(p)) {
        if (error)
            *error = QStringLiteral("\"%1\" changed while being created.").arg(p);
        return nullptr;
    }
    const QByteArray hash = contentHash(bytes);
    d->selfWrites.recordWrite(p, st->size, st->mtimeNs, hash, st->id);
    VaultFileNode* f = Impl::fnode(d->addNode(parent, p, *st));
    f->knownHash = hash;
    emit created(f, false);
    return d->isLive(f) ? f : nullptr;
}

TFolder* Vault::createFolder(const QString& path, QString* error)
{
    if (!d->checkOnline(error))
        return nullptr;
    QString p;
    if (!d->validateNewPath(path, &p, error))
        return nullptr;
    if (TAbstractFile* it = d->byPath.value(p); it && !it->isFolder()) {
        if (error)
            *error = QStringLiteral("A file named \"%1\" already exists.").arg(p);
        return nullptr;
    }
    Impl::OpScope op(d.get());
    return d->ensureFolderChain(p, error); // mkpath: returns the existing folder (re-created on disk if needed)
}

bool Vault::rename(TAbstractFile* item, const QString& newPath, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!d->checkOnline(error))
        return false;
    if (!d->isLive(item))
        return fail(QStringLiteral("Unknown file."));
    if (item == d->rootNode)
        return fail(QStringLiteral("The vault root cannot be renamed."));
    QString p;
    if (!d->validateNewPath(newPath, &p, error))
        return false;
    const QString oldPath = item->path();
    if (p == oldPath)
        return true;
    if (item->isFolder() && isStrictlyInside(p, oldPath))
        return fail(QStringLiteral("A folder cannot be moved into itself."));
    if (TAbstractFile* occupant = d->byPath.value(p); occupant && occupant != item)
        return fail(QStringLiteral("\"%1\" already exists.").arg(p));

    const QByteArray from = d->absEnc(oldPath);
    const QByteArray to = d->absEnc(p);
    struct stat sf {};
    if (::lstat(from.constData(), &sf) != 0)
        return fail(QStringLiteral("Cannot rename \"%1\": %2").arg(oldPath, errnoString(errno)));
    bool sameFileOtherCase = false;
    struct stat stTo {};
    if (::lstat(to.constData(), &stTo) == 0) {
        // On a case-insensitive filesystem "Note.md" -> "note.md" finds the source itself.
        if (stTo.st_dev == sf.st_dev && stTo.st_ino == sf.st_ino)
            sameFileOtherCase = true;
        else
            return fail(QStringLiteral("\"%1\" already exists.").arg(p));
    }

    Impl::OpScope op(d.get());
    VaultFolderNode* parent = d->ensureFolderChain(parentPath(p), error);
    if (!parent)
        return false;
    if (!d->isLive(item) || !d->isLive(parent))
        return fail(QStringLiteral("The file changed while being renamed."));
    int re = 0;
    if (sameFileOtherCase) {
        const qsizetype slash = from.lastIndexOf('/');
        const QByteArray tmp = from.left(slash) + "/.oc~rename~" + randomHex(6);
        if (::rename(from.constData(), tmp.constData()) != 0) {
            re = errno;
        } else if (::rename(tmp.constData(), to.constData()) != 0) {
            re = errno;
            (void)::rename(tmp.constData(), from.constData());
        }
    } else {
        re = renameNoReplace(from, to);
    }
    if (re != 0)
        return fail(QStringLiteral("Cannot rename \"%1\": %2").arg(oldPath, errnoString(re)));

    const auto moved = d->moveNode(item, parent, fileNameOf(p));
    d->selfWrites.recordRename(oldPath, p, Impl::info(item).id);
    d->emitRenamed(moved, false);
    return true;
}

bool Vault::trash(TAbstractFile* item, TrashMode mode, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!d->checkOnline(error))
        return false;
    if (!d->isLive(item))
        return fail(QStringLiteral("Unknown file."));
    if (item == d->rootNode)
        return fail(QStringLiteral("The vault root cannot be deleted."));

    Impl::OpScope op(d.get()); // also keeps KIO's nested event loop from running our watcher processing
    const QString path = item->path();
    const QString absPath = d->abs(path);
    const QByteArray absEnc = encodeName(absPath);
    const DevIno id = Impl::info(item).id;
    struct stat st {};
    if (::lstat(absEnc.constData(), &st) != 0 && errno == ENOENT) {
        d->removeSubtree(item, false); // already gone from disk
        return true;
    }
    bool ok = false;
    QString err;
    switch (mode) {
    case TrashMode::System: ok = trashToSystem(absPath, &err); break;
    case TrashMode::Local: ok = trashToLocalFolder(d->root, absPath, item->isFolder(), &err); break;
    case TrashMode::Permanent: ok = removeRecursively(absEnc, &err); break;
    }
    if (!ok)
        return fail(err);
    d->selfWrites.recordRemoved(path, id);
    d->removeSubtree(item, false);
    return true;
}

TFile* Vault::copy(TFile* file, const QString& newPath, QString* error)
{
    auto fail = [&](const QString& msg) -> TFile* {
        if (error)
            *error = msg;
        return nullptr;
    };
    if (!d->checkOnline(error))
        return nullptr;
    if (!d->isLive(file))
        return fail(QStringLiteral("Unknown file."));
    QString p;
    if (!d->validateNewPath(newPath, &p, error))
        return nullptr;
    if (d->byPath.contains(p) || d->existsOnDisk(p))
        return fail(QStringLiteral("\"%1\" already exists.").arg(p));
    QByteArray bytes;
    DiskEntry src;
    int e = 0;
    if (!readWholeFile(d->absEnc(file->path()), &bytes, &src, &e))
        return fail(QStringLiteral("Cannot read \"%1\": %2").arg(file->path(), errnoString(e)));

    Impl::OpScope op(d.get());
    VaultFolderNode* parent = d->ensureFolderChain(parentPath(p), error);
    if (!parent)
        return nullptr;
    const QByteArray abs = d->absEnc(p);
    QString werr;
    const IoResult r = writeFileAtomic(abs, bytes, WriteMode::CreateNew, &werr, mode_t(src.mode & 07777));
    if (r != IoResult::Ok)
        return fail(r == IoResult::Exists ? QStringLiteral("\"%1\" already exists.").arg(p) : werr);
    std::optional<DiskEntry> st = statEntry(abs);
    if (!st || st->isDir || !d->isLive(parent) || d->byPath.contains(p))
        return fail(QStringLiteral("\"%1\" changed while being created.").arg(p));
    const QByteArray hash = contentHash(bytes);
    d->selfWrites.recordWrite(p, st->size, st->mtimeNs, hash, st->id);
    VaultFileNode* f = Impl::fnode(d->addNode(parent, p, *st));
    f->knownHash = hash;
    emit created(f, false);
    return d->isLive(f) ? f : nullptr;
}

QString Vault::availablePath(const QString& folderPath, const QString& baseName, const QString& ext) const
{
    QString folderNorm;
    if (!normalizePath(folderPath, &folderNorm, /*allowRoot*/ true))
        folderNorm.clear();
    const TFolder* parent = folder(folderNorm);
    auto taken = [&](const QString& name) {
        const QString path = joinPath(folderNorm, name);
        if (d->byPath.contains(path))
            return true;
        if (parent) // case-insensitive: don't create "untitled.md" next to "Untitled.md" (NTFS, Windows)
            for (const TAbstractFile* c : parent->children())
                if (c->name().compare(name, Qt::CaseInsensitive) == 0)
                    return true;
        return d->existsOnDisk(path);
    };
    for (int n = 0;; ++n) {
        QString name = n == 0 ? baseName : QString(baseName + u' ' + QString::number(n));
        if (!ext.isEmpty())
            name += u'.' + ext;
        if (!taken(name))
            return joinPath(folderNorm, name);
    }
}

void Vault::rescan()
{
    if (!d->loaded)
        return;
    if (!d->online) {
        d->offlineTick();
        return;
    }
    if (d->watcher) {
        d->watcher->requestSweep();
        return;
    }
    Busy::Scope busy;
    std::shared_ptr<FsSnapshot> snap = d->syncSnapshot();
    Impl::OpScope op(d.get());
    d->applySnapshot(*snap);
}

// =============================================================================================
// VaultImplAccess

VaultConfig& VaultImplAccess::config(Vault& v) { return v.d->cfg; }

QString VaultImplAccess::watcherBackend(const Vault& v)
{
    QString s = watcherKindName(v.d->watcherKind);
    if (v.d->watcher && v.d->watcherKind != WatcherKind::Poll && !v.d->watcher->usingInotify())
        s += QStringLiteral("+poll");
    return s;
}

int VaultImplAccess::watcherPasses(const Vault& v) { return v.d->passes; }

void VaultImplAccess::setDropWatcherEvents(Vault& v, bool drop) { v.d->dropWatcherEvents = drop; }

int VaultImplAccess::selfWriteEntries(const Vault& v) { return v.d->selfWrites.count(); }

} // namespace oc
