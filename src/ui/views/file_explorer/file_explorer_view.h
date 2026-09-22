#pragma once

#include "ui/views/file_explorer/file_tree.h"
#include "ui/workspace/view.h"

#include <QAbstractButton>
#include <QTimer>

class QMenu;

namespace oc {

class CommandRegistry;
class FileTree;
class NavActionButton;
class TAbstractFile;
class TFile;
class TFolder;
class ViewRegistry;
class VaultSession;

// Obsidian's "Files" view (core plugin file-explorer, view type "file-explorer"): the nav header
// (New note, New folder, Change sort order, Auto-reveal current file, Expand/Collapse all) over the
// FileTree. Lives in a sidedock (no view header, not navigable).
//
// Persistent state (workspace.json) uses Obsidian's keys: {"sortOrder", "autoReveal", "showSearch",
// "searchQuery"}; setState({"newFile": path}) finishes a creation (renames a new folder in the tree,
// opens a new file for renaming). Expanded folders are kept like Obsidian's localStorage
// "file-explorer-unfold", in <clone data dir>/<vault id>/file-explorer.json.
class FileExplorerView : public View {
    Q_OBJECT
public:
    explicit FileExplorerView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~FileExplorerView() override;

    QString viewType() const override { return QStringLiteral("file-explorer"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("folder-closed"); }
    bool showsViewHeader() const override { return false; }
    bool navigation() const override { return false; }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void onOpen() override;
    void onClose() override;
    void focusContent() override;

    FileTree* tree() const { return m_tree; }
    QAbstractButton* newNoteButton() const;
    QAbstractButton* newFolderButton() const;
    QAbstractButton* sortButton() const;
    QAbstractButton* autoRevealButton() const;
    QAbstractButton* collapseAllButton() const;

    ExplorerSortOrder sortOrder() const;
    void setSortOrder(ExplorerSortOrder order);
    bool autoReveal() const { return m_autoReveal; }
    void setAutoReveal(bool on);

    // ---- Obsidian's explorer actions ----------------------------------------------------------------
    // Nav "New note": in the folder for new notes (from the active file), in a new tab (Mod: per mods).
    TFile* createNewNote(Qt::KeyboardModifiers mods = Qt::NoModifier);
    // Context menu "New note" in a folder: opens in the current tab (Ctrl: a new one).
    TFile* createNoteIn(TFolder* folder, Workspace::NewLeaf mode);
    // "New folder" (nav button: the root): creates "Untitled" and starts renaming it in the tree.
    TFolder* createFolderIn(TFolder* parent);
    // Obsidian's afterCreate(): a folder is renamed in the tree, a file opened for renaming.
    void afterCreate(TAbstractFile* item, Workspace::NewLeaf mode);
    void startRename(TAbstractFile* item);
    // Expand to the item, focus and flash it, activate the view (file-explorer:reveal-active-file).
    void revealInFolder(TAbstractFile* item);
    void revealActiveFile(); // auto-reveal: expand + scroll only
    void deleteItems(const QList<TAbstractFile*>& items);
    void moveItems(const QList<TAbstractFile*>& items, TFolder* target);
    TAbstractFile* makeCopy(TAbstractFile* item, Workspace::NewLeaf mode = Workspace::NewLeaf::No);

    // Obsidian's file / folder / multi-selection context menu for `item` (null: the vault root).
    void populateContextMenu(QMenu* menu, TAbstractFile* item);
    void showContextMenu(TAbstractFile* item, const QPoint& globalPos);
    void populateSortMenu(QMenu* menu);

    // Obsidian's copy / cut / paste of files in the explorer ("obsidian/files" clipboard data).
    void copyToClipboard(const QList<TAbstractFile*>& items, bool cut);
    void pasteInto(TFolder* target);

private:
    void attach();
    void onFileOpen(TFile* file);
    void updateReadOnly(bool offline); // offline vault: dimmed and read-only
    void updateCollapseButton();
    void loadFolds();
    void saveFolds();
    QString foldsFile() const;
    void showSortMenu();
    void openFile(TFile* file, Workspace::NewLeaf mode, bool keyboardNav);
    void dropOnWorkspace(const QList<TAbstractFile*>& items, const QPoint& globalPos, bool drop);
    bool isSupported(const QString& extension) const;

    FileTree* m_tree;
    QWidget* m_header;
    NavActionButton* m_newNote;
    NavActionButton* m_newFolder;
    NavActionButton* m_sort;
    NavActionButton* m_autoRevealButton;
    NavActionButton* m_collapseAll;
    bool m_autoReveal = false;
    bool m_showSearch = false;
    QString m_searchQuery;
    bool m_attached = false;
    bool m_foldsLoaded = false;
    QTimer m_foldSave;
    QList<QMetaObject::Connection> m_connections;
    QRect m_workspaceTarget; // current drop highlight over the workspace (window coordinates)
};

// .clickable-icon.nav-action-button: 30x26 (4/6 px padding around an 18 px icon); .is-active shows
// --icon-color-focused on --background-modifier-hover (a toggled Auto-reveal, an open sort menu).
class NavActionButton : public QAbstractButton {
    Q_OBJECT
public:
    NavActionButton(const QString& icon, const QString& tooltip, QWidget* parent = nullptr);
    void setIconName(const QString& icon);
    QString iconName() const { return m_icon; }
    void setActive(bool active); // .is-active / .has-active-menu
    bool isActive() const { return m_active; }
    QSize sizeHint() const override { return {30, 26}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QString m_icon;
    bool m_hover = false;
    bool m_active = false;
};

// Registers the "file-explorer" view type (replaces the sidebar placeholder of registerCoreViews()).
void registerFileExplorerView(ViewRegistry& registry);

// Obsidian's file-explorer:* commands (open, reveal-active-file, new-file (Mod+N), new-file-in-current-tab,
// new-file-in-new-pane (Mod+Shift+N), new-folder, move-file, duplicate-file) plus the
// open-with-default-app:open / :show commands that share the explorer's system actions.
void registerFileExplorerCommands(CommandRegistry& commands, VaultSession* session);

} // namespace oc
