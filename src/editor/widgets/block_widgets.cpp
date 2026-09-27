#include "editor/widgets/block_widgets.h"

#include "editor/live_preview_decorator.h"
#include "editor/widgets/nested_renderer.h"
#include "markdown/doc_source.h"
#include "render/services/math_renderer.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QTextOption>

#include <cmath>

namespace oc {

// ---------------------------------------------------------------------------------------------
// MathBlockWidget
// ---------------------------------------------------------------------------------------------
MathBlockWidget::MathBlockWidget(QString tex, qsizetype docStart, StyleSheetPtr styles)
    : m_tex(std::move(tex))
    , m_docStart(docStart)
    , m_styles(std::move(styles))
{
}

qreal MathBlockWidget::layout(qreal)
{
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    const auto r = MathRenderer::instance().render(m_tex, true, lm.font.pixelSize(), lm.color);
    m_height = std::ceil(r->size.height()) + lm.lineBox * 0.5; // a little breathing room like MathJax's display margin
    return m_height;
}

void MathBlockWidget::paint(QPainter* p, const QRectF& rect)
{
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const auto r = MathRenderer::instance().render(m_tex, true, lm.font.pixelSize(), lm.color, dpr);
    const QPointF tl(rect.left() + std::max<qreal>(0, (rect.width() - r->size.width()) / 2), rect.top() + (rect.height() - r->size.height()) / 2);
    p->drawImage(QRectF(tl, r->size), r->image);
}

quint64 MathBlockWidget::signature() const { return qHashMulti(11, m_tex); }

// ---------------------------------------------------------------------------------------------
// TableWidget
// ---------------------------------------------------------------------------------------------
TableWidget::TableWidget(const md::DocSource* doc, int tableNode, const LivePreviewDecorator* deco, StyleSheetPtr styles)
    : m_styles(std::move(styles))
{
    const md::SyntaxTree& tree = doc->syntax();
    const Text& text = doc->text();
    const md::BlockNode& n = tree.node(tableNode);
    const md::TableInfo info = tree.table(tableNode);
    m_columns = info.columns;
    m_docStart = text.lineStart(n.fromLine);
    quint64 sig = qHashMulti(12, info.columns);
    for (int line = n.fromLine; line < n.toLine; ++line) {
        const md::LineRole role = tree.role(line);
        if (role == md::LineRole::TableDelimiter)
            continue;
        const QList<md::TableCell> cells = tree.tableCells(line, text);
        QList<Cell> row;
        for (int ci = 0; ci < std::max<int>(m_columns, int(cells.size())); ++ci) {
            Cell c;
            c.line = line;
            c.header = role == md::LineRole::TableHeader;
            c.align = ci < info.align.size() && info.align[ci] ? info.align[ci] : Qt::AlignLeft;
            if (ci < cells.size()) {
                c.docCol = cells[ci].from;
                c.docStart = text.lineStart(line) + cells[ci].from;
                const QString cellText = text.lineView(line).mid(cells[ci].from, cells[ci].to - cells[ci].from).toString();
                LineDecoration d;
                d.lineStyle = StyleId::Normal;
                const md::LeafContent content = tree.tableCellContent(line, ci, text);
                if (deco && !content.segments.isEmpty())
                    deco->inlineDecorationsFor(content, tree.inlineTree(content), c.docStart, c.docStart + cellText.size(), c.docStart, d,
                                               StyleId::Normal, true);
                if (c.header)
                    d.marks.push_front({0, int(cellText.size()), StyleId::Strong});
                std::sort(d.replaces.begin(), d.replaces.end(), [](const Replace& a, const Replace& b) { return a.from < b.from; });
                c.block = DisplayBlock::build(cellText, d, *m_styles);
                sig = qHashMulti(sig, cellText, d.signature());
            } else {
                c.block = DisplayBlock::build(QString(), LineDecoration{}, *m_styles);
            }
            row.push_back(std::move(c));
        }
        m_rows.push_back(std::move(row));
    }
    m_signature = sig;
}

void TableWidget::layoutCell(Cell& c, qreal width)
{
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    c.layout = std::make_shared<QTextLayout>(c.block.display(), lm.font);
    QTextOption opt;
    opt.setUseDesignMetrics(true);
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    opt.setAlignment(c.align);
    c.layout->setTextOption(opt);
    c.layout->setFormats(c.block.formats());
    c.layout->beginLayout();
    qreal y = 0;
    const qreal lineH = std::round(lm.font.pixelSize() * 1.3); // --table-line-height
    for (;;) {
        QTextLine l = c.layout->createLine();
        if (!l.isValid())
            break;
        l.setLineWidth(width);
        l.setPosition(QPointF(0, y + (lineH - l.height()) / 2));
        y += lineH;
    }
    c.layout->endLayout();
}

qreal TableWidget::layout(qreal width)
{
    if (width == m_width)
        return m_height;
    m_width = width;
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    const qreal padX = 8, padY = 4;
    // natural widths
    QList<qreal> natural(m_columns, 30.0);
    for (QList<Cell>& row : m_rows) {
        for (int ci = 0; ci < row.size() && ci < m_columns; ++ci) {
            Cell& c = row[ci];
            layoutCell(c, 100000);
            qreal w = 0;
            for (int i = 0; i < c.layout->lineCount(); ++i)
                w = std::max(w, c.layout->lineAt(i).naturalTextWidth());
            natural[ci] = std::max(natural[ci], std::ceil(w) + 2 * padX + 1);
        }
    }
    qreal total = 0;
    for (qreal w : natural)
        total += w;
    QList<qreal> widths = natural;
    if (total > width) {
        const qreal minW = lm.avgCharWidth * 6 + 2 * padX;
        for (qreal& w : widths)
            w = std::max(minW, w * width / total);
    }
    qreal y = 0;
    for (QList<Cell>& row : m_rows) {
        qreal x = 0;
        qreal rowH = 0;
        for (int ci = 0; ci < row.size() && ci < m_columns; ++ci) {
            Cell& c = row[ci];
            layoutCell(c, widths[ci] - 2 * padX);
            const qreal h = c.layout->lineCount() * std::round(lm.font.pixelSize() * 1.3) + 2 * padY;
            rowH = std::max(rowH, h);
            c.rect = QRectF(x, y, widths[ci], 0);
            x += widths[ci];
        }
        for (Cell& c : row)
            c.rect.setHeight(rowH);
        y += rowH;
    }
    m_height = y + lm.lineBox * 0.5; // margin below like a paragraph gap
    return m_height;
}

void TableWidget::paint(QPainter* p, const QRectF& rect)
{
    const ThemePtr t = currentTheme();
    const QColor border = t->color(QStringLiteral("--table-border-color"), t->color(QStringLiteral("--background-modifier-border")));
    const QColor headerBg = t->color(QStringLiteral("--table-header-background"));
    const qreal padX = 8, padY = 4;
    p->save();
    p->translate(rect.topLeft());
    for (const QList<Cell>& row : m_rows) {
        for (const Cell& c : row) {
            if (c.rect.width() <= 0)
                continue;
            if (c.header && headerBg.isValid() && headerBg.alpha() > 0)
                p->fillRect(c.rect, headerBg);
            p->setPen(QPen(border, 1));
            p->drawRect(c.rect.adjusted(0.5, 0.5, -0.5, -0.5));
            p->setPen(m_styles->line(StyleId::Normal).color); // cell text (QTextLayout draws unformatted text with the pen)
            if (c.layout)
                c.layout->draw(p, c.rect.topLeft() + QPointF(padX, padY));
        }
    }
    p->restore();
}

qsizetype TableWidget::docPosAt(const QPointF& local) const
{
    const qreal padX = 8, padY = 4;
    for (const QList<Cell>& row : m_rows) {
        for (const Cell& c : row) {
            if (!c.rect.contains(local) || !c.layout)
                continue;
            const QPointF inCell = local - c.rect.topLeft() - QPointF(padX, padY);
            int li = 0;
            for (int i = 0; i < c.layout->lineCount(); ++i)
                if (inCell.y() >= c.layout->lineAt(i).y())
                    li = i;
            const int disp = c.layout->lineAt(li).xToCursor(inCell.x());
            return c.docStart + c.block.dispToDoc(disp);
        }
    }
    return m_docStart;
}

QString TableWidget::linkAt(const QPointF&) const { return {}; }

// ---------------------------------------------------------------------------------------------
// CalloutWidget
// ---------------------------------------------------------------------------------------------
QString CalloutWidget::colorToken(const QString& type)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("note"), QStringLiteral("--callout-default")},     {QStringLiteral("abstract"), QStringLiteral("--callout-summary")},
        {QStringLiteral("summary"), QStringLiteral("--callout-summary")},  {QStringLiteral("tldr"), QStringLiteral("--callout-summary")},
        {QStringLiteral("info"), QStringLiteral("--callout-info")},        {QStringLiteral("todo"), QStringLiteral("--callout-todo")},
        {QStringLiteral("important"), QStringLiteral("--callout-important")}, {QStringLiteral("tip"), QStringLiteral("--callout-tip")},
        {QStringLiteral("hint"), QStringLiteral("--callout-tip")},         {QStringLiteral("success"), QStringLiteral("--callout-success")},
        {QStringLiteral("check"), QStringLiteral("--callout-success")},    {QStringLiteral("done"), QStringLiteral("--callout-success")},
        {QStringLiteral("question"), QStringLiteral("--callout-question")},{QStringLiteral("help"), QStringLiteral("--callout-question")},
        {QStringLiteral("faq"), QStringLiteral("--callout-question")},     {QStringLiteral("warning"), QStringLiteral("--callout-warning")},
        {QStringLiteral("caution"), QStringLiteral("--callout-warning")},  {QStringLiteral("attention"), QStringLiteral("--callout-warning")},
        {QStringLiteral("failure"), QStringLiteral("--callout-fail")},     {QStringLiteral("fail"), QStringLiteral("--callout-fail")},
        {QStringLiteral("missing"), QStringLiteral("--callout-fail")},     {QStringLiteral("danger"), QStringLiteral("--callout-error")},
        {QStringLiteral("error"), QStringLiteral("--callout-error")},      {QStringLiteral("bug"), QStringLiteral("--callout-bug")},
        {QStringLiteral("example"), QStringLiteral("--callout-example")},  {QStringLiteral("quote"), QStringLiteral("--callout-quote")},
        {QStringLiteral("cite"), QStringLiteral("--callout-quote")},
    };
    return map.value(type, QStringLiteral("--callout-default"));
}

QString CalloutWidget::iconName(const QString& type)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("--callout-default"), QStringLiteral("pencil")}, {QStringLiteral("--callout-summary"), QStringLiteral("clipboard-list")},
        {QStringLiteral("--callout-info"), QStringLiteral("info")},      {QStringLiteral("--callout-todo"), QStringLiteral("circle-check")},
        {QStringLiteral("--callout-important"), QStringLiteral("flame")},{QStringLiteral("--callout-tip"), QStringLiteral("flame")},
        {QStringLiteral("--callout-success"), QStringLiteral("check")},  {QStringLiteral("--callout-question"), QStringLiteral("circle-help")},
        {QStringLiteral("--callout-warning"), QStringLiteral("triangle-alert")}, {QStringLiteral("--callout-fail"), QStringLiteral("x")},
        {QStringLiteral("--callout-error"), QStringLiteral("zap")},      {QStringLiteral("--callout-bug"), QStringLiteral("bug")},
        {QStringLiteral("--callout-example"), QStringLiteral("list")},   {QStringLiteral("--callout-quote"), QStringLiteral("quote")},
    };
    return map.value(colorToken(type), QStringLiteral("pencil"));
}

CalloutWidget::CalloutWidget(const md::DocSource* doc, int calloutNode, const LivePreviewDecorator* deco, StyleSheetPtr styles, int depth)
    : m_doc(doc)
    , m_styles(std::move(styles))
{
    const md::SyntaxTree& tree = doc->syntax();
    const Text& text = doc->text();
    const md::BlockNode& n = tree.node(calloutNode);
    const md::CalloutInfo ci = tree.callout(calloutNode);
    m_type = ci.type;
    m_firstLine = n.fromLine;
    m_foldable = n.flags & md::FlagFoldable;
    m_folded = n.flags & md::FlagFolded;
    m_title = text.lineView(n.fromLine).mid(ci.titleFrom, ci.titleTo - ci.titleFrom).toString().trimmed();
    if (m_title.isEmpty() && !m_type.isEmpty())
        m_title = m_type.left(1).toUpper() + m_type.mid(1);
    // Body: strip this callout's '>' marker from each following line.
    const int depthHere = n.depth;
    QStringList lines;
    for (int line = n.fromLine + 1; line < n.toLine; ++line) {
        int col = 0;
        int quotes = 0;
        for (const md::ContainerMarker& m : tree.containerMarkers(line)) {
            if (m.kind == md::ContainerMarker::Kind::Quote) {
                ++quotes;
                col = m.to;
                // the callout's own marker is the quote at its nesting level
                if (m.node == calloutNode)
                    break;
            }
        }
        Q_UNUSED(quotes);
        Q_UNUSED(depthHere);
        col = std::min(col, text.lineLength(line));
        lines << text.lineView(line).mid(col).toString();
        m_bodyLines.push_back(line);
        m_bodyCols.push_back(col);
    }
    const QString body = lines.join(QLatin1Char('\n'));
    m_signature = qHashMulti(13, m_type, m_title, body, m_folded);
    if (!body.trimmed().isEmpty() && depth < 4)
        m_body = std::make_unique<NestedRenderer>(Text::fromString(body), m_styles, depth + 1, deco);
}

CalloutWidget::~CalloutWidget() = default;

qreal CalloutWidget::layout(qreal width)
{
    if (width == m_width)
        return m_height;
    m_width = width;
    const ThemePtr t = currentTheme();
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    const qreal padTop = 12, padRight = 12, padBottom = 12, padLeft = 24; // --callout-padding
    m_headerH = lm.lineBox;
    qreal h = padTop + m_headerH;
    if (m_body && !m_folded) {
        const qreal bw = std::max<qreal>(40, width - padLeft - padRight);
        const qreal bh = m_body->layout(bw);
        m_bodyRect = QRectF(padLeft, h + 4, bw, bh);
        h += 4 + bh;
    }
    h += padBottom;
    m_height = h + lm.lineBox * 0.5; // margin 1em 0 (approx.)
    return m_height;
}

void CalloutWidget::paint(QPainter* p, const QRectF& rect)
{
    const ThemePtr t = currentTheme();
    const QColor color = t->color(colorToken(m_type), t->color(QStringLiteral("--color-blue")));
    const LineMetrics& lm = m_styles->line(StyleId::Normal);
    const QRectF box(rect.left(), rect.top(), rect.width(), m_height - lm.lineBox * 0.5);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QColor bg = color;
    bg.setAlphaF(0.1f);
    QPainterPath path;
    path.addRoundedRect(box, 4, 4);
    p->fillPath(path, bg);
    // header: icon + bold title (+ fold chevron)
    const qreal padTop = 12, padLeft = 24;
    const QRectF header(box.left() + padLeft, box.top() + padTop, box.width() - padLeft - 12, m_headerH);
    const QPixmap icon = IconProvider::instance().pixmap(iconName(m_type), 16, color, 1.75, p->device() ? p->device()->devicePixelRatioF() : 1.0);
    p->drawPixmap(QPointF(header.left(), header.center().y() - 8), icon);
    QFont tf = lm.font;
    tf.setWeight(QFont::Bold);
    p->setFont(tf);
    p->setPen(color);
    p->drawText(header.adjusted(24, 0, -24, 0), Qt::AlignLeft | Qt::AlignVCenter, m_title);
    if (m_foldable) {
        const QPixmap chev = IconProvider::instance().pixmap(m_folded ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"), 16, color);
        const qreal tw = QFontMetricsF(tf).horizontalAdvance(m_title);
        p->drawPixmap(QPointF(header.left() + 24 + tw + 6, header.center().y() - 8), chev);
    }
    p->restore();
    if (m_body && !m_folded)
        m_body->paint(p, rect.topLeft() + m_bodyRect.topLeft(), rect);
}

qsizetype CalloutWidget::docPosAt(const QPointF& local) const
{
    if (m_body && !m_folded && m_bodyRect.contains(local)) {
        const qsizetype nested = m_body->hitTest(local - m_bodyRect.topLeft());
        const Text& nt = m_body->text();
        const int nl = nt.lineAt(nested);
        const int col = int(nested - nt.lineStart(nl));
        if (nl < m_bodyLines.size())
            return m_doc->text().lineStart(m_bodyLines[nl]) + m_bodyCols[nl] + col;
    }
    return m_doc->text().lineStart(m_firstLine);
}

bool CalloutWidget::click(const QPointF& local, Qt::KeyboardModifiers)
{
    if (m_foldable && local.y() < 12 + m_headerH + 4) {
        m_folded = !m_folded;
        m_width = -1; // relayout
        return true;
    }
    return false;
}

QString CalloutWidget::linkAt(const QPointF& local) const
{
    if (m_body && !m_folded && m_bodyRect.contains(local))
        return m_body->linkAt(local - m_bodyRect.topLeft());
    return {};
}

} // namespace oc
