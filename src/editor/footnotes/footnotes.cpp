#include "editor/footnotes/footnotes.h"

#include "editor/live_preview_decorator.h"
#include "markdown/doc_source.h"

#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace oc::footnotes {

namespace {
using md::InlineType;
using md::NodeType;

// "↩︎": U+21A9 with the text presentation selector (Obsidian's back-link text).
const QString kBackref = QStringLiteral("↩︎");

bool isHeadingOrParagraph(NodeType t) { return t == NodeType::Paragraph || t == NodeType::AtxHeading || t == NodeType::SetextHeading; }
} // namespace

// ---------------------------------------------------------------------------------------------
// Index
// ---------------------------------------------------------------------------------------------
std::shared_ptr<const Index> Index::build(const md::SyntaxTree& tree, const Text& text)
{
    auto idx = std::make_shared<Index>();
    // Definitions (a later one with the same label replaces an earlier one, like remark's transformer).
    const int nodes = tree.nodeCount();
    for (int id = 1; id < nodes; ++id) {
        const md::BlockNode& n = tree.node(id);
        if (n.type == NodeType::FootnoteDef)
            idx->m_defined.insert(tree.footnoteDef(id).label.toLower(), {n.fromLine, n.toLine});
    }
    const bool anyDefinition = !idx->m_defined.isEmpty();

    // References in document order: only lines that can hold one are parsed.
    struct Found {
        qsizetype from = 0;
        bool isInline = false;
        QString id;
        qsizetype contentFrom = 0, contentTo = 0;
    };
    QList<Found> found;
    QSet<int> seenLeaves;
    auto collect = [&](const md::LeafContent& content) {
        const md::InlineTreePtr it = tree.inlineTree(content);
        if (!it)
            return;
        for (int id = 0; id < it->size(); ++id) {
            const md::InlineNode& n = it->node(id);
            if (n.type == InlineType::FootnoteRef) {
                found.push_back(Found{content.toDoc(n.from, text), false, it->footnote(id).label.toLower(), 0, 0});
            } else if (n.type == InlineType::InlineFootnote) {
                // children: the "^[" mark first, the "]" mark last
                int open = -1, close = -1;
                for (int c = n.firstChild; c >= 0; c = it->node(c).next) {
                    if (it->node(c).type != InlineType::Mark)
                        continue;
                    if (open < 0)
                        open = c;
                    close = c;
                }
                const int cFrom = open >= 0 ? it->node(open).to : n.from + 2;
                const int cTo = close >= 0 && close != open ? it->node(close).from : n.to - 1;
                found.push_back(Found{content.toDoc(n.from, text), true, {}, content.toDoc(cFrom, text), content.toDoc(std::max(cFrom, cTo), text)});
            }
        }
    };
    const int lines = std::min(text.lineCount(), tree.lineCount());
    for (int line = 0; line < lines; ++line) {
        const QStringView v = text.lineView(line);
        if (!v.contains(u"^[") && !(anyDefinition && v.contains(u"[^")))
            continue;
        const int leaf = tree.leafAt(line);
        if (leaf < 0)
            continue;
        const NodeType type = tree.node(leaf).type;
        if (type == NodeType::Table) {
            const qsizetype cells = tree.tableCells(line, text).size();
            for (int ci = 0; ci < cells; ++ci)
                collect(tree.tableCellContent(line, ci, text));
        } else if (isHeadingOrParagraph(type) && !seenLeaves.contains(leaf)) {
            seenLeaves.insert(leaf);
            collect(tree.leafContent(leaf, text));
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.from < b.from; });

    for (const Found& f : std::as_const(found)) {
        if (f.isInline) {
            Note n;
            n.isInline = true;
            n.inlineFrom = f.contentFrom;
            n.inlineTo = f.contentTo;
            n.inlineText = text.slice(f.contentFrom, f.contentTo);
            n.refs.push_back(f.from);
            idx->m_refs.insert(f.from, Ref{int(idx->m_notes.size()), 0});
            idx->m_notes.push_back(std::move(n));
            continue;
        }
        const auto def = idx->m_defined.constFind(f.id);
        if (def == idx->m_defined.cend()) {
            idx->m_refs.insert(f.from, Ref{-1, 0});
            continue;
        }
        int note = idx->m_byId.value(f.id, -1);
        if (note < 0) {
            note = int(idx->m_notes.size());
            Note n;
            n.id = f.id;
            n.defFromLine = def->first;
            n.defToLine = def->second;
            idx->m_notes.push_back(std::move(n));
            idx->m_byId.insert(f.id, note);
        }
        Note& n = idx->m_notes[note];
        idx->m_refs.insert(f.from, Ref{note, int(n.refs.size())});
        n.refs.push_back(f.from);
    }
    return idx;
}

std::optional<Index::Ref> Index::refAt(qsizetype docFrom) const
{
    const auto it = m_refs.constFind(docFrom);
    if (it == m_refs.cend())
        return std::nullopt;
    return *it;
}

int Index::noteForInlineText(const QString& text) const
{
    for (qsizetype i = 0; i < m_notes.size(); ++i)
        if (m_notes[i].isInline && m_notes[i].inlineText == text)
            return int(i);
    return -1;
}

QString Index::refLabel(int note, int occurrence)
{
    QString s = QString::number(note + 1);
    if (occurrence > 0)
        s += QLatin1Char('-') + QString::number(occurrence);
    return s;
}

std::optional<qsizetype> Index::refPosition(const QString& anchor) const
{
    if (!anchor.startsWith(QLatin1String("fnref-")))
        return std::nullopt;
    const QStringList parts = anchor.mid(6).split(QLatin1Char('-'));
    bool ok = false;
    const int note = parts.value(0).toInt(&ok) - 1;
    if (!ok || note < 0 || note >= m_notes.size())
        return std::nullopt;
    int occurrence = 0;
    if (parts.size() > 1) {
        occurrence = parts[1].toInt(&ok);
        if (!ok)
            return std::nullopt;
    }
    const QList<qsizetype>& refs = m_notes[note].refs;
    if (occurrence < 0 || occurrence >= refs.size())
        return std::nullopt;
    return refs[occurrence];
}

bool isDefinitionLine(const md::SyntaxTree& tree, const Text& text, int line)
{
    // A definition's text starts after its "[^1]: " marker / continuation indent: most lines (top-level paragraphs,
    // content column 0) are ruled out without looking at them.
    if (tree.role(line) != md::LineRole::Paragraph || tree.contentColumn(line) == 0 || line >= text.lineCount())
        return false;
    // Cheap test first: a definition's first line starts with "[^", its continuation lines are indented.
    const QStringView v = text.lineView(line);
    qsizetype i = 0;
    while (i < v.size() && (v[i] == u' ' || v[i] == u'\t'))
        ++i;
    if (i == 0 && !v.startsWith(u"[^"))
        return false;
    const int leaf = tree.leafAt(line);
    const int parent = leaf >= 0 ? tree.parent(leaf) : -1;
    return parent > 0 && tree.node(parent).type == NodeType::FootnoteDef;
}

bool inDefinition(const md::SyntaxTree& tree, int line)
{
    for (int id = tree.deepestAt(line); id > 0; id = tree.parent(id))
        if (tree.node(id).type == NodeType::FootnoteDef)
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// SectionWidget
// ---------------------------------------------------------------------------------------------
namespace {
// Whether back-links can follow the text of this line (a paragraph line) or go on their own line.
bool paragraphLike(QStringView line)
{
    const QStringView t = line.trimmed();
    if (t.isEmpty())
        return false;
    static const QStringList starts = {QStringLiteral("```"), QStringLiteral("~~~"), QStringLiteral("$$"), QStringLiteral("|"),
                                       QStringLiteral("<"),   QStringLiteral(">"),   QStringLiteral("%%")};
    for (const QString& s : starts)
        if (t.startsWith(s))
            return false;
    return true;
}
} // namespace

SectionWidget::SectionWidget(std::shared_ptr<const Index> index, const md::SyntaxTree& tree, const Text& text, StyleSheetPtr styles,
                             const LivePreviewDecorator* parent, qreal topGap)
    : m_styles(std::move(styles))
    , m_index(std::move(index))
    , m_topGap(topGap)
{
    // --footnote-size relative to the note's text size
    const qreal base = m_styles->textSize();
    const qreal footnotePx = m_styles->line(StyleId::FootnoteLine).font.pixelSize();
    if (base > 0 && footnotePx > 0)
        m_scale = footnotePx / base;

    // The list as Markdown: "N. content ↩︎", continuation lines indented under the item.
    QString md;
    int line = 0;
    const QList<Note>& notes = m_index->notes();
    for (int k = 0; k < notes.size(); ++k) {
        const Note& n = notes[k];
        QStringList content;
        if (n.isInline) {
            content = n.inlineText.split(QLatin1Char('\n'));
        } else {
            for (int l = n.defFromLine; l < n.defToLine && l < text.lineCount(); ++l) {
                const QStringView v = text.lineView(l);
                content << v.mid(std::min<qsizetype>(tree.contentColumn(l), v.size())).toString();
            }
        }
        for (QString& s : content)
            if (s.trimmed().isEmpty())
                s.clear();
        while (!content.isEmpty() && content.last().isEmpty())
            content.removeLast();
        if (content.isEmpty())
            content << QString();
        const QString prefix = QString::number(k + 1) + QStringLiteral(". ");
        const QString indent(prefix.size(), QLatin1Char(' '));
        if (!md.isEmpty())
            md += QLatin1Char('\n');
        Item item;
        item.firstLine = line;
        for (qsizetype i = 0; i < content.size(); ++i) {
            if (i > 0) {
                md += QLatin1Char('\n');
                ++line;
            }
            md += (i == 0 ? prefix : content[i].isEmpty() ? QString() : indent) + content[i];
        }
        // one back-link per reference
        if (!paragraphLike(content.last())) {
            md += QLatin1Char('\n') + indent;
            ++line;
        } else {
            md += QLatin1Char(' ');
        }
        for (int r = 0; r < n.refs.size(); ++r) {
            if (r > 0)
                md += QLatin1Char(' ');
            item.backrefs.push_back({md.size(), md.size() + kBackref.size()});
            item.anchors.push_back(QStringLiteral("fnref-") + Index::refLabel(k, r));
            md += kBackref;
        }
        item.lastLine = line;
        ++line;
        m_items.push_back(std::move(item));
    }
    m_signature = qHashMulti(0x666e, md, m_topGap);

    m_source = std::make_unique<md::StaticDocSource>(Text::fromString(md));
    m_decorator = std::make_unique<LivePreviewDecorator>(m_source.get());
    m_decorator->setStyleSheet(m_styles.get());
    m_decorator->setReading(true);
    m_decorator->setNestingDepth(1);
    m_decorator->setSharedFootnotes(m_index);
    if (parent) {
        m_decorator->copyRenderOptions(*parent);
        m_decorator->isResolved = parent->isResolved;
        m_decorator->resolveAttachment = parent->resolveAttachment;
    }
    // back-links: .footnote-backref { color: var(--text-faint) }
    QHash<int, QList<QPair<int, int>>> backrefColumns;
    const Text& lt = m_source->text();
    for (const Item& it : std::as_const(m_items))
        for (const auto& [from, to] : it.backrefs) {
            const int l = lt.lineAt(from);
            backrefColumns[l].push_back({int(from - lt.lineStart(l)), int(to - lt.lineStart(l))});
        }
    m_decorator->extra = [backrefColumns](int l, LineDecoration& d) {
        for (const auto& [from, to] : backrefColumns.value(l))
            d.marks.push_back(Mark{from, to, StyleId::FootnoteBackref});
    };
    m_layout.setDecorator(m_decorator.get());
    m_layout.setStyleSheet(m_styles);
    m_layout.setText(lt);
}

SectionWidget::~SectionWidget() = default;

const Text& SectionWidget::listText() const { return m_source->text(); }

qreal SectionWidget::layout(qreal width)
{
    if (width == m_width)
        return m_height;
    m_width = width;
    const qreal listW = std::max<qreal>(40, width / m_scale);
    m_decorator->setContentWidth(listW);
    m_layout.setContentWidth(listW);
    m_layout.ensureLaidOut(0, 1e12);
    // hr { margin: 2rem 0 } (its bottom margin collapses with the list's --p-spacing)
    m_listTop = m_topGap + m_styles->hrThickness() + 32;
    m_height = std::ceil(m_listTop + m_layout.totalHeight() * m_scale);
    return m_height;
}

void SectionWidget::paint(QPainter* p, const QRectF& rect)
{
    p->fillRect(QRectF(rect.left(), rect.top() + m_topGap, rect.width(), m_styles->hrThickness()), m_styles->hrColor());
    // only the visible part of a long list (the layout clips the trailer to the viewport)
    QRectF visible = rect;
    if (p->hasClipping())
        visible = visible.intersected(p->clipBoundingRect());
    p->save();
    p->translate(rect.left(), rect.top() + m_listTop);
    p->scale(m_scale, m_scale);
    DocLayout::PaintContext ctx;
    ctx.drawSelection = false;
    const qreal y0 = (visible.top() - rect.top() - m_listTop) / m_scale, y1 = (visible.bottom() - rect.top() - m_listTop) / m_scale;
    ctx.clip = QRectF(0, std::max<qreal>(0, y0 - 1), m_layout.contentWidth(), std::max<qreal>(0, y1 - y0) + 2);
    ctx.rightEdge = m_layout.contentWidth();
    m_layout.paint(p, ctx);
    p->restore();
}

QPointF SectionWidget::toList(const QPointF& local) const { return QPointF(local.x(), local.y() - m_listTop) / m_scale; }

QString SectionWidget::linkAt(const QPointF& local) const
{
    if (local.y() < m_listTop)
        return {};
    const QPointF pt = toList(local);
    const qsizetype pos = m_layout.hitTest(pt);
    // only when the point is on the text's box, not beside it
    auto over = [&](qsizetype from, qsizetype to) {
        const QRectF a = m_layout.caretRect(from), b = m_layout.caretRect(to);
        if (!qFuzzyCompare(a.top() + 1, b.top() + 1))
            return pt.y() >= a.top() && pt.y() <= b.bottom();
        return pt.x() >= a.left() - 1 && pt.x() <= b.left() + 1 && pt.y() >= a.top() - 2 && pt.y() <= a.bottom() + 2;
    };
    for (const Item& it : m_items)
        for (qsizetype i = 0; i < it.backrefs.size(); ++i) {
            const auto& [from, to] = it.backrefs[i];
            if (pos >= from && pos <= to && over(from, to))
                return QLatin1Char('#') + it.anchors[i];
        }
    const auto hit = m_decorator->linkAt(pos);
    if (!hit || !over(hit->from, hit->to))
        return {};
    if (!hit->anchor.isEmpty())
        return QLatin1Char('#') + hit->anchor;
    return hit->target;
}

qreal SectionWidget::anchorY(const QString& anchor) const
{
    if (!anchor.startsWith(QLatin1String("fn-")))
        return -1;
    bool ok = false;
    const int k = anchor.mid(3).toInt(&ok) - 1;
    if (!ok || k < 0 || k >= m_items.size())
        return -1;
    return m_listTop + m_layout.lineTop(m_items[k].firstLine) * m_scale;
}

QList<qreal> SectionWidget::breakPoints() const
{
    QList<qreal> out{m_listTop};
    const Text& t = m_source->text();
    for (int l = 0; l < t.lineCount(); ++l)
        out.push_back(m_listTop + (m_layout.lineTop(l) + m_layout.lineHeight(l)) * m_scale);
    return out;
}

} // namespace oc::footnotes
