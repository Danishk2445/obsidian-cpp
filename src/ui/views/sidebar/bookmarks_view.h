#pragma once

#include "core/bookmarks/bookmarks.h"
#include "core/search/search_query.h"
#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"
#include "ui/workspace/workspace.h"

#include <QTimer>

#include <memory>
#include <vector>

class QComboBox;
class QLineEdit;

namespace oc {

class Modal;

// Obsidian's "Bookmarks" view (view type "bookmarks", core plugin "bookmarks"): the vault's bookmarks
// (sessionBookmarks(): files, headings and blocks, folders, searches, graphs, URLs) in groups, with the
// active note highlighted. Items are opened by a click (Mod: new tab), renamed inline (F2 / "Rename",
// sets a custom title), removed (Delete / "Remove"), edited in the "Bookmark..." modal and reordered by
// dragging (into groups too). Nav header: Bookmark the active tab..., New group, Collapse all / Expand
// all, Show search filter. Group folds are kept like Obsidian's localStorage "bookmarks-folds".
//
// View state: {"showSearch", "searchQuery"}.
class BookmarksView : public View {
    Q_OBJECT
public:
    explicit BookmarksView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~BookmarksView() override;

    QString viewType() const override { return QStringLiteral("bookmarks"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("bookmark"); }
    bool navigation() const override { return false; }
    bool showsViewHeader() const override { return false; }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void onOpen() override;
    void focusContent() override;

    TreeList* tree() const { return m_tree; }
    NavHeader* header() const { return m_header; }
    BookmarkModel* model() const { return m_model; }
    void update();
    void setSearchQuery(const QString& query);
    TreeList::Node* nodeFor(const Bookmark* item) const;
    Bookmark* bookmarkOf(const TreeList::Node* node) const;

    // Obsidian's actions.
    void open(Bookmark* item, Workspace::NewLeaf mode);
    Bookmark* createNewGroup(Bookmark* parentGroup = nullptr); // "Untitled group", renamed in place
    void startRename(Bookmark* item);
    void populateContextMenu(QMenu* menu, TreeList::Node* node);

private:
    void showContextMenu(TreeList::Node* node, const QPoint& globalPos);
    void dropItems(const QList<TreeList::Node*>& nodes, TreeList::Node* target, TreeList::DropPlacement placement);
    void saveFoldsNow();
    void updateCollapseButton();
    QString titleOf(const Bookmark& item) const;

    NavHeader* m_header;
    TreeList* m_tree;
    NavActionButton* m_collapseButton = nullptr;
    QPointer<BookmarkModel> m_model;
    search::SearchQuery m_filter;
    QString m_activePath;
    QTimer m_update;
    QTimer m_foldSave;
    QSet<QString> m_folded;
    bool m_foldsLoaded = false;
    QList<QMetaObject::Connection> m_connections;
};

// Obsidian's "Add bookmark" / "Edit bookmark" modal: the path (or query), the title (placeholder: the
// default title) and the bookmark group; Save (Enter in the title), Cancel, and Remove for an existing
// bookmark. For a new item the modal owns it until saved. `onSave` runs after saving (e.g. inserting a
// generated block id).
class BookmarkModal {
public:
    static Modal* edit(MainWindow* window, BookmarkModel* model, Bookmark* existing);
    static Modal* add(MainWindow* window, BookmarkModel* model, std::unique_ptr<Bookmark> item, Bookmark* group = nullptr,
                      std::function<void()> onSave = {});
    // "Add N bookmarks" (Bookmark all tabs...): a new group (titled, in a group) holding the items.
    static Modal* addMany(MainWindow* window, BookmarkModel* model, std::vector<std::unique_ptr<Bookmark>> items);
};

// Obsidian's bookmark for what a leaf shows (X4): its file, or the search view's query.
std::unique_ptr<Bookmark> bookmarkForLeaf(WorkspaceLeaf* leaf);

} // namespace oc
