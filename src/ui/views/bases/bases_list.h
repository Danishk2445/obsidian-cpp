#pragma once

// Obsidian's Bases list view (.bases-list-*): one list item per result — a marker (the "markers"
// option: bullet, number or none) and the view's non-empty properties inline, separated by the
// "separator" option (", "), or with "indentProperties" the first property on the item line and
// the others nested below it, each with a dash. Group-by sections get a heading. Rows are painted
// only when in view; links / tags in the items are clickable.

#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_render.h"

namespace oc::basesui {

class ListLayout : public BaseLayout {
    Q_OBJECT
public:
    explicit ListLayout(BasesController* controller, QWidget* parent = nullptr);

    QString type() const override { return QStringLiteral("list"); }
    void setData(const bases::ViewData& data) override;
    int contentHeight() const override { return m_contentHeight; }

    int itemHeight(const bases::EntryPtr& e) const;
    QRect itemRect(int group, int index) const; // viewport coordinates

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    struct GroupGeom {
        int headingTop = 0;
        int headingHeight = 0;
        QList<int> itemTops; // content y of each item
        QList<int> itemHeights;
    };
    void relayout();
    QStringList visibleProps(const bases::EntryPtr& e) const;
    bases::EntryPtr entryAt(const QPoint& p) const;

    QList<GroupGeom> m_groups;
    QString m_markers = QStringLiteral("bullet");
    QString m_separator = QStringLiteral(", ");
    bool m_indent = false;
    int m_lineHeight = 24;
    int m_padLeft = 8, m_padRight = 16, m_padTop = 8, m_padBottom = 60;
    int m_contentHeight = 0;
    QList<ValueHit> m_hits;
    QString m_hoverTarget;
};

} // namespace oc::basesui
