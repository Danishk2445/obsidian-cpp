#pragma once

#include "render/layout/decoration.h"
#include "render/layout/display_block.h"
#include "render/layout/height_map.h"
#include "render/layout/style_sheet.h"
#include "text/selection.h"
#include "text/text.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QTextLayout>

#include <memory>
#include <optional>
#include <vector>

class QPainter;

namespace oc {

// Virtualized layout of a document as a sequence of units: a text unit per line, or a block widget
// covering several lines (rendered table/callout/math...). Coordinates: x relative to the text column's
// left edge, y to the document top. Off-screen units keep an estimated (or last measured) height;
// ensureLaidOut() measures the visible range exactly. Line boxes emulate CSS: each visual line is
// round(fontPx * line-height) tall with the baseline centred, independent of fallback fonts.
class DocLayout {
public:
    struct VisualLine {
        qreal y = 0;        // relative to the unit top
        qreal height = 0;
        qreal baseline = 0; // relative to the unit top
        qreal x = 0;
        qreal width = 0;
        int dispStart = 0;
        int dispLength = 0;
    };
    struct UnitLayout {
        DisplayBlock block;
        std::unique_ptr<QTextLayout> text;
        LineDecoration deco;
        QList<VisualLine> lines;
        qreal height = 0;
        QList<QRectF> widgetRects; // per block.widgets() entry, relative to the unit top-left
    };

    DocLayout();
    ~DocLayout();

    void setDecorator(LineDecorator* decorator) { m_decorator = decorator; }
    void setStyleSheet(StyleSheetPtr styles);
    StyleSheetPtr styleSheet() const { return m_styles; }
    void setContentWidth(qreal width);
    qreal contentWidth() const { return m_width; }

    void setText(const Text& text);                 // full reset (re-queries block ranges)
    // Lines [from, oldTo) of the previous text became [from, newTo) of `text`.
    void replaceLines(const Text& text, int from, int oldTo, int newTo);
    // Decorations of these lines changed; block structure there is re-queried.
    void invalidateLines(int from, int to);
    void invalidateAll();

    // Lines hidden by folds: sorted, non-overlapping [from, to) ranges. Hidden lines are zero-height
    // text units (never decorated), and block widgets touching them fall back to plain lines.
    void setHiddenLines(QList<QPair<int, int>> ranges);
    const QList<QPair<int, int>>& hiddenLines() const { return m_hidden; }
    bool isHidden(int line) const;
    int hiddenRunEnd(int line) const; // end (exclusive) of the hidden range containing `line`, or `line`
    bool anyHidden(int from, int to) const;

    const Text& text() const { return m_text; }
    int lineCount() const { return int(m_lineToUnit.size()); }
    double totalHeight() const { return m_heights.total(); }
    double lineTop(int line) const;
    double lineHeight(int line) const;              // height of the unit containing the line
    int lineAtY(double y) const;                    // first line of the unit at y
    bool isLaidOut(int line) const;
    bool isInBlock(int line) const;                 // covered by a block widget

    bool ensureLaidOut(double y0, double y1);       // true if any height changed
    void evictOutside(double y0, double y1);
    const UnitLayout* layoutFor(int line);          // text units only (nullptr for block units)

    struct PaintContext {
        QList<SelRange> selection;
        bool drawSelection = true;
        QRectF clip;
        qreal rightEdge = 0;
    };
    void paint(QPainter* p, const PaintContext& ctx);

    qsizetype hitTest(const QPointF& pt, int* assocOut = nullptr);
    QRectF caretRect(qsizetype pos, int assoc = 1);
    qsizetype verticalMove(qsizetype pos, qreal goalX, int direction, int assoc = 1);
    qsizetype visualLineStart(qsizetype pos);
    qsizetype visualLineEnd(qsizetype pos);
    QList<QRectF> rangeRects(qsizetype from, qsizetype to);

    struct WidgetHit {
        int line = -1;
        std::shared_ptr<InlineWidget> widget;
        QRectF rect;
        int docFrom = 0;
        int docTo = 0;
    };
    std::optional<WidgetHit> widgetAt(const QPointF& pt);

    struct BlockHit {
        int fromLine = -1;
        int toLine = -1;
        std::shared_ptr<BlockWidget> widget;
        QRectF rect; // doc coordinates
    };
    std::optional<BlockHit> blockAt(const QPointF& pt);

    int laidOutCount() const;
    int unitCount() const { return int(m_units.size()); }

private:
    struct Unit {
        int firstLine = 0;
        int lineCount = 1;
        std::shared_ptr<BlockWidget> block;
        std::shared_ptr<UnitLayout> layout;
        double height = -1;
        bool measured = false;
    };

    void buildUnits(int fromLine, int toLine, std::vector<Unit>& out);
    void rebuildIndex();
    void rebuildHeights();
    double estimateHeight(const Unit& u);
    bool measure(int unitIndex);
    std::shared_ptr<UnitLayout> buildLayout(int line);
    void layoutText(UnitLayout& ul, const LineMetrics& lm);
    int visualLineIndex(const UnitLayout& ul, int dispPos) const;
    void restructure(int fromLine, int oldToLine, int newToLine);
    int unitOfLine(int line) const { return m_lineToUnit[size_t(std::clamp(line, 0, lineCount() - 1))]; }

    bool isHiddenUnit(const Unit& u) const { return !u.block && isHidden(u.firstLine); }

    Text m_text;
    QList<QPair<int, int>> m_hidden;
    std::shared_ptr<UnitLayout> m_hiddenLayout;
    LineDecorator* m_decorator = nullptr;
    StyleSheetPtr m_styles;
    qreal m_width = 700;
    std::vector<Unit> m_units;
    std::vector<int> m_lineToUnit;
    HeightMap m_heights;
};

} // namespace oc
