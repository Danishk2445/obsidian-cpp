#include "core/files/file_manager.h"

#include "core/config/vault_config.h"
#include "core/documents/document_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "text/codec.h"

namespace oc {

namespace {
QString countNoun(int n, const QString& one, const QString& many) { return (n == 1 ? one : many).arg(n); }
} // namespace

FileManager::FileManager(VaultSession* session, QObject* parent)
    : QObject(parent)
    , m_session(session)
{
}

bool FileManager::renameFile(TAbstractFile* item, const QString& rawNewPath, QString* error)
{
    Vault* vault = m_session->vault();
    if (!vault || !item) {
        if (error)
            *error = tr("No vault");
        return false;
    }
    QString newPath = rawNewPath;
    while (newPath.startsWith(QLatin1Char('/')))
        newPath.remove(0, 1);
    const QString oldPath = item->path();
    if (newPath == oldPath)
        return true;

    MetadataCache* mc = m_session->metadata();
    // Links are snapshotted from the metadata, so it must reflect what's on disk (Obsidian waits for a
    // clean cache): flush pending autosaves and parses first.
    if (DocumentManager* dm = m_session->documents())
        dm->saveAll();
    QList<LinkUpdater::Reference> refs;
    if (mc) {
        mc->waitUntilClean();
        refs = LinkUpdater::snapshot(*mc);
    }
    if (!vault->rename(item, newPath, error))
        return false;
    if (!mc || refs.isEmpty())
        return true;

    // The vault's renamed signal already updated the metadata cache (resolver) through the bindings.
    VaultConfig* cfg = m_session->vaultConfig();
    const auto edits = LinkUpdater::edits(refs, mc->resolver(), LinkUpdater::renameMap(oldPath, newPath),
                                          cfg ? cfg->newLinkFormat() : LinkResolver::LinkFormat::Shortest);
    int links = 0;
    for (const auto& list : edits)
        links += int(list.size());
    if (links == 0)
        return true;
    bool update = cfg && cfg->alwaysUpdateLinks();
    if (!update && m_confirm) {
        switch (m_confirm(links, int(edits.size()))) {
        case LinkUpdateChoice::AlwaysUpdate:
            if (cfg)
                cfg->set(QStringLiteral("alwaysUpdateLinks"), true);
            update = true;
            break;
        case LinkUpdateChoice::JustOnce:
            update = true;
            break;
        case LinkUpdateChoice::DoNotUpdate:
            break;
        }
    }
    if (!update)
        return true;
    int files = 0;
    const int applied = applyLinkEdits(edits, &files);
    if (applied > 0) {
        emit linksUpdated(applied, files);
        emit notice(tr("Updated %1 in %2.")
                        .arg(countNoun(applied, tr("%1 link"), tr("%1 links")), countNoun(files, tr("%1 file"), tr("%1 files"))));
    }
    return true;
}

int FileManager::applyLinkEdits(const QHash<QString, QList<LinkUpdater::Edit>>& edits, int* filesChanged)
{
    Vault* vault = m_session->vault();
    DocumentManager* dm = m_session->documents();
    int applied = 0;
    int files = 0;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
        const QString& path = it.key();
        int n = 0;
        if (Document* doc = dm ? dm->find(path) : nullptr; doc && !doc->isReadOnly()) {
            // Open (or cached) document: one undoable change, saved right away.
            Text t = doc->text();
            QList<ChangeSpec> specs;
            for (const LinkUpdater::Edit& e : it.value())
                if (e.from >= 0 && e.to <= t.length() && t.slice(e.from, e.to) == e.expected)
                    specs.push_back({e.from, e.to, e.text});
            if (!specs.isEmpty()) {
                std::sort(specs.begin(), specs.end(), [](const ChangeSpec& a, const ChangeSpec& b) { return a.from < b.from; });
                Transaction tr;
                tr.changes = ChangeSet::of(specs, t.length());
                tr.userEvent = QStringLiteral("link-update");
                tr.scrollIntoView = false;
                doc->dispatch(tr, nullptr, Selection::cursor(0));
                doc->save();
                n = int(specs.size());
            }
        } else if (vault) {
            // Closed file: patch the text and write it back with a compare-and-swap (retry once).
            for (int attempt = 0; attempt < 2 && n == 0; ++attempt) {
                DiskState state;
                QString err;
                const std::optional<QByteArray> bytes = vault->read(path, &state, &err);
                if (!bytes)
                    break;
                const DecodedFile dec = decodeFile(*bytes);
                if (dec.hasNul)
                    break; // not a text file we should touch
                Text t = dec.text;
                const int k = LinkUpdater::apply(t, it.value());
                if (k == 0)
                    break;
                const DocumentStorage::WriteResult r = vault->write(path, encodeFile(t, dec.bom), state, nullptr, &err);
                if (r == DocumentStorage::WriteResult::Ok)
                    n = k;
                else if (r == DocumentStorage::WriteResult::Error)
                    break;
            }
        }
        if (n > 0) {
            applied += n;
            ++files;
        }
    }
    if (filesChanged)
        *filesChanged = files;
    return applied;
}

bool FileManager::trashFile(TAbstractFile* item, QString* error)
{
    Vault* vault = m_session->vault();
    if (!vault || !item)
        return false;
    const QString option = m_session->vaultConfig() ? m_session->vaultConfig()->string(QStringLiteral("trashOption")) : QStringLiteral("system");
    Vault::TrashMode mode = Vault::TrashMode::System;
    if (option == u"local")
        mode = Vault::TrashMode::Local;
    else if (option == u"none")
        mode = Vault::TrashMode::Permanent;
    return vault->trash(item, mode, error);
}

TFolder* FileManager::newFileParent(const QString& sourcePath) const
{
    Vault* vault = m_session->vault();
    if (!vault)
        return nullptr;
    VaultConfig* cfg = m_session->vaultConfig();
    const QString location = cfg ? cfg->string(QStringLiteral("newFileLocation")) : QStringLiteral("root");
    if (location == u"current") {
        if (TAbstractFile* f = vault->item(sourcePath); f && f->parent())
            return f->parent();
    } else if (location == u"folder" && cfg) {
        QString folder = cfg->string(QStringLiteral("newFileFolderPath"));
        while (folder.startsWith(QLatin1Char('/')))
            folder.remove(0, 1);
        while (folder.endsWith(QLatin1Char('/')))
            folder.chop(1);
        if (TFolder* f = folder.isEmpty() ? vault->rootFolder() : vault->folder(folder))
            return f;
    }
    return vault->rootFolder();
}

TFile* FileManager::createNewMarkdownFile(TFolder* parent, const QString& baseName, const QByteArray& content, QString* error)
{
    Vault* vault = m_session->vault();
    if (!vault)
        return nullptr;
    const QString folder = parent ? parent->path() : QString();
    const QString name = baseName.isEmpty() ? tr("Untitled") : baseName;
    return vault->createFile(vault->availablePath(folder, name, QStringLiteral("md")), content, error);
}

QString FileManager::generateMarkdownLink(const QString& targetPath, const QString& sourcePath, const QString& subpath,
                                          const QString& rawAlias) const
{
    VaultConfig* cfg = m_session->vaultConfig();
    MetadataCache* mc = m_session->metadata();
    const bool markdown = cfg && cfg->useMarkdownLinks();
    const LinkResolver::LinkFormat format = cfg ? cfg->newLinkFormat() : LinkResolver::LinkFormat::Shortest;
    const bool isNote = targetPath.endsWith(QLatin1String(".md"), Qt::CaseInsensitive);
    QString link = (mc ? mc->resolver().linkText(targetPath, sourcePath, format, !markdown) : targetPath) + subpath;
    if (targetPath == sourcePath && !subpath.isEmpty())
        link = subpath;
    QString alias = rawAlias;
    if (markdown) {
        const QString dest = LinkUpdater::encodeMarkdownDestination(link);
        QString text = alias;
        if (text.isEmpty() && isNote) {
            const QString name = targetPath.mid(targetPath.lastIndexOf(QLatin1Char('/')) + 1);
            text = name.left(name.size() - 3);
        }
        return QStringLiteral("[%1](%2)").arg(text, dest);
    }
    if (!alias.isEmpty() && alias.compare(link, Qt::CaseInsensitive) == 0) {
        link = alias;
        alias.clear();
    }
    return alias.isEmpty() ? QStringLiteral("[[%1]]").arg(link) : QStringLiteral("[[%1|%2]]").arg(link, alias);
}

} // namespace oc
