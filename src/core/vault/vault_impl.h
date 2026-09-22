#pragma once

// Vault::Impl — shared by vault.cpp (model + operations), vault_sync.cpp (watcher integration,
// reconciliation, offline handling). Private to src/core/vault.

#include "core/vault/fs_scan.h"
#include "core/vault/fs_watcher.h"
#include "core/vault/vault.h"
#include "core/vault/vault_io.h"
#include "core/vault/vault_private.h"

#include <QHash>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace oc {

// Extra per-node state (TFile/TFolder are frozen; nodes are always created as these subclasses).
struct VaultNodeInfo {
    vaultfs::DevIno id; // of the target for symlinks
    qint64 mtimeNs = 0;
    bool symlink = false;
};

class VaultFileNode final : public TFile {
public:
    VaultNodeInfo info;
    QByteArray knownHash; // contentHash() of the on-disk bytes when known (read()/write() through the vault)
};

class VaultFolderNode final : public TFolder {
public:
    VaultNodeInfo info;
};

struct Vault::Impl {
    Impl(Vault* q, QString root);
    ~Impl();

    // ---- helpers
    QString abs(const QString& vaultPath) const;
    QByteArray absEnc(const QString& vaultPath) const { return vaultfs::encodeName(abs(vaultPath)); }
    static VaultFileNode* fnode(TAbstractFile* f) { return static_cast<VaultFileNode*>(f); }
    static VaultFolderNode* dnode(TAbstractFile* f) { return static_cast<VaultFolderNode*>(f); }
    static const VaultNodeInfo& info(const TAbstractFile* f)
    {
        return f->isFolder() ? static_cast<const VaultFolderNode*>(f)->info : static_cast<const VaultFileNode*>(f)->info;
    }
    bool isLive(const TAbstractFile* f) const { return f && live.contains(f); }
    VaultFolderNode* folderAt(const QString& path) const;
    bool checkOnline(QString* error) const;
    bool validateNewPath(const QString& in, QString* out, QString* error) const;
    bool existsOnDisk(const QString& vaultPath) const;

    // ---- model mutation (no signals unless stated)
    TAbstractFile* addNode(VaultFolderNode* parent, const QString& path, const vaultfs::DiskEntry& e);
    static void setFileStat(VaultFileNode* f, const vaultfs::DiskEntry& e);
    static void setPathAndName(TAbstractFile* item, const QString& path);
    void forEachDescendant(TFolder* folder, const std::function<void(TAbstractFile*)>& fn) const; // pre-order
    // Detaches the whole subtree, emits deleted() for each item (deepest first, the item last), then
    // destroys them.
    void removeSubtree(TAbstractFile* item, bool external);
    // Moves/renames a node (and re-paths its descendants). Returns (item, oldPath), pre-order.
    QList<QPair<TAbstractFile*, QString>> moveNode(TAbstractFile* item, VaultFolderNode* newParent,
                                                   const QString& newName);
    void emitRenamed(const QList<QPair<TAbstractFile*, QString>>& moved, bool external);
    // Adds a directory that exists on disk (but not in the model) together with its contents,
    // emitting created() for each (the folder with `externalSelf`, its contents as external).
    VaultFolderNode* adoptFolder(VaultFolderNode* parent, const QString& path, const vaultfs::DiskEntry& e,
                                 bool externalSelf);
    // mkdir -p for our own operations; creates the model folders and emits created(folder, false).
    VaultFolderNode* ensureFolderChain(const QString& folderPath, QString* error);
    bool hasRealFolderWithId(const vaultfs::DevIno& id) const;

    // ---- watcher integration / reconciliation (vault_sync.cpp)
    void startWatching();
    void onBatch(vaultfs::FsBatch batch);
    void scheduleProcessing(int delayMs);
    void processPending();
    void processEvents(const QList<vaultfs::FsEvent>& events);
    void applySnapshot(const vaultfs::FsSnapshot& snap);
    void onRestartSnapshot(const vaultfs::FsSnapshot& snap);
    std::shared_ptr<vaultfs::FsSnapshot> syncSnapshot() const;
    void reconcile(const QSet<QString>& touched, const QHash<QString, QString>& movedFrom,
                   const QSet<QString>& contentHints, const QSet<QString>& deep);
    bool rootHealthy() const;
    void goOffline();
    void offlineTick();
    QByteArray diskHash(const QString& vaultPath) const;

    struct OpScope { // defers watcher processing while our own operation / a reconcile pass runs
        explicit OpScope(Impl* d) : d(d) { ++d->opDepth; }
        ~OpScope() { --d->opDepth; }
        Impl* d;
    };

    Vault* q;
    QString root;          // absolute, cleaned, no trailing slash
    QString rootCanonical; // symlinks resolved (for vaultPath())
    QByteArray rootEnc;
    VaultConfig cfg;

    VaultFolderNode* rootNode = nullptr;
    QHash<QString, TAbstractFile*> byPath; // includes the root under ""
    QSet<const TAbstractFile*> live;
    int nFiles = 0;
    bool loaded = false;
    bool online = true;
    vaultfs::DevIno rootId;
    vaultfs::SelfWriteRegistry selfWrites;

    std::unique_ptr<vaultfs::IFsWatcher> watcher;
    vaultfs::WatcherKind watcherKind = vaultfs::WatcherKind::None;
    QList<vaultfs::FsEvent> pendingEvents;
    bool pendingOverflow = false;
    bool pendingRootLost = false;
    std::vector<vaultfs::FsBatch> pendingSnapshots;
    std::vector<vaultfs::BusyToken> pendingBusy;
    qint64 pendingSinceMs = 0;
    bool processUrgent = false; // processTimer was started with no delay
    QTimer processTimer;
    QTimer offlineTimer;
    bool restartPending = false;
    int opDepth = 0;
    int passes = 0;
    bool dropWatcherEvents = false;
};

} // namespace oc
