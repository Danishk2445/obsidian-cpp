#pragma once

#include "core/metadata/property_index.h"
#include "core/search/search_query.h"
#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"

#include <QTimer>

class QMenu;

namespace oc {

// Obsidian's "All properties" view (view type "all-properties", core plugin "properties"): every
// frontmatter property name in the vault with its type icon and the number of notes using it (plus the
// reserved aliases / cssclasses / tags and the types assigned in types.json), sorted by name or
// frequency, filtered by a search query. Clicking one searches for ["name"] in the global search
// (Mod+click adds / removes it from the current query).
//
// View state: {"sortOrder", "showSearch", "searchQuery"}.
class AllPropertiesView : public View {
    Q_OBJECT
public:
    explicit AllPropertiesView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~AllPropertiesView() override;

    QString viewType() const override { return QStringLiteral("all-properties"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("archive"); }
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
    void setSearchQuery(const QString& query);
    QList<PropertyInfo> properties() const { return m_properties; } // in display order
    void update();
    void activateProperty(const QString& name, bool modifier);
    // The context menu (Obsidian's): Property type submenu, Rename, Delete property — through the
    // properties core (PropertyTypeManager, props::renameProperty / deleteProperty).
    void populateMenu(QMenu* menu, const QString& name);
    void renameProperty(const QString& from, const QString& to); // asks before merging into an existing one
    void deleteProperty(const QString& name);

private:
    NavHeader* m_header;
    TreeList* m_tree;
    CountSortOrder m_order = CountSortOrder::Frequency;
    search::SearchQuery m_filter;
    QList<PropertyInfo> m_properties;
    QTimer m_update;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace oc
