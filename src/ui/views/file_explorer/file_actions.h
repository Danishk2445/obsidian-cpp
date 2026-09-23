#pragma once

// File operations of Obsidian's file explorer, shared by the Files view and the file-explorer:*
// commands (and usable by other views: the markdown view's "Delete file", the tab menu, ...). They go
// through the session's FileManager (link updates, trash option) and show Obsidian's modals/notices in
// the session window's overlay.

#include "core/files/file_manager.h"
#include "ui/workspace/workspace.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

namespace oc {

class MainWindow;
class OverlayLayer;
class TAbstractFile;
class TFile;
class TFolder;
class Vault;
class VaultSession;

// The session's window / overlay (null without a window, e.g. in core tests).
MainWindow* sessionWindow(VaultSession* session);
OverlayLayer* sessionOverlay(VaultSession* session);
void showNotice(VaultSession* session, const QString& message);

// Obsidian's getActiveFile(): the file of the active leaf when it's a file view, else of the most
// recently active root leaf.
TFile* workspaceActiveFile(Workspace* workspace);

// Obsidian's rename validation for the file explorer: invalid characters (\ / :), empty name, leading
// dot, an existing file with that name, and (on Linux) the characters that break links (# ^ [ ] |).
// `name` is what the user typed (a file's basename). Empty when fine.
QString validateFileName(Vault* vault, const TAbstractFile* item, const QString& name, bool final = true);
// "folder/<name>.<ext>" for a file, "folder/<name>" for a folder (Obsidian's getNewPathAfterRename()).
QString pathAfterRename(const TAbstractFile* item, const QString& name);

// Obsidian's fileManager.createNewFolder(): "Untitled", "Untitled 1", ... in `parent` (root when null).
TFolder* createNewFolder(VaultSession* session, TFolder* parent, QString* error = nullptr);
// "Make a copy": a file becomes "Name 1.ext" next to it, a folder "Folder 1" (copied recursively).
TAbstractFile* makeCopy(VaultSession* session, TAbstractFile* item, QString* error = nullptr);
// Copies `item` into `target` under a free name (Obsidian's paste of copied files).
TAbstractFile* copyInto(VaultSession* session, TAbstractFile* item, TFolder* target, QString* error = nullptr);
// Obsidian's importAttachments for files dropped from outside the app: local files and folders are
// copied into `target` under free names. Returns what was created (errors become notices).
QList<TAbstractFile*> importFiles(VaultSession* session, const QList<QUrl>& urls, TFolder* target);
// Obsidian's moveFiles: each item goes into `target` under a free name (items already there and
// items that can't go there are skipped). Returns how many were moved; errors become notices.
int moveItems(VaultSession* session, const QList<TAbstractFile*>& items, TFolder* target);
// Drops items that are inside other (folder) items of the list (Obsidian's xA).
QList<TAbstractFile*> outermostItems(const QList<TAbstractFile*>& items);

// Opens a newly created file for renaming, like Obsidian's afterCreate(): editing mode, the inline
// title selected for renaming. Returns the leaf.
WorkspaceLeaf* openNewFile(Workspace* workspace, TFile* file, Workspace::NewLeaf mode);

// Obsidian's fileManager.promptForDeletion(): the "Delete file"/"Delete folder" confirmation (unless
// "Confirm file deletion" is off; "Don't ask again" turns it off), then the configured trash.
// done(deleted) runs once the choice is made.
void promptForDeletion(VaultSession* session, TAbstractFile* item, std::function<void(bool deleted)> done = {});
// One prompt after the other (Obsidian's multi-select delete).
void promptForDeletion(VaultSession* session, const QList<TAbstractFile*>& items, std::function<void()> done = {});

// Obsidian's "Update links" modal, for FileManager::setConfirmLinkUpdate (synchronous: a local event
// loop runs while it's open). Without an overlay: DoNotUpdate.
FileManager::LinkUpdateChoice promptLinkUpdate(OverlayLayer* overlay, int links, int files);

// Obsidian's "Move file to..." suggester (file-explorer:move-file, the context menu): folders by fuzzy
// match; Enter moves, Shift+Enter creates the typed folder first, Tab completes the path.
void openMoveModal(VaultSession* session, const QList<TAbstractFile*>& items);

// obsidian://open?vault=<name>&file=<path without .md>
QString obsidianUrl(Vault* vault, const TFile* file);

// System integration (Obsidian's openWithDefaultApp / showInFolder). Tests replace the handlers.
struct DesktopHandlers {
    std::function<void(const QUrl& url)> openUrl;             // default: QDesktopServices::openUrl
    std::function<void(const QString& absPath)> showInFolder; // default: KIO::highlightInFileManager
};
DesktopHandlers& desktopHandlers();
void openWithDefaultApp(Vault* vault, const TAbstractFile* item);
void showInSystemExplorer(Vault* vault, const TAbstractFile* item);

} // namespace oc
