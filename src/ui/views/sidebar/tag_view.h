#pragma once

#include "core/metadata/tag_tree.h"
#include "core/search/search_query.h"
#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"

#include <QTimer>

namespace oc {

// Obsidian's "Tags" view (view type "tag", core plugin "tag-pane"): every tag of the vault with its
// number of occurrences (vaultTags(): nested tags count for their parents), flat or nested ("Show
// nested tags"), sorted by name or frequency, filtered by a search query. Clicking a tag searches for
// "tag:#tag" in the global search (Mod+click adds / removes it from the current query). Folds are kept
// like Obsidian's localStorage "tag-pane-fold".
//
// View state: {"sortOrder", "useHierarchy", "showSearch", "searchQuery"}.
class TagView : public View {
    Q_OBJECT
public:
    explicit TagView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~TagView() override;

    QString viewType() const override { return QStringLiteral("tag"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("tags"); }
    bool navigation() const override { return false; }
    bool showsViewHeader() const override { return false; }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void onOpen() override;
    void focusContent() override;

    TreeList* tree() const { return m_tree; }
    NavHeader* header() const { return m_header; }
    CountSortOrder sortOrder() const { return m_order; }
    void setSortOrder(CountSortOrder order);
    bool useHierarchy() const { return m_nested; }
    void setUseHierarchy(bool nested);
    void setSearchQuery(const QString& query);
    void updateTags(); // rebuild now (normally debounced after metadata changes)
    // What a click does (default: searchFor(window, "tag:" + tag, mod)).
    void activateTag(const QString& tag, bool modifier);

private:
    void saveFoldsNow();
    void updateCollapseButton();

    NavHeader* m_header;
    TreeList* m_tree;
    NavActionButton* m_nestedButton = nullptr;
    NavActionButton* m_collapseButton = nullptr;
    CountSortOrder m_order = CountSortOrder::Frequency;
    bool m_nested = true;
    search::SearchQuery m_filter;
    QTimer m_update;
    QTimer m_foldSave;
    bool m_foldsLoaded = false;
    QSet<QString> m_folded; // lowercase tags of collapsed nodes
    QList<QMetaObject::Connection> m_connections;
};

} // namespace oc
