#include "render/layout/doc_layout.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextOption>

#include <cmath>

namespace oc {

DocLayout::DocLayout() = default;
DocLayout::~DocLayout() = default;

void DocLayout::setStyleSheet(StyleSheetPtr styles)
{
    m_styles = std::move(styles);
    invalidateAll();
}

void DocLayout::setContentWidth(qreal width)
{
    width = std::max<qreal>(40, std::floor(width));
    if (width == m_width)
        return;
    m_width = width;
    // Keep measured heights as estimates (they're close) but drop the layouts.
    for (Unit& u : m_units) {
        u.layout.reset();
        u.measured = false;
        u.height = -1; // width changed: re-estimate (visible lines get measured right away)
    }
    rebuildHeights();
}

void DocLayout::setText(const Text& text)
{
    m_text = text;
    m_units.assign(size_t(text.lineCount()), Unit{});
    rebuildHeights();
}

void DocLayout::rebuildHeights()
{
    std::vector<double> h(m_units.size());
    for (size_t i = 0; i < m_units.size(); ++i) {
        Unit& u = m_units[i];
        if (u.layout)
            u.height = u.layout->height;
        else if (u.height < 0)
            u.height = estimateHeight(int(i));
        h[i] = u.height;
    }
    m_heights.assign(h);
}

void DocLayout::replaceLines(const Text& text, int from, int oldTo, int newTo)
{
    m_text = text;
    from = std::clamp(from, 0, int(m_units.size()));
    oldTo = std::clamp(oldTo, from, int(m_units.size()));
    const int added = newTo - from;
    const int removed = oldTo - from;
    if (added == removed) {
        // Same line count: keep previous heights as estimates (no jitter) until re-measured.
        for (int i = from; i < oldTo; ++i) {
            m_units[size_t(i)].layout.reset();
            m_units[size_t(i)].measured = false;
        }
        return;
    }
    const double keep = removed > 0 ? m_units[size_t(from)].height : -1;
    m_units.erase(m_units.begin() + from, m_units.begin() + oldTo);
    m_units.insert(m_units.begin() + from, size_t(added), Unit{});
    if (added > 0 && keep > 0)
        m_units[size_t(from)].height = keep;
    rebuildHeights();
}

void DocLayout::invalidateLines(int from, int to)
{
    from = std::max(0, from);
    to = std::min(to, int(m_units.size()));
    for (int i = from; i < to; ++i) {
        Unit& u = m_units[size_t(i)];
        if (u.layout) {
            u.layout.reset();
            u.measured = false;
        }
    }
}

void DocLayout::invalidateAll()
{
    for (Unit& u : m_units) {
        u.layout.reset();
        u.measured = false;
        u.height = -1;
    }
    if (!m_units.empty() && m_styles)
        rebuildHeights();
}

bool DocLayout::isLaidOut(int line) const { return line >= 0 && line < int(m_units.size()) && m_units[size_t(line)].layout != nullptr; }

int DocLayout::laidOutCount() const
{
    int n = 0;
    for (const Unit& u : m_units)
        n += u.layout ? 1 : 0;
    return n;
}

double DocLayout::estimateHeight(int line)
{
    if (!m_styles || !m_decorator)
        return 24;
    const StyleId ls = m_decorator->quickLineStyle(line);
    const LineMetrics& lm = m_styles->line(ls);
    const int len = m_text.lineLength(line);
    int visual = 1;
    if (len > 0) {
        const double w = len * lm.avgCharWidth;
        const double avail = std::max<qreal>(40, m_width - (ls == StyleId::CodeBlock ? m_styles->codePaddingX() : 0));
        // Account for wrap waste (half an average word per wrapped line).
        visual = std::max(1, int(std::ceil(w / std::max(20.0, avail - 3.0 * lm.avgCharWidth))));
    }
    return visual * lm.lineBox + m_decorator->quickVerticalPadding(line);
}

std::shared_ptr<DocLayout::UnitLayout> DocLayout::buildLayout(int line)
{
    auto ul = std::make_shared<UnitLayout>();
    ul->deco = m_decorator ? m_decorator->decorate(line) : LineDecoration{};
    const LineMetrics& lm = m_styles->line(ul->deco.lineStyle);
    ul->block = DisplayBlock::build(m_text.lineView(line), ul->deco, *m_styles);
    layoutText(*ul, lm);
    return ul;
}

void DocLayout::layoutText(UnitLayout& ul, const LineMetrics& lm)
{
    const LineDecoration& deco = ul.deco;
    ul.text = std::make_unique<QTextLayout>(ul.block.display(), lm.font);
    QTextLayout& tl = *ul.text;
    QTextOption opt;
    opt.setUseDesignMetrics(true);
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    opt.setTextDirection(Qt::LayoutDirectionAuto);
    opt.setAlignment(Qt::AlignLeft);
    const qreal tab = deco.tabStop > 0 ? deco.tabStop : lm.spaceWidth * m_styles->tabSizeSpaces();
    opt.setTabStopDistance(tab);
    tl.setTextOption(opt);
    QList<QTextLayout::FormatRange> fmts = ul.block.formats();
    tl.setFormats(fmts);
    tl.setCacheEnabled(true);

    const qreal quoteIndent = deco.quoteDepth > 0 ? deco.quoteDepth * m_styles->quoteIndent() : 0;
    const qreal x0 = deco.indent + quoteIndent + (deco.code != LineDecoration::CodeEdge::None ? m_styles->codePaddingX() : 0);
    const qreal avail = std::max<qreal>(20, m_width - x0 - (deco.code != LineDecoration::CodeEdge::None ? m_styles->codePaddingX() : 0));

    qreal y = deco.paddingTop;
    qreal hang = -1;
    ul.lines.clear();
    tl.beginLayout();
    for (int i = 0;; ++i) {
        QTextLine line = tl.createLine();
        if (!line.isValid())
            break;
        const qreal indent = (i == 0 || hang < 0) ? 0 : hang;
        line.setLineWidth(std::max<qreal>(10, avail - indent));
        if (i == 0 && deco.hangingColumn > 0 && line.textLength() < ul.block.display().size()) {
            const int hd = ul.block.docToDisp(deco.hangingColumn);
            if (hd < line.textStart() + line.textLength())
                hang = std::min(line.cursorToX(hd), avail * 0.5);
        }
        // CSS-like line box: fixed height, baseline centred on the base font, taller widgets grow it.
        qreal boxH = lm.lineBox;
        qreal extraAbove = 0;
        for (const DisplayBlock::WidgetSlot& w : ul.block.widgets()) {
            if (w.dispPos >= line.textStart() && w.dispPos < line.textStart() + line.textLength()) {
                const qreal above = w.ascent - lm.ascent;
                const qreal below = (w.size.height() - w.ascent) - lm.descent;
                extraAbove = std::max(extraAbove, above);
                boxH = std::max(boxH, lm.lineBox + std::max<qreal>(0, above) + std::max<qreal>(0, below));
            }
        }
        const qreal baseline = y + std::max<qreal>(0, extraAbove) + (lm.lineBox - (lm.ascent + lm.descent)) / 2 + lm.ascent;
        line.setPosition(QPointF(x0 + indent, baseline - line.ascent()));
        VisualLine vl;
        vl.y = y;
        vl.height = boxH;
        vl.baseline = baseline;
        vl.x = x0 + indent;
        vl.width = line.naturalTextWidth();
        vl.dispStart = line.textStart();
        vl.dispLength = line.textLength();
        ul.lines.push_back(vl);
        y += boxH;
    }
    tl.endLayout();
    if (ul.lines.isEmpty()) {
        VisualLine vl;
        vl.y = y;
        vl.height = lm.lineBox;
        vl.baseline = y + (lm.lineBox - (lm.ascent + lm.descent)) / 2 + lm.ascent;
        vl.x = x0;
        ul.lines.push_back(vl);
        y += lm.lineBox;
    }
    ul.height = y + deco.paddingBottom;

    ul.widgetRects.clear();
    for (const DisplayBlock::WidgetSlot& w : ul.block.widgets()) {
        const int li = visualLineIndex(ul, w.dispPos);
        const VisualLine& vl = ul.lines[li];
        const QTextLine ql = tl.lineAt(li);
        const qreal x = ql.cursorToX(w.dispPos);
        ul.widgetRects.push_back(QRectF(x, vl.baseline - w.ascent, w.size.width(), w.size.height()));
    }
}

int DocLayout::visualLineIndex(const UnitLayout& ul, int dispPos) const
{
    for (int i = int(ul.lines.size()) - 1; i >= 0; --i)
        if (dispPos >= ul.lines[i].dispStart)
            return i;
    return 0;
}

const DocLayout::UnitLayout* DocLayout::layoutFor(int line)
{
    if (line < 0 || line >= int(m_units.size()) || !m_styles)
        return nullptr;
    Unit& u = m_units[size_t(line)];
    if (!u.layout) {
        u.layout = buildLayout(line);
        u.measured = true;
        u.height = u.layout->height;
        m_heights.set(line, u.layout->height);
    }
    return u.layout.get();
}

bool DocLayout::ensureLaidOut(double y0, double y1)
{
    if (m_units.empty() || !m_styles)
        return false;
    bool changed = false;
    int i = m_heights.indexAt(y0);
    // Iterate while the unit top (with the heights as they are updated) is above y1.
    double top = m_heights.top(i);
    while (i < int(m_units.size()) && top <= y1) {
        Unit& u = m_units[size_t(i)];
        if (!u.layout) {
            const double before = m_heights.height(i);
            u.layout = buildLayout(i);
            u.measured = true;
            u.height = u.layout->height;
            if (u.layout->height != before) {
                m_heights.set(i, u.layout->height);
                changed = true;
            }
        }
        top += m_heights.height(i);
        ++i;
    }
    return changed;
}

void DocLayout::evictOutside(double y0, double y1)
{
    if (m_units.empty())
        return;
    const int a = m_heights.indexAt(y0), b = m_heights.indexAt(y1);
    for (int i = 0; i < int(m_units.size()); ++i) {
        if (i >= a && i <= b)
            continue;
        m_units[size_t(i)].layout.reset(); // measured height stays in the height map
    }
}

void DocLayout::paint(QPainter* p, const PaintContext& ctx)
{
    if (m_units.empty() || !m_styles)
        return;
    const StyleSheet& ss = *m_styles;
    int i = m_heights.indexAt(ctx.clip.top());
    double top = m_heights.top(i);
    const QColor selColor = ss.selection();
    while (i < int(m_units.size()) && top <= ctx.clip.bottom()) {
        const UnitLayout* ul = layoutFor(i);
        const double h = m_heights.height(i);
        if (!ul) {
            top += h;
            ++i;
            continue;
        }
        const LineDecoration& deco = ul->deco;
        const qreal unitTop = top;

        // Code block background (full text-column width, rounded at the block's ends).
        if (deco.code != LineDecoration::CodeEdge::None) {
            const QRectF r(deco.indent, unitTop, m_width - deco.indent, ul->height);
            const qreal rad = ss.codeRadius();
            QPainterPath path;
            const bool topRound = deco.code == LineDecoration::CodeEdge::Top || deco.code == LineDecoration::CodeEdge::Single;
            const bool botRound = deco.code == LineDecoration::CodeEdge::Bottom || deco.code == LineDecoration::CodeEdge::Single;
            path.moveTo(r.left(), r.top() + (topRound ? rad : 0));
            if (topRound)
                path.quadTo(r.left(), r.top(), r.left() + rad, r.top());
            path.lineTo(r.right() - (topRound ? rad : 0), r.top());
            if (topRound)
                path.quadTo(r.right(), r.top(), r.right(), r.top() + rad);
            path.lineTo(r.right(), r.bottom() - (botRound ? rad : 0));
            if (botRound)
                path.quadTo(r.right(), r.bottom(), r.right() - rad, r.bottom());
            path.lineTo(r.left() + (botRound ? rad : 0), r.bottom());
            if (botRound)
                path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - rad);
            path.closeSubpath();
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->fillPath(path, ss.codeBackground());
            p->restore();
        }
        // Quote bars.
        for (int q = 0; q < deco.quoteDepth; ++q) {
            const qreal x = deco.indent + q * ss.quoteIndent();
            p->fillRect(QRectF(x, unitTop, ss.quoteBarWidth(), ul->height), ss.quoteBarColor());
        }
        // Horizontal rule (live preview).
        if (deco.hrRule) {
            const qreal y = unitTop + ul->height / 2;
            p->fillRect(QRectF(deco.indent, y - ss.hrThickness() / 2, m_width - deco.indent, ss.hrThickness()), ss.hrColor());
        }
        // Indentation guides (at columns, drawn through the whole unit height).
        if (!deco.guideColumns.isEmpty() && !ul->lines.isEmpty() && ul->text) {
            const QTextLine first = ul->text->lineAt(0);
            for (int col : deco.guideColumns) {
                const int dp = ul->block.docToDisp(col);
                const qreal x = first.cursorToX(dp) + ss.listIndent() * 0.25;
                p->fillRect(QRectF(std::round(x), unitTop, 1, ul->height), ss.guideColor());
            }
        }

        // Pills (inline code / tags / highlights with boxes).
        if (!ul->block.boxes().isEmpty() && ul->text) {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            for (const DisplayBlock::Box& b : ul->block.boxes()) {
                for (int li = 0; li < ul->lines.size(); ++li) {
                    const VisualLine& vl = ul->lines[li];
                    const int s = std::max(b.dispFrom, vl.dispStart), e = std::min(b.dispTo, vl.dispStart + vl.dispLength);
                    if (e <= s)
                        continue;
                    const QTextLine ql = ul->text->lineAt(li);
                    const qreal x1 = ql.cursorToX(s), x2 = ql.cursorToX(e);
                    const qreal pad = b.style.padX;
                    const LineMetrics& lm = ss.line(deco.lineStyle);
                    const qreal hh = (lm.ascent + lm.descent) * 1.0;
                    QRectF r(std::min(x1, x2) - pad, unitTop + vl.baseline - lm.ascent + (lm.ascent + lm.descent - hh) / 2, std::abs(x2 - x1) + 2 * pad, hh);
                    const qreal rad = std::min(b.style.radius, r.height() / 2);
                    QPainterPath path;
                    path.addRoundedRect(r, rad, rad);
                    p->fillPath(path, b.style.background);
                }
            }
            p->restore();
        }

        // Selection -> background format ranges drawn by QTextLayout (bidi-correct).
        QList<QTextLayout::FormatRange> selFormats;
        bool extendEol = false;
        if (ctx.drawSelection && ul->text) {
            const qsizetype ls = m_text.lineStart(i), le = m_text.lineEnd(i);
            for (const SelRange& r : ctx.selection) {
                if (r.empty() || r.to() < ls || r.from() > le + 1)
                    continue;
                const int a = ul->block.docToDisp(int(std::max(r.from(), ls) - ls));
                const int b = ul->block.docToDisp(int(std::min(r.to(), le) - ls));
                if (b > a) {
                    QTextLayout::FormatRange fr;
                    fr.start = a;
                    fr.length = b - a;
                    fr.format.setBackground(selColor);
                    selFormats.push_back(fr);
                }
                if (r.to() > le && r.from() <= le)
                    extendEol = true;
            }
        }
        if (ul->text)
            ul->text->draw(p, QPointF(0, unitTop), selFormats);
        if (extendEol) {
            const VisualLine& last = ul->lines.last();
            const QTextLine ql = ul->text->lineAt(int(ul->lines.size()) - 1);
            const qreal x = ql.cursorToX(last.dispStart + last.dispLength);
            const qreal right = std::max(ctx.rightEdge, m_width);
            if (right > x)
                p->fillRect(QRectF(x, unitTop + last.y, right - x, last.height), selColor);
        }
        // Inline widgets.
        for (int w = 0; w < ul->block.widgets().size(); ++w) {
            const auto& slot = ul->block.widgets()[w];
            if (!slot.widget)
                continue;
            slot.widget->paint(p, ul->widgetRects[w].translated(0, unitTop), ss.line(deco.lineStyle));
        }
        top += h;
        ++i;
    }
}

qsizetype DocLayout::hitTest(const QPointF& pt, int* assocOut)
{
    if (m_units.empty())
        return 0;
    const int line = m_heights.indexAt(pt.y());
    const UnitLayout* ul = layoutFor(line);
    const qsizetype ls = m_text.lineStart(line);
    if (!ul || !ul->text)
        return ls;
    const double top = m_heights.top(line);
    const qreal y = pt.y() - top;
    int li = 0;
    for (int k = 0; k < ul->lines.size(); ++k) {
        if (y >= ul->lines[k].y)
            li = k;
    }
    if (pt.y() < top)
        li = 0;
    const QTextLine ql = ul->text->lineAt(li);
    int disp = ql.xToCursor(pt.x(), QTextLine::CursorBetweenCharacters);
    // Clicking past the end of a wrapped line lands at its end, before the wrap.
    const VisualLine& vl = ul->lines[li];
    disp = std::clamp(disp, vl.dispStart, vl.dispStart + vl.dispLength);
    if (li + 1 < ul->lines.size() && disp == vl.dispStart + vl.dispLength && vl.dispLength > 0) {
        // the break position belongs to the next visual line; stay on this one when at its end
        if (assocOut)
            *assocOut = -1;
    } else if (assocOut) {
        *assocOut = 1;
    }
    const int col = ul->block.dispToDoc(disp, 1);
    return ls + col;
}

QRectF DocLayout::caretRect(qsizetype pos, int assoc)
{
    if (m_units.empty())
        return {};
    pos = std::clamp<qsizetype>(pos, 0, m_text.length());
    const int line = m_text.lineAt(pos);
    const UnitLayout* ul = layoutFor(line);
    if (!ul)
        return {};
    const int col = int(pos - m_text.lineStart(line));
    const int disp = ul->block.docToDisp(col, assoc);
    int li = visualLineIndex(*ul, disp);
    // At a soft wrap boundary with assoc < 0 keep the caret at the end of the previous visual line.
    if (assoc < 0 && li > 0 && disp == ul->lines[li].dispStart)
        --li;
    const QTextLine ql = ul->text->lineAt(li);
    const qreal x = ql.cursorToX(disp);
    const VisualLine& vl = ul->lines[li];
    const LineMetrics& lm = m_styles->line(ul->deco.lineStyle);
    const double top = m_heights.top(line);
    const qreal h = lm.ascent + lm.descent;
    return QRectF(x, top + vl.baseline - lm.ascent, 1, h);
}

qsizetype DocLayout::verticalMove(qsizetype pos, qreal goalX, int direction, int assoc)
{
    const int line = m_text.lineAt(pos);
    const UnitLayout* ul = layoutFor(line);
    if (!ul)
        return pos;
    const int col = int(pos - m_text.lineStart(line));
    const int disp = ul->block.docToDisp(col, assoc);
    int li = visualLineIndex(*ul, disp);
    if (assoc < 0 && li > 0 && disp == ul->lines[li].dispStart)
        --li;
    int targetLine = line;
    int targetVl = li + direction;
    const UnitLayout* target = ul;
    if (targetVl < 0) {
        if (line == 0)
            return 0;
        targetLine = line - 1;
        target = layoutFor(targetLine);
        targetVl = int(target->lines.size()) - 1;
    } else if (targetVl >= ul->lines.size()) {
        if (line + 1 >= int(m_units.size()))
            return m_text.length();
        targetLine = line + 1;
        target = layoutFor(targetLine);
        targetVl = 0;
    }
    const QTextLine ql = target->text->lineAt(targetVl);
    int d = ql.xToCursor(goalX, QTextLine::CursorBetweenCharacters);
    const VisualLine& vl = target->lines[targetVl];
    d = std::clamp(d, vl.dispStart, vl.dispStart + vl.dispLength);
    // Don't land after the wrap point of a non-final visual line (it belongs to the next line).
    if (targetVl + 1 < target->lines.size() && d == vl.dispStart + vl.dispLength && vl.dispLength > 0)
        d = std::max(vl.dispStart, d - 1);
    return m_text.lineStart(targetLine) + target->block.dispToDoc(d, -1);
}

qsizetype DocLayout::visualLineStart(qsizetype pos)
{
    const int line = m_text.lineAt(pos);
    const UnitLayout* ul = layoutFor(line);
    if (!ul)
        return m_text.lineStart(line);
    const int disp = ul->block.docToDisp(int(pos - m_text.lineStart(line)));
    const int li = visualLineIndex(*ul, disp);
    return m_text.lineStart(line) + ul->block.dispToDoc(ul->lines[li].dispStart, 1);
}

qsizetype DocLayout::visualLineEnd(qsizetype pos)
{
    const int line = m_text.lineAt(pos);
    const UnitLayout* ul = layoutFor(line);
    if (!ul)
        return m_text.lineEnd(line);
    const int disp = ul->block.docToDisp(int(pos - m_text.lineStart(line)));
    const int li = visualLineIndex(*ul, disp);
    const VisualLine& vl = ul->lines[li];
    int end = vl.dispStart + vl.dispLength;
    if (li + 1 < ul->lines.size()) {
        // Stop before the trailing space that caused the wrap.
        const QString& d = ul->block.display();
        if (end > vl.dispStart && end - 1 < d.size() && d[end - 1].isSpace())
            --end;
    }
    return m_text.lineStart(line) + ul->block.dispToDoc(end, -1);
}

QList<QRectF> DocLayout::rangeRects(qsizetype from, qsizetype to)
{
    QList<QRectF> out;
    if (m_units.empty())
        return out;
    const int la = m_text.lineAt(from), lb = m_text.lineAt(to);
    for (int line = la; line <= lb; ++line) {
        const UnitLayout* ul = layoutFor(line);
        if (!ul)
            continue;
        const double top = m_heights.top(line);
        const qsizetype ls = m_text.lineStart(line);
        const int a = ul->block.docToDisp(int(std::max(from, ls) - ls));
        const int b = ul->block.docToDisp(int(std::min(to, m_text.lineEnd(line)) - ls));
        for (int li = 0; li < ul->lines.size(); ++li) {
            const VisualLine& vl = ul->lines[li];
            const int s = std::max(a, vl.dispStart), e = std::min(b, vl.dispStart + vl.dispLength);
            if (e < s || (e == s && a != b))
                continue;
            const QTextLine ql = ul->text->lineAt(li);
            const qreal x1 = ql.cursorToX(s), x2 = ql.cursorToX(e);
            out.push_back(QRectF(std::min(x1, x2), top + vl.y, std::max<qreal>(1, std::abs(x2 - x1)), vl.height));
        }
    }
    return out;
}

} // namespace oc
