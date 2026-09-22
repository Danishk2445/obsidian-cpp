// oc::md::SyntaxTree — public API over the block parser store (md_engine.h).
#include "markdown/syntax_tree.h"

#include "markdown/md_engine.h"
#include "markdown/md_scan.h"

#include <QHash>

#include <algorithm>

namespace oc::md {

using namespace detail;

bool isContainer(NodeType t)
{
    switch (t) {
    case NodeType::Document:
    case NodeType::BlockQuote:
    case NodeType::Callout:
    case NodeType::BulletList:
    case NodeType::OrderedList:
    case NodeType::ListItem:
    case NodeType::FootnoteDef:
        return true;
    default:
        return false;
    }
}

bool isLeaf(NodeType t) { return !isContainer(t); }

const char* nodeTypeName(NodeType t)
{
    switch (t) {
    case NodeType::Document:
        return "Document";
    case NodeType::BlockQuote:
        return "BlockQuote";
    case NodeType::Callout:
        return "Callout";
    case NodeType::BulletList:
        return "BulletList";
    case NodeType::OrderedList:
        return "OrderedList";
    case NodeType::ListItem:
        return "ListItem";
    case NodeType::FootnoteDef:
        return "FootnoteDef";
    case NodeType::Paragraph:
        return "Paragraph";
    case NodeType::AtxHeading:
        return "AtxHeading";
    case NodeType::SetextHeading:
        return "SetextHeading";
    case NodeType::ThematicBreak:
        return "ThematicBreak";
    case NodeType::FencedCode:
        return "FencedCode";
    case NodeType::IndentedCode:
        return "IndentedCode";
    case NodeType::HtmlBlock:
        return "HtmlBlock";
    case NodeType::LinkRefDef:
        return "LinkRefDef";
    case NodeType::Table:
        return "Table";
    case NodeType::MathBlock:
        return "MathBlock";
    case NodeType::CommentBlock:
        return "CommentBlock";
    case NodeType::Frontmatter:
        return "Frontmatter";
    }
    return "?";
}

const char* lineRoleName(LineRole r)
{
    switch (r) {
    case LineRole::Blank:
        return "Blank";
    case LineRole::Paragraph:
        return "Paragraph";
    case LineRole::AtxHeading:
        return "AtxHeading";
    case LineRole::SetextText:
        return "SetextText";
    case LineRole::SetextUnderline:
        return "SetextUnderline";
    case LineRole::ThematicBreak:
        return "ThematicBreak";
    case LineRole::FenceOpen:
        return "FenceOpen";
    case LineRole::FenceBody:
        return "FenceBody";
    case LineRole::FenceClose:
        return "FenceClose";
    case LineRole::IndentedCode:
        return "IndentedCode";
    case LineRole::Html:
        return "Html";
    case LineRole::TableHeader:
        return "TableHeader";
    case LineRole::TableDelimiter:
        return "TableDelimiter";
    case LineRole::TableRow:
        return "TableRow";
    case LineRole::MathOpen:
        return "MathOpen";
    case LineRole::MathBody:
        return "MathBody";
    case LineRole::MathClose:
        return "MathClose";
    case LineRole::MathOneLine:
        return "MathOneLine";
    case LineRole::CommentOpen:
        return "CommentOpen";
    case LineRole::CommentBody:
        return "CommentBody";
    case LineRole::CommentClose:
        return "CommentClose";
    case LineRole::FrontmatterOpen:
        return "FrontmatterOpen";
    case LineRole::FrontmatterBody:
        return "FrontmatterBody";
    case LineRole::FrontmatterClose:
        return "FrontmatterClose";
    case LineRole::LinkRefDef:
        return "LinkRefDef";
    case LineRole::CalloutHeader:
        return "CalloutHeader";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------
// Inline cache
// ---------------------------------------------------------------------------------------------------
namespace detail {

InlineTreePtr InlineCache::get(const QString& content, quint64 gen, const QList<LinkRefDefinition>& refs)
{
    const quint64 key = (quint64(qHash(content, 0x51ed270bU)) << 20) ^ gen ^ (quint64(content.size()) << 44);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries) {
        if (e.key == key && e.tree && e.tree->content() == content) {
            e.tick = ++m_tick;
            return e.tree;
        }
    }
    InlineTreePtr tree = parseInline(content, refs);
    Entry fresh{key, ++m_tick, tree};
    if (m_entries.size() < kCapacity) {
        m_entries.push_back(std::move(fresh));
    } else {
        auto victim = std::min_element(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.tick < b.tick; });
        *victim = std::move(fresh);
    }
    return tree;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------------
// SyntaxTree
// ---------------------------------------------------------------------------------------------------
struct SyntaxTree::Impl {
    Store st;
};

SyntaxTree::SyntaxTree()
    : d(std::make_unique<Impl>())
{
}

SyntaxTree::~SyntaxTree() = default;

SyntaxTree::SyntaxTree(const SyntaxTree& o)
    : d(std::make_unique<Impl>(*o.d))
{
}

SyntaxTree& SyntaxTree::operator=(const SyntaxTree& o)
{
    if (this != &o)
        d->st = o.d->st;
    return *this;
}

void SyntaxTree::parse(const Text& text) { fullParse(d->st, text); }

TreeUpdate SyntaxTree::update(const Text& newText, const ChangeDesc& change, const ParseBudget& budget)
{
    return incrementalUpdate(d->st, newText, change, budget);
}

bool SyntaxTree::resume(const Text& text, qint64 maxNanos, TreeUpdate* out) { return resumeParse(d->st, text, maxNanos, out); }

int SyntaxTree::validUpTo() const { return d->st.frontier < 0 ? d->st.lineCount() : d->st.frontier; }

int SyntaxTree::lineCount() const { return d->st.lineCount(); }

LineRole SyntaxTree::role(int line) const
{
    if (line < 0 || line >= d->st.lineCount())
        return LineRole::Blank;
    return LineRole(d->st.lines[size_t(line)].role);
}

qint32 SyntaxTree::contentColumn(int line) const
{
    if (line < 0 || line >= d->st.lineCount())
        return 0;
    return d->st.lines[size_t(line)].col;
}

QList<ContainerMarker> SyntaxTree::containerMarkers(int line) const
{
    QList<ContainerMarker> out;
    const Store& st = d->st;
    if (line < 0 || line >= st.lineCount() || line >= st.text.lineCount())
        return out;
    const quint32 sid = st.lines[size_t(line)].state;
    if (sid == kNoState || sid >= st.states->size())
        return out;
    const PState& ps = st.states->get(sid);
    QVarLengthArray<MarkerRec, 8> recs;
    analyzeMarkers(st.text.lineView(line), ps, recs);
    if (recs.isEmpty())
        return out;
    std::vector<int> chain;
    st.chainAt(line, int(ps.cs.size()), false, chain);
    int next = std::max(1, st.firstNodeAtOrAfter(line)); // never the Document (it starts at line 0)
    const int N = int(st.nodes.size());
    for (const MarkerRec& r : recs) {
        int node = -1;
        if (!r.opened) {
            if (r.level - 1 >= int(chain.size()))
                break;
            node = chain[size_t(r.level - 1)];
            if (st.nodes[size_t(node)].toLine <= line)
                break; // trailing blank line outside the container's range
        } else {
            while (next < N && (st.nodes[size_t(next)].type == NodeType::BulletList || st.nodes[size_t(next)].type == NodeType::OrderedList))
                ++next;
            if (next < N && st.nodes[size_t(next)].fromLine == line && isContainer(st.nodes[size_t(next)].type))
                node = next++;
        }
        out.push_back(ContainerMarker{r.kind, r.from, r.to, node});
    }
    return out;
}

int SyntaxTree::leafAt(int line) const
{
    const int x = d->st.deepestAt(line);
    return x > 0 && isLeaf(d->st.nodes[size_t(x)].type) ? x : -1;
}

int SyntaxTree::nodeCount() const { return int(d->st.nodes.size()); }

const BlockNode& SyntaxTree::node(int id) const
{
    static const BlockNode empty;
    if (id < 0 || id >= int(d->st.nodes.size()))
        return empty;
    return d->st.nodes[size_t(id)];
}

int SyntaxTree::parent(int id) const
{
    if (id <= 0 || id >= int(d->st.nodes.size()))
        return -1;
    return d->st.parents[size_t(id)];
}

int SyntaxTree::firstChild(int id) const
{
    if (id < 0 || id >= int(d->st.nodes.size()))
        return -1;
    return id + 1 < d->st.subEnd[size_t(id)] ? id + 1 : -1;
}

int SyntaxTree::nextSibling(int id) const
{
    if (id <= 0 || id >= int(d->st.nodes.size()))
        return -1;
    const int e = d->st.subEnd[size_t(id)];
    const int p = d->st.parents[size_t(id)];
    return e < d->st.subEnd[size_t(p)] ? e : -1;
}

int SyntaxTree::deepestAt(int line) const { return d->st.deepestAt(line); }

QList<int> SyntaxTree::ancestorsAt(int line) const
{
    QList<int> out;
    for (int id = d->st.deepestAt(line); id >= 0; id = parent(id)) {
        out.push_front(id);
        if (id == 0)
            break;
    }
    return out;
}

int SyntaxTree::firstNodeAtOrAfter(int line) const { return d->st.firstNodeAtOrAfter(line); }

QList<int> SyntaxTree::headings() const
{
    const Store& st = d->st;
    if (!st.headingsValid) {
        st.headingIds.clear();
        for (int id = 1; id < int(st.nodes.size()); ++id)
            if (st.nodes[size_t(id)].type == NodeType::AtxHeading || st.nodes[size_t(id)].type == NodeType::SetextHeading)
                st.headingIds.push_back(id);
        st.headingsValid = true;
    }
    return QList<int>(st.headingIds.begin(), st.headingIds.end());
}

namespace {
template<class T>
T sideInfo(const Store& st, const std::vector<T>& table, int id, NodeType a, NodeType b = NodeType::Document)
{
    if (id <= 0 || id >= int(st.nodes.size()))
        return T{};
    const BlockNode& n = st.nodes[size_t(id)];
    if ((n.type != a && n.type != b) || n.data < 0 || n.data >= int(table.size()))
        return T{};
    return table[size_t(n.data)];
}
} // namespace

HeadingInfo SyntaxTree::heading(int id) const { return sideInfo(d->st, d->st.headings, id, NodeType::AtxHeading, NodeType::SetextHeading); }

FenceInfo SyntaxTree::fence(int id) const
{
    FenceInfo fi = sideInfo(d->st, d->st.fences, id, NodeType::FencedCode);
    fi.closed = id > 0 && id < nodeCount() && !(node(id).flags & FlagUnclosed);
    return fi;
}

ListInfo SyntaxTree::list(int id) const
{
    ListInfo li = sideInfo(d->st, d->st.lists, id, NodeType::BulletList, NodeType::OrderedList);
    li.tight = id > 0 && id < nodeCount() && (node(id).flags & FlagTight);
    return li;
}

ListItemInfo SyntaxTree::listItem(int id) const { return sideInfo(d->st, d->st.items, id, NodeType::ListItem); }

CalloutInfo SyntaxTree::callout(int id) const { return sideInfo(d->st, d->st.callouts, id, NodeType::Callout); }

TableInfo SyntaxTree::table(int id) const { return sideInfo(d->st, d->st.tables, id, NodeType::Table); }

QList<TableCell> SyntaxTree::tableCells(int line, const Text& text) const
{
    QList<TableCell> out;
    const LineRole r = role(line);
    if (r != LineRole::TableHeader && r != LineRole::TableRow && r != LineRole::TableDelimiter)
        return out;
    if (line >= text.lineCount())
        return out;
    const QStringView s = text.lineView(line);
    const int col = std::min<int>(contentColumn(line), int(s.size()));
    scan::tableCellRanges(s, scan::skipSpaces(s, col), &out);
    return out;
}

FootnoteDefInfo SyntaxTree::footnoteDef(int id) const { return sideInfo(d->st, d->st.footnotes, id, NodeType::FootnoteDef); }

int SyntaxTree::htmlBlockKind(int id) const
{
    if (id <= 0 || id >= nodeCount() || node(id).type != NodeType::HtmlBlock)
        return 0;
    return node(id).data;
}

const QList<LinkRefDefinition>& SyntaxTree::linkRefDefs() const { return d->st.refdefs; }

int SyntaxTree::frontmatter() const
{
    return d->st.nodes.size() > 1 && d->st.nodes[1].type == NodeType::Frontmatter ? 1 : -1;
}

LeafContent SyntaxTree::leafContent(int leafId, const Text& text) const
{
    LeafContent c;
    const Store& st = d->st;
    if (leafId <= 0 || leafId >= int(st.nodes.size()))
        return c;
    const BlockNode& n = st.nodes[size_t(leafId)];
    const int lines = text.lineCount();
    auto addSegment = [&](int line, int from, int to) {
        if (!c.segments.isEmpty())
            c.text += QLatin1Char('\n');
        const QStringView s = text.lineView(line);
        from = std::clamp(from, 0, int(s.size()));
        to = std::clamp(to, from, int(s.size()));
        c.segments.push_back(LeafContent::Segment{line, from, int(c.text.size()), to - from});
        c.text += s.mid(from, to - from);
    };
    auto addLines = [&](int from, int to) {
        for (int l = from; l < to && l < lines; ++l) {
            const QStringView s = text.lineView(l);
            const int start = scan::skipSpaces(s, std::min<int>(contentColumn(l), int(s.size())));
            const int end = l + 1 == to ? scan::rtrim(s, start, int(s.size())) : int(s.size());
            addSegment(l, start, end);
        }
    };
    switch (n.type) {
    case NodeType::Paragraph: {
        if (role(n.fromLine) == LineRole::CalloutHeader) {
            const int p = parent(leafId);
            if (p > 0 && node(p).type == NodeType::Callout && n.fromLine < lines) {
                const CalloutInfo ci = callout(p);
                addSegment(n.fromLine, ci.titleFrom, ci.titleTo);
            }
            return c;
        }
        addLines(n.fromLine, n.toLine);
        return c;
    }
    case NodeType::LinkRefDef:
        addLines(n.fromLine, n.toLine);
        return c;
    case NodeType::AtxHeading: {
        if (n.fromLine < lines) {
            const HeadingInfo h = heading(leafId);
            addSegment(n.fromLine, h.textFrom, h.textTo);
        }
        return c;
    }
    case NodeType::SetextHeading:
        addLines(n.fromLine, n.toLine - 1);
        return c;
    default:
        return c;
    }
}

LeafContent SyntaxTree::tableCellContent(int line, int cellIndex, const Text& text) const
{
    LeafContent c;
    const QList<TableCell> cells = tableCells(line, text);
    if (cellIndex < 0 || cellIndex >= cells.size())
        return c;
    const TableCell& cell = cells[cellIndex];
    c.text = text.lineView(line).mid(cell.from, cell.to - cell.from).toString();
    c.segments.push_back(LeafContent::Segment{line, cell.from, 0, cell.to - cell.from});
    return c;
}

InlineTreePtr SyntaxTree::inlineTree(const LeafContent& content) const
{
    return d->st.inlineCache->get(content.text, d->st.refdefGen, d->st.refdefs);
}

// ---------------------------------------------------------------------------------------------------
// dump / equivalent
// ---------------------------------------------------------------------------------------------------
namespace {

QString alignName(Qt::Alignment a)
{
    if (a == Qt::AlignLeft)
        return QStringLiteral("L");
    if (a == Qt::AlignHCenter)
        return QStringLiteral("C");
    if (a == Qt::AlignRight)
        return QStringLiteral("R");
    return QStringLiteral("-");
}

QString markerName(ContainerMarker::Kind k)
{
    switch (k) {
    case ContainerMarker::Kind::Quote:
        return QStringLiteral("Q");
    case ContainerMarker::Kind::ListMarker:
        return QStringLiteral("LM");
    case ContainerMarker::Kind::ListIndent:
        return QStringLiteral("LI");
    case ContainerMarker::Kind::FootnoteLabel:
        return QStringLiteral("FL");
    case ContainerMarker::Kind::FootnoteIndent:
        return QStringLiteral("FI");
    }
    return QStringLiteral("?");
}

QString nodeAttrs(const SyntaxTree& t, int id)
{
    const BlockNode& n = t.node(id);
    QStringList a;
    switch (n.type) {
    case NodeType::BulletList:
    case NodeType::OrderedList: {
        const ListInfo li = t.list(id);
        a << (li.tight ? QStringLiteral("tight") : QStringLiteral("loose"));
        a << QStringLiteral("delim=%1").arg(li.delimiter);
        if (li.ordered)
            a << QStringLiteral("start=%1").arg(li.start);
        break;
    }
    case NodeType::ListItem: {
        const ListItemInfo ii = t.listItem(id);
        a << QStringLiteral("marker=%1-%2").arg(ii.markerFrom).arg(ii.markerTo) << QStringLiteral("content=%1").arg(ii.contentCol);
        if (n.flags & FlagTask)
            a << QStringLiteral("task=[%1]@%2").arg(ii.taskChar).arg(ii.taskFrom);
        if (n.flags & FlagEmptyItem)
            a << QStringLiteral("empty");
        break;
    }
    case NodeType::AtxHeading:
    case NodeType::SetextHeading: {
        const HeadingInfo h = t.heading(id);
        a << QStringLiteral("h%1").arg(h.level) << QStringLiteral("text=%1-%2").arg(h.textFrom).arg(h.textTo);
        if (n.type == NodeType::AtxHeading)
            a << QStringLiteral("marker=%1-%2").arg(h.markerFrom).arg(h.markerTo);
        if (h.closeFrom >= 0)
            a << QStringLiteral("close=%1").arg(h.closeFrom);
        break;
    }
    case NodeType::FencedCode: {
        const FenceInfo f = t.fence(id);
        a << QStringLiteral("fence=%1").arg(QString(f.length, f.ch));
        if (f.indent)
            a << QStringLiteral("indent=%1").arg(f.indent);
        if (!f.info.isEmpty())
            a << QStringLiteral("info=\"%1\"@%2-%3").arg(f.info).arg(f.infoFrom).arg(f.infoTo) << QStringLiteral("lang=%1").arg(f.language);
        if (!f.closed)
            a << QStringLiteral("unclosed");
        break;
    }
    case NodeType::Callout: {
        const CalloutInfo ci = t.callout(id);
        a << QStringLiteral("type=%1").arg(ci.type);
        if (!ci.fold.isNull())
            a << QStringLiteral("fold=%1").arg(ci.fold);
        if (!ci.metadata.isEmpty())
            a << QStringLiteral("meta=\"%1\"").arg(ci.metadata);
        a << QStringLiteral("marker=%1-%2").arg(ci.markerFrom).arg(ci.markerTo) << QStringLiteral("title=%1-%2").arg(ci.titleFrom).arg(ci.titleTo);
        break;
    }
    case NodeType::Table: {
        const TableInfo ti = t.table(id);
        QStringList al;
        for (Qt::Alignment x : ti.align)
            al << alignName(x);
        a << QStringLiteral("cols=%1").arg(ti.columns) << QStringLiteral("align=%1").arg(al.join(QLatin1Char(',')));
        break;
    }
    case NodeType::FootnoteDef:
        a << QStringLiteral("label=%1").arg(t.footnoteDef(id).label);
        break;
    case NodeType::HtmlBlock:
        a << QStringLiteral("kind=%1").arg(t.htmlBlockKind(id));
        break;
    case NodeType::MathBlock:
    case NodeType::CommentBlock:
        if (n.flags & FlagUnclosed)
            a << QStringLiteral("unclosed");
        break;
    default:
        break;
    }
    return a.join(QLatin1Char(' '));
}

} // namespace

QString SyntaxTree::dump(const Text& text) const
{
    QString out;
    for (int id = 0; id < nodeCount(); ++id) {
        const BlockNode& n = node(id);
        out += QString(n.depth * 2, QLatin1Char(' '));
        out += QLatin1StringView(nodeTypeName(n.type));
        out += QStringLiteral(" [%1,%2)").arg(n.fromLine).arg(n.toLine);
        const QString attrs = nodeAttrs(*this, id);
        if (!attrs.isEmpty())
            out += QLatin1Char(' ') + attrs;
        out += QLatin1Char('\n');
    }
    out += QStringLiteral("--\n");
    for (int l = 0; l < lineCount(); ++l) {
        out += QStringLiteral("%1 %2 c%3").arg(l).arg(QLatin1StringView(lineRoleName(role(l)))).arg(contentColumn(l));
        for (const ContainerMarker& m : containerMarkers(l))
            out += QStringLiteral(" %1%2-%3#%4").arg(markerName(m.kind)).arg(m.from).arg(m.to).arg(m.node);
        if (l < text.lineCount())
            out += QStringLiteral(" |%1|").arg(text.lineView(l).toString());
        out += QLatin1Char('\n');
    }
    for (const LinkRefDefinition& r : linkRefDefs())
        out += QStringLiteral("refdef [%1] -> <%2> \"%3\" @%4\n").arg(r.label, r.destination, r.title).arg(r.line);
    return out;
}

namespace detail {

bool storesEquivalent(const Store& a, const Store& b, QString* why)
{
    auto fail = [&](const QString& msg) {
        if (why)
            *why = msg;
        return false;
    };
    auto nodeStr = [](const Store& s, int id) {
        const BlockNode& n = s.nodes[size_t(id)];
        return QStringLiteral("#%1 %2 [%3,%4) depth=%5 flags=%6 parent=%7 subEnd=%8")
            .arg(id)
            .arg(QLatin1StringView(nodeTypeName(n.type)))
            .arg(n.fromLine)
            .arg(n.toLine)
            .arg(n.depth)
            .arg(n.flags)
            .arg(s.parents[size_t(id)])
            .arg(s.subEnd[size_t(id)]);
    };
    if (a.lineCount() != b.lineCount())
        return fail(QStringLiteral("line count %1 vs %2").arg(a.lineCount()).arg(b.lineCount()));
    if (a.frontier != b.frontier)
        return fail(QStringLiteral("validUpTo differs (frontier %1 vs %2)").arg(a.frontier).arg(b.frontier));
    for (int l = 0; l < a.lineCount(); ++l) {
        const LineRec& x = a.lines[size_t(l)];
        const LineRec& y = b.lines[size_t(l)];
        if (x.role != y.role || x.col != y.col || x.flags != y.flags || x.vmd != y.vmd)
            return fail(QStringLiteral("line %1: role %2 col %3 flags %4 vmd %5 vs role %6 col %7 flags %8 vmd %9")
                            .arg(l)
                            .arg(QLatin1StringView(lineRoleName(LineRole(x.role))))
                            .arg(x.col)
                            .arg(x.flags)
                            .arg(x.vmd)
                            .arg(QLatin1StringView(lineRoleName(LineRole(y.role))))
                            .arg(y.col)
                            .arg(y.flags)
                            .arg(y.vmd));
        const bool xs = x.state != kNoState && x.state < a.states->size();
        const bool ys = y.state != kNoState && y.state < b.states->size();
        if (xs != ys || (xs && !(a.states->get(x.state) == b.states->get(y.state))))
            return fail(QStringLiteral("line %1: parser state at line start differs").arg(l));
    }
    if (a.nodes.size() != b.nodes.size()) {
        const size_t n = std::min(a.nodes.size(), b.nodes.size());
        size_t i = 0;
        while (i < n && a.nodes[i].type == b.nodes[i].type && a.nodes[i].fromLine == b.nodes[i].fromLine && a.nodes[i].toLine == b.nodes[i].toLine)
            ++i;
        return fail(QStringLiteral("node count %1 vs %2; first difference at #%3: %4 vs %5")
                        .arg(a.nodes.size())
                        .arg(b.nodes.size())
                        .arg(i)
                        .arg(i < a.nodes.size() ? nodeStr(a, int(i)) : QStringLiteral("-"))
                        .arg(i < b.nodes.size() ? nodeStr(b, int(i)) : QStringLiteral("-")));
    }
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const BlockNode& x = a.nodes[i];
        const BlockNode& y = b.nodes[i];
        if (x.type != y.type || x.fromLine != y.fromLine || x.toLine != y.toLine || x.depth != y.depth || x.flags != y.flags
            || a.parents[i] != b.parents[i] || a.subEnd[i] != b.subEnd[i])
            return fail(QStringLiteral("node %1 vs %2").arg(nodeStr(a, int(i)), nodeStr(b, int(i))));
        auto infoDiff = [&](bool same) {
            return same ? false : true;
        };
        bool differs = false;
        const int da = x.data, db = y.data;
        switch (x.type) {
        case NodeType::AtxHeading:
        case NodeType::SetextHeading: {
            const HeadingInfo& p = a.headings[size_t(da)];
            const HeadingInfo& q = b.headings[size_t(db)];
            differs = infoDiff(p.level == q.level && p.markerFrom == q.markerFrom && p.markerTo == q.markerTo && p.closeFrom == q.closeFrom
                               && p.textFrom == q.textFrom && p.textTo == q.textTo);
            break;
        }
        case NodeType::FencedCode: {
            const FenceInfo& p = a.fences[size_t(da)];
            const FenceInfo& q = b.fences[size_t(db)];
            differs = infoDiff(p.ch == q.ch && p.length == q.length && p.indent == q.indent && p.info == q.info && p.language == q.language
                               && p.infoFrom == q.infoFrom && p.infoTo == q.infoTo);
            break;
        }
        case NodeType::BulletList:
        case NodeType::OrderedList: {
            const ListInfo& p = a.lists[size_t(da)];
            const ListInfo& q = b.lists[size_t(db)];
            differs = infoDiff(p.ordered == q.ordered && p.start == q.start && p.delimiter == q.delimiter);
            break;
        }
        case NodeType::ListItem: {
            const ListItemInfo& p = a.items[size_t(da)];
            const ListItemInfo& q = b.items[size_t(db)];
            differs = infoDiff(p.bullet == q.bullet && p.number == q.number && p.markerFrom == q.markerFrom && p.markerTo == q.markerTo
                               && p.contentCol == q.contentCol && p.taskChar == q.taskChar && p.taskFrom == q.taskFrom);
            break;
        }
        case NodeType::Callout: {
            const CalloutInfo& p = a.callouts[size_t(da)];
            const CalloutInfo& q = b.callouts[size_t(db)];
            differs = infoDiff(p.type == q.type && p.metadata == q.metadata && p.fold == q.fold && p.markerFrom == q.markerFrom
                               && p.markerTo == q.markerTo && p.titleFrom == q.titleFrom && p.titleTo == q.titleTo);
            break;
        }
        case NodeType::Table: {
            const TableInfo& p = a.tables[size_t(da)];
            const TableInfo& q = b.tables[size_t(db)];
            differs = infoDiff(p.columns == q.columns && p.align == q.align);
            break;
        }
        case NodeType::FootnoteDef:
            differs = infoDiff(a.footnotes[size_t(da)].label == b.footnotes[size_t(db)].label);
            break;
        case NodeType::HtmlBlock:
            differs = da != db;
            break;
        default:
            break;
        }
        if (differs)
            return fail(QStringLiteral("node %1: per-type info differs").arg(nodeStr(a, int(i))));
    }
    if (a.refdefs.size() != b.refdefs.size())
        return fail(QStringLiteral("refdef count %1 vs %2").arg(a.refdefs.size()).arg(b.refdefs.size()));
    for (qsizetype i = 0; i < a.refdefs.size(); ++i) {
        const LinkRefDefinition& x = a.refdefs[i];
        const LinkRefDefinition& y = b.refdefs[i];
        if (x.label != y.label || x.destination != y.destination || x.title != y.title || x.line != y.line)
            return fail(QStringLiteral("refdef %1: [%2]@%3 vs [%4]@%5").arg(i).arg(x.label).arg(x.line).arg(y.label).arg(y.line));
    }
    if (a.fmClose != b.fmClose)
        return fail(QStringLiteral("frontmatter close %1 vs %2").arg(a.fmClose).arg(b.fmClose));
    return true;
}

} // namespace detail

bool SyntaxTree::equivalent(const SyntaxTree& a, const SyntaxTree& b, QString* why) { return storesEquivalent(a.d->st, b.d->st, why); }

} // namespace oc::md
