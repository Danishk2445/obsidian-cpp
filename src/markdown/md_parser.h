#pragma once

// The line state machine (private to oc_markdown). One LineParser run parses a contiguous range of
// lines: either a whole document (base = 0, the Document is fresh node 0) or, for an incremental
// update, lines from a restart point with the open containers/leaf of that line adopted from the
// old tree (ids < base). New nodes are appended to `fresh` with final ids base + index, in preorder.

#include "markdown/md_engine.h"
#include "markdown/md_scan.h"

namespace oc::md::detail {

constexpr int kMaxDepth = 100; // container nesting limit (cmark-gfm uses 100 for lists)

class LineParser {
public:
    // Changes to a node of the old tree (id < base) made while parsing.
    struct Adopted {
        int id = 0;
        int toLine = -1; // >= 0 once closed
        int subEnd = -1;
        quint16 setFlags = 0;
        quint16 clearFlags = 0;
    };

    LineParser(Store& st, const Text& text, int fmClose, int base);

    void beginDocument();
    // chain[i] = node of stack level i + 1; chain[depth] = the open leaf node (when state has one).
    void beginAt(int line, const PState& state, const std::vector<int>& chain);

    quint32 stateId();
    bool lookaheadOpen() const { return isLookahead(m_leaf.st.kind); }
    void parseLine(int L);
    // Close every open node at `endLine` (end of document, or a budget pause).
    void finish(int endLine);

    std::vector<int> openChain() const; // levels 1..k (+ leaf), node ids
    int stackDepth() const { return int(m_stack.size()); }
    bool leafOpen() const { return m_leaf.st.kind != LKind::None; }
    int base() const { return m_base; }

    std::vector<BlockNode> fresh;
    std::vector<qint32> freshParent;
    std::vector<qint32> freshSubEnd; // -1 while open
    std::vector<Adopted> adopted;
    QList<LinkRefDefinition> refdefs; // collected, document order
    bool headingSeen = false;         // a heading node was created

private:
    struct OpenC {
        CState st;
        int node = 0;
        int from = 0;
    };
    struct OpenLeaf {
        LState st;
        int node = -1;
        int from = 0;
        bool tableVisited = false;
    };
    friend struct RealSink;

    // node helpers
    bool isFresh(int id) const { return id >= m_base; }
    BlockNode& freshNode(int id) { return fresh[size_t(id - m_base)]; }
    int nodeDepth(int id) const;
    int nodeFrom(int id) const;
    Adopted& adoptedRec(int id);
    int addNode(NodeType t, int fromLine, quint16 flags = 0, int data = -1);
    void closeNode(int id, int toLine, quint16 setFlags = 0, quint16 clearFlags = 0);
    int currentParent() const { return m_stack.empty() ? 0 : m_stack.back().node; }
    void setRole(int line, LineRole r) { m_st.lines[size_t(line)].role = quint8(r); }

    // block management
    void commitNonLazy();
    void closeLeaf(int endLine);
    void closeContainer(int endLine);
    void closeMatchedLeafForNewBlock();
    void closeListIfDeepest();
    int trimBack(int endLine, int from, int depth) const;
    void finalizeParagraph(int endLine);
    int scanRefdefs(int from, int to, QList<LinkRefDefinition>* out) const;
    int paragraphLineStart(int line) const; // content start used for refdefs / table header rows

    // open_new_blocks effects
    void openQuote(int markerFrom);
    void openFootnote(QStringView s, int labelFrom, int labelTo);
    void openListItem(QStringView s, const scan::ListMarker& lm, int markerFrom, int markerTo, int contentPos, int indent, bool newList);
    void openLeafNode(NodeType t, LKind k, int data = -1);
    bool convertSetext(int level);
    bool convertTable(QStringView s, int delimPos);
    void startParagraph(QStringView s, int pos);
    void splitCalloutTitle(); // a callout title paragraph continues: the new line starts the body

    Store& m_st;
    const Text& m_text;
    int m_fmClose = -1;
    int m_base = 0;
    std::vector<OpenC> m_stack;
    OpenLeaf m_leaf;
    PState m_tmp;
    quint32 m_stateId = 0;
    bool m_dirty = true;

    // per-line
    int m_L = 0;
    int m_matched = 0;
    bool m_leafMatched = false;
    bool m_committed = false;
    bool m_newBlock = false;  // a node was created (or converted) on this line
    bool m_lineDone = false;  // the line's role is final
    LineRole m_role = LineRole::Blank;
    int m_col = 0;
    int m_vmd = 0; // visible marker depth of the current line
};

} // namespace oc::md::detail
