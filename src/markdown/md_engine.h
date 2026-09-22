#pragma once

// Private implementation of oc::md::SyntaxTree: the line-driven block parser, its interned
// per-line states, and the incremental splice.
//
// Design (see also docs/INTERFACES.md):
//  * The parser processes one line at a time following cmark's block algorithm
//    (check_open_blocks / open_new_blocks / add_text_to_container) plus GFM tables and the
//    Obsidian extensions. Everything that influences how the *following* lines are parsed is kept
//    in a PState (open container stack + open leaf context). PStates contain no line numbers and
//    no node ids and are hash-consed, so "same state" is an integer compare.
//  * Every line stores the id of the state at its start. An edit re-parses from the first
//    changed line (with the open nodes of that line *adopted* from the old tree) until a line at
//    or after the edit is reached whose new start state equals the stored old one; from there on
//    the old result is reused (shifted).
//  * Node ranges are derived, not stored in the state: a container ends at the line where it was
//    closed minus trailing blank lines that carry none of its (or its descendants') markers.
//  * Per-type information (heading/fence/list/callout geometry...) is recomputed on demand from
//    the stored text snapshot, the per-line content column and, when needed, a dry re-analysis of
//    one line from its stored state.

#include "markdown/md_refdef.h"
#include "markdown/syntax_tree.h"

#include <QVarLengthArray>

#include <memory>
#include <unordered_map>
#include <vector>

namespace oc::md::detail {

enum class CKind : quint8 { Quote = 1, List = 2, Item = 3, Footnote = 4 };
enum class LKind : quint8 { None = 0, Paragraph, Fence, Indented, Html, Math, Comment, Frontmatter, Table };

// An open container, as far as parsing the following lines is concerned.
struct CState {
    CKind kind = CKind::Quote;
    quint8 flags = 0;  // Item: kHasContent. List: kOrdered.
    char16_t ch = 0;   // List: bullet char or ordered delimiter (lists_match).
    qint32 indent = 0; // Item: marker_offset + padding (columns).
    static constexpr quint8 kHasContent = 1;
    static constexpr quint8 kOrdered = 2;
    bool operator==(const CState&) const = default;
};

// The open leaf block.
struct LState {
    LKind kind = LKind::None;
    quint8 a = 0;     // Fence: fence char code; Html: kind 1..7; Paragraph: kTableVisited
    char16_t pad = 0; // (always 0, keeps the struct free of indeterminate padding)
    qint32 b = 0;     // Fence: length; Indented: pending blank lines; Paragraph: cells of the last line
    qint32 c = 0;     // Fence: indentation of the opening fence (chars, cmark fence_offset)
    RefState ref;     // Paragraph: link reference definition scanner
    static constexpr quint8 kTableVisited = 1;
    bool operator==(const LState&) const = default;
};

struct PState {
    std::vector<CState> cs;
    LState leaf;
    bool operator==(const PState&) const = default;
};

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

struct LineRec {
    quint32 state = 0; // interned parser state at the start of the line
    qint32 col = 0;    // content column (after the container markers present on the line)
    quint8 role = 0;   // LineRole
    quint8 flags = 0;  // kParaCont, kLazy
    quint8 vmd = 0;    // depth of the deepest container that has a visible marker on this line
    quint8 reserved = 0;
    static constexpr quint8 kParaCont = 1; // line continued an already open paragraph
    static constexpr quint8 kLazy = 2;     // ... as a lazy continuation line
    bool operator==(const LineRec&) const = default;
};

// Everything a SyntaxTree stores.
struct Store {
    Text text;                        // snapshot of the parsed text
    std::vector<BlockNode> nodes;     // preorder; [0] = Document
    std::vector<qint32> parents;      // parent node id (-1 for the Document)
    std::vector<LineRec> lines;       // one per text line
    QList<LinkRefDefinition> refdefs; // document order
    std::shared_ptr<StateTable> states;
    int validUpTo = 1;   // lines [0, validUpTo) are exact
    int dirtyEnd = 1;    // >= validUpTo: stored data from here on is a consistent (old) parse suffix
    quint32 frontierState = 0; // parser state at validUpTo while incomplete
    bool fmOpen = false; // frontmatter decision for `text`
    // cached heading ids
    mutable std::vector<int> headingIds;
    mutable bool headingsValid = false;

    Store();
    Store(const Store& o);
    Store& operator=(const Store& o);
    bool complete() const { return validUpTo >= int(lines.size()); }
    int firstNodeAtOrAfter(int line) const;
    int deepestAt(int line) const;
};

// --- single-line analysis (shared by the parser and the on-demand queries) ------------------------

struct MarkerRec {
    ContainerMarker::Kind kind = ContainerMarker::Kind::Quote;
    int from = 0, to = 0;
    int level = 0; // stack index of the container (depth - 1)
};

struct OpenRec {
    CKind kind = CKind::Quote;
    NodeType type = NodeType::BlockQuote;
    quint16 flags = 0; // node flags (task/empty/foldable/folded)
    CState st;
    // list / item
    bool ordered = false;
    QChar ch;
    int start = 0;
    int markerFrom = 0, markerTo = 0, contentCol = 0;
    QChar task;
    int taskFrom = -1;
    // footnote
    int labelFrom = 0, labelTo = 0;
};

enum class Act : quint8 {
    None,
    FmOpen, FmBody, FmClose,
    FenceClose,     // closing fence line of the open fence
    Continue,       // the open leaf takes the line
    Lazy,           // lazy paragraph continuation
    NewPara,
    Blank,
    Atx, Fence, Html, Math, MathOneLine, Comment, Setext, Thematic, Indented, TableConvert,
    CalloutHeader,
};

struct Analysis {
    int matched = 0;          // existing containers whose continuation markers matched
    bool leafMatched = false; // the open leaf continues on this line
    int keep = 0;             // existing containers that stay open
    QVarLengthArray<OpenRec, 4> opens;
    QVarLengthArray<MarkerRec, 8> markers;
    Act act = Act::None;
    int col = 0;       // content column
    int textPos = 0;   // first non-space character of the leaf text
    int vmd = 0;
    bool blank = false;
    QChar fenceCh;
    int fenceLen = 0, fenceIndent = 0;
    int htmlKind = 0;
    int setextLevel = 0;
    int tableCols = 0;
    bool markTableVisited = false;
};

void analyzeLine(QStringView s, int line, const PState& st, bool fmOpen, Analysis& a);

// Dry analysis of a stored line (from its stored start state).
void analyzeStoredLine(const Store& st, int line, Analysis& a);

// --- parsing entry points ---------------------------------------------------------------------------

void fullParse(Store& st, const Text& text);
TreeUpdate incrementalUpdate(Store& st, const Text& newText, const ChangeDesc& change, const ParseBudget& budget);
bool resumeParse(Store& st, const Text& text, qint64 maxNanos, TreeUpdate* out);

// Tightness per CommonMark: a list is loose if two consecutive items, or two consecutive children
// of one item, are separated by a blank line.
bool computeTight(const Store& st, int listId);

bool isFrontmatterOpen(const Text& text);

} // namespace oc::md::detail
