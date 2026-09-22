#include "render/layout/doc_layout.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextOption>

#include <cmath>

namespace oc {

DocLayout::DocLayout() = default;
DocLayout::~DocLayout() = default;

// ---------------------------------------------------------------------------------------------
// structure
// ---------------------------------------------------------------------------------------------
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
    for (Unit& u : m_units) {
        u.layout.reset();
        u.measured = false;
        u.height = -1;
    }
    rebuildHeights();
}

void DocLayout::buildUnits(int fromLine, int toLine, std::vector<Unit>& out)
{
    QList<BlockRange> blocks;
    if (m_decorator && toLine > fromLine)
        m_decorator->blockRanges(fromLine, toLine, blocks);
    int line = fromLine;
    for (const BlockRange& b : blocks) {
        if (b.fromLine < line || b.toLine <= b.fromLine || !b.widget)
            continue;
        for (; line < b.fromLine && line < toLine; ++line)
            out.push_back(Unit{line, 1, nullptr, nullptr, -1, false});
        Unit u;
        u.firstLine = b.fromLine;
        u.lineCount = b.toLine - b.fromLine;
        u.block = b.widget;
        out.push_back(u);
        line = b.toLine;
    }
    for (; line < toLine; ++line)
        out.push_back(Unit{line, 1, nullptr, nullptr, -1, false});
}

void DocLayout::rebuildIndex()
{
    m_lineToUnit.assign(size_t(m_text.lineCount()), 0);
    for (size_t u = 0; u < m_units.size(); ++u) {
        const Unit& un = m_units[u];
        for (int l = 0; l < un.lineCount; ++l)
            if (size_t(un.firstLine + l) < m_lineToUnit.size())
                m_lineToUnit[size_t(un.firstLine + l)] = int(u);
    }
}

void DocLayout::rebuildHeights()
{
    std::vector<double> h(m_units.size());
    for (size_t i = 0; i < m_units.size(); ++i) {
        Unit& u = m_units[i];
        if (u.layout)
            u.height = u.layout->height;
        else if (u.height < 0)
            u.height = estimateHeight(u);
        h[i] = u.height;
    }
    m_heights.assign(h);
}

void DocLayout::setText(const Text& text)
{
    m_text = text;
    m_units.clear();
    buildUnits(0, text.lineCount(), m_units);
    rebuildIndex();
    rebuildHeights();
}

void DocLayout::restructure(int from, int oldTo, int newTo)
{
    // Old units overlapping [from, oldTo) (at least the unit at `from`), expanded to whole units.
    const int oldLines = int(m_lineToUnit.size());
    if (m_units.empty() || oldLines == 0) {
        setText(m_text);
        return;
    }
    const int uFrom = m_lineToUnit[size_t(std::clamp(from, 0, oldLines - 1))];
    int uTo = m_lineToUnit[size_t(std::clamp(std::max(from, oldTo - 1), 0, oldLines - 1))] + 1;
    int L0 = m_units[size_t(uFrom)].firstLine;
    int L1old = m_units[size_t(uTo - 1)].firstLine + m_units[size_t(uTo - 1)].lineCount;
    const int delta = (newTo - from) - (oldTo - from);
    int L1new = L1old + delta;

    std::vector<Unit> fresh;
    for (;;) {
        fresh.clear();
        buildUnits(L0, std::min(L1new, m_text.lineCount()), fresh);
        // A block may extend beyond the rebuilt region: grow the region to its end.
        int reach = L1new;
        for (const Unit& u : fresh)
            reach = std::max(reach, u.firstLine + u.lineCount);
        if (reach <= L1new || uTo >= int(m_units.size()))
            break;
        while (uTo < int(m_units.size()) && m_units[size_t(uTo)].firstLine + delta < reach)
            ++uTo;
        L1old = m_units[size_t(uTo - 1)].firstLine + m_units[size_t(uTo - 1)].lineCount;
        L1new = L1old + delta;
    }
    // Fast path: same unit shape (typing inside lines, inline reveal changes) -> drop the layouts only,
    // keep heights as estimates; no O(lines) index/height rebuild.
    if (delta == 0 && int(fresh.size()) == uTo - uFrom) {
        bool same = true;
        for (int k = 0; k < uTo - uFrom && same; ++k) {
            const Unit& o = m_units[size_t(uFrom + k)];
            const Unit& n = fresh[size_t(k)];
            same = !o.block && !n.block && o.firstLine == n.firstLine && o.lineCount == n.lineCount;
        }
        if (same) {
            for (int k = uFrom; k < uTo; ++k) {
                m_units[size_t(k)].layout.reset();
                m_units[size_t(k)].measured = false;
            }
            return;
        }
    }
    // Preserve measured heights of unchanged text units where possible (reduces jitter).
    if (!fresh.empty() && uTo - uFrom == 1 && fresh.size() == 1 && !fresh[0].block && !m_units[size_t(uFrom)].block)
        fresh[0].height = m_units[size_t(uFrom)].height;
    for (size_t k = size_t(uTo); k < m_units.size(); ++k)
        m_units[k].firstLine += delta;
    m_units.erase(m_units.begin() + uFrom, m_units.begin() + uTo);
    m_units.insert(m_units.begin() + uFrom, fresh.begin(), fresh.end());
    rebuildIndex();
    rebuildHeights();
}

void DocLayout::replaceLines(const Text& text, int from, int oldTo, int newTo)
{
    m_text = text;
    restructure(from, oldTo, newTo);
}

void DocLayout::invalidateLines(int from, int to)
{
    if (m_units.empty())
        return;
    from = std::max(0, from);
    to = std::min(to, lineCount());
    if (from >= to)
        return;
    // Drop text layouts; re-query blocks in the region (reveal state may have changed).
    restructure(from, to, to);
}

void DocLayout::invalidateAll()
{
    if (!m_styles)
        return;
    m_units.clear();
    buildUnits(0, m_text.lineCount(), m_units);
    rebuildIndex();
    rebuildHeights();
}

// ---------------------------------------------------------------------------------------------
// geometry queries
// ---------------------------------------------------------------------------------------------
double DocLayout::lineTop(int line) const { return m_units.empty() ? 0 : m_heights.top(unitOfLine(line)); }
double DocLayout::lineHeight(int line) const { return m_units.empty() ? 0 : m_heights.height(unitOfLine(line)); }
int DocLayout::lineAtY(double y) const { return m_units.empty() ? 0 : m_units[size_t(m_heights.indexAt(y))].firstLine; }
bool DocLayout::isLaidOut(int line) const
{
    if (m_units.empty())
        return false;
    const Unit& u = m_units[size_t(unitOfLine(line))];
    return u.block ? u.measured : u.layout != nullptr;
}
bool DocLayout::isInBlock(int line) const { return !m_units.empty() && m_units[size_t(unitOfLine(line))].block != nullptr; }

int DocLayout::laidOutCount() const
{
    int n = 0;
    for (const Unit& u : m_units)
        n += u.layout ? 1 : 0;
    return n;
}

double DocLayout::estimateHeight(const Unit& u)
{
    if (!m_styles || !m_decorator)
        return 24;
    if (u.block)
        return u.lineCount * m_styles->line(StyleId::Normal).lineBox;
    const int line = u.firstLine;
    const StyleId ls = m_decorator->quickLineStyle(line);
    const LineMetrics& lm = m_styles->line(ls);
    const int len = m_text.lineLength(line);
    int visual = 1;
    if (len > 0) {
        const double w = len * lm.avgCharWidth;
        const double avail = std::max<qreal>(40, m_width - (ls == StyleId::CodeBlock ? 2 * m_styles->codePaddingX() : 0));
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
    if (deco.hidden) {
        ul.text = std::make_unique<QTextLayout>(QString(), lm.font);
        ul.text->beginLayout();
        QTextLine l = ul.text->createLine();
        l.setLineWidth(10);
        l.setPosition(QPointF(0, 0));
        ul.text->endLayout();
        VisualLine vl;
        vl.height = 0;
        ul.lines = {vl};
        ul.height = 0;
        return;
    }
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
    tl.setFormats(ul.block.formats());
    tl.setCacheEnabled(true);

    const bool code = deco.code != LineDecoration::CodeEdge::None;
    const qreal quoteIndent = deco.quoteDepth > 0 ? deco.quoteDepth * m_styles->quoteIndent() : 0;
    const qreal x0 = deco.indent + quoteIndent + (code ? m_styles->codePaddingX() : 0);
    const qreal avail = std::max<qreal>(20, m_width - x0 - (code ? m_styles->codePaddingX() : 0));

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
    if (deco.fixedHeight >= 0) {
        const qreal shift = (deco.fixedHeight - ul.height) / 2;
        for (int i = 0; i < ul.lines.size(); ++i) {
            ul.lines[i].y += shift;
            ul.lines[i].baseline += shift;
            if (i < tl.lineCount()) {
                QTextLine ql = tl.lineAt(i);
                ql.setPosition(ql.position() + QPointF(0, shift));
            }
        }
        ul.height = deco.fixedHeight;
    }

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

bool DocLayout::measure(int ui)
{
    Unit& u = m_units[size_t(ui)];
    const double before = m_heights.height(ui);
    if (u.block) {
        if (u.measured)
            return false;
        u.height = u.block->layout(m_width);
        u.measured = true;
    } else {
        if (u.layout)
            return false;
        u.layout = buildLayout(u.firstLine);
        u.height = u.layout->height;
        u.measured = true;
    }
    if (u.height != before) {
        m_heights.set(ui, u.height);
        return true;
    }
    return false;
}

const DocLayout::UnitLayout* DocLayout::layoutFor(int line)
{
    if (m_units.empty() || !m_styles || line < 0 || line >= lineCount())
        return nullptr;
    const int ui = unitOfLine(line);
    Unit& u = m_units[size_t(ui)];
    if (u.block)
        return nullptr;
    measure(ui);
    return u.layout.get();
}

bool DocLayout::ensureLaidOut(double y0, double y1)
{
    if (m_units.empty() || !m_styles)
        return false;
    bool changed = false;
    int i = m_heights.indexAt(y0);
    double top = m_heights.top(i);
    while (i < int(m_units.size()) && top <= y1) {
        changed |= measure(i);
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
        m_units[size_t(i)].layout.reset();
    }
}

// ---------------------------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------------------------
void DocLayout::paint(QPainter* p, const PaintContext& ctx)
{
    if (m_units.empty() || !m_styles)
        return;
    const StyleSheet& ss = *m_styles;
    int i = m_heights.indexAt(ctx.clip.top());
    double top = m_heights.top(i);
    const QColor selColor = ss.selection();
    while (i < int(m_units.size()) && top <= ctx.clip.bottom()) {
        measure(i);
        Unit& unit = m_units[size_t(i)];
        const double h = m_heights.height(i);
        if (unit.block) {
            unit.block->paint(p, QRectF(0, top, m_width, h));
            // selection over a block: tint it
            if (ctx.drawSelection) {
                const qsizetype bs = m_text.lineStart(unit.firstLine), be = m_text.lineEnd(unit.firstLine + unit.lineCount - 1);
                for (const SelRange& r : ctx.selection)
                    if (!r.empty() && r.from() <= bs && r.to() >= be)
                        p->fillRect(QRectF(0, top, m_width, h), selColor);
            }
            top += h;
            ++i;
            continue;
        }
        const UnitLayout* ul = unit.layout.get();
        if (!ul) {
            top += h;
            ++i;
            continue;
        }
        const LineDecoration& deco = ul->deco;
        const qreal unitTop = top;
        const int line = unit.firstLine;

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
        for (int q = 0; q < deco.quoteDepth; ++q) {
            const qreal x = deco.indent + q * ss.quoteIndent();
            p->fillRect(QRectF(x, unitTop, ss.quoteBarWidth(), ul->height), ss.quoteBarColor());
        }
        if (!deco.quoteColumns.isEmpty() && ul->text && ul->text->lineCount() > 0) {
            const QTextLine first = ul->text->lineAt(0);
            for (int col : deco.quoteColumns) {
                const qreal x = first.cursorToX(ul->block.docToDisp(col, -1));
                p->fillRect(QRectF(std::round(x), unitTop, ss.quoteBarWidth(), ul->height), ss.quoteBarColor());
            }
        }
        if (deco.hrRule) {
            const qreal y = unitTop + ul->height / 2;
            p->fillRect(QRectF(deco.indent, std::round(y - ss.hrThickness() / 2), m_width - deco.indent, ss.hrThickness()), ss.hrColor());
        }
        if (!deco.guideColumns.isEmpty() && ul->text && ul->text->lineCount() > 0) {
            const QTextLine first = ul->text->lineAt(0);
            for (int col : deco.guideColumns) {
                const qreal x = first.cursorToX(ul->block.docToDisp(col)) + deco.guideOffset;
                p->fillRect(QRectF(std::round(x), unitTop, 1, ul->height), ss.guideColor());
            }
        }
        if (!ul->block.boxes().isEmpty() && ul->text) {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            const LineMetrics& lm = ss.line(deco.lineStyle);
            for (const DisplayBlock::Box& b : ul->block.boxes()) {
                for (int li = 0; li < ul->lines.size(); ++li) {
                    const VisualLine& vl = ul->lines[li];
                    const int s = std::max(b.dispFrom, vl.dispStart), e = std::min(b.dispTo, vl.dispStart + vl.dispLength);
                    if (e <= s)
                        continue;
                    const QTextLine ql = ul->text->lineAt(li);
                    const qreal x1 = ql.cursorToX(s), x2 = ql.cursorToX(e);
                    const qreal pad = b.style.padX;
                    const qreal hh = lm.ascent + lm.descent;
                    QRectF r(std::min(x1, x2) - pad, unitTop + vl.baseline - lm.ascent, std::abs(x2 - x1) + 2 * pad, hh);
                    const qreal rad = std::min(b.style.radius, r.height() / 2);
                    QPainterPath path;
                    path.addRoundedRect(r, rad, rad);
                    p->fillPath(path, b.style.background);
                }
            }
            p->restore();
        }

        QList<QTextLayout::FormatRange> selFormats;
        bool extendEol = false;
        if (ctx.drawSelection && ul->text) {
            const qsizetype ls = m_text.lineStart(line), le = m_text.lineEnd(line);
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
        if (extendEol && !deco.hidden) {
            const VisualLine& last = ul->lines.last();
            const QTextLine ql = ul->text->lineAt(int(ul->lines.size()) - 1);
            const qreal x = ql.cursorToX(last.dispStart + last.dispLength);
            const qreal right = std::max(ctx.rightEdge, m_width);
            if (right > x)
                p->fillRect(QRectF(x, unitTop + last.y, right - x, last.height), selColor);
        }
        for (int w = 0; w < ul->block.widgets().size(); ++w) {
            const auto& slot = ul->block.widgets()[w];
            if (slot.widget)
                slot.widget->paint(p, ul->widgetRects[w].translated(0, unitTop), ss.line(deco.lineStyle));
        }
        top += h;
        ++i;
    }
}

// ---------------------------------------------------------------------------------------------
// hit testing & navigation
// ---------------------------------------------------------------------------------------------
qsizetype DocLayout::hitTest(const QPointF& pt, int* assocOut)
{
    if (m_units.empty())
        return 0;
    const int ui = m_heights.indexAt(pt.y());
    measure(ui);
    const Unit& u = m_units[size_t(ui)];
    const double top = m_heights.top(ui);
    if (u.block) {
        const qsizetype p = u.block->docPosAt(pt - QPointF(0, top));
        return p >= 0 ? p : m_text.lineStart(u.firstLine);
    }
    const UnitLayout* ul = u.layout.get();
    const qsizetype ls = m_text.lineStart(u.firstLine);
    if (!ul || !ul->text || ul->deco.hidden)
        return ls;
    const qreal y = pt.y() - top;
    int li = 0;
    for (int k = 0; k < ul->lines.size(); ++k)
        if (y >= ul->lines[k].y)
            li = k;
    if (pt.y() < top)
        li = 0;
    const QTextLine ql = ul->text->lineAt(li);
    int disp = ql.xToCursor(pt.x(), QTextLine::CursorBetweenCharacters);
    const VisualLine& vl = ul->lines[li];
    disp = std::clamp(disp, vl.dispStart, vl.dispStart + vl.dispLength);
    if (assocOut)
        *assocOut = (li + 1 < ul->lines.size() && disp == vl.dispStart + vl.dispLength && vl.dispLength > 0) ? -1 : 1;
    return ls + ul->block.dispToDoc(disp, 1);
}

QRectF DocLayout::caretRect(qsizetype pos, int assoc)
{
    if (m_units.empty() || !m_styles)
        return {};
    pos = std::clamp<qsizetype>(pos, 0, m_text.length());
    const int line = m_text.lineAt(pos);
    const int ui = unitOfLine(line);
    measure(ui);
    const Unit& u = m_units[size_t(ui)];
    const double top = m_heights.top(ui);
    const LineMetrics& normal = m_styles->line(StyleId::Normal);
    if (u.block || !u.layout)
        return QRectF(0, top, 1, normal.ascent + normal.descent);
    const UnitLayout* ul = u.layout.get();
    const int col = int(pos - m_text.lineStart(line));
    const int disp = ul->block.docToDisp(col, assoc);
    int li = visualLineIndex(*ul, disp);
    if (assoc < 0 && li > 0 && disp == ul->lines[li].dispStart)
        --li;
    const QTextLine ql = ul->text->lineAt(std::min(li, ul->text->lineCount() - 1));
    const qreal x = ql.cursorToX(disp);
    const VisualLine& vl = ul->lines[li];
    const LineMetrics& lm = m_styles->line(ul->deco.lineStyle);
    return QRectF(x, top + vl.baseline - lm.ascent, 1, lm.ascent + lm.descent);
}

qsizetype DocLayout::verticalMove(qsizetype pos, qreal goalX, int direction, int assoc)
{
    if (m_units.empty())
        return pos;
    const int line = m_text.lineAt(pos);
    const int ui = unitOfLine(line);
    measure(ui);
    const Unit& u = m_units[size_t(ui)];
    int li = 0;
    int lineCountHere = 1;
    if (!u.block && u.layout) {
        const UnitLayout* ul = u.layout.get();
        const int disp = ul->block.docToDisp(int(pos - m_text.lineStart(line)), assoc);
        li = visualLineIndex(*ul, disp);
        if (assoc < 0 && li > 0 && disp == ul->lines[li].dispStart)
            --li;
        lineCountHere = int(ul->lines.size());
    }
    const int targetVl = li + direction;
    if (!u.block && targetVl >= 0 && targetVl < lineCountHere) {
        const UnitLayout* ul = u.layout.get();
        const QTextLine ql = ul->text->lineAt(targetVl);
        int d = ql.xToCursor(goalX, QTextLine::CursorBetweenCharacters);
        const VisualLine& vl = ul->lines[targetVl];
        d = std::clamp(d, vl.dispStart, vl.dispStart + vl.dispLength);
        if (targetVl + 1 < lineCountHere && d == vl.dispStart + vl.dispLength && vl.dispLength > 0)
            d = std::max(vl.dispStart, d - 1);
        return m_text.lineStart(line) + ul->block.dispToDoc(d, -1);
    }
    // Move to the neighbouring unit.
    int tu = ui + direction;
    // skip hidden text units
    while (tu >= 0 && tu < int(m_units.size()) && !m_units[size_t(tu)].block && (measure(tu), m_units[size_t(tu)].layout && m_units[size_t(tu)].layout->deco.hidden))
        tu += direction;
    if (tu < 0)
        return 0;
    if (tu >= int(m_units.size()))
        return m_text.length();
    measure(tu);
    const Unit& t = m_units[size_t(tu)];
    if (t.block) {
        // Entering a rendered block puts the caret on its first/last source line (it then reveals).
        const int l = direction > 0 ? t.firstLine : t.firstLine + t.lineCount - 1;
        return m_text.lineStart(l);
    }
    const UnitLayout* tl = t.layout.get();
    const int tv = direction > 0 ? 0 : int(tl->lines.size()) - 1;
    const QTextLine ql = tl->text->lineAt(tv);
    int d = ql.xToCursor(goalX, QTextLine::CursorBetweenCharacters);
    const VisualLine& vl = tl->lines[tv];
    d = std::clamp(d, vl.dispStart, vl.dispStart + vl.dispLength);
    if (tv + 1 < tl->lines.size() && d == vl.dispStart + vl.dispLength && vl.dispLength > 0)
        d = std::max(vl.dispStart, d - 1);
    return m_text.lineStart(t.firstLine) + tl->block.dispToDoc(d, -1);
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
        const int ui = unitOfLine(line);
        measure(ui);
        const Unit& u = m_units[size_t(ui)];
        const double top = m_heights.top(ui);
        if (u.block) {
            out.push_back(QRectF(0, top, m_width, m_heights.height(ui)));
            line = u.firstLine + u.lineCount - 1;
            continue;
        }
        const UnitLayout* ul = u.layout.get();
        if (!ul)
            continue;
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

std::optional<DocLayout::WidgetHit> DocLayout::widgetAt(const QPointF& pt)
{
    if (m_units.empty())
        return std::nullopt;
    const int ui = m_heights.indexAt(pt.y());
    measure(ui);
    const Unit& u = m_units[size_t(ui)];
    if (u.block || !u.layout)
        return std::nullopt;
    const UnitLayout* ul = u.layout.get();
    const double top = m_heights.top(ui);
    for (int i = 0; i < ul->block.widgets().size(); ++i) {
        const QRectF r = ul->widgetRects[i].translated(0, top);
        if (r.adjusted(-2, -2, 2, 2).contains(pt)) {
            WidgetHit h;
            h.line = u.firstLine;
            h.widget = ul->block.widgets()[i].widget;
            h.rect = r;
            for (const DisplayBlock::Segment& seg : ul->block.segments()) {
                if (seg.kind == DisplayBlock::Segment::Kind::Widget && seg.dispFrom == ul->block.widgets()[i].dispPos) {
                    h.docFrom = seg.docFrom;
                    h.docTo = seg.docTo;
                }
            }
            return h;
        }
    }
    return std::nullopt;
}

std::optional<DocLayout::BlockHit> DocLayout::blockAt(const QPointF& pt)
{
    if (m_units.empty())
        return std::nullopt;
    const int ui = m_heights.indexAt(pt.y());
    measure(ui);
    const Unit& u = m_units[size_t(ui)];
    if (!u.block)
        return std::nullopt;
    BlockHit h;
    h.fromLine = u.firstLine;
    h.toLine = u.firstLine + u.lineCount;
    h.widget = u.block;
    h.rect = QRectF(0, m_heights.top(ui), m_width, m_heights.height(ui));
    return h;
}

} // namespace oc
