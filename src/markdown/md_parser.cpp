// Line state machine: a port of cmark 0.31.2's block algorithm (blocks.c: check_open_blocks,
// open_new_blocks, add_text_to_container, finalize) plus cmark-gfm's table extension and the
// Obsidian block extensions (frontmatter, $$ math, %% comments, callouts, footnote definitions).
#include "markdown/md_parser.h"

#include "markdown/md_refdef.h"

#include <algorithm>

namespace oc::md::detail {

using scan::isSpaceOrTab;

// ---------------------------------------------------------------------------------------------------
// Ctx (cmark S_find_first_nonspace / S_advance_offset)
// ---------------------------------------------------------------------------------------------------
void Ctx::findFirstNonspace()
{
    int charsToTab = 4 - (column % 4);
    if (firstNonspace <= offset) {
        firstNonspace = offset;
        firstNonspaceColumn = column;
        while (firstNonspace < n) {
            const QChar c = s[firstNonspace];
            if (c == u' ') {
                ++firstNonspace;
                ++firstNonspaceColumn;
                if (--charsToTab == 0)
                    charsToTab = 4;
            } else if (c == u'\t') {
                ++firstNonspace;
                firstNonspaceColumn += charsToTab;
                charsToTab = 4;
            } else {
                break;
            }
        }
    }
    indent = firstNonspaceColumn - column;
    blank = firstNonspace >= n;
}

void Ctx::advanceOffset(int count, bool columns)
{
    while (count > 0 && offset < n) {
        if (s[offset] == u'\t') {
            const int charsToTab = 4 - (column % 4);
            if (columns) {
                partial = charsToTab > count;
                const int adv = std::min(count, charsToTab);
                column += adv;
                offset += partial ? 0 : 1;
                count -= adv;
            } else {
                partial = false;
                column += charsToTab;
                offset += 1;
                count -= 1;
            }
        } else {
            partial = false;
            offset += 1;
            column += 1;
            count -= 1;
        }
    }
}

namespace {

enum class ContType : quint8 { Document, Quote, List, Item, Footnote, Paragraph, Table };

ContType contTypeOf(CKind k)
{
    switch (k) {
    case CKind::Quote:
        return ContType::Quote;
    case CKind::List:
        return ContType::List;
    case CKind::Item:
        return ContType::Item;
    case CKind::Footnote:
        return ContType::Footnote;
    }
    return ContType::Document;
}

bool opaqueLeaf(LKind k)
{
    return k == LKind::Fence || k == LKind::Indented || k == LKind::Html || k == LKind::Math || k == LKind::Comment;
}

// check_open_blocks for one container. `markerFrom` receives the start of its marker.
bool matchContainer(Ctx& c, const CState& cs, int* markerFrom)
{
    c.findFirstNonspace();
    switch (cs.kind) {
    case CKind::Quote:
        if (c.indent <= 3 && c.peek(c.firstNonspace) == u'>') {
            *markerFrom = c.firstNonspace;
            c.advanceOffset(c.indent + 1, true);
            if (isSpaceOrTab(c.peek(c.offset)))
                c.advanceOffset(1, true);
            return true;
        }
        return false;
    case CKind::List:
        *markerFrom = c.pos();
        return true;
    case CKind::Item:
        *markerFrom = c.pos();
        if (c.indent >= cs.indent) {
            c.advanceOffset(cs.indent, true);
            return true;
        }
        if (c.blank && (cs.flags & CState::kHasContent)) {
            c.advanceOffset(c.firstNonspace - c.offset, false);
            return true;
        }
        return false;
    case CKind::Footnote:
        // cmark-gfm only continues over truly empty lines; any blank line is accepted here.
        *markerFrom = c.pos();
        if (c.indent >= 4) {
            c.advanceOffset(4, true);
            return true;
        }
        if (c.blank) {
            c.advanceOffset(c.firstNonspace - c.offset, false);
            return true;
        }
        return false;
    }
    return false;
}

enum class LeafMatch : quint8 { No, Yes, CloseFence };

// check_open_blocks for the open leaf (only when all containers matched).
LeafMatch checkLeaf(Ctx& c, const LState& ls)
{
    c.findFirstNonspace();
    switch (ls.kind) {
    case LKind::None:
        return LeafMatch::No;
    case LKind::Paragraph:
        return c.blank ? LeafMatch::No : LeafMatch::Yes;
    case LKind::Table:
        return !c.blank && scan::tableRowCells(c.s, c.firstNonspace) > 0 ? LeafMatch::Yes : LeafMatch::No;
    case LKind::Fence: {
        const QChar fch(ls.ch);
        int m = 0;
        if (c.indent <= 3 && c.peek(c.firstNonspace) == fch)
            m = scan::closeFence(c.s, c.firstNonspace, fch);
        if (m >= ls.b) {
            c.advanceOffset(c.firstNonspace + m - c.offset, false);
            return LeafMatch::CloseFence;
        }
        int i = ls.c;
        while (i > 0 && isSpaceOrTab(c.peek(c.offset))) {
            c.advanceOffset(1, true);
            --i;
        }
        return LeafMatch::Yes;
    }
    case LKind::Indented:
        if (c.indent >= 4) {
            c.advanceOffset(4, true);
            return LeafMatch::Yes;
        }
        if (c.blank) {
            c.advanceOffset(c.firstNonspace - c.offset, false);
            return LeafMatch::Yes;
        }
        return LeafMatch::No;
    case LKind::Html:
        return ls.a <= 5 || !c.blank ? LeafMatch::Yes : LeafMatch::No;
    case LKind::Math:
    case LKind::Comment:
    case LKind::Frontmatter:
        return LeafMatch::Yes;
    }
    return LeafMatch::No;
}

struct LoopEnv {
    ContType cont = ContType::Document;
    bool maybeLazy = false;
    bool allMatched = true;
    int depth = 0;  // containers open
    CState list;    // the deepest matched list when cont == List
    bool calloutTitle = false; // cont == Paragraph is a callout's title line: no setext / table header
};

bool listsMatch(const CState& list, const scan::ListMarker& lm)
{
    const bool ordered = list.flags & CState::kOrdered;
    return ordered == lm.ordered && list.ch == lm.ch.unicode();
}

// open_new_blocks: detection (shared by the parser and the marker analysis); effects go to the sink.
template<class Sink>
void openNewBlocks(Ctx& c, LoopEnv env, Sink& sink)
{
    for (;;) {
        c.findFirstNonspace();
        const bool indented = c.indent >= 4;
        const int fnp = c.firstNonspace;
        const QChar ch = c.peek(fnp);
        // Adding a non-item block to a list closes the list first.
        const int baseDepth = env.cont == ContType::List ? env.depth - 1 : env.depth;
        if (!indented && ch == u'>' && baseDepth < kMaxDepth) {
            c.advanceOffset(fnp + 1 - c.offset, false);
            if (isSpaceOrTab(c.peek(c.offset)))
                c.advanceOffset(1, true);
            sink.quote(fnp, c.pos());
            env = LoopEnv{ContType::Quote, false, env.allMatched, baseDepth + 1, {}};
            continue;
        }
        if (!indented) {
            if (const int level = scan::atxStart(c.s, fnp)) {
                sink.atx(level);
                return;
            }
            QChar fch;
            if (const int len = scan::openFence(c.s, fnp, &fch)) {
                const int fenceOffset = fnp - c.offset;
                const int fenceIndent = c.indent;
                c.advanceOffset(fnp + len - c.offset, false);
                sink.fence(fch, len, fenceOffset, fenceIndent, fnp);
                return;
            }
            if (ch == u'$') {
                const scan::DelimBlock m = scan::mathStart(c.s, fnp);
                if (m != scan::DelimBlock::None) {
                    sink.math(m == scan::DelimBlock::OneLine);
                    return;
                }
            }
            if (ch == u'%' && scan::commentStart(c.s, fnp) == scan::DelimBlock::Open) {
                sink.comment();
                return;
            }
            if (ch == u'<') {
                if (const int kind = scan::htmlBlockStart(c.s, fnp, env.cont != ContType::Paragraph)) {
                    sink.html(kind, fnp);
                    return;
                }
            }
            if (env.cont == ContType::Paragraph && !env.calloutTitle) {
                if (const int level = scan::setextLine(c.s, fnp)) {
                    sink.setext(level);
                    return;
                }
            }
            if (!(env.cont == ContType::Paragraph && !env.allMatched) && scan::thematicBreak(c.s, fnp)) {
                sink.thematic();
                return;
            }
            if (baseDepth < kMaxDepth) {
                int labelFrom = 0, labelTo = 0;
                if (const int m = scan::footnoteDef(c.s, fnp, &labelFrom, &labelTo)) {
                    c.advanceOffset(fnp + m - c.offset, false);
                    sink.footnote(fnp, labelFrom, labelTo, c.pos());
                    env = LoopEnv{ContType::Footnote, false, env.allMatched, baseDepth + 1, {}};
                    continue;
                }
            }
            scan::ListMarker lm;
            if (const int m = scan::listMarker(c.s, fnp, env.cont == ContType::Paragraph, &lm)) {
                const bool joins = env.cont == ContType::List && listsMatch(env.list, lm);
                const int need = joins ? env.depth + 1 : baseDepth + 2;
                if (need <= kMaxDepth) {
                    const int markerOffset = c.indent;
                    c.advanceOffset(fnp + m - c.offset, false);
                    const bool savePartial = c.partial;
                    const int saveOffset = c.offset, saveColumn = c.column;
                    while (c.column - saveColumn <= 5 && isSpaceOrTab(c.peek(c.offset)))
                        c.advanceOffset(1, true);
                    const int i = c.column - saveColumn;
                    int padding = 0;
                    if (i >= 5 || i < 1 || c.offset >= c.n) {
                        padding = m + 1;
                        c.offset = saveOffset;
                        c.column = saveColumn;
                        c.partial = savePartial;
                        if (i > 0)
                            c.advanceOffset(1, true);
                    } else {
                        padding = m + i;
                    }
                    sink.listItem(lm, fnp, fnp + m, c.pos(), markerOffset + padding, !joins, c.offset);
                    env = LoopEnv{ContType::Item, false, env.allMatched, need, {}};
                    continue;
                }
            }
        }
        if (indented && !env.maybeLazy && !c.blank) {
            c.advanceOffset(4, true);
            sink.indented(c.pos());
            return;
        }
        if (!indented && env.cont == ContType::Paragraph && !env.calloutTitle && scan::tableDelimiterRow(c.s, fnp, nullptr)) {
            sink.table(fnp);
            return;
        }
        return;
    }
}

// Records container markers only (containerMarkers()).
struct DrySink {
    QVarLengthArray<MarkerRec, 8>& out;
    int level;
    void quote(int from, int to) { out.push_back({ContainerMarker::Kind::Quote, from, to, ++level, true}); }
    void footnote(int from, int, int, int to) { out.push_back({ContainerMarker::Kind::FootnoteLabel, from, to, ++level, true}); }
    void listItem(const scan::ListMarker&, int from, int, int to, int, bool, int)
    {
        out.push_back({ContainerMarker::Kind::ListMarker, from, to, ++level, true});
    }
    void atx(int) {}
    void fence(QChar, int, int, int, int) {}
    void math(bool) {}
    void comment() {}
    void html(int, int) {}
    void setext(int) {}
    void thematic() {}
    void indented(int) {}
    void table(int) {}
};

QString firstWordLower(const QString& info)
{
    qsizetype e = 0;
    while (e < info.size() && !info[e].isSpace())
        ++e;
    return info.left(e).toLower();
}

// Collects link reference definitions and the first content line of a paragraph.
class RefCollector : public RefSink {
public:
    explicit RefCollector(QList<LinkRefDefinition>* out) : m_out(out) {}
    void refdefCompleted(const LinkRefDefinition& def) override
    {
        if (m_out)
            m_out->push_back(def);
    }
    void contentFrom(int line) override
    {
        if (m_content < 0)
            m_content = line;
    }
    int content() const { return m_content; }

private:
    QList<LinkRefDefinition>* m_out;
    int m_content = -1;
};

} // namespace

// ---------------------------------------------------------------------------------------------------
// RealSink: open_new_blocks effects
// ---------------------------------------------------------------------------------------------------
struct RealSink {
    LineParser& p;
    Ctx& c;
    QStringView s;

    void quote(int from, int to)
    {
        p.openQuote(from);
        p.m_col = to;
        p.m_vmd = int(p.m_stack.size());
    }
    void footnote(int, int labelFrom, int labelTo, int to)
    {
        p.openFootnote(s, labelFrom, labelTo);
        p.m_col = to;
        p.m_vmd = int(p.m_stack.size());
    }
    void listItem(const scan::ListMarker& lm, int from, int markerTo, int contentPos, int indent, bool newList, int rawOffset)
    {
        p.openListItem(s, lm, from, markerTo, contentPos, indent, newList);
        if (scan::isBlankFrom(s, rawOffset))
            p.freshNode(p.m_stack.back().node).flags |= FlagEmptyItem;
        p.m_col = contentPos;
        p.m_vmd = int(p.m_stack.size());
    }
    void atx(int)
    {
        HeadingInfo h = scan::atxInfo(s, p.m_col);
        p.m_st.headings.push_back(h);
        p.openLeafNode(NodeType::AtxHeading, LKind::None, int(p.m_st.headings.size() - 1));
        p.headingSeen = true;
        p.m_role = LineRole::AtxHeading;
        p.m_lineDone = true;
    }
    void fence(QChar fch, int len, int fenceOffset, int fenceIndent, int fnp)
    {
        FenceInfo fi;
        fi.ch = fch;
        fi.length = len;
        fi.indent = fenceIndent;
        const int n = int(s.size());
        const int infoFrom = scan::skipSpaces(s, fnp + len);
        const int infoTo = scan::rtrim(s, infoFrom, n);
        fi.infoFrom = infoFrom;
        fi.infoTo = std::max(infoFrom, infoTo);
        fi.info = scan::unescapeBackslashes(s.mid(infoFrom, fi.infoTo - infoFrom));
        fi.language = firstWordLower(fi.info);
        p.m_st.fences.push_back(std::move(fi));
        p.openLeafNode(NodeType::FencedCode, LKind::Fence, int(p.m_st.fences.size() - 1));
        p.m_leaf.st.ch = fch.unicode();
        p.m_leaf.st.b = len;
        p.m_leaf.st.c = fenceOffset;
        p.m_role = LineRole::FenceOpen;
        p.m_lineDone = true;
    }
    void math(bool oneLine)
    {
        p.openLeafNode(NodeType::MathBlock, oneLine ? LKind::None : LKind::Math);
        p.m_role = oneLine ? LineRole::MathOneLine : LineRole::MathOpen;
        p.m_lineDone = true;
    }
    void comment()
    {
        p.openLeafNode(NodeType::CommentBlock, LKind::Comment);
        p.m_role = LineRole::CommentOpen;
        p.m_lineDone = true;
    }
    void html(int kind, int fnp)
    {
        p.openLeafNode(NodeType::HtmlBlock, LKind::Html, kind);
        p.m_leaf.st.a = quint8(kind);
        p.m_role = LineRole::Html;
        p.m_lineDone = true;
        if (kind <= 5 && scan::htmlBlockEnd(kind, s, fnp)) {
            p.closeNode(p.m_leaf.node, p.m_L + 1);
            p.m_leaf = {};
            p.m_dirty = true;
        }
    }
    void setext(int level)
    {
        if (p.convertSetext(level))
            p.m_lineDone = true;
    }
    void thematic()
    {
        p.openLeafNode(NodeType::ThematicBreak, LKind::None);
        p.m_role = LineRole::ThematicBreak;
        p.m_lineDone = true;
    }
    void indented(int pos)
    {
        p.openLeafNode(NodeType::IndentedCode, LKind::Indented);
        p.m_role = LineRole::IndentedCode;
        p.m_col = pos;
        p.m_lineDone = true;
    }
    void table(int fnp)
    {
        if (p.convertTable(s, fnp))
            p.m_lineDone = true;
    }
};

// ---------------------------------------------------------------------------------------------------
// LineParser
// ---------------------------------------------------------------------------------------------------
LineParser::LineParser(Store& st, const Text& text, int fmClose, int base)
    : m_st(st)
    , m_text(text)
    , m_fmClose(fmClose)
    , m_base(base)
{
}

void LineParser::beginDocument()
{
    BlockNode doc;
    doc.type = NodeType::Document;
    doc.fromLine = 0;
    doc.toLine = m_text.lineCount();
    fresh.push_back(doc);
    freshParent.push_back(-1);
    freshSubEnd.push_back(-1);
    m_stack.clear();
    m_leaf = {};
    m_dirty = true;
}

void LineParser::beginAt(int line, const PState& state, const std::vector<int>& chain, bool sealedLeaf)
{
    Q_UNUSED(line);
    m_stack.clear();
    const size_t k = state.cs.size();
    for (size_t i = 0; i < k; ++i)
        m_stack.push_back(OpenC{state.cs[i], chain[i], nodeFrom(chain[i])});
    m_leaf = {};
    m_leafSealed = false;
    if (state.leaf.kind != LKind::None) {
        m_leaf.st = state.leaf;
        m_leaf.node = chain[k];
        m_leaf.from = nodeFrom(chain[k]);
        m_leafSealed = sealedLeaf;
    }
    // Only fresh lookahead leaves are ever finalized (update() restarts before open ones).
    Q_ASSERT(!isLookahead(m_leaf.st.kind) || m_leafSealed);
    m_dirty = true;
}

quint32 LineParser::stateId()
{
    if (m_dirty) {
        m_tmp.cs.clear();
        for (const OpenC& o : m_stack)
            m_tmp.cs.push_back(o.st);
        m_tmp.leaf = m_leaf.st;
        m_stateId = m_st.states->intern(m_tmp);
        m_dirty = false;
    }
    return m_stateId;
}

std::vector<int> LineParser::openChain() const
{
    std::vector<int> out;
    out.reserve(m_stack.size() + 1);
    for (const OpenC& o : m_stack)
        out.push_back(o.node);
    if (m_leaf.st.kind != LKind::None)
        out.push_back(m_leaf.node);
    return out;
}

int LineParser::nodeDepth(int id) const
{
    return isFresh(id) ? fresh[size_t(id - m_base)].depth : m_st.nodes[size_t(id)].depth;
}

int LineParser::nodeFrom(int id) const
{
    return isFresh(id) ? fresh[size_t(id - m_base)].fromLine : m_st.nodes[size_t(id)].fromLine;
}

LineParser::Adopted& LineParser::adoptedRec(int id)
{
    for (Adopted& a : adopted)
        if (a.id == id)
            return a;
    adopted.push_back(Adopted{id});
    return adopted.back();
}

int LineParser::addNode(NodeType t, int fromLine, quint16 flags, int data)
{
    const int parent = currentParent();
    const int depth = std::min(nodeDepth(parent) + 1, 255);
    const int id = m_base + int(fresh.size());
    BlockNode b;
    b.type = t;
    b.depth = quint8(depth);
    b.flags = flags;
    b.fromLine = fromLine;
    b.toLine = fromLine + 1;
    b.data = data;
    fresh.push_back(b);
    freshParent.push_back(parent);
    freshSubEnd.push_back(-1);
    if (!m_stack.empty() && m_stack.back().st.kind == CKind::Item && !(m_stack.back().st.flags & CState::kHasContent)) {
        m_stack.back().st.flags |= CState::kHasContent;
        m_dirty = true;
    }
    m_newBlock = true;
    return id;
}

void LineParser::closeNode(int id, int toLine, quint16 setFlags, quint16 clearFlags)
{
    const int subEnd = m_base + int(fresh.size());
    if (isFresh(id)) {
        BlockNode& b = freshNode(id);
        b.toLine = toLine;
        b.flags = quint16((b.flags | setFlags) & ~clearFlags);
        freshSubEnd[size_t(id - m_base)] = subEnd;
        return;
    }
    Adopted& a = adoptedRec(id);
    a.toLine = toLine;
    a.subEnd = subEnd;
    a.setFlags = quint16((a.setFlags | setFlags) & ~clearFlags);
    a.clearFlags = quint16((a.clearFlags | clearFlags) & ~setFlags);
}

int LineParser::trimBack(int endLine, int from, int depth) const
{
    int e = endLine;
    while (e > from + 1) {
        const LineRec& r = m_st.lines[size_t(e - 1)];
        if (LineRole(r.role) != LineRole::Blank || r.vmd >= depth)
            break;
        --e;
    }
    return e;
}

void LineParser::commitNonLazy()
{
    if (m_committed)
        return;
    m_committed = true;
    if (m_leaf.st.kind != LKind::None && !m_leafMatched)
        closeLeaf(m_L);
    while (int(m_stack.size()) > m_matched)
        closeContainer(m_L);
}

void LineParser::closeMatchedLeafForNewBlock()
{
    if (m_leaf.st.kind != LKind::None)
        closeLeaf(m_L);
}

void LineParser::closeListIfDeepest()
{
    if (!m_stack.empty() && m_stack.back().st.kind == CKind::List)
        closeContainer(m_L);
}

void LineParser::closeContainer(int endLine)
{
    const OpenC oc = m_stack.back();
    m_stack.pop_back();
    m_dirty = true;
    closeNode(oc.node, trimBack(endLine, oc.from, nodeDepth(oc.node)));
}

void LineParser::closeLeaf(int endLine)
{
    const OpenLeaf lf = m_leaf;
    if (m_leafSealed) {
        // Already final in the old tree (its shape depended only on lines before this one).
        m_leafSealed = false;
        m_broken = m_broken || endLine != m_L;
        m_leaf = {};
        m_dirty = true;
        return;
    }
    switch (lf.st.kind) {
    case LKind::None:
        return;
    case LKind::Paragraph:
        finalizeParagraph(endLine);
        return;
    case LKind::Indented: {
        int e = endLine;
        while (e > lf.from + 1) {
            const LineRec& r = m_st.lines[size_t(e - 1)];
            if (!scan::isBlankFrom(m_text.lineView(e - 1), r.col))
                break;
            setRole(e - 1, LineRole::Blank);
            --e;
        }
        closeNode(lf.node, e);
        break;
    }
    case LKind::Fence:
    case LKind::Math:
    case LKind::Comment:
        closeNode(lf.node, endLine, FlagUnclosed);
        break;
    case LKind::Html:
    case LKind::Table:
    case LKind::Frontmatter:
        closeNode(lf.node, endLine);
        break;
    }
    m_leaf = {};
    m_dirty = true;
}

int LineParser::paragraphLineStart(int line) const
{
    const LineRec& r = m_st.lines[size_t(line)];
    if (r.flags & LineRec::kLazy)
        return r.col;
    return scan::skipSpaces(m_text.lineView(line), r.col);
}

int LineParser::scanRefdefs(int from, int to, QList<LinkRefDefinition>* out) const
{
    if (from >= to)
        return to;
    {
        const QStringView s0 = m_text.lineView(from);
        const int p0 = paragraphLineStart(from);
        if (p0 >= s0.size() || s0[p0] != u'[')
            return from;
    }
    RefState st{RefMode::Start, 0, 0};
    RefScratch sc;
    RefCollector sink(out);
    for (int l = from; l < to && st.mode != RefMode::Content; ++l)
        refFeed(st, sc, m_text.lineView(l), paragraphLineStart(l), l, sink);
    if (st.mode != RefMode::Content)
        refFinish(st, sc, sink);
    return sink.content() < 0 ? to : std::min(sink.content(), to);
}

void LineParser::finalizeParagraph(int endLine)
{
    const int P = m_leaf.from;
    const int id = m_leaf.node;
    m_leaf = {};
    m_dirty = true;
    const int C = scanRefdefs(P, endLine, &refdefs);
    if (C > P) {
        for (int l = P; l < C; ++l)
            setRole(l, LineRole::LinkRefDef);
        freshNode(id).type = NodeType::LinkRefDef;
        closeNode(id, C);
        if (C < endLine) {
            const int p2 = addNode(NodeType::Paragraph, C);
            closeNode(p2, endLine);
        }
        return;
    }
    closeNode(id, endLine);
}

bool LineParser::convertSetext(int level)
{
    if (m_leafSealed) {
        m_broken = true; // a sealed leaf is closed by its line, never converted
        return false;
    }
    const int P = m_leaf.from;
    QList<LinkRefDefinition> defs;
    const int C = scanRefdefs(P, m_L, &defs);
    if (C >= m_L)
        return false; // only link reference definitions: the underline is paragraph text
    refdefs += defs;
    const int id = m_leaf.node;
    m_leaf = {};
    m_dirty = true;
    HeadingInfo h;
    h.level = level;
    const QStringView first = m_text.lineView(C);
    h.textFrom = scan::skipSpaces(first, m_st.lines[size_t(C)].col);
    h.textTo = std::max(h.textFrom, scan::rtrim(first, 0, int(first.size())));
    m_st.headings.push_back(h);
    const int data = int(m_st.headings.size() - 1);
    int hid = id;
    if (C > P) {
        for (int l = P; l < C; ++l)
            setRole(l, LineRole::LinkRefDef);
        freshNode(id).type = NodeType::LinkRefDef;
        closeNode(id, C);
        hid = addNode(NodeType::SetextHeading, C, 0, data);
    } else {
        freshNode(id).type = NodeType::SetextHeading;
        freshNode(id).data = data;
    }
    closeNode(hid, m_L + 1);
    for (int l = C; l < m_L; ++l)
        setRole(l, LineRole::SetextText);
    m_role = LineRole::SetextUnderline;
    headingSeen = true;
    m_newBlock = true;
    return true;
}

bool LineParser::convertTable(QStringView s, int delimPos)
{
    if (m_leafSealed) {
        m_broken = true;
        return false;
    }
    if (m_leaf.tableVisited)
        return false;
    const int H = m_L - 1;
    const int delimCells = scan::tableRowCells(s, delimPos);
    const int headerCells = scan::tableRowCells(m_text.lineView(H), paragraphLineStart(H));
    if (delimCells <= 0 || headerCells != delimCells) {
        m_leaf.tableVisited = true;
        return false;
    }
    TableInfo ti;
    ti.columns = delimCells;
    scan::tableAlignments(s, delimPos, &ti.align);
    m_st.tables.push_back(std::move(ti));
    const int data = int(m_st.tables.size() - 1);
    const int P = m_leaf.from;
    int tid = m_leaf.node;
    if (P < H) {
        finalizeParagraph(H);
        tid = addNode(NodeType::Table, H, 0, data);
    } else {
        freshNode(tid).type = NodeType::Table;
        freshNode(tid).data = data;
    }
    m_leaf = {};
    m_leaf.st.kind = LKind::Table;
    m_leaf.node = tid;
    m_leaf.from = H;
    m_dirty = true;
    setRole(H, LineRole::TableHeader);
    m_role = LineRole::TableDelimiter;
    m_newBlock = true;
    return true;
}

void LineParser::openQuote(int)
{
    commitNonLazy();
    closeMatchedLeafForNewBlock();
    closeListIfDeepest();
    const int id = addNode(NodeType::BlockQuote, m_L);
    CState cs;
    cs.kind = CKind::Quote;
    m_stack.push_back(OpenC{cs, id, m_L});
    m_dirty = true;
}

void LineParser::openFootnote(QStringView s, int labelFrom, int labelTo)
{
    commitNonLazy();
    closeMatchedLeafForNewBlock();
    closeListIfDeepest();
    FootnoteDefInfo fi;
    fi.label = s.mid(labelFrom, labelTo - labelFrom).toString();
    m_st.footnotes.push_back(std::move(fi));
    const int id = addNode(NodeType::FootnoteDef, m_L, 0, int(m_st.footnotes.size() - 1));
    CState cs;
    cs.kind = CKind::Footnote;
    m_stack.push_back(OpenC{cs, id, m_L});
    m_dirty = true;
}

void LineParser::openListItem(QStringView, const scan::ListMarker& lm, int markerFrom, int markerTo, int contentPos, int indent,
                              bool newList)
{
    commitNonLazy();
    closeMatchedLeafForNewBlock();
    if (newList) {
        closeListIfDeepest();
        ListInfo li;
        li.ordered = lm.ordered;
        li.start = lm.ordered ? lm.start : 1;
        li.delimiter = lm.ch;
        m_st.lists.push_back(li);
        const int lid = addNode(lm.ordered ? NodeType::OrderedList : NodeType::BulletList, m_L, FlagTight, int(m_st.lists.size() - 1));
        CState cs;
        cs.kind = CKind::List;
        cs.flags = lm.ordered ? CState::kOrdered : 0;
        cs.ch = lm.ch.unicode();
        m_stack.push_back(OpenC{cs, lid, m_L});
    }
    ListItemInfo ii;
    ii.bullet = lm.ch;
    ii.number = lm.ordered ? lm.start : 0;
    ii.markerFrom = markerFrom;
    ii.markerTo = markerTo;
    ii.contentCol = contentPos;
    m_st.items.push_back(ii);
    const int id = addNode(NodeType::ListItem, m_L, 0, int(m_st.items.size() - 1));
    CState cs;
    cs.kind = CKind::Item;
    cs.indent = indent;
    m_stack.push_back(OpenC{cs, id, m_L});
    m_dirty = true;
}

void LineParser::openLeafNode(NodeType t, LKind k, int data)
{
    commitNonLazy();
    closeMatchedLeafForNewBlock();
    closeListIfDeepest();
    const int id = addNode(t, m_L, 0, data);
    if (k == LKind::None) {
        closeNode(id, m_L + 1);
        return;
    }
    m_leaf = {};
    m_leaf.st.kind = k;
    m_leaf.node = id;
    m_leaf.from = m_L;
    m_dirty = true;
}

void LineParser::startParagraph(QStringView s, int pos)
{
    closeListIfDeepest();
    if (!m_stack.empty() && m_stack.back().st.kind == CKind::Quote && m_stack.back().from == m_L && isFresh(m_stack.back().node)) {
        scan::CalloutScan cs;
        if (scan::calloutHeader(s, pos, &cs)) {
            BlockNode& q = freshNode(m_stack.back().node);
            CalloutInfo ci;
            ci.type = s.mid(cs.typeFrom, cs.typeTo - cs.typeFrom).toString().trimmed().toLower();
            if (cs.metaFrom >= 0)
                ci.metadata = s.mid(cs.metaFrom, cs.metaTo - cs.metaFrom).toString().trimmed();
            ci.fold = cs.fold;
            ci.markerFrom = cs.markerFrom;
            ci.markerTo = cs.markerTo;
            ci.titleFrom = cs.titleFrom;
            ci.titleTo = std::max(cs.titleFrom, cs.titleTo);
            m_st.callouts.push_back(std::move(ci));
            q.type = NodeType::Callout;
            q.data = int(m_st.callouts.size() - 1);
            if (!cs.fold.isNull())
                q.flags |= FlagFoldable;
            if (cs.fold == u'-')
                q.flags |= FlagFolded;
            // The title is a paragraph that stays open, so the next line can continue it (lazily
            // too) as in CommonMark; that continuation becomes the callout's first body paragraph
            // (splitCalloutTitle()). It never becomes a setext heading or a table header.
            const int id = addNode(NodeType::Paragraph, m_L);
            m_leaf = {};
            m_leaf.st.kind = LKind::Paragraph;
            m_leaf.st.a = LState::kCalloutTitle;
            m_leaf.node = id;
            m_leaf.from = m_L;
            m_dirty = true;
            m_role = LineRole::CalloutHeader;
            return;
        }
    }
    if (!m_stack.empty() && m_stack.back().st.kind == CKind::Item && m_stack.back().from == m_L && isFresh(m_stack.back().node)) {
        const QChar t = scan::taskMarker(s, pos);
        if (!t.isNull()) {
            BlockNode& item = freshNode(m_stack.back().node);
            item.flags |= FlagTask;
            if (t != u' ')
                item.flags |= FlagTaskChecked;
            ListItemInfo& ii = m_st.items[size_t(item.data)];
            ii.taskChar = t;
            ii.taskFrom = pos;
        }
    }
    const int id = addNode(NodeType::Paragraph, m_L);
    m_leaf = {};
    m_leaf.st.kind = LKind::Paragraph;
    m_leaf.node = id;
    m_leaf.from = m_L;
    m_dirty = true;
    m_role = LineRole::Paragraph;
}

void LineParser::splitCalloutTitle()
{
    if (m_leafSealed) {
        m_broken = true; // a sealed leaf is closed by its line, never continued
        return;
    }
    if (m_leaf.st.kind != LKind::Paragraph || m_leaf.st.a != LState::kCalloutTitle)
        return;
    closeNode(m_leaf.node, m_L);
    m_leaf.node = addNode(NodeType::Paragraph, m_L);
    m_leaf.from = m_L;
    m_leaf.st.a = 0;
    m_dirty = true;
}

void LineParser::parseLine(int L)
{
    parseLineImpl(L);
    if (m_leafSealed) {
        m_leafSealed = false; // continued: the old parse did not close it here after all
        m_broken = true;
    }
}

void LineParser::parseLineImpl(int L)
{
    const QStringView s = m_text.lineView(L);
    LineRec& rec = m_st.lines[size_t(L)];
    rec.state = stateId();
    rec.flags = 0;
    rec.vmd = 0;
    m_vmd = 0;
    m_L = L;
    m_matched = 0;
    m_leafMatched = false;
    m_committed = false;
    m_newBlock = false;
    m_lineDone = false;
    m_role = LineRole::Blank;
    m_col = 0;

    // Frontmatter (decided up front for the whole document).
    if (m_leaf.st.kind == LKind::Frontmatter) {
        if (scan::isFrontmatterDelim(s)) {
            closeNode(m_leaf.node, L + 1);
            m_leaf = {};
            m_dirty = true;
            rec.role = quint8(LineRole::FrontmatterClose);
        } else {
            rec.role = quint8(LineRole::FrontmatterBody);
        }
        rec.col = 0;
        return;
    }
    if (L == 0 && m_fmClose > 0 && m_stack.empty() && m_leaf.st.kind == LKind::None) {
        const int id = addNode(NodeType::Frontmatter, 0);
        m_leaf.st.kind = LKind::Frontmatter;
        m_leaf.node = id;
        m_leaf.from = 0;
        m_dirty = true;
        rec.role = quint8(LineRole::FrontmatterOpen);
        rec.col = 0;
        return;
    }

    Ctx c(s);
    // 1. containers
    int markerFrom = 0;
    while (m_matched < int(m_stack.size()) && matchContainer(c, m_stack[size_t(m_matched)].st, &markerFrom)) {
        if (m_stack[size_t(m_matched)].st.kind == CKind::Quote)
            m_vmd = m_matched + 1;
        ++m_matched;
    }
    rec.vmd = quint8(std::min(m_vmd, 255));
    const bool allMatched = m_matched == int(m_stack.size());
    const int contCol0 = c.pos();
    m_col = contCol0;

    // 2. the open leaf
    if (allMatched && m_leaf.st.kind != LKind::None) {
        const LeafMatch lm = checkLeaf(c, m_leaf.st);
        if (lm == LeafMatch::CloseFence) {
            closeNode(m_leaf.node, L + 1, 0, FlagUnclosed);
            m_leaf = {};
            m_dirty = true;
            rec.role = quint8(LineRole::FenceClose);
            rec.col = contCol0;
            return;
        }
        m_leafMatched = lm == LeafMatch::Yes;
    }

    // 3. new blocks
    const LKind lk = m_leaf.st.kind;
    if (!(m_leafMatched && opaqueLeaf(lk))) {
        LoopEnv env;
        if (m_leafMatched)
            env.cont = lk == LKind::Paragraph ? ContType::Paragraph : ContType::Table;
        else
            env.cont = m_matched > 0 ? contTypeOf(m_stack[size_t(m_matched - 1)].st.kind) : ContType::Document;
        env.maybeLazy = lk == LKind::Paragraph;
        env.calloutTitle = env.cont == ContType::Paragraph && m_leaf.st.a == LState::kCalloutTitle;
        env.allMatched = allMatched;
        env.depth = m_matched;
        if (env.cont == ContType::List)
            env.list = m_stack[size_t(m_matched - 1)].st;
        RealSink sink{*this, c, s};
        openNewBlocks(c, env, sink);
        rec.vmd = quint8(std::min(m_vmd, 255));
        if (m_lineDone) {
            rec.role = quint8(m_role);
            rec.col = m_col;
            return;
        }
    }

    // 4. add_text_to_container
    c.findFirstNonspace();
    if (!m_newBlock && lk == LKind::Paragraph && !m_leafMatched && !c.blank) {
        // Lazy continuation line. cmark keeps its leading whitespace (a partially consumed tab
        // included) in the paragraph content, which matters for refdefs and table header rows.
        splitCalloutTitle();
        rec.flags |= LineRec::kLazy;
        rec.role = quint8(LineRole::Paragraph);
        rec.col = c.offset;
        return;
    }
    commitNonLazy();
    if (!m_newBlock && m_leafMatched) {
        switch (lk) {
        case LKind::Paragraph:
            splitCalloutTitle();
            m_role = LineRole::Paragraph;
            break;
        case LKind::Table:
            m_role = LineRole::TableRow;
            break;
        case LKind::Fence:
            m_role = LineRole::FenceBody;
            m_col = c.pos();
            break;
        case LKind::Indented:
            m_role = LineRole::IndentedCode;
            m_col = c.pos();
            break;
        case LKind::Html:
            m_role = LineRole::Html;
            if (m_leaf.st.a <= 5 && scan::htmlBlockEnd(m_leaf.st.a, s, c.firstNonspace)) {
                closeNode(m_leaf.node, L + 1);
                m_leaf = {};
                m_dirty = true;
            }
            break;
        case LKind::Math:
            m_role = LineRole::MathBody;
            if (scan::findPair(s, contCol0, u'$') >= 0) {
                m_role = LineRole::MathClose;
                closeNode(m_leaf.node, L + 1, 0, FlagUnclosed);
                m_leaf = {};
                m_dirty = true;
            }
            break;
        case LKind::Comment:
            m_role = LineRole::CommentBody;
            if (scan::findPair(s, contCol0, u'%') >= 0) {
                m_role = LineRole::CommentClose;
                closeNode(m_leaf.node, L + 1, 0, FlagUnclosed);
                m_leaf = {};
                m_dirty = true;
            }
            break;
        default:
            break;
        }
    } else if (c.blank) {
        m_role = LineRole::Blank;
    } else {
        startParagraph(s, c.firstNonspace);
    }
    rec.role = quint8(m_role);
    rec.col = m_col;
}

void LineParser::checkpoint(int line, Checkpoint& cp) const
{
    cp.line = line;
    cp.fresh = fresh.size();
    cp.leafNode = m_leaf.node;
    cp.stack = m_stack;
    cp.headingSeen = headingSeen;
    cp.oldRec = m_st.lines[size_t(line)];
}

bool LineParser::closedLookaheadPlainly(const Checkpoint& cp) const
{
    // The checkpoint's leaf is fresh (update() restarts before any open lookahead leaf).
    if (!isFresh(cp.leafNode) || m_leaf.node == cp.leafNode)
        return false;
    const size_t x = size_t(cp.leafNode - m_base);
    const NodeType t = fresh[x].type;
    if ((t != NodeType::Paragraph && t != NodeType::LinkRefDef && t != NodeType::IndentedCode) || freshSubEnd[x] < 0 || fresh[x].toLine > cp.line)
        return false;
    // Line L only finalized that leaf (nodes ending by L) and opened new blocks (nodes starting at L).
    for (size_t i = cp.fresh; i < fresh.size(); ++i) {
        const BlockNode& n = fresh[i];
        if (n.fromLine != cp.line && (freshSubEnd[i] < 0 || n.toLine > cp.line))
            return false;
    }
    return true;
}

void LineParser::rollback(const Checkpoint& cp)
{
    size_t keep = cp.fresh;
    while (keep < fresh.size() && fresh[keep].fromLine < cp.line)
        ++keep; // finalization of the leaf (the paragraph after its refdefs); new blocks follow
    fresh.resize(keep);
    freshParent.resize(keep);
    freshSubEnd.resize(keep);
    // Containers that line L closed are open again (the splice gives them the old continuation).
    m_stack = cp.stack;
    for (const OpenC& o : m_stack) {
        if (isFresh(o.node)) {
            freshSubEnd[size_t(o.node - m_base)] = -1;
        } else {
            for (Adopted& a : adopted)
                if (a.id == o.node)
                    a.toLine = -1;
        }
    }
    m_leaf = {};
    m_dirty = true;
    headingSeen = cp.headingSeen;
    m_st.lines[size_t(cp.line)] = cp.oldRec;
}

void LineParser::finish(int endLine)
{
    m_L = endLine;
    if (m_leaf.st.kind != LKind::None)
        closeLeaf(endLine);
    while (!m_stack.empty())
        closeContainer(endLine);
}

// ---------------------------------------------------------------------------------------------------
// Marker analysis for containerMarkers()
// ---------------------------------------------------------------------------------------------------
void analyzeMarkers(QStringView line, const PState& st, QVarLengthArray<MarkerRec, 8>& out)
{
    out.clear();
    if (st.leaf.kind == LKind::Frontmatter)
        return;
    Ctx c(line);
    int matched = 0;
    for (; matched < int(st.cs.size()); ++matched) {
        const CState& cs = st.cs[size_t(matched)];
        int from = 0;
        if (!matchContainer(c, cs, &from))
            break;
        if (cs.kind == CKind::List)
            continue;
        const ContainerMarker::Kind k = cs.kind == CKind::Quote  ? ContainerMarker::Kind::Quote
                                        : cs.kind == CKind::Item ? ContainerMarker::Kind::ListIndent
                                                                 : ContainerMarker::Kind::FootnoteIndent;
        out.push_back({k, std::min(from, c.pos()), c.pos(), matched + 1, false});
    }
    const bool allMatched = matched == int(st.cs.size());
    bool leafMatched = false;
    if (allMatched && st.leaf.kind != LKind::None) {
        const LeafMatch lm = checkLeaf(c, st.leaf);
        if (lm == LeafMatch::CloseFence)
            return;
        leafMatched = lm == LeafMatch::Yes;
    }
    if (leafMatched && opaqueLeaf(st.leaf.kind))
        return;
    LoopEnv env;
    if (leafMatched)
        env.cont = st.leaf.kind == LKind::Paragraph ? ContType::Paragraph : ContType::Table;
    else
        env.cont = matched > 0 ? contTypeOf(st.cs[size_t(matched - 1)].kind) : ContType::Document;
    env.maybeLazy = st.leaf.kind == LKind::Paragraph;
    env.calloutTitle = env.cont == ContType::Paragraph && st.leaf.a == LState::kCalloutTitle;
    env.allMatched = allMatched;
    env.depth = matched;
    if (env.cont == ContType::List)
        env.list = st.cs[size_t(matched - 1)];
    DrySink sink{out, matched};
    openNewBlocks(c, env, sink);
}

} // namespace oc::md::detail
