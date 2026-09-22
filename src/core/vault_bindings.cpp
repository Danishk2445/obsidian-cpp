#include "core/vault_bindings.h"

#include "core/documents/document_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/recovery/file_recovery.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"

#include <QFile>
#include <QPointer>

namespace oc {

void bindVaultServices(VaultSession* session, FileRecovery* recovery)
{
    Vault* vault = session->vault();
    if (!vault)
        return;
    QPointer<MetadataCache> mc = session->metadata();
    QPointer<DocumentManager> dm = session->documents();
    QPointer<FileRecovery> fr = recovery;
    QPointer<Vault> v = vault;

    if (mc) {
        // Called on worker threads: read the file directly (Vault::read touches the model, which
        // belongs to the GUI thread). Read-only, so it can't disturb the vault.
        const QString root = vault->root();
        mc->setContentProvider([root](const QString& path) -> std::optional<QByteArray> {
            QFile f(root + QLatin1Char('/') + path);
            if (!f.open(QIODevice::ReadOnly))
                return std::nullopt;
            return f.readAll();
        });
    }

    QObject::connect(vault, &Vault::loaded, session, [v, mc] {
        if (!v || !mc)
            return;
        QStringList paths;
        for (TFile* f : v->files())
            paths << f->path();
        mc->indexAll(paths);
    });
    QObject::connect(vault, &Vault::created, session, [mc, fr](TAbstractFile* item, bool) {
        if (item->isFolder())
            return;
        if (mc)
            mc->fileCreated(item->path());
        if (fr)
            fr->fileChanged(item->path());
    });
    QObject::connect(vault, &Vault::modified, session, [mc, dm, fr](TFile* file, bool external) {
        if (mc)
            mc->fileModified(file->path());
        if (external && dm)
            dm->fileModified(file->path()); // our own writes are already in the document
        if (fr)
            fr->fileChanged(file->path());
    });
    QObject::connect(vault, &Vault::deleted, session, [mc, dm](TAbstractFile* item, bool) {
        if (dm)
            dm->fileDeleted(item->path());
        if (mc && !item->isFolder())
            mc->fileDeleted(item->path());
    });
    QObject::connect(vault, &Vault::renamed, session, [mc, dm](TAbstractFile* item, const QString& oldPath, bool) {
        if (dm)
            dm->fileRenamed(oldPath, item->path()); // folders move their descendants' documents too
        if (mc && !item->isFolder())
            mc->fileRenamed(oldPath, item->path()); // the vault reports each moved descendant
    });
    QObject::connect(vault, &Vault::onlineChanged, session, [dm](bool online) {
        if (dm)
            dm->setOnline(online);
    });
}

} // namespace oc
