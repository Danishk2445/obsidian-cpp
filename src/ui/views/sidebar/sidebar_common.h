#pragma once

// Pieces shared by the sidebar views (backlinks, outgoing links, tags, outline, bookmarks, all
// properties): Obsidian's nav header with its search filter, the "linked file view" base that follows
// the active file, and small helpers (reading / editing notes, the global-search hooks).

#include "core/search/search_query.h"
#include "core/util/match_range.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

class QHBoxLayout;
class QMenu;
class QVBoxLayout;

namespace oc {

class MainWindow;
class NavActionButton;
class SearchInput;
class TFile;
class TreeList;
class VaultSession;
class Workspace;
class WorkspaceLeaf;

// Obsidian's .nav-header: a centered, wrapping row of .nav-action-button icons (8 px padding, 2 px
// gaps) and, below it, an optional search filter input (hidden until its button is toggled).
class NavHeader : public QWidget {
    Q_OBJECT
public:
    explicit NavHeader(QWidget* parent = nullptr);

    NavActionButton* addButton(const QString& icon, const QString& tooltip, std::function<void()> onClick);
    // "Change sort order": a menu of `groups` (id, label) separated by lines, the current one checked.
    NavActionButton* addSortButton(const QList<QList<QPair<QString, QString>>>& groups, std::function<QString()> current,
                                   std::function<void(const QString& id)> choose);
    // Obsidian's search filter (addSearchButton / the backlink pane's own): a "Show search filter"
    // button that shows / hides the input; `changed` runs debounced (and on Enter), `toggled` when
    // the input is shown or hidden. Hiding clears the query.
    NavActionButton* addSearchFilter(std::function<void(const QString& query)> changed, std::function<void(bool shown)> toggled = {},
                                     int debounceMs = 150);

    SearchInput* searchInput() const { return m_input; }
    NavActionButton* searchButton() const { return m_searchButton; }
    bool isSearchShown() const { return m_searchShown; }
    void setSearchShown(bool shown, bool focus = false);
    QString searchQuery() const;
    void setSearchQuery(const QString& query); // without running `changed`
    QList<NavActionButton*> buttons() const { return m_buttons; }

private:
    QHBoxLayout* m_row;
    QVBoxLayout* m_layout;
    QList<NavActionButton*> m_buttons;
    SearchInput* m_input = nullptr;
    NavActionButton* m_searchButton = nullptr;
    bool m_searchShown = false;
    QTimer m_debounce;
    std::function<void(const QString&)> m_changed;
    std::function<void(bool)> m_toggled;
};

// Obsidian's linked-file view (backlinks, outgoing links, outline): shows data about one file. Unless
// its leaf is pinned or linked to another leaf (the "…for the current file" commands open one next to
// the note), it follows the workspace's active file. View state: {"file": path} plus the subclass's
// keys; tab title "<Name> for <file>" when bound.
class LinkedFileView : public View {
    Q_OBJECT
public:
    explicit LinkedFileView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~LinkedFileView() override;

    bool navigation() const override { return false; }
    bool showsViewHeader() const override { return false; }
    QString displayText() const override;
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void onOpen() override;
    void onClose() override;

    TFile* file() const;
    QString filePath() const { return m_path; }
    // Shows `file` (null: none) and updates the view.
    void loadFile(TFile* file);
    bool isBound() const;
    // Follow `source`'s file instead of the active file (Obsidian's leaf groups, not persisted).
    void linkTo(WorkspaceLeaf* source);
    WorkspaceLeaf* linkedLeaf() const { return m_linked; }

protected:
    // The plugin name ("Backlinks") and the bound title template ("Backlinks for %1").
    virtual QString baseTitle() const = 0;
    virtual QString boundTitle() const = 0;
    // The file changed (also: renamed, deleted, first shown).
    virtual void fileChanged() = 0;
    // The leaf became (in)active: focus rings follow it.
    virtual void leafActiveChanged(bool active) { Q_UNUSED(active); }
    TFile* activeWorkspaceFile() const;

private:
    void attach();
    void followLinked();

    QString m_path;
    QPointer<WorkspaceLeaf> m_linked;
    QList<QMetaObject::Connection> m_connections;
    QMetaObject::Connection m_linkedConn;
    bool m_attached = false;
};

// ---- helpers -------------------------------------------------------------------------------------

// A note's current text ('\n'-joined): the open document's, else decoded from disk.
std::optional<QString> readNoteText(VaultSession* session, const QString& path);
// Replaces text[from, to) (which must still hold `expected`) in a note: through its open document
// (one undoable change, saved) or on disk with a compare-and-swap write. False if it changed meanwhile.
bool replaceNoteText(VaultSession* session, const QString& path, int from, int to, const QString& expected, const QString& replacement,
                     const QString& userEvent = QStringLiteral("input"));

// A sidebar search filter: the query's highlights in `text`, or nullopt when it doesn't match.
std::optional<QList<MatchRange>> filterMatch(const search::SearchQuery& query, const QString& text);

// Page previews for a view (source "backlink", "outline", ...; Obsidian requires Mod by default).
void hoverLink(View* view, const QString& source, const QString& linktext, const QString& sourcePath, const QRect& globalRect, QWidget* hoverParent);
void unhoverLink(View* view, QWidget* hoverParent);

// Obsidian's "open or toggle a search term": a click opens the global search with `term`; a Mod
// click adds it to the current query (or removes it when present).
void searchFor(MainWindow* window, const QString& term, bool modifier);

// Optional overrides of how the sidebar views reach the global search. By default they use the search
// view (ui/navigation: openGlobalSearch(), SearchView::query()) when the "search" view is registered.
struct GlobalSearchHooks {
    std::function<void(MainWindow* window, const QString& query)> openSearch;
    std::function<QString(MainWindow* window)> currentQuery;
};
GlobalSearchHooks& globalSearchHooks();
// Obsidian's getGlobalSearchQuery() (empty without a search view).
QString currentGlobalSearchQuery(MainWindow* window);

// Obsidian's handleLinkContextMenu(): for a resolved link Open in new tab / Open to the right plus the
// file-menu hooks' items (source "link-context-menu"); for an unresolved one "Create this file".
// Render with buildSectionedMenu() (ui/workspace/file_menu.h).
QList<MenuEntry> linkMenuEntries(Workspace* ws, const QString& linktext, const QString& sourcePath, QWidget* origin);

// Obsidian's file sort orders for results (ExplorerSortOrder ids), with the file explorer's labels.
QList<QList<QPair<QString, QString>>> fileSortGroups();
// The tag pane / all properties orders (CountSortOrder ids) with their labels ("Tag name (A to Z)", ...).
QList<QList<QPair<QString, QString>>> countSortGroups(const QString& nameAToZ, const QString& nameZToA);

// Obsidian keeps some trees' folds in localStorage ("tag-pane-fold", "bookmarks-folds"): here in
// <clone data dir>/<vault id>/sidebar-folds.json, one array of node ids per key.
QStringList loadFolds(VaultSession* session, const QString& key);
void saveFolds(VaultSession* session, const QString& key, const QStringList& ids);

} // namespace oc
