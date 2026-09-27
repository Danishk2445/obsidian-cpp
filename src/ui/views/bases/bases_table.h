#pragma once

// Obsidian's Bases table view (app.css .bases-table*), painted and virtualized:
//  - a sticky header row (.bases-thead): each column shows the property's type icon and display
//    name; a click toggles the sort (A→Z, Z→A, none; the chevron shows the direction), the right
//    edge resizes the column (the width is saved in the view's "columnSize"), a right click opens
//    the column menu (sort, hide, group by, summarize, property type, edit display name / formula);
//  - rows of 30px (x2 / x4 / x8 with the "rowHeight" option), cells separated by 1px borders;
//    column widths fit the longest value between 40 and 300px unless resized, the last column
//    fills the width;
//  - group-by sections (.bases-table per group, 10px apart on --bases-table-group-background) with
//    a heading (property name + value, "None" last) and, with several groups, a summary row;
//  - a sticky summary footer (.bases-table-footer) when the view has summaries: click a cell to
//    pick one (the built-in functions for the column's values, custom ones, "None");
//  - cells render values like Obsidian (links, tags, lists, dates, checkboxes, errors); note
//    properties of markdown files are editable in place with the property widgets (a click opens the
//    editor, checkboxes toggle), writing the note's frontmatter through the Properties core;
//  - an active cell (keyboard: arrows, Enter to edit, Esc), link clicks / hover previews.
// Only the rows and columns in view are painted, so thousands of rows scroll smoothly.

#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_render.h"

#include <QHash>
#include <QPointer>

namespace oc {
class PropertyValueEditor;
}

namespace oc::basesui {

class TableLayout : public BaseLayout {
    Q_OBJECT
public:
    explicit TableLayout(BasesController* controller, QWidget* parent = nullptr);
    ~TableLayout() override;

    QString type() const override { return QStringLiteral("table"); }
    void setData(const bases::ViewData& data) override;
    int contentHeight() const override;
    QJsonObject ephemeralState() const override;
    void setEphemeralState(const QJsonObject& state) override;

    // Geometry (tests): column widths, the content rect of a cell / header cell in viewport
    // coordinates, the cell at a viewport point.
    QList<int> columnWidths() const { return m_widths; }
    int rowHeight() const { return m_rowHeight; }
    QRect cellRect(int group, int row, int column) const;
    QRect headerRect(int column) const;
    QRect footerRect(int column) const;
    struct CellPos {
        int group = -1;
        int row = -1;
        int column = -1;
        bool valid() const { return group >= 0 && row >= 0 && column >= 0; }
        bool operator==(const CellPos&) const = default;
    };
    CellPos cellAt(const QPoint& viewportPos) const;
    CellPos activeCell() const { return m_active; }
    void setActiveCell(const CellPos& pos);
    // Open the in-place editor of a note property cell; false when the cell isn't editable.
    bool editCell(const CellPos& pos);
    PropertyValueEditor* editor() const;
    void closeEditor();
    void scrollToCell(const CellPos& pos);
    bool hasFooter() const { return m_footer; }
    int groupTop(int group) const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool viewportEvent(QEvent* e) override;

private:
    struct GroupGeom {
        int top = 0;        // content y of the group's table
        int heading = 0;    // heading height (0 = none)
        int summary = 0;    // group summary row height
        int rowsTop = 0;    // content y of the first row
        int rows = 0;
    };
    void measure();
    void computeWidths();
    void layoutGroups();
    int headerHeight() const { return m_rowHeight0; }
    int footerTop() const; // viewport y of the footer
    int columnLeft(int column) const;
    int totalWidth() const;
    int rowViewportTop(int group, int row) const;
    bases::EntryPtr entryAt(const CellPos& pos) const;
    int resizerAt(const QPoint& p) const;
    int headerColumnAt(const QPoint& p) const;
    int footerColumnAt(const QPoint& p) const;
    const ValueHit* hitAt(const QPoint& p) const;
    void paintCell(QPainter& p, const QRect& r, const bases::EntryPtr& e, const QString& prop, int column);
    void paintHeader(QPainter& p);
    void paintSummaryRow(QPainter& p, int y, const QList<bases::EntryPtr>& entries, bool footer);
    void showHeaderMenu(int column, const QPoint& globalPos);
    void showSummaryMenu(int column, const QList<bases::EntryPtr>& entries, const QPoint& globalPos);
    void saveColumnWidth(const QString& prop, int width);
    bool isEditable(const bases::EntryPtr& e, const QString& prop) const;
    void placeEditor();
    QString summaryText(const QString& prop, const QList<bases::EntryPtr>& entries, QString* name) const;

    QStringList m_columns;
    QList<int> m_widths;
    QHash<QString, int> m_contentWidths;
    QHash<QString, int> m_headerWidths;
    QHash<QString, int> m_customWidths;
    QList<GroupGeom> m_groups;
    int m_rowHeight = 30;   // with the rowHeight multiplier
    int m_rowHeight0 = 30;  // the base row height (header, headings, summaries)
    bool m_multiline = false;
    bool m_grouped = false;
    bool m_footer = false;
    bool m_groupSummaries = false;
    int m_bodyHeight = 0; // groups (content coordinates below the header)
    CellPos m_active;
    CellPos m_editing;
    QPointer<PropertyValueEditor> m_editor;
    QList<ValueHit> m_hits;   // links of the painted cells (viewport coordinates)
    QString m_hoverTarget;
    int m_hoverHeader = -1;
    int m_hoverResizer = -1;
    int m_hoverFooter = -1;
    // column resizing
    int m_resizing = -1;
    int m_resizeStartX = 0;
    int m_resizeStartWidth = 0;
    // header click (sort toggle)
    int m_pressHeader = -1;
    QPoint m_pressPos;
};

} // namespace oc::basesui
