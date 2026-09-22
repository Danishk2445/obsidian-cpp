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

struct LineDecoration {
    enum class CodeEdge : quint8 { None, Top, Middle, Bottom, Single };

    StyleId lineStyle = StyleId::Normal;
    QList<Mark> marks;       // may overlap; later marks win for conflicting properties
    QList<Replace> replaces; // sorted by `from`, non-overlapping
    qreal paddingTop = 0;
    qreal paddingBottom = 0;
    qreal indent = 0;        // left padding (px) applied to every visual line
    int hangingColumn = -1;  // wrapped lines start at the x of this column (list/quote content)
    qreal tabStop = 0;       // px; 0 = tabSize spaces of the base font
    int quoteDepth = 0;      // quote bars drawn at the left (live preview / reading)
    CodeEdge code = CodeEdge::None;
    bool hideCaret = false;
    bool hrRule = false;     // draw a horizontal rule across the line (live preview)
    QList<int> guideColumns; // indentation guide positions (columns) for list lines

    quint64 signature() const;
};

// Produces decorations for lines. Implementations: source-mode styling (block tree), live preview,
// reading. `quick*` methods must be cheap (used for height estimation of off-screen lines).
class LineDecorator {
public:
    virtual ~LineDecorator() = default;
    virtual LineDecoration decorate(int line) = 0;
    virtual StyleId quickLineStyle(int line) = 0;
    virtual qreal quickVerticalPadding(int line) { Q_UNUSED(line); return 0; }
};

} // namespace oc
