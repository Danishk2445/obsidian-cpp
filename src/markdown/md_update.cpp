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
    , lastSplit(o.lastSplit)
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
        lastSplit = tmp.lastSplit;
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

Counters& counters()
{
    static Counters c;
    return c;
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

// Old nodes that start before line `l` and extend past it, outermost first (a path from a child
// of the Document down). These are exactly the possible non-suffix parents of nodes starting at
// >= l, so a resync or a pause at `l` hands their continuation over. Derived from the arena alone:
// behind a pause the stored line states describe structure the stale tree no longer has (e.g. a
// trimmed list item that was still open), so the state cannot be used to find these nodes.
void coveringChain(const Store& st, int l, std::vector<int>& out)
{
    out.clear();
    int x = st.lastNodeBefore(l);
    while (x > 0 && st.nodes[size_t(x)].toLine <= l)
        x = st.parents[size_t(x)];
    for (; x > 0; x = st.parents[size_t(x)])
        out.push_back(x);
    std::reverse(out.begin(), out.end());
}

// A covering chain is a prefix of the open levels of state `ps` (containers, then the leaf).
bool chainFitsState(const Store& st, const std::vector<int>& cov, const PState& ps)
{
    const size_t d = ps.cs.size();
    if (cov.size() > d + (ps.leaf.kind != LKind::None ? 1 : 0))
        return false;
    for (size_t j = 0; j < cov.size(); ++j) {
        const NodeType t = st.nodes[size_t(cov[j])].type;
        if (j < d ? !kindMatches(ps.cs[j].kind, t) : !leafMatches(ps.leaf.kind, t))
            return false;
    }
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

// Keeps the entries of `table` that nodes still reference (a pause clone shares its original's).
template<class T>
class SideCompactor {
public:
    explicit SideCompactor(std::vector<T>& table) : m_table(table), m_map(table.size(), -1) {}
    void remap(qint32& data)
    {
        qint32& m = m_map[size_t(data)];
        if (m < 0) {
            m = qint32(m_kept.size());
            m_kept.push_back(m_table[size_t(data)]);
        }
        data = m;
    }
    ~SideCompactor() { m_table = std::move(m_kept); }

private:
    std::vector<T>& m_table;
    std::vector<qint32> m_map;
    std::vector<T> m_kept;
};

void maybeCompactSideTables(Store& st)
{
    const size_t total = st.sideEntries();
    if (total < 4096 || total < 3 * st.nodes.size())
        return;
    SideCompactor<HeadingInfo> headings(st.headings);
    SideCompactor<FenceInfo> fences(st.fences);
    SideCompactor<ListInfo> lists(st.lists);
    SideCompactor<ListItemInfo> items(st.items);
    SideCompactor<CalloutInfo> callouts(st.callouts);
    SideCompactor<TableInfo> tables(st.tables);
    SideCompactor<FootnoteDefInfo> footnotes(st.footnotes);
    for (BlockNode& n : st.nodes) {
        if (n.data < 0)
            continue;
        switch (n.type) {
        case NodeType::AtxHeading:
        case NodeType::SetextHeading:
            headings.remap(n.data);
            break;
        case NodeType::FencedCode:
            fences.remap(n.data);
            break;
        case NodeType::BulletList:
        case NodeType::OrderedList:
            lists.remap(n.data);
            break;
        case NodeType::ListItem:
            items.remap(n.data);
            break;
        case NodeType::Callout:
            callouts.remap(n.data);
            break;
        case NodeType::Table:
            tables.remap(n.data);
            break;
        case NodeType::FootnoteDef:
            footnotes.remap(n.data);
            break;
        default:
            break;
        }
    }
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
    if (const QString bad = checkInvariants(st); !bad.isEmpty())
        qFatal("OC_VERIFY_INCREMENTAL: inconsistent tree after an update: %s", qPrintable(bad));
    if (!st.complete())
        return; // a paused tree is only exact below the frontier
    Store full;
    fullParse(full, st.text);
    QString why;
    int at = -1;
    if (storesEquivalent(st, full, &why, &at))
        return;
    // The text around the first difference with both line roles (incremental / full).
    QString context;
    for (int l = std::max(0, at - 4), e = std::min(st.lineCount(), at + 5); at >= 0 && l < e; ++l)
        context += QStringLiteral("%1%2 %3 / %4 |%5|\n")
                       .arg(l == at ? QLatin1Char('>') : QLatin1Char(' '))
                       .arg(l, 6)
                       .arg(QLatin1StringView(lineRoleName(LineRole(st.lines[size_t(l)].role))))
                       .arg(QLatin1StringView(lineRoleName(LineRole(full.lines[size_t(l)].role))))
                       .arg(st.text.lineView(l).left(160).toString());
    qFatal("OC_VERIFY_INCREMENTAL: incremental parse differs from a full parse:\n%s\n%s", qPrintable(why), qPrintable(context));
}

// One re-parse run shared by update() and resume().
struct Run {
    int R = 0;           // first re-parsed line (new coords)
    int changeEnd = 0;   // Enew: resync/pause only at lines >= this
    int delta = 0;       // line delta of the change
    int frontier = -1;   // clone line of an incomplete tree (new coords), -1 if complete
    int lastSplit = -1;  // Store::lastSplit (new coords), -1 if complete
    int ensureLine = -1;
    qint64 maxNanos = 0;
    bool useFrontierState = false;
    bool sealedLeaf = false; // the lookahead leaf open at R is closed by line R (see incrementalUpdate)
    int R0 = 0, Eold = 0; // changed line range (old coords) for structural comparison
};

TreeUpdate fullReparseUpdate(Store& st, const Text& newText, int oldLines)
{
    counters().fullReparses.fetch_add(1, std::memory_order_relaxed);
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
    p.beginAt(run.R, startState, chain, run.sealedLeaf);

    QElapsedTimer timer;
    timer.start();
    int L = run.R;
    enum class End { Complete, Resync, Pause } end = End::Complete;
    // Resync / pause: the old nodes covering the stop line (see coveringChain()).
    std::vector<int> oldChain;
    LineParser::Checkpoint cp;
    for (;;) {
        if (L >= newN)
            break;
        bool candidate = false; // same state as before, lookahead leaf open: try a resync after L
        if (L >= run.changeEnd && L > run.R) {
            // Resync only where the old structure after L is what parsing from its stored state
            // gives: below the frontier (exact), or past every split (the stale structure there
            // continues the old parse; the clones at splits in between are covering nodes that the
            // new open chain takes over). Never at or before a split line inside the stale region,
            // where clones would stay separate nodes.
            const quint32 cur = p.stateId();
            const bool same = (run.frontier < 0 || L < run.frontier || L > run.lastSplit) && st.lines[size_t(L)].state == cur;
            if (!p.lookaheadOpen()) {
                if (same) {
                    coveringChain(st, L - run.delta, oldChain);
                    if (chainFitsState(st, oldChain, st.states->get(cur))) {
                        end = End::Resync;
                        break;
                    }
                }
                if (L > run.ensureLine && ((L - run.R) % 16 == 0 || run.maxNanos <= 0) && timer.nsecsElapsed() > run.maxNanos) {
                    coveringChain(st, L - run.delta, oldChain);
                    end = End::Pause;
                    break;
                }
            } else if (same) {
                candidate = true;
                p.checkpoint(L, cp);
            }
        }
        p.parseLine(L);
        if (p.broken()) {
            *ok = false; // a sealed leaf was not closed by its line: nothing was spliced yet
            return u;
        }
        if (candidate && p.closedLookaheadPlainly(cp)) {
            // The old parse closed the same leaf at L too (same state, same line): resync at L with
            // the leaf finalized there. The old covering chain holds containers only.
            coveringChain(st, L - run.delta, oldChain);
            const PState& ps = st.states->get(cp.oldRec.state);
            if (oldChain.size() <= ps.cs.size() && chainFitsState(st, oldChain, ps)) {
                p.rollback(cp);
                end = End::Resync;
                counters().lookaheadResyncs.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        ++L;
    }
    const int S = L;
    const int Sold = S - run.delta;
    {
        Counters& c = counters();
        c.updates.fetch_add(1, std::memory_order_relaxed);
        c.linesParsed.fetch_add(quint64(S - run.R), std::memory_order_relaxed);
        if (run.sealedLeaf)
            c.sealedRestarts.fetch_add(1, std::memory_order_relaxed);
        if (end == End::Resync)
            c.resyncs.fetch_add(1, std::memory_order_relaxed);
        if (end == End::Pause) {
            c.pauses.fetch_add(1, std::memory_order_relaxed);
            if (S < run.lastSplit)
                c.pausesBeforeSplit.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const std::vector<int> newChain = p.openChain();
    quint32 pauseState = 0;
    if (end == End::Pause)
        pauseState = p.stateId();
    if (end != End::Resync)
        p.finish(S);

    // ---- splice -----------------------------------------------------------------------------------
    // Old nodes [A, Bend) are replaced by the fresh ones (+ the pause clones). Every old node >= Bend
    // (the suffix) has its parent either in the suffix or in oldChain (the covering chain).
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
    // Clones (pause): the old covering nodes continue from the pause line as stale nodes.
    const int C = end == End::Pause ? int(oldInfo.size()) : 0;
    const int F = int(p.fresh.size());
    const int B = Bend - A;
    const int shift = F + C - B;

    // Structural comparison (before overwriting the old region).
    // While nothing is structural, re-created list nodes keep the old tightness (it only depends on
    // node ranges and types, plus HTML blocks ending in a blank line), so no list is recomputed.
    bool structural = F != B;
    bool headingsChanged = p.headingSeen;
    bool htmlInRegion = false;
    for (int i = 0; i < B; ++i) {
        const BlockNode& o = st.nodes[size_t(A + i)];
        if (isHeadingType(o.type))
            headingsChanged = true;
        htmlInRegion = htmlInRegion || o.type == NodeType::HtmlBlock;
        if (structural)
            continue;
        BlockNode& nw = p.fresh[size_t(i)];
        if (isListType(o.type) && nw.type == o.type)
            nw.flags = quint16((nw.flags & ~FlagTight) | (o.flags & FlagTight));
        if (o.type != nw.type || o.depth != nw.depth || o.flags != nw.flags || mapBoundary(o.fromLine, run) != nw.fromLine
            || ((end != End::Resync || p.freshSubEnd[size_t(i)] >= 0) && mapBoundary(o.toLine, run) != nw.toLine))
            structural = true;
    }
    for (int i = B; i < F && !htmlInRegion; ++i)
        htmlInRegion = p.fresh[size_t(i)].type == NodeType::HtmlBlock;

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

    // New chain (resync). The state at S equals the old one, so the levels the old covering chain
    // occupies continue exactly like the old nodes did (range end, suffix children, unclosed flag);
    // the deeper open levels end at S: a container whose remaining lines were trailing blank lines,
    // a table / HTML block that line S ends, or a fence / math / comment block whose container
    // ends at S (closed as unclosed, like LineParser::closeLeaf does).
    int toLine = S;
    if (end == End::Resync) {
        for (size_t j = 0; j < newChain.size(); ++j) {
            const int nw = newChain[j];
            BlockNode& n = st.nodes[size_t(nw)];
            const bool leaf = isLeaf(n.type);
            if (j < oldInfo.size()) {
                const OldInfo& o = oldInfo[j];
                n.toLine = o.node.toLine + run.delta;
                if (leaf)
                    n.flags = quint16((n.flags & ~FlagUnclosed) | (o.node.flags & FlagUnclosed));
                st.subEnd[size_t(nw)] = std::max(A + F, o.subEnd > Bend ? o.subEnd + shift : 0);
            } else {
                n.toLine = leaf ? S : trimBackLines(st, S, n.fromLine, n.depth);
                if (n.type == NodeType::FencedCode || n.type == NodeType::MathBlock || n.type == NodeType::CommentBlock)
                    n.flags |= FlagUnclosed;
                st.subEnd[size_t(nw)] = A + F;
            }
            if (isListType(n.type))
                touchedLists.push_back(nw);
        }
    }
    // Clones (pause).
    for (int j = 0; j < C; ++j) {
        const OldInfo& o = oldInfo[size_t(j)];
        BlockNode n = o.node;
        n.fromLine = S;
        n.toLine = o.node.toLine + run.delta; // > S: the node covers the pause line
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
                return target[j];
        Q_ASSERT_X(p0 == 0, "SyntaxTree::update", "suffix node parented outside the covering chain");
        return 0;
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

    // Tightness of affected lists (see the structural comparison above).
    st.text = newText;
    for (int id : chain)
        htmlInRegion = htmlInRegion || st.nodes[size_t(id)].type == NodeType::HtmlBlock;
    if (structural || htmlInRegion || end == End::Pause) {
        std::sort(touchedLists.begin(), touchedLists.end());
        touchedLists.erase(std::unique(touchedLists.begin(), touchedLists.end()), touchedLists.end());
        for (int id : touchedLists)
            if (id < total && isListType(st.nodes[size_t(id)].type))
                computeTightness(st, id);
    }

    // Frontier.
    if (end == End::Pause) {
        st.frontier = S;
        st.frontierState = pauseState;
        st.lastSplit = std::max(S, run.lastSplit); // older splits past S stay
    } else if (end == End::Complete || run.frontier < 0 || S > run.lastSplit) {
        st.frontier = -1;
        st.lastSplit = -1;
    } else {
        st.frontier = run.frontier; // resynced before the old frontier: it stays
        st.lastSplit = run.lastSplit;
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
    st.lastSplit = -1;
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
    int LS = st.lastSplit;
    if (LS >= 0) {
        if (LS >= run.Eold)
            LS += run.delta;
        else if (LS > run.R0)
            LS = Enew;
    }
    run.lastSplit = LS;
    run.R = restartAt0 ? 0 : run.R0;
    run.useFrontierState = false;
    if (F >= 0 && F <= run.R) {
        run.R = F;
        run.useFrontierState = st.frontier == F; // the frontier line itself was not edited
        if (!run.useFrontierState)
            return fullReparseUpdate(st, newText, oldN);
    }
    // Move back over open lookahead-sensitive leaves (paragraph / indented code): the one open at
    // the changed line must be parsed again from its start. At that start another one may be open;
    // when the leaf found before starts on this line inside a container opened on this line (a new
    // list item / quote / footnote), this unchanged line closed the older leaf plainly (its shape
    // depended only on earlier lines), so the walk stops and it is adopted "sealed". Anything else
    // (refdef / setext / table splits, lazy lines) keeps walking back.
    std::vector<int> chain;
    int prevLeaf = -1;
    while (run.R > 0 && !run.useFrontierState) {
        const quint32 sid = st.lines[size_t(run.R)].state;
        if (sid == kNoState || sid >= st.states->size())
            return fullReparseUpdate(st, newText, oldN);
        const PState& ps = st.states->get(sid);
        if (!isLookahead(ps.leaf.kind))
            break;
        if (!validChain(st, run.R, ps, chain))
            return fullReparseUpdate(st, newText, oldN);
        if (prevLeaf > 0) {
            const int container = st.parents[size_t(prevLeaf)];
            if (container > 0 && st.nodes[size_t(container)].fromLine == run.R && st.nodes[size_t(prevLeaf)].fromLine == run.R
                && st.nodes[size_t(chain.back())].toLine <= run.R) {
                run.sealedLeaf = true;
                break;
            }
        }
        const int start = st.nodes[size_t(chain.back())].fromLine;
        if (start >= run.R)
            return fullReparseUpdate(st, newText, oldN);
        prevLeaf = chain.back();
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
    if (verifyEnabled())
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
    run.lastSplit = st.lastSplit;
    run.ensureLine = -1;
    run.maxNanos = maxNanos;
    run.useFrontierState = true;
    bool ok = true;
    TreeUpdate u = runReparse(st, text, run, &ok);
    if (!ok)
        u = fullReparseUpdate(st, text, st.lineCount());
    if (verifyEnabled())
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
