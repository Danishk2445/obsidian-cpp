#pragma once

#include "core/metadata/link_updater.h"

#include <QObject>
#include <QPointer>

#include <functional>

namespace oc {

class TAbstractFile;
class TFile;
class TFolder;
class VaultSession;

// Obsidian's app.fileManager: file operations that keep the vault consistent — renames/moves with
// "Automatically update internal links", new files placed per "Default location for new notes",
// deletes per "Deleted files" (system trash / .trash / permanent), and link text generation.
class FileManager : public QObject {
    Q_OBJECT
public:
    explicit FileManager(VaultSession* session, QObject* parent = nullptr);

    enum class LinkUpdateChoice { AlwaysUpdate, JustOnce, DoNotUpdate };
    // Asked when links would change and "alwaysUpdateLinks" is off (Obsidian's "Update links" modal).
    // Without a handler, links are not updated.
    void setConfirmLinkUpdate(std::function<LinkUpdateChoice(int links, int files)> fn) { m_confirm = std::move(fn); }

    // Rename or move a file/folder. Returns false (and *error) if the vault refused.
    bool renameFile(TAbstractFile* item, const QString& newPath, QString* error = nullptr);
    // Delete per the "trashOption" setting ("system", "local" = <vault>/.trash, "none" = permanent).
    bool trashFile(TAbstractFile* item, QString* error = nullptr);

    // The folder for a new note created from `sourcePath` ("root" / "current" / "folder" setting).
    TFolder* newFileParent(const QString& sourcePath) const;
    // "Untitled.md", "Untitled 1.md", ... in `parent` (the root when null).
    TFile* createNewMarkdownFile(TFolder* parent, const QString& baseName = {}, const QByteArray& content = {},
                                 QString* error = nullptr);

    // Link text for `file` from `sourcePath` in the configured format, as a wikilink or a markdown
    // link ("useMarkdownLinks"), with an optional subpath ("#Heading") and display text.
    QString generateMarkdownLink(const QString& targetPath, const QString& sourcePath, const QString& subpath = {},
                                 const QString& alias = {}) const;

signals:
    // After a rename rewrote links: counts for the "Updated N links in M files." notice.
    void linksUpdated(int links, int files);
    void notice(const QString& message);

private:
    int applyLinkEdits(const QHash<QString, QList<LinkUpdater::Edit>>& edits, int* filesChanged);

    VaultSession* m_session;
    std::function<LinkUpdateChoice(int, int)> m_confirm;
};

} // namespace oc
