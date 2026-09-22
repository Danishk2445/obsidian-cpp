#pragma once

// Private implementation of oc::md::SyntaxTree: the line-driven block parser, its interned per-line
// states and the incremental splice.
//
// Design
//  * The parser processes one line at a time following cmark's block algorithm (check_open_blocks /
//    open_new_blocks / add_text_to_container) plus GFM tables (cmark-gfm) and the Obsidian extensions.
//    Everything that influences how the *following* lines are parsed is kept in a PState (open
//    container stack + open leaf). PStates contain no line numbers and no node ids and are
//    hash-consed per tree, so "same state" is an integer compare.
//  * Paragraphs and indented code are "lookahead-sensitive": their final shape (setext heading,
//    table header, link reference definitions, trailing blank lines) is only known when they close.
//    Their PState therefore carries no details; the parser never resyncs nor pauses while one is
//    open, and a re-parse always starts before them.
//  * Every line stores the id of the state at its start. update() re-parses from the first changed
//    line (moved back to the start of an open lookahead-sensitive leaf, or to 0 when the frontmatter
//    decision changes), adopting the containers/leaf that are open there from the old tree, until a
//    line at/after the edit whose new start state equals the stored old one. Nodes in between are
//    replaced; the new open chain takes over the old open chain's continuation (ranges, children).
//  * Nodes are a preorder arena (ids are indices) with parent and subtree-end arrays. Container
//    ranges exclude trailing whitespace-only blank lines; leaf ranges are exact.
//  * Per-type info lives in append-only side tables indexed by BlockNode::data (garbage from
//    replaced nodes is compacted occasionally).

#include "markdown/syntax_tree.h"

#include <QElapsedTimer>
#include <QVarLengthArray>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace oc::md::detail {

enum class CKind : quint8 { Quote = 1, List = 2, Item = 3, Footnote = 4 };
enum class LKind : quint8 { None = 0, Paragraph, Fence, Indented, Html, Math, Comment, Frontmatter, Table };

// An open container, as far as parsing the following lines is concerned. 8 bytes, no padding.
struct CState {
    CKind kind = CKind::Quote;
    quint8 flags = 0;  // Item: kHasContent. List: kOrdered.
    char16_t ch = 0;   // List: bullet char or ordered delimiter (cmark lists_match)
    qint32 indent = 0; // Item: marker_offset + padding (columns, relative to the parent's content)
    static constexpr quint8 kHasContent = 1;
    static constexpr quint8 kOrdered = 2;
    bool operator==(const CState&) const = default;
};

// The open leaf block. 12 bytes, no padding.
struct LState {
    LKind kind = LKind::None;
    quint8 a = 0;     // Html: kind 1..7. Paragraph: kCalloutTitle
    static constexpr quint8 kCalloutTitle = 1; // the one-line title paragraph of a callout
    char16_t ch = 0;  // Fence: fence char
    qint32 b = 0;     // Fence: fence length
    qint32 c = 0;     // Fence: indentation of the opening fence (chars, cmark fence_offset)
    bool operator==(const LState&) const = default;
};

struct PState {
    std::vector<CState> cs;
    LState leaf;
    bool operator==(const PState&) const = default;
};

inline bool isLookahead(LKind k) { return k == LKind::Paragraph || k == LKind::Indented; }

// Hash-consing table for PStates. Ids are dense and stable for the table's lifetime.
class StateTable {
public:
    StateTable();
    quint32 intern(const PState& s);
    const PState& get(quint32 id) const { return m_states[id]; }
    size_t size() const { return m_states.size(); }

private:
    static quint64 hashOf(const PState& s);
    std::vector<PState> m_states;
    std::unordered_multimap<quint64, quint32> m_index;
};

constexpr quint32 kNoState = 0xffffffffu; // placeholder for lines inside a changed region

struct LineRec {
    quint32 state = 0; // interned parser state at the start of the line
    qint32 col = 0;    // content column (after container markers; FenceBody/IndentedCode: code start)
    quint8 role = 0;   // LineRole
    quint8 flags = 0;  // kLazy
    quint8 vmd = 0;    // depth of the deepest container with a visible marker ('>', item marker,
                       // footnote label) on this line; 0 = none. Trailing lines of a container X
                       // are trimmed while they are Blank with vmd < depth(X).
    quint8 pad = 0;
    static constexpr quint8 kLazy = 1; // lazy paragraph continuation line
};

// LRU cache of inline trees (content hash + refdef generation).
class InlineCache {
public:
    InlineTreePtr get(const QString& content, quint64 gen, const QList<LinkRefDefinition>& refs);

private:
    struct Entry {
        quint64 key = 0;
        quint64 tick = 0;
        InlineTreePtr tree;
    };
    std::mutex m_mutex;
    std::vector<Entry> m_entries;
    quint64 m_tick = 0;
    static constexpr size_t kCapacity = 256;
};

// Everything a SyntaxTree stores.
struct Store {
    Text text;                        // snapshot of the parsed text
    std::vector<BlockNode> nodes;     // preorder; [0] = Document
    std::vector<qint32> parents;      // parent node id (-1 for the Document)
    std::vector<qint32> subEnd;       // index after the node's last descendant
    std::vector<LineRec> lines;       // one per text line
    // per-type side tables (BlockNode::data)
    std::vector<HeadingInfo> headings;
    std::vector<FenceInfo> fences;
    std::vector<ListInfo> lists;
    std::vector<ListItemInfo> items;
    std::vector<CalloutInfo> callouts;
    std::vector<TableInfo> tables;
    std::vector<FootnoteDefInfo> footnotes;
    QList<LinkRefDefinition> refdefs; // document order
    quint64 refdefGen = 1;
    std::shared_ptr<StateTable> states;
    int fmClose = -1;        // frontmatter closing line, -1 when the document has none
    int frontier = -1;       // >= 0 while incomplete: lines [0, frontier) are exact
    quint32 frontierState = 0; // true parser state at `frontier`
    // caches
    mutable std::vector<int> headingIds;
    mutable bool headingsValid = false;
    mutable std::shared_ptr<InlineCache> inlineCache;

    Store();
    Store(const Store& o);
    Store& operator=(const Store& o);

    int lineCount() const { return int(lines.size()); }
    bool complete() const { return frontier < 0; }
    int firstNodeAtOrAfter(int line) const;
    int lastNodeBefore(int line) const; // last node with fromLine < line (0 = Document if none)
    int deepestAt(int line) const;
    // Open chain at the start of `line` for a stack of `depth` containers (+ leaf): node ids for
    // levels 1..depth(+1). Uses the preorder/fromLine structure (valid for exact and paused trees).
    void chainAt(int line, int depth, bool withLeaf, std::vector<int>& out) const;
    size_t sideEntries() const;
};

// --- single-line analysis ---------------------------------------------------------------------------

// Line scanning context (cmark's parser offset/column bookkeeping, 4-column tab stops).
struct Ctx {
    QStringView s;
    int n = 0;
    int offset = 0;
    int column = 0;
    int firstNonspace = 0;
    int firstNonspaceColumn = 0;
    int indent = 0;
    bool blank = false;
    bool partial = false; // partially consumed tab at `offset`

    explicit Ctx(QStringView line) : s(line), n(int(line.size())) {}
    QChar peek(int i) const { return i < n ? s[i] : QChar(u'\n'); }
    void findFirstNonspace();
    void advanceOffset(int count, bool columns);
    // Character position used for marker boundaries: a partially consumed tab belongs to the
    // container that started consuming it.
    int pos() const { return offset + (partial ? 1 : 0); }
};

// Container syntax found on a line by the dry analysis (containerMarkers()).
struct MarkerRec {
    ContainerMarker::Kind kind = ContainerMarker::Kind::Quote;
    int from = 0, to = 0;
    int level = 0;    // stack level (1-based depth) of the container
    bool opened = false; // container opened on this line (else matched continuation)
};

// Re-run the container part of the line algorithm for a stored line.
void analyzeMarkers(QStringView line, const PState& st, QVarLengthArray<MarkerRec, 8>& out);

// --- parsing entry points ---------------------------------------------------------------------------

int frontmatterClose(const Text& text); // closing line of a frontmatter block at line 0, or -1
void fullParse(Store& st, const Text& text);
TreeUpdate incrementalUpdate(Store& st, const Text& newText, const ChangeDesc& change, const ParseBudget& budget);
bool resumeParse(Store& st, const Text& text, qint64 maxNanos, TreeUpdate* out);
void computeTightness(Store& st, int listId);
bool verifyEnabled();
bool storesEquivalent(const Store& a, const Store& b, QString* why); // SyntaxTree::equivalent
QString checkInvariants(const Store& st); // "" when the arena is structurally consistent

} // namespace oc::md::detail
