// Full parse, incremental update / resume and the node-arena splice.
#include "markdown/md_parser.h"

#include <QtGlobal>

#include <algorithm>
#include <climits>

namespace oc::md::detail {

// ---------------------------------------------------------------------------------------------------
// StateTable
// ---------------------------------------------------------------------------------------------------
StateTable::StateTable()
{
    intern(PState{}); // id 0 = empty state (start of document)
}

quint64 StateTable::hashOf(const PState& s)
{
    quint64 h = 1469598103934665603ULL;
    auto mix = [&h](quint64 v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 1099511628211ULL;
    };
    for (const CState& c : s.cs)
        mix(quint64(c.kind) | quint64(c.flags) << 8 | quint64(c.ch) << 16 | quint64(quint32(c.indent)) << 32);
    mix(quint64(s.leaf.kind) | quint64(s.leaf.a) << 8 | quint64(s.leaf.ch) << 16 | quint64(s.cs.size()) << 32);
    mix(quint64(quint32(s.leaf.b)) | quint64(quint32(s.leaf.c)) << 32);
    return h;
}

quint32 StateTable::intern(const PState& s)
{
    const quint64 h = hashOf(s);
    const auto range = m_index.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (m_states[it->second] == s)
            return it->second;
    const quint32 id = quint32(m_states.size());
    m_states.push_back(s);
    m_index.emplace(h, id);
    return id;
}

// ---------------------------------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------------------------------
Store::Store()
    : states(std::make_shared<StateTable>())
    , inlineCache(std::make_shared<InlineCache>())
{
    BlockNode doc;
    doc.type = NodeType::Document;
    doc.toLine = 1;
    nodes.push_back(doc);
    parents.push_back(-1);
    subEnd.push_back(1);
    lines.push_back(LineRec{});
}

Store::Store(const Store& o)
    : text(o.text)
    , nodes(o.nodes)
    , parents(o.parents)
    , subEnd(o.subEnd)
    , lines(o.lines)
    , headings(o.headings)
    , fences(o.fences)
    , lists(o.lists)
    , items(o.items)
    , callouts(o.callouts)
    , tables(o.tables)
    , footnotes(o.footnotes)
    , refdefs(o.refdefs)
    , refdefGen(o.refdefGen)
    , states(std::make_shared<StateTable>(*o.states))
    , fmClose(o.fmClose)
    , frontier(o.frontier)
    , frontierState(o.frontierState)
    , inlineCache(std::make_shared<InlineCache>())
{
}

Store& Store::operator=(const Store& o)
{
    if (this != &o) {
        Store tmp(o);
        text = std::move(tmp.text);
        nodes = std::move(tmp.nodes);
        parents = std::move(tmp.parents);
        subEnd = std::move(tmp.subEnd);
        lines = std::move(tmp.lines);
        headings = std::move(tmp.headings);
        fences = std::move(tmp.fences);
        lists = std::move(tmp.lists);
        items = std::move(tmp.items);
        callouts = std::move(tmp.callouts);
        tables = std::move(tmp.tables);
        footnotes = std::move(tmp.footnotes);
        refdefs = std::move(tmp.refdefs);
        refdefGen = tmp.refdefGen;
        states = std::move(tmp.states);
        fmClose = tmp.fmClose;
        frontier = tmp.frontier;
        frontierState = tmp.frontierState;
        headingIds.clear();
        headingsValid = false;
        inlineCache = std::make_shared<InlineCache>();
    }
    return *this;
}

int Store::firstNodeAtOrAfter(int line) const
{
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), line, [](const BlockNode& n, int l) { return n.fromLine < l; });
    return int(it - nodes.begin());
}

int Store::lastNodeBefore(int line) const
{
    return std::max(0, firstNodeAtOrAfter(line) - 1);
}

int Store::deepestAt(int line) const
{
    if (line < 0 || line >= lineCount())
        return 0;
    int x = firstNodeAtOrAfter(line + 1) - 1;
    while (x > 0 && nodes[size_t(x)].toLine <= line)
        x = parents[size_t(x)];
    return std::max(x, 0);
}

void Store::chainAt(int line, int depth, bool withLeaf, std::vector<int>& out) const
{
    out.clear();
    QVarLengthArray<int, 32> up;
    for (int id = lastNodeBefore(line); id > 0; id = parents[size_t(id)])
        up.push_back(id);
    const int need = depth + (withLeaf ? 1 : 0);
    if (up.size() < need)
        return;
    out.reserve(size_t(need));
    for (int i = 0; i < need; ++i)
        out.push_back(up[up.size() - 1 - i]);
}

size_t Store::sideEntries() const
{
    return headings.size() + fences.size() + lists.size() + items.size() + callouts.size() + tables.size() + footnotes.size();
}

// ---------------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------------
int frontmatterClose(const Text& text)
{
    if (!scan::isFrontmatterDelim(text.lineView(0)))
        return -1;
    for (int l = 1, n = text.lineCount(); l < n; ++l)
        if (scan::isFrontmatterDelim(text.lineView(l)))
            return l;
    return -1;
}

bool verifyEnabled()
{
    return qEnvironmentVariableIsSet("OC_VERIFY_INCREMENTAL");
}

namespace {

bool isListType(NodeType t) { return t == NodeType::BulletList || t == NodeType::OrderedList; }
bool isHeadingType(NodeType t) { return t == NodeType::AtxHeading || t == NodeType::SetextHeading; }

bool kindMatches(CKind k, NodeType t)
{
    switch (k) {
    case CKind::Quote:
        return t == NodeType::BlockQuote || t == NodeType::Callout;
    case CKind::List:
        return isListType(t);
    case CKind::Item:
        return t == NodeType::ListItem;
    case CKind::Footnote:
        return t == NodeType::FootnoteDef;
    }
    return false;
}

bool leafMatches(LKind k, NodeType t)
{
    switch (k) {
    case LKind::None:
        return false;
    case LKind::Paragraph:
        return t == NodeType::Paragraph || t == NodeType::LinkRefDef || t == NodeType::SetextHeading || t == NodeType::Table;
    case LKind::Fence:
        return t == NodeType::FencedCode;
    case LKind::Indented:
        return t == NodeType::IndentedCode;
    case LKind::Html:
        return t == NodeType::HtmlBlock;
    case LKind::Math:
        return t == NodeType::MathBlock;
    case LKind::Comment:
        return t == NodeType::CommentBlock;
    case LKind::Frontmatter:
        return t == NodeType::Frontmatter;
    case LKind::Table:
        return t == NodeType::Table;
    }
    return false;
}

// Node chain open at `line` for state `ps` (validated), or false.
bool validChain(const Store& st, int line, const PState& ps, std::vector<int>& chain)
{
    const bool withLeaf = ps.leaf.kind != LKind::None;
    st.chainAt(line, int(ps.cs.size()), withLeaf, chain);
    if (chain.size() != ps.cs.size() + (withLeaf ? 1 : 0))
        return false;
    for (size_t i = 0; i < ps.cs.size(); ++i)
        if (!kindMatches(ps.cs[i].kind, st.nodes[size_t(chain[i])].type))
            return false;
    if (withLeaf && !leafMatches(ps.leaf.kind, st.nodes[size_t(chain.back())].type))
        return false;
    return true;
}

int trimBackLines(const Store& st, int endLine, int from, int depth)
{
    int e = endLine;
    while (e > from + 1) {
        const LineRec& r = st.lines[size_t(e - 1)];
        if (LineRole(r.role) != LineRole::Blank || r.vmd >= depth)
            break;
        --e;
    }
    return e;
}

bool sameRefdef(const LinkRefDefinition& a, const LinkRefDefinition& b)
{
    return a.label == b.label && a.destination == b.destination && a.title == b.title;
}

void recomputeAllTightness(Store& st)
{
    for (int id = 1; id < int(st.nodes.size()); ++id)
        if (isListType(st.nodes[size_t(id)].type))
            computeTightness(st, id);
}

void maybeCompactSideTables(Store& st)
{
    const size_t total = st.sideEntries();
    if (total < 4096 || total < 3 * st.nodes.size())
        return;
    std::vector<HeadingInfo> headings;
    std::vector<FenceInfo> fences;
    std::vector<ListInfo> lists;
    std::vector<ListItemInfo> items;
    std::vector<CalloutInfo> callouts;
    std::vector<TableInfo> tables;
    std::vector<FootnoteDefInfo> footnotes;
    for (BlockNode& n : st.nodes) {
        if (n.data < 0)
            continue;
        switch (n.type) {
        case NodeType::AtxHeading:
        case NodeType::SetextHeading:
            headings.push_back(st.headings[size_t(n.data)]);
            n.data = int(headings.size() - 1);
            break;
        case NodeType::FencedCode:
            fences.push_back(std::move(st.fences[size_t(n.data)]));
            n.data = int(fences.size() - 1);
            break;
        case NodeType::BulletList:
        case NodeType::OrderedList:
            lists.push_back(st.lists[size_t(n.data)]);
            n.data = int(lists.size() - 1);
            break;
        case NodeType::ListItem:
            items.push_back(st.items[size_t(n.data)]);
            n.data = int(items.size() - 1);
            break;
        case NodeType::Callout:
            callouts.push_back(std::move(st.callouts[size_t(n.data)]));
            n.data = int(callouts.size() - 1);
            break;
        case NodeType::Table:
            tables.push_back(std::move(st.tables[size_t(n.data)]));
            n.data = int(tables.size() - 1);
            break;
        case NodeType::FootnoteDef:
            footnotes.push_back(std::move(st.footnotes[size_t(n.data)]));
            n.data = int(footnotes.size() - 1);
            break;
        default:
            break;
        }
    }
    st.headings = std::move(headings);
    st.fences = std::move(fences);
    st.lists = std::move(lists);
    st.items = std::move(items);
    st.callouts = std::move(callouts);
    st.tables = std::move(tables);
    st.footnotes = std::move(footnotes);
}

void installRefdefs(Store& st, QList<LinkRefDefinition> defs)
{
    bool same = defs.size() == st.refdefs.size();
    for (qsizetype i = 0; same && i < defs.size(); ++i)
        same = sameRefdef(defs[i], st.refdefs[i]);
    st.refdefs = std::move(defs);
    if (!same)
        ++st.refdefGen;
}

void verifyAgainstFullParse(const Store& st)
{
    Store full;
    fullParse(full, st.text);
    QString why;
    if (!storesEquivalent(st, full, &why))
        qFatal("OC_VERIFY_INCREMENTAL: incremental parse differs from a full parse:\n%s", qPrintable(why));
}

// One re-parse run shared by update() and resume().
struct Run {
    int R = 0;           // first re-parsed line (new coords)
    int changeEnd = 0;   // Enew: resync/pause only at lines >= this
    int delta = 0;       // line delta of the change
    int frontier = -1;   // clone line of an incomplete tree (new coords), -1 if complete
    int ensureLine = -1;
    qint64 maxNanos = 0;
    bool useFrontierState = false;
    int R0 = 0, Eold = 0; // changed line range (old coords) for structural comparison
};

TreeUpdate fullReparseUpdate(Store& st, const Text& newText, int oldLines)
{
    fullParse(st, newText);
    TreeUpdate u;
    u.fromLine = 0;
    u.toLine = newText.lineCount();
    u.oldToLine = oldLines;
    u.lineDelta = newText.lineCount() - oldLines;
    u.structural = true;
    u.headingsChanged = true;
    u.complete = true;
    return u;
}

// Map an old line boundary to new coordinates (-1 when it falls inside the changed lines).
int mapBoundary(int b, const Run& run)
{
    if (b <= run.R0)
        return b;
    if (b >= run.Eold)
        return b + run.delta;
    return -1;
}

TreeUpdate runReparse(Store& st, const Text& newText, const Run& run, bool* ok)
{
    *ok = true;
    TreeUpdate u;
    const int newN = newText.lineCount();
    const int oldNodeCount = int(st.nodes.size());

    // Parser start state and the adopted open chain.
    const PState startState = run.useFrontierState ? st.states->get(st.frontierState) : st.states->get(st.lines[size_t(run.R)].state);
    std::vector<int> chain;
    if (!validChain(st, run.R, startState, chain)) {
        *ok = false;
        return u;
    }
    const int A = std::max(1, st.firstNodeAtOrAfter(run.R)); // the Document (id 0) always stays
    // Old ends of the adopted chain (prefix nodes whose range the re-parse may change).
    std::vector<int> adoptedOldTo;
    for (int id : chain)
        adoptedOldTo.push_back(st.nodes[size_t(id)].toLine);
    LineParser p(st, newText, st.fmClose, A);
    p.beginAt(run.R, startState, chain);

    QElapsedTimer timer;
    timer.start();
    int L = run.R;
    enum class End { Complete, Resync, Pause } end = End::Complete;
    std::vector<int> oldChain;
    PState oldState;
    for (;;) {
        if (L >= newN)
            break;
        if (L >= run.changeEnd && L > run.R && !p.lookaheadOpen()) {
            const quint32 cur = p.stateId();
            if (L != run.frontier && st.lines[size_t(L)].state == cur) {
                oldState = st.states->get(cur);
                if (validChain(st, L - run.delta, oldState, oldChain)) {
                    end = End::Resync;
                    break;
                }
            }
            if (L > run.ensureLine && (run.frontier < 0 || L >= run.frontier) && ((L - run.R) % 16 == 0 || run.maxNanos <= 0)
                && timer.nsecsElapsed() > run.maxNanos) {
                const quint32 oldId = st.lines[size_t(L)].state;
                if (oldId != kNoState && oldId < st.states->size()) {
                    oldState = st.states->get(oldId);
                    // clone only the old nodes that cover the pause line
                    st.chainAt(L - run.delta, int(oldState.cs.size()), oldState.leaf.kind != LKind::None, oldChain);
                    end = End::Pause;
                    break;
                }
            }
        }
        p.parseLine(L);
        ++L;
    }
    const int S = L;
    const int Sold = S - run.delta;
    const std::vector<int> newChain = p.openChain();
    quint32 pauseState = 0;
    if (end == End::Pause)
        pauseState = p.stateId();
    if (end != End::Resync)
        p.finish(S);

    // ---- splice -----------------------------------------------------------------------------------
    const int Bend = end == End::Complete ? oldNodeCount : st.firstNodeAtOrAfter(Sold);
    // Old values of the old chain (before anything is overwritten).
    struct OldInfo {
        BlockNode node;
        int subEnd = 0;
    };
    std::vector<OldInfo> oldInfo;
    if (end != End::Complete) {
        for (int id : oldChain)
            oldInfo.push_back(OldInfo{st.nodes[size_t(id)], st.subEnd[size_t(id)]});
    }
    // Clones (pause): old chain nodes that cover the pause line.
    int C = 0;
    if (end == End::Pause) {
        while (C < int(oldInfo.size()) && oldInfo[size_t(C)].node.toLine > Sold)
            ++C;
    }
    const int F = int(p.fresh.size());
    const int B = Bend - A;
    const int shift = F + C - B;

    // Structural comparison (before overwriting the old region).
    bool structural = F != B;
    bool headingsChanged = p.headingSeen;
    for (int i = 0; i < B; ++i) {
        const BlockNode& o = st.nodes[size_t(A + i)];
        if (isHeadingType(o.type))
            headingsChanged = true;
        if (structural)
            continue;
        const BlockNode& nw = p.fresh[size_t(i)];
        if (o.type != nw.type || o.depth != nw.depth || o.flags != nw.flags || mapBoundary(o.fromLine, run) != nw.fromLine
            || ((end != End::Resync || p.freshSubEnd[size_t(i)] >= 0) && mapBoundary(o.toLine, run) != nw.toLine))
            structural = true;
    }

    // Adopted (prefix) chain nodes closed inside the region.
    std::vector<int> touchedLists;
    int fromLine = run.R;
    for (const LineParser::Adopted& a : p.adopted) {
        if (a.toLine < 0)
            continue;
        BlockNode& n = st.nodes[size_t(a.id)];
        n.toLine = a.toLine;
        n.flags = quint16((n.flags | a.setFlags) & ~a.clearFlags);
        st.subEnd[size_t(a.id)] = a.subEnd; // final ids (fresh nodes start at A)
        if (isListType(n.type))
            touchedLists.push_back(a.id);
    }

    // Resize the arena around the old region.
    if (shift > 0) {
        st.nodes.insert(st.nodes.begin() + Bend, size_t(shift), BlockNode{});
        st.parents.insert(st.parents.begin() + Bend, size_t(shift), 0);
        st.subEnd.insert(st.subEnd.begin() + Bend, size_t(shift), 0);
    } else if (shift < 0) {
        st.nodes.erase(st.nodes.begin() + (Bend + shift), st.nodes.begin() + Bend);
        st.parents.erase(st.parents.begin() + (Bend + shift), st.parents.begin() + Bend);
        st.subEnd.erase(st.subEnd.begin() + (Bend + shift), st.subEnd.begin() + Bend);
    }
    const int suffixStart = A + F + C;
    const int total = int(st.nodes.size());

    // Fresh nodes.
    for (int i = 0; i < F; ++i) {
        st.nodes[size_t(A + i)] = p.fresh[size_t(i)];
        st.parents[size_t(A + i)] = p.freshParent[size_t(i)];
        st.subEnd[size_t(A + i)] = p.freshSubEnd[size_t(i)];
        if (isListType(p.fresh[size_t(i)].type))
            touchedLists.push_back(A + i);
    }

    // Targets of the old chain levels (the nodes that inherit their continuation).
    std::vector<int> target(oldInfo.size(), -1);
    if (end == End::Resync) {
        for (size_t j = 0; j < oldInfo.size(); ++j)
            target[j] = newChain[j];
    } else if (end == End::Pause) {
        for (int j = 0; j < C; ++j)
            target[size_t(j)] = A + F + j;
    }
    auto mapOld = [&](int x) -> int { return x >= Bend ? x + shift : x; };

    // New chain (resync): take over the old chain's ranges and suffix children.
    int toLine = S;
    if (end == End::Resync) {
        for (size_t j = 0; j < oldInfo.size(); ++j) {
            const int nw = newChain[j];
            BlockNode& n = st.nodes[size_t(nw)];
            const OldInfo& o = oldInfo[j];
            const bool leaf = isLeaf(n.type);
            int newTo = o.node.toLine + run.delta;
            if (!leaf && newTo <= S)
                newTo = trimBackLines(st, S, n.fromLine, n.depth);
            if (newTo != o.node.toLine + run.delta) {
                structural = true;
                fromLine = std::min(fromLine, std::min(newTo, o.node.toLine + run.delta));
                toLine = std::max(toLine, std::max(newTo, o.node.toLine + run.delta));
            }
            n.toLine = newTo;
            if (leaf)
                n.flags = quint16((n.flags & ~FlagUnclosed) | (o.node.flags & FlagUnclosed));
            st.subEnd[size_t(nw)] = std::max(A + F, o.subEnd > Bend ? o.subEnd + shift : 0);
            if (isListType(n.type))
                touchedLists.push_back(nw);
        }
    }
    // Clones (pause).
    for (int j = 0; j < C; ++j) {
        const OldInfo& o = oldInfo[size_t(j)];
        BlockNode n = o.node;
        n.fromLine = S;
        n.toLine = std::max(o.node.toLine + run.delta, S + 1);
        st.nodes[size_t(A + F + j)] = n;
        st.parents[size_t(A + F + j)] = j == 0 ? 0 : A + F + j - 1;
        st.subEnd[size_t(A + F + j)] = std::max(suffixStart, o.subEnd > Bend ? o.subEnd + shift : 0);
        if (isListType(n.type))
            touchedLists.push_back(A + F + j);
    }

    // Suffix: shift ids / lines, re-parent children of the old chain.
    auto remapParent = [&](int p0) -> int {
        if (p0 >= Bend)
            return p0 + shift;
        for (size_t j = 0; j < oldChain.size(); ++j)
            if (oldChain[j] == p0)
                return target[j] >= 0 ? target[j] : (j > 0 && target[j - 1] >= 0 ? target[j - 1] : 0);
        return p0;
    };
    if (shift != 0 || run.delta != 0) {
        for (int x = suffixStart; x < total; ++x) {
            BlockNode& n = st.nodes[size_t(x)];
            if (run.delta != 0) {
                n.fromLine += run.delta;
                n.toLine += run.delta;
                if (isHeadingType(n.type))
                    headingsChanged = true;
            }
            st.parents[size_t(x)] = remapParent(st.parents[size_t(x)]);
            st.subEnd[size_t(x)] = mapOld(st.subEnd[size_t(x)]); // stored values are old ids
        }
    } else {
        bool chainMoved = false;
        for (size_t j = 0; j < oldChain.size(); ++j)
            chainMoved = chainMoved || target[j] != oldChain[j];
        if (chainMoved) {
            for (int x = suffixStart; x < total; x = st.subEnd[size_t(x)])
                st.parents[size_t(x)] = remapParent(st.parents[size_t(x)]);
        }
    }

    // Adopted chain nodes whose range changed: the lines between the old and the new end changed
    // container coverage (markers of trailing blank lines, block widgets...).
    for (size_t j = 0; j < chain.size(); ++j) {
        const int newTo = st.nodes[size_t(chain[j])].toLine;
        const int oldMapped = mapBoundary(adoptedOldTo[j], run);
        if (oldMapped != newTo) {
            structural = true;
            fromLine = std::min(fromLine, oldMapped < 0 ? newTo : std::min(newTo, oldMapped));
            toLine = std::max(toLine, std::max(newTo, oldMapped));
        }
    }

    // Document.
    st.nodes[0].toLine = newN;
    st.nodes[0].fromLine = 0;
    st.subEnd[0] = total;

    // Refdefs.
    {
        QList<LinkRefDefinition> defs;
        defs.reserve(st.refdefs.size() + p.refdefs.size());
        for (const LinkRefDefinition& d : st.refdefs)
            if (d.line < run.R)
                defs.push_back(d);
        defs += p.refdefs;
        const int oldSuffixFrom = end == End::Complete ? INT_MAX : Sold;
        for (const LinkRefDefinition& d : st.refdefs) {
            if (d.line >= oldSuffixFrom) {
                LinkRefDefinition s2 = d;
                s2.line += run.delta;
                defs.push_back(s2);
            }
        }
        // Only a change of labels/destinations/titles bumps the generation (line shifts don't).
        bool same = defs.size() == st.refdefs.size();
        for (qsizetype i = 0; same && i < defs.size(); ++i)
            same = sameRefdef(defs[i], st.refdefs[i]);
        st.refdefs = std::move(defs);
        if (!same)
            ++st.refdefGen;
    }

    // Tightness of affected lists.
    st.text = newText;
    std::sort(touchedLists.begin(), touchedLists.end());
    touchedLists.erase(std::unique(touchedLists.begin(), touchedLists.end()), touchedLists.end());
    for (int id : touchedLists)
        if (id < total && isListType(st.nodes[size_t(id)].type))
            computeTightness(st, id);

    // Frontier.
    if (end == End::Pause) {
        st.frontier = S;
        st.frontierState = pauseState;
    } else if (end == End::Complete || run.frontier < 0 || S >= run.frontier) {
        st.frontier = -1;
    } else {
        st.frontier = run.frontier; // resynced before the old frontier: it stays
    }
    st.text = newText;
    st.headingsValid = false;
    maybeCompactSideTables(st);

    u.fromLine = std::max(0, fromLine);
    u.toLine = std::min(std::max(toLine, S), newN);
    u.oldToLine = u.toLine - run.delta;
    u.lineDelta = run.delta;
    u.structural = structural || end == End::Pause;
    u.headingsChanged = headingsChanged;
    u.complete = st.frontier < 0;
    return u;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------
namespace {
int lastChildOf(const Store& st, int x)
{
    int y = st.subEnd[size_t(x)] - 1;
    if (y <= x)
        return -1;
    while (st.parents[size_t(y)] != x)
        y = st.parents[size_t(y)];
    return y;
}

// cmark's S_ends_with_blank_line for the one leaf that can end with a blank line it received
// itself: an HTML block of kind 1-5 (blank lines inside fenced code/math/comments don't count).
bool endsWithBlankHtml(const Store& st, int x)
{
    while (x > 0) {
        const BlockNode& n = st.nodes[size_t(x)];
        if (n.type == NodeType::HtmlBlock) {
            const int last = n.toLine - 1;
            if (last < 0 || last >= st.text.lineCount())
                return false;
            const QStringView s = st.text.lineView(last);
            return scan::isBlankFrom(s, std::min<int>(st.lines[size_t(last)].col, int(s.size())));
        }
        if (n.type != NodeType::BulletList && n.type != NodeType::OrderedList && n.type != NodeType::ListItem)
            return false;
        x = lastChildOf(st, x);
    }
    return false;
}
} // namespace

void computeTightness(Store& st, int listId)
{
    // Loose when two items, or two children of an item, are separated by blank lines (our
    // container ranges exclude trailing blank lines, so a gap between siblings is exactly that).
    // As in cmark, a child "ends with a blank line" when blank lines follow it inside the item;
    // that makes the list loose when a later child that is not a link reference definition
    // (cmark removes those paragraphs) follows it, or when the item has a next item.
    bool tight = true;
    const int end = st.subEnd[size_t(listId)];
    QVarLengthArray<int, 16> kids;
    for (int item = listId + 1; item < end && tight; item = st.subEnd[size_t(item)]) {
        const int next = st.subEnd[size_t(item)];
        const bool hasNext = next < end;
        if (hasNext && st.nodes[size_t(item)].toLine < st.nodes[size_t(next)].fromLine) {
            tight = false;
            break;
        }
        kids.clear();
        for (int c = item + 1; c < next; c = st.subEnd[size_t(c)])
            kids.push_back(c);
        int laterContent = 0; // non-refdef children after index i
        for (int i = int(kids.size()) - 1; i >= 0 && tight; --i) {
            const int c = kids[i];
            if (st.nodes[size_t(c)].type == NodeType::LinkRefDef)
                continue;
            const bool gapAfter = i + 1 < kids.size() && st.nodes[size_t(c)].toLine < st.nodes[size_t(kids[i + 1])].fromLine;
            if ((gapAfter || endsWithBlankHtml(st, c)) && (laterContent > 0 || hasNext))
                tight = false;
            ++laterContent;
        }
    }
    BlockNode& n = st.nodes[size_t(listId)];
    n.flags = tight ? quint16(n.flags | FlagTight) : quint16(n.flags & ~FlagTight);
}

void fullParse(Store& st, const Text& text)
{
    const int n = text.lineCount();
    st.text = text;
    st.lines.assign(size_t(n), LineRec{});
    st.headings.clear();
    st.fences.clear();
    st.lists.clear();
    st.items.clear();
    st.callouts.clear();
    st.tables.clear();
    st.footnotes.clear();
    st.states = std::make_shared<StateTable>();
    st.fmClose = frontmatterClose(text);
    st.frontier = -1;
    st.frontierState = 0;
    LineParser p(st, text, st.fmClose, 0);
    p.beginDocument();
    for (int L = 0; L < n; ++L)
        p.parseLine(L);
    p.finish(n);
    st.nodes = std::move(p.fresh);
    st.parents = std::move(p.freshParent);
    st.subEnd = std::move(p.freshSubEnd);
    st.nodes[0].toLine = n;
    st.subEnd[0] = int(st.nodes.size());
    installRefdefs(st, std::move(p.refdefs));
    recomputeAllTightness(st);
    st.headingsValid = false;
}

TreeUpdate incrementalUpdate(Store& st, const Text& newText, const ChangeDesc& change, const ParseBudget& budget)
{
    const Text oldText = st.text;
    const int oldN = oldText.lineCount();
    const int newN = newText.lineCount();
    qsizetype fa = -1, ta = -1, fb = -1, tb = -1;
    change.iterChangedRanges([&](qsizetype fromA, qsizetype toA, qsizetype fromB, qsizetype toB) {
        if (fa < 0) {
            fa = fromA;
            fb = fromB;
        }
        ta = toA;
        tb = toB;
    });
    if (fa < 0) {
        st.text = newText;
        TreeUpdate u;
        u.complete = st.complete();
        return u;
    }
    if (change.length() != oldText.length() || change.newLength() != newText.length() || oldN != st.lineCount())
        return fullReparseUpdate(st, newText, oldN);

    Run run;
    run.R0 = oldText.lineAt(fa);
    run.Eold = oldText.lineAt(ta) + 1;
    const int Enew = newText.lineAt(tb) + 1;
    run.delta = newN - oldN;
    Q_ASSERT(Enew - run.Eold == run.delta);
    Q_UNUSED(fb);

    // Frontmatter decision.
    const int oldFm = st.fmClose;
    int newFm = -1;
    bool restartAt0 = run.R0 == 0;
    if (run.R0 == 0) {
        newFm = frontmatterClose(newText);
    } else if (scan::isFrontmatterDelim(newText.lineView(0))) {
        if (oldFm >= 0 && run.R0 > oldFm) {
            newFm = oldFm;
        } else if (oldFm >= 0) {
            newFm = frontmatterClose(newText);
            restartAt0 = true; // the frontmatter or its closing line was edited
        } else {
            for (int l = std::max(1, run.R0); l < Enew; ++l) {
                if (scan::isFrontmatterDelim(newText.lineView(l))) {
                    newFm = l;
                    restartAt0 = true;
                    break;
                }
            }
        }
    }
    st.fmClose = newFm;

    // Per-line array: replace the changed lines with placeholders.
    {
        std::vector<LineRec>& lines = st.lines;
        if (run.delta > 0)
            lines.insert(lines.begin() + run.Eold, size_t(run.delta), LineRec{kNoState});
        else if (run.delta < 0)
            lines.erase(lines.begin() + Enew, lines.begin() + run.Eold);
        // The state at the start of R0 only depends on earlier lines and stays valid.
        for (int l = run.R0 + 1; l < Enew; ++l)
            lines[size_t(l)] = LineRec{kNoState};
    }

    // Frontier of an incomplete tree, in new coordinates.
    int F = st.frontier;
    if (F >= 0) {
        if (F >= run.Eold)
            F += run.delta;
        else if (F > run.R0)
            F = Enew;
    }
    run.frontier = F;
    run.R = restartAt0 ? 0 : run.R0;
    run.useFrontierState = false;
    if (F >= 0 && F <= run.R) {
        run.R = F;
        run.useFrontierState = st.frontier == F; // the frontier line itself was not edited
        if (!run.useFrontierState)
            return fullReparseUpdate(st, newText, oldN);
    }
    // Move back over open lookahead-sensitive leaves (paragraph / indented code).
    std::vector<int> chain;
    while (run.R > 0 && !run.useFrontierState) {
        const quint32 sid = st.lines[size_t(run.R)].state;
        if (sid == kNoState || sid >= st.states->size())
            return fullReparseUpdate(st, newText, oldN);
        const PState& ps = st.states->get(sid);
        if (!isLookahead(ps.leaf.kind))
            break;
        if (!validChain(st, run.R, ps, chain))
            return fullReparseUpdate(st, newText, oldN);
        const int start = st.nodes[size_t(chain.back())].fromLine;
        if (start >= run.R)
            return fullReparseUpdate(st, newText, oldN);
        run.R = start;
    }
    if (run.R == 0 && !run.useFrontierState)
        st.lines[0].state = 0; // start of document: empty state
    run.changeEnd = Enew;
    run.ensureLine = budget.ensureLine;
    run.maxNanos = budget.maxNanos;

    bool ok = true;
    TreeUpdate u = runReparse(st, newText, run, &ok);
    if (!ok)
        return fullReparseUpdate(st, newText, oldN);
    if (u.complete && verifyEnabled())
        verifyAgainstFullParse(st);
    return u;
}

bool resumeParse(Store& st, const Text& text, qint64 maxNanos, TreeUpdate* out)
{
    if (st.frontier < 0) {
        if (out)
            *out = TreeUpdate{};
        return false;
    }
    if (text.lineCount() != st.lineCount()) {
        const TreeUpdate u = fullReparseUpdate(st, text, st.lineCount());
        if (out)
            *out = u;
        return false;
    }
    Run run;
    run.R = st.frontier;
    run.R0 = st.frontier;
    run.Eold = st.frontier;
    run.changeEnd = st.frontier;
    run.delta = 0;
    run.frontier = st.frontier;
    run.ensureLine = -1;
    run.maxNanos = maxNanos;
    run.useFrontierState = true;
    bool ok = true;
    TreeUpdate u = runReparse(st, text, run, &ok);
    if (!ok)
        u = fullReparseUpdate(st, text, st.lineCount());
    if (u.complete && verifyEnabled())
        verifyAgainstFullParse(st);
    if (out)
        *out = u;
    return !u.complete;
}

QString checkInvariants(const Store& st)
{
    const int N = int(st.nodes.size());
    if (int(st.parents.size()) != N || int(st.subEnd.size()) != N)
        return QStringLiteral("array sizes differ");
    if (N < 1 || st.nodes[0].type != NodeType::Document || st.parents[0] != -1 || st.subEnd[0] != N)
        return QStringLiteral("bad document node");
    if (st.nodes[0].fromLine != 0 || st.nodes[0].toLine != st.lineCount())
        return QStringLiteral("document range %1-%2 vs %3 lines").arg(st.nodes[0].fromLine).arg(st.nodes[0].toLine).arg(st.lineCount());
    if (st.text.lineCount() != st.lineCount())
        return QStringLiteral("line array size %1 vs text %2").arg(st.lineCount()).arg(st.text.lineCount());
    for (int id = 1; id < N; ++id) {
        const BlockNode& n = st.nodes[size_t(id)];
        const int p = st.parents[size_t(id)];
        if (p < 0 || p >= id)
            return QStringLiteral("node %1: bad parent %2").arg(id).arg(p);
        const BlockNode& pn = st.nodes[size_t(p)];
        if (st.subEnd[size_t(id)] <= id || st.subEnd[size_t(id)] > st.subEnd[size_t(p)])
            return QStringLiteral("node %1: bad subtree end %2 (parent %3 ends %4)").arg(id).arg(st.subEnd[size_t(id)]).arg(p).arg(st.subEnd[size_t(p)]);
        if (n.fromLine >= n.toLine)
            return QStringLiteral("node %1 (%2): empty range %3-%4").arg(id).arg(QLatin1StringView(nodeTypeName(n.type))).arg(n.fromLine).arg(n.toLine);
        if (n.fromLine < pn.fromLine || n.toLine > pn.toLine)
            return QStringLiteral("node %1 [%2,%3) outside parent %4 [%5,%6)").arg(id).arg(n.fromLine).arg(n.toLine).arg(p).arg(pn.fromLine).arg(pn.toLine);
        if (n.depth != pn.depth + 1)
            return QStringLiteral("node %1: depth %2, parent depth %3").arg(id).arg(n.depth).arg(pn.depth);
        if (st.nodes[size_t(id - 1)].fromLine > n.fromLine)
            return QStringLiteral("node %1: preorder fromLine decreases").arg(id);
        // previous sibling must end before this node starts
        if (id - 1 != p) {
            int prev = id - 1;
            while (prev > 0 && st.parents[size_t(prev)] != p)
                prev = st.parents[size_t(prev)];
            if (prev > 0 && st.nodes[size_t(prev)].toLine > n.fromLine)
                return QStringLiteral("node %1 overlaps previous sibling %2").arg(id).arg(prev);
        }
        if (isLeaf(n.type) && st.subEnd[size_t(id)] != id + 1)
            return QStringLiteral("leaf %1 has children").arg(id);
    }
    return {};
}

} // namespace oc::md::detail
