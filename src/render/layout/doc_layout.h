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
#include <vector>

class QPainter;

namespace oc {

// Virtualized layout of a document, one unit per line (block widgets spanning several lines come
// with live preview). Coordinates: x is relative to the text column's left edge, y to the document
// top. Off-screen units keep an estimated (or last measured) height; ensureLaidOut() measures the
// visible range exactly. Line boxes emulate CSS: every visual line is round(fontPx * line-height) tall
// with the baseline centred, independent of fallback fonts.
class DocLayout {
public:
    struct VisualLine {
        qreal y = 0;        // relative to the unit top
        qreal height = 0;
        qreal baseline = 0; // relative to the unit top
        qreal x = 0;        // line start x (indent/hanging)
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
        quint64 key = 0;
        QList<QRectF> widgetRects; // per block.widgets() entry, relative to the unit top-left
    };

    DocLayout();
    ~DocLayout();

    void setDecorator(LineDecorator* decorator) { m_decorator = decorator; }
    void setStyleSheet(StyleSheetPtr styles);
    StyleSheetPtr styleSheet() const { return m_styles; }
    void setContentWidth(qreal width);
    qreal contentWidth() const { return m_width; }

    void setText(const Text& text);                 // full reset
    // Lines [from, oldTo) of the previous text became [from, newTo) of `text`.
    void replaceLines(const Text& text, int from, int oldTo, int newTo);
    void invalidateLines(int from, int to);          // decorations changed
    void invalidateAll();

    const Text& text() const { return m_text; }
    int lineCount() const { return int(m_units.size()); }
    double totalHeight() const { return m_heights.total(); }
    double lineTop(int line) const { return m_heights.top(line); }
    double lineHeight(int line) const { return m_heights.height(line); }
    int lineAtY(double y) const { return m_heights.indexAt(y); }
    bool isLaidOut(int line) const;

    // Measure lines intersecting [y0, y1]. Returns true if any height changed.
    bool ensureLaidOut(double y0, double y1);
    // Free text layouts of lines outside [y0, y1] (keeps their measured heights).
    void evictOutside(double y0, double y1);
    const UnitLayout* layoutFor(int line); // lays out on demand

    struct PaintContext {
        QList<SelRange> selection;
        bool drawSelection = true;
        QRectF clip; // doc coordinates
        qreal rightEdge = 0; // x where full-line selection highlights end
    };
    void paint(QPainter* p, const PaintContext& ctx);

    // Hit testing and geometry (doc coordinates).
    qsizetype hitTest(const QPointF& pt, int* assocOut = nullptr);
    QRectF caretRect(qsizetype pos, int assoc = 1);
    qsizetype verticalMove(qsizetype pos, qreal goalX, int direction, int assoc = 1);
    qsizetype visualLineStart(qsizetype pos);
    qsizetype visualLineEnd(qsizetype pos);
    // Rects covering [from, to) (for scroll-into-view / overlays).
    QList<QRectF> rangeRects(qsizetype from, qsizetype to);

    // Stats (tests/perf)
    int laidOutCount() const;

private:
    struct Unit {
        std::shared_ptr<UnitLayout> layout;
        double height = -1; // last estimate/measurement (-1 = unknown)
        bool measured = false;
    };

    double estimateHeight(int line);
    std::shared_ptr<UnitLayout> buildLayout(int line);
    void layoutText(UnitLayout& ul, const LineMetrics& lm);
    int visualLineIndex(const UnitLayout& ul, int dispPos) const;
    void rebuildHeights();

    Text m_text;
    LineDecorator* m_decorator = nullptr;
    StyleSheetPtr m_styles;
    qreal m_width = 700;
    std::vector<Unit> m_units;
    HeightMap m_heights;
};

} // namespace oc
