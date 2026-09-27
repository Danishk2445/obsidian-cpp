// EditorView: folding (headings, indented blocks, frontmatter), fold indicators and placeholders.
#include "editor/editor_view.h"

#include "render/theme/theme.h"
#include "render/widgets/inline_widgets.h"

#include <QPainter>
#include <QPainterPath>

namespace oc {

namespace {
// The "…" shown after a folded line (Obsidian's .cm-foldPlaceholder: faint, 8px left margin).
class FoldPlaceholderWidget : public InlineWidget {
public:
    QSizeF size(const LineMetrics& line) const override
    {
        const QFontMetricsF fm(line.font);
        return {8 + fm.horizontalAdvance(QStringLiteral("…")) + 2, line.ascent + line.descent};
    }
    qreal ascent(const LineMetrics& line) const override { return line.ascent; }
    void paint(QPainter* p, const QRectF& rect, const LineMetrics& line) const override
    {
        p->save();
        p->setFont(line.font);
        p->setPen(currentTheme()->color(QStringLiteral("--text-faint")));
        p->drawText(QPointF(rect.left() + 8, rect.top() + line.ascent), QStringLiteral("…"));
        p->restore();
    }
    quint64 signature() const override { return 0x70'1d'e1'1c; }
    QString accessibleText() const override { return {}; }
};

const std::shared_ptr<InlineWidget>& placeholder()
{
    static const std::shared_ptr<InlineWidget> w = std::make_shared<FoldPlaceholderWidget>();
    return w;
}
} // namespace

bool isFoldPlaceholder(const InlineWidget* w) { return dynamic_cast<const FoldPlaceholderWidget*>(w) != nullptr; }

void EditorView::setFoldOptions(const FoldOptions& options)
{
    m_foldOptions = options;
    viewport()->update();
}

std::optional<FoldRange> EditorView::foldableAt(int line) const
{
    if (!m_doc)
        return std::nullopt;
    const Text& t = m_doc->text();
    if (line < 0 || line >= t.lineCount())
        return std::nullopt;
    m_doc->ensureParsedThrough(std::min(t.lineCount() - 1, line + 1));
    return oc::foldableAt(t, m_doc->syntax(), line, m_foldOptions);
}

bool EditorView::isLineFoldable(int line) const
{
    if (!m_doc)
        return false;
    return oc::isFoldable(m_doc->text(), m_doc->syntax(), line, m_foldOptions);
}

bool EditorView::isFoldedAt(int line) const
{
    auto it = std::lower_bound(m_folds.cbegin(), m_folds.cend(), line, [](const FoldRange& f, int l) { return f.line < l; });
    return it != m_folds.cend() && it->line == line;
}

void EditorView::setVisibleLineRange(int from, int to)
{
    from = std::max(0, from);
    if (from == m_clipFrom && to == m_clipTo)
        return;
    m_clipFrom = from;
    m_clipTo = to;
    applyFolds(false);
}

double EditorView::documentHeight() const { return m_marginTop + m_layout.totalHeight() + m_footerH + m_marginBottom; }

void EditorView::applyFolds(bool emitChange)
{
    QList<QPair<int, int>> hidden;
    hidden.reserve(m_folds.size());
    for (const FoldRange& f : std::as_const(m_folds))
        hidden.append({f.line + 1, f.lastLine + 1});
    // setVisibleLineRange(): everything outside the range is hidden too
    const int lines = m_layout.lineCount();
    if (m_clipFrom > 0)
        hidden.append({0, std::min(m_clipFrom, lines)});
    if (m_clipTo >= 0 && m_clipTo + 1 < lines)
        hidden.append({m_clipTo + 1, lines});
    m_layout.setHiddenLines(hidden);
    // Fold start lines get (or lose) their placeholder.
    updateScrollbar();
    ensureVisibleLayout();
    viewport()->update();
    if (emitChange)
        emit foldsChanged();
}

bool EditorView::addFold(const FoldRange& r)
{
    if (r.lastLine <= r.line || isFoldedAt(r.line))
        return false;
    auto it = std::lower_bound(m_folds.begin(), m_folds.end(), r.line, [](const FoldRange& f, int l) { return f.line < l; });
    m_folds.insert(it, r);
    return true;
}

bool EditorView::foldLine(int line)
{
    const std::optional<FoldRange> r = foldableAt(line);
    if (!r || !addFold(*r))
        return false;
    m_layout.invalidateLines(line, line + 1); // placeholder
    applyFolds();
    // Cursors inside the folded region move to the end of its first line.
    setSelection(skipFolds(m_sel, m_sel), false, false);
    return true;
}

bool EditorView::unfoldLine(int line)
{
    auto it = std::lower_bound(m_folds.begin(), m_folds.end(), line, [](const FoldRange& f, int l) { return f.line < l; });
    if (it == m_folds.end() || it->line != line)
        return false;
    m_folds.erase(it);
    m_layout.invalidateLines(line, line + 1);
    applyFolds();
    return true;
}

bool EditorView::toggleFoldLine(int line) { return isFoldedAt(line) ? unfoldLine(line) : foldLine(line); }

void EditorView::setFolds(const QList<FoldRange>& folds)
{
    const int n = m_doc ? m_doc->text().lineCount() : 0;
    QList<FoldRange> valid;
    for (const FoldRange& f : folds)
        if (f.line >= 0 && f.lastLine > f.line && f.lastLine < n)
            valid.append(f);
    std::sort(valid.begin(), valid.end(), [](const FoldRange& a, const FoldRange& b) { return a.line < b.line; });
    valid.erase(std::unique(valid.begin(), valid.end(), [](const FoldRange& a, const FoldRange& b) { return a.line == b.line; }),
                valid.end());
    QList<int> touched;
    for (const FoldRange& f : std::as_const(m_folds))
        touched << f.line;
    for (const FoldRange& f : std::as_const(valid))
        touched << f.line;
    m_folds = valid;
    for (int l : std::as_const(touched))
        m_layout.invalidateLines(l, l + 1);
    applyFolds(false);
    setSelection(skipFolds(m_sel, m_sel), false, false);
}

bool EditorView::foldAtSelection()
{
    if (!m_doc)
        return false;
    const Text& t = m_doc->text();
    QList<int> lines;
    for (const SelRange& r : m_sel.ranges()) {
        const int l = t.lineAt(r.head);
        if (!lines.contains(l))
            lines << l;
    }
    for (int l : std::as_const(lines))
        if (!isFoldedAt(l) && foldLine(l))
            return true;
    return false;
}

bool EditorView::unfoldAround(qsizetype from, qsizetype to)
{
    if (!m_doc || m_folds.isEmpty())
        return false;
    const Text& t = m_doc->text();
    const int a = t.lineAt(std::clamp<qsizetype>(std::min(from, to), 0, t.length()));
    const int b = t.lineAt(std::clamp<qsizetype>(std::max(from, to), 0, t.length()));
    QList<int> starts;
    for (const FoldRange& f : std::as_const(m_folds))
        if (f.line < b && f.lastLine >= a && f.lastLine > f.line && std::max(a, f.line + 1) <= std::min(b, f.lastLine))
            starts << f.line; // hides some of lines a..b
    bool any = false;
    for (int line : std::as_const(starts))
        any |= unfoldLine(line);
    return any;
}

bool EditorView::unfoldAtSelection()
{
    if (!m_doc)
        return false;
    const Text& t = m_doc->text();
    bool any = false;
    for (const SelRange& r : m_sel.ranges())
        any |= unfoldLine(t.lineAt(r.head));
    return any;
}

void EditorView::foldAll()
{
    if (!m_doc)
        return;
    const Text& t = m_doc->text();
    m_doc->ensureParsedThrough(t.lineCount() - 1);
    bool any = false;
    for (int l = 0; l < t.lineCount(); ++l) {
        if (!isLineFoldable(l))
            continue;
        if (auto r = oc::foldableAt(t, m_doc->syntax(), l, m_foldOptions)) {
            any |= addFold(*r);
            m_layout.invalidateLines(l, l + 1);
        }
    }
    if (any) {
        applyFolds();
        setSelection(skipFolds(m_sel, m_sel), false, false);
    }
}

void EditorView::unfoldAll()
{
    if (m_folds.isEmpty())
        return;
    const QList<FoldRange> old = std::exchange(m_folds, {});
    for (const FoldRange& f : old)
        m_layout.invalidateLines(f.line, f.line + 1);
    applyFolds();
}

namespace {
// Foldable regions intersecting [fromLine, toLine]: candidates are the lines in range plus the lines
// above that may enclose it (headings of lower level, less indented lines), in document order.
QList<FoldRange> foldablesAround(const Text& t, const md::SyntaxTree& tree, int fromLine, int toLine, const FoldOptions& o)
{
    QList<int> candidates;
    for (int l = fromLine; l <= toLine; ++l)
        candidates << l;
    int headLevel = 100;
    int indent = -1;
    for (int l = fromLine; l >= 0 && l > fromLine - 5000; --l) {
        const int lvl = foldHeadingLevel(t, tree, l);
        if (lvl < headLevel) {
            headLevel = lvl;
            if (l < fromLine)
                candidates.prepend(l);
            continue;
        }
        const int ind = indentColumns(t.lineView(l), o.tabSize);
        if (ind >= 0 && (indent < 0 || ind < indent)) {
            indent = ind;
            if (l < fromLine && !candidates.contains(l))
                candidates.prepend(l);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    QList<FoldRange> out;
    for (int l : std::as_const(candidates)) {
        if (auto r = foldableAt(t, tree, l, o); r && r->line <= toLine && r->lastLine >= fromLine)
            out.append(*r);
    }
    return out;
}
} // namespace

bool EditorView::foldMore()
{
    if (!m_doc)
        return false;
    const Text& t = m_doc->text();
    bool any = false;
    for (const SelRange& r : m_sel.ranges()) {
        const QList<FoldRange> around = foldablesAround(t, m_doc->syntax(), t.lineAt(r.from()), t.lineAt(r.to()), m_foldOptions);
        for (qsizetype i = around.size() - 1; i >= 0; --i) {
            if (!isFoldedAt(around[i].line)) {
                any |= addFold(around[i]);
                m_layout.invalidateLines(around[i].line, around[i].line + 1);
                break;
            }
        }
    }
    if (any) {
        applyFolds();
        setSelection(skipFolds(m_sel, m_sel), false, false);
    }
    return any;
}

bool EditorView::foldLess()
{
    if (!m_doc)
        return false;
    const Text& t = m_doc->text();
    bool any = false;
    for (const SelRange& r : m_sel.ranges()) {
        const QList<FoldRange> around = foldablesAround(t, m_doc->syntax(), t.lineAt(r.from()), t.lineAt(r.to()), m_foldOptions);
        for (const FoldRange& f : around) {
            if (isFoldedAt(f.line)) {
                any |= unfoldLine(f.line);
                break;
            }
        }
    }
    return any;
}

void EditorView::mapFolds(const DocUpdate& u)
{
    if (m_folds.isEmpty())
        return;
    const bool userEdit = u.tr.userEvent != u"external";
    const ChangeDesc desc = u.tr.changes.desc();
    QList<FoldRange> out;
    for (const FoldRange& f : std::as_const(m_folds)) {
        if (f.lastLine >= u.before.lineCount())
            continue;
        const qsizetype a = u.before.lineEnd(f.line), b = u.before.lineEnd(f.lastLine);
        bool touched = false;
        if (userEdit) {
            u.tr.changes.iterChangedRanges([&](qsizetype fromA, qsizetype toA, qsizetype, qsizetype) {
                if (fromA <= b && toA >= a)
                    touched = true;
            });
        }
        if (touched) {
            // editing inside (or at the edge of) a fold unfolds it, like Obsidian; drop its "…"
            const int sl = u.after.lineAt(desc.mapPos(a, -1));
            m_layout.invalidateLines(sl, sl + 1);
            continue;
        }
        const int nl = u.after.lineAt(desc.mapPos(a, -1));
        const int nlast = u.after.lineAt(desc.mapPos(b, -1));
        if (nlast > nl)
            out.append(FoldRange{nl, nlast});
    }
    std::sort(out.begin(), out.end(), [](const FoldRange& x, const FoldRange& y) { return x.line < y.line; });
    out.erase(std::unique(out.begin(), out.end(), [](const FoldRange& x, const FoldRange& y) { return x.line == y.line; }), out.end());
    if (out != m_folds) {
        m_folds = out;
        applyFolds();
    } else {
        applyFolds(false);
    }
}

Selection EditorView::skipFolds(const Selection& sel, const Selection& old) const
{
    if (m_folds.isEmpty() || !m_doc)
        return sel;
    const Text& t = m_doc->text();
    const qsizetype oldHead = old.main().head;
    QList<SelRange> out;
    bool changed = false;
    for (const SelRange& r : sel.ranges()) {
        qsizetype head = r.head;
        for (int guard = 0; guard < 64; ++guard) {
            const int line = t.lineAt(head);
            if (!m_layout.isHidden(line))
                break;
            // the innermost... outermost fold hiding this line
            const FoldRange* hit = nullptr;
            for (const FoldRange& f : m_folds)
                if (f.line < line && f.lastLine >= line && (!hit || f.line < hit->line))
                    hit = &f;
            if (!hit)
                break;
            const bool forward = head > oldHead;
            if (forward && hit->lastLine + 1 < t.lineCount())
                head = t.lineStart(hit->lastLine + 1);
            else
                head = t.lineEnd(hit->line);
        }
        if (head != r.head) {
            changed = true;
            SelRange nr = r.empty() ? SelRange::cursor(head) : SelRange(r.anchor, head);
            out.append(nr);
        } else {
            out.append(r);
        }
    }
    return changed ? Selection::create(out, sel.mainIndex()) : sel;
}

QRectF EditorView::foldIndicatorRect(int line)
{
    if (!m_doc || m_layout.isHidden(line) || m_layout.isInBlock(line))
        return {};
    const Text& t = m_doc->text();
    if (line < 0 || line >= t.lineCount())
        return {};
    const QStringView v = t.lineView(line);
    int col = 0;
    while (col < v.size() && (v[col] == u' ' || v[col] == u'\t'))
        ++col;
    const QRectF cr = m_layout.caretRect(t.lineStart(line) + col, -1); // (before a list marker's padding)
    // Obsidian: a 10px chevron whose right edge sits 5px left of the line's first character.
    const qreal right = cr.x() - 5;
    const QRectF doc(right - 10, cr.y(), 10, cr.height());
    return QRectF(docToViewport(doc.topLeft()), doc.size());
}

int EditorView::foldLineAtIndicator(const QPoint& vp)
{
    if (!m_doc || !(m_foldOptions.headings || m_foldOptions.indent))
        return -1;
    const QPointF docPt = viewportToDoc(vp);
    const int line = m_layout.lineAtY(docPt.y());
    if (!isFoldedAt(line) && !isLineFoldable(line))
        return -1;
    const QRectF r = foldIndicatorRect(line);
    if (r.isEmpty())
        return -1;
    // a comfortable hit area: the indicator plus its padding, over the whole line box
    const QRectF hit(r.left() - 4, r.top(), r.width() + 4 + 5, r.height());
    if (!hit.contains(vp))
        return -1;
    // a task's checkbox is above it (Reading view: the positioned input paints over the list's collapse indicator)
    if (const auto w = m_layout.widgetAt(docPt); w && dynamic_cast<const CheckboxWidget*>(w->widget.get()))
        return -1;
    return line;
}

void EditorView::setHoverLine(int line)
{
    if (line == m_hoverLine)
        return;
    m_hoverLine = line;
    viewport()->update();
}

void EditorView::paintFoldIndicators(QPainter& p)
{
    if (!m_doc || m_mode == Mode::Reading)
        return;
    const double docTop = scrollTop();
    const int first = m_layout.lineAtY(std::max(0.0, docTop));
    const int last = m_layout.lineAtY(docTop + viewport()->height());
    const ThemePtr th = currentTheme();
    const QColor normal = th->color(QStringLiteral("--collapse-icon-color"), th->color(QStringLiteral("--text-faint")));
    const QColor collapsed = th->color(QStringLiteral("--collapse-icon-color-collapsed"), th->color(QStringLiteral("--text-accent")));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int line = first; line <= last && line < m_doc->text().lineCount(); ++line) {
        if (m_layout.isHidden(line)) {
            line = m_layout.hiddenRunEnd(line) - 1;
            continue;
        }
        const bool folded = isFoldedAt(line);
        if (!folded && line != m_hoverLine)
            continue;
        if (!folded && !isLineFoldable(line))
            continue;
        const QRectF r = foldIndicatorRect(line);
        if (r.isEmpty())
            continue;
        // right-triangle: "M3 8 L12 17 L21 8" in a 24 box, 10px, stroke 4 (box units); rotated -90° when folded
        const QPointF c(r.center().x(), r.center().y());
        const qreal k = 10.0 / 24.0;
        QPainterPath path;
        path.moveTo(-9 * k, -4 * k);
        path.lineTo(0, 5 * k);
        path.lineTo(9 * k, -4 * k);
        QTransform tr;
        tr.translate(c.x(), c.y());
        if (folded)
            tr.rotate(-90);
        QPen pen(folded ? collapsed : normal, 4 * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(tr.map(path));
    }
    p.restore();
}

void EditorView::applyFoldPlaceholder(int line, LineDecoration& d) const
{
    if (!isFoldedAt(line) || !m_doc)
        return;
    Replace r;
    r.from = r.to = m_doc->text().lineLength(line);
    r.kind = Replace::Kind::Widget;
    r.widget = placeholder();
    d.replaces.push_back(r);
    std::stable_sort(d.replaces.begin(), d.replaces.end(), [](const Replace& a, const Replace& b) { return a.from < b.from; });
}

} // namespace oc
