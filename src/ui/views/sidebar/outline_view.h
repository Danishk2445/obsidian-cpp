#pragma once

#include "core/search/search_query.h"
#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"

#include <QTimer>

namespace oc {

class MarkdownView;

// Obsidian's "Outline" view (view type "outline", core plugin "outline"): the heading tree of the
// active note. Clicking a heading scrolls the note's editor there (eState {"line"} on the leaf showing
// the note, else the note opens); the heading of the section the cursor is in (the scroll position in
// reading view) is highlighted, and with "Auto-scroll to current section" kept in view. Headings can be
// dragged to move their sections. Nav header: Show search filter, Auto-scroll to current section,
// Collapse all / Expand all.
//
// View state: {"file", "followCursor", "showSearch", "searchQuery"}.
class OutlineView : public LinkedFileView {
    Q_OBJECT
public:
    explicit OutlineView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~OutlineView() override;

    QString viewType() const override { return QStringLiteral("outline"); }
    QString icon() const override { return QStringLiteral("list"); }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void focusContent() override;

    TreeList* tree() const { return m_tree; }
    NavHeader* header() const { return m_header; }
    bool followCursor() const { return m_followCursor; }
    void setFollowCursor(bool on);
    void setSearchQuery(const QString& query);
    int activeHeading() const { return m_active; } // index into the note's headings, -1 none
    void update();                                 // rebuild now
    void updateActiveHeading();
    // Scroll the note to heading `index` (what a click does).
    void navigateTo(int index, bool newLeaf = false);
    // Move the section of heading `from` before heading `target` (== count: to the end).
    bool moveSection(int from, int target);

protected:
    QString baseTitle() const override;
    QString boundTitle() const override;
    void fileChanged() override;
    void leafActiveChanged(bool active) override;

private:
    MarkdownView* owner() const;
    MarkdownView* correspondingView() const;
    void watchOwner();
    void updateCollapseButton();

    NavHeader* m_header;
    TreeList* m_tree;
    NavActionButton* m_followButton = nullptr;
    NavActionButton* m_collapseButton = nullptr;
    bool m_followCursor = false;
    search::SearchQuery m_filter;
    QString m_builtFor; // the file the tree shows (folds reset for another file)
    int m_active = -1;
    QTimer m_update;
    QTimer m_selection;
    QPointer<MarkdownView> m_watched;
    QList<QMetaObject::Connection> m_ownerConnections;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace oc
