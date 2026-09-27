#pragma once

#include "render/layout/style_sheet.h"

#include <QList>
#include <QPainter>
#include <QSizeF>
#include <QString>

#include <memory>

namespace oc {

// An inline widget occupying one placeholder character in the display string (checkbox, bullet,
// rendered inline math, image...). Width is reserved via letter spacing; tall widgets grow the line.
class InlineWidget {
public:
    virtual ~InlineWidget() = default;
    // Size in px given the line's metrics; ascent = part above the baseline.
    virtual QSizeF size(const LineMetrics& line) const = 0;
    virtual qreal ascent(const LineMetrics& line) const { return size(line).height() * 0.8; }
    virtual void paint(QPainter* p, const QRectF& rect, const LineMetrics& line) const = 0;
    virtual bool equals(const InlineWidget& other) const { return this == &other; }
    virtual quint64 signature() const = 0; // content hash for layout caching
    // Text used when copying a selection that covers the widget (empty = the hidden source is copied).
    virtual QString accessibleText() const { return {}; }
};

// Character styling over columns [from, to) of a document line.
struct Mark {
    int from = 0;
    int to = 0;
    StyleId style = StyleId::Normal;
    quint16 custom = 0; // > 0: 1-based index into LineDecoration::customStyles, used instead of `style`
};

// Replacement of columns [from, to) in the display: hidden, substituted by text (atomic), or by a widget.
struct Replace {
    enum class Kind : quint8 { Hide, Text, Widget };
    int from = 0;
    int to = 0;
    Kind kind = Kind::Hide;
    QString text;            // Kind::Text
    StyleId style = StyleId::Normal;
    std::shared_ptr<InlineWidget> widget; // Kind::Widget
};

// A widget outside the text flow, at a fixed x on a line's first visual line (CSS floats and outside markers:
// Reading view's list bullets, numbers and checkboxes). It sits on the first visual line's baseline like an
// inline widget; [from, to) are the source columns it stands for (a checkbox click toggles column from + 1).
struct FloatWidget {
    qreal x = 0; // px, text column coordinates (may be negative: in the left margin)
    std::shared_ptr<InlineWidget> widget;
    int from = 0;
    int to = 0;
};

struct LineDecoration {
    enum class CodeEdge : quint8 { None, Top, Middle, Bottom, Single };

    StyleId lineStyle = StyleId::Normal;
    QList<Mark> marks;       // may overlap; later marks win for conflicting properties
    QList<Replace> replaces; // sorted by `from`, non-overlapping
    qreal paddingTop = 0;
    qreal paddingBottom = 0;
    qreal indent = 0;        // left padding (px) applied to every visual line
    int hangingColumn = -1;  // wrapped lines start at the x of this column, floored to a pixel (Obsidian's hanging indent)
    qreal tabStop = 0;       // px; 0 = tabSize spaces of the base font
    int quoteDepth = 0;      // quote bars drawn at the left (block/reading rendering)
    QList<int> quoteColumns; // live preview: a quote bar at the x of each '>' marker column
    qreal guideOffset = 0;   // px added to a guide column's x
    CodeEdge code = CodeEdge::None;
    bool hideCaret = false;
    bool hidden = false;     // line not shown at all (reading view comments, folds)
    qreal fixedHeight = -1;  // >= 0: the line's unit is exactly this tall (reading view blank lines)
    bool hrRule = false;     // draw a horizontal rule across the line (live preview)
    QList<int> guideColumns; // indentation guides: at the x of these columns (+ guideOffset), the unit's full height
    int activeGuideColumn = -1; // the guideColumns entry drawn in --indentation-guide-color-active (.cm-active-indent)
    QList<qreal> guidePositions; // guides at a fixed x (px, text column coordinates): Reading view's nested lists
    QList<qreal> quoteBars;      // quote bars at a fixed x (px, text column coordinates): Reading view's blockquotes
    QList<FloatWidget> floatWidgets; // widgets outside the text flow (Reading view's list markers)
    // Styles that aren't theme constants (raw HTML `style="color: …"`), referenced by Mark::custom.
    QList<StyleSpec> customStyles;

    // Appends a custom style; returns the value for Mark::custom.
    quint16 addCustomStyle(const StyleSpec& spec)
    {
        customStyles.push_back(spec);
        return quint16(customStyles.size());
    }
    const StyleSpec& markSpec(const Mark& m, const StyleSheet& styles) const
    {
        return m.custom > 0 && m.custom <= customStyles.size() ? customStyles[m.custom - 1] : styles.spec(m.style);
    }

    quint64 signature() const;
};

// A widget replacing a range of whole lines (rendered table, callout, math block, embed...).
class BlockWidget {
public:
    virtual ~BlockWidget() = default;
    virtual qreal layout(qreal width) = 0;                      // lays out for `width`, returns the height
    virtual void paint(QPainter* p, const QRectF& rect) = 0;
    virtual quint64 signature() const = 0;
    // Document offset for a click at `local` (relative to the widget's top-left), or -1 (= block start).
    virtual qsizetype docPosAt(const QPointF& local) const { Q_UNUSED(local); return -1; }
    // Handle a click internally (fold toggles, checkboxes...). Return true if consumed.
    virtual bool click(const QPointF& local, Qt::KeyboardModifiers mods) { Q_UNUSED(local); Q_UNUSED(mods); return false; }
    // Link under `local` ("" if none): wikilink target / URL.
    virtual QString linkAt(const QPointF& local) const { Q_UNUSED(local); return {}; }
    // y (relative to the widget's top) of an in-document anchor it contains ("fn-1"), or -1.
    virtual qreal anchorY(const QString& anchor) const { Q_UNUSED(anchor); return -1; }
};

struct BlockRange {
    int fromLine = 0;
    int toLine = 0; // exclusive
    std::shared_ptr<BlockWidget> widget;
    // No widget: lines [fromLine, toLine) (at least two) are laid out as one text unit decorated by
    // LineDecorator::decorateRun() (Reading view with strict line breaks: a paragraph's lines flow together).
    bool textRun = false;
};

// Produces decorations for lines. Implementations: source-mode styling (block tree), live preview,
// reading. `quick*` methods must be cheap (used for height estimation of off-screen lines).
class LineDecorator {
public:
    virtual ~LineDecorator() = default;
    virtual LineDecoration decorate(int line) = 0;
    virtual StyleId quickLineStyle(int line) = 0;
    virtual qreal quickVerticalPadding(int line) { Q_UNUSED(line); return 0; }
    // Block widgets (and text runs) starting in [fromLine, toLine), sorted and non-overlapping.
    virtual void blockRanges(int fromLine, int toLine, QList<BlockRange>& out) { Q_UNUSED(fromLine); Q_UNUSED(toLine); Q_UNUSED(out); }
    // A text run (BlockRange::textRun) as one line: its text is lines [fromLine, toLine) joined with '\n' and the
    // columns are offsets from the first line's start. Decorators that return text runs implement it (the
    // default decorates the first line only); DocLayout shows a '\n' that no replace covers as a space.
    virtual LineDecoration decorateRun(int fromLine, int toLine)
    {
        Q_UNUSED(toLine);
        return decorate(fromLine);
    }
    // True while blockRanges() may return text runs: a relaid region then also re-queries the text unit before it
    // (an edit can join a paragraph with the one above).
    virtual bool hasTextRuns() const { return false; }
    // A block shown after the last line (Reading view's footnotes section), or nullptr. Queried whenever the
    // layout's text or decorations change, so it must be cheap when nothing changed.
    virtual std::shared_ptr<BlockWidget> trailer() { return nullptr; }
};

} // namespace oc
