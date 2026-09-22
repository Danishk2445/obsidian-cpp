#pragma once

// FROZEN INTERFACE (M1) — block-level syntax tree for Obsidian-flavoured Markdown.
//
// One parser serves the editor (live preview / source styling), the reading view and the metadata
// indexer. It follows the CommonMark 0.31 block algorithm (+ GFM tables) with Obsidian extensions
// (frontmatter, $$ math blocks, %% comment blocks, callouts, footnote definitions) and is
// *incremental*: every line stores the parser state at its start, so an edit re-parses from the
// edited line (or the start of the lookahead-sensitive leaf containing it) until the state at a line
// start equals the old state for that line, then splices. See docs/INTERFACES.md for the contract.
//
// Coordinates: lines are 0-based indices into oc::Text; columns are UTF-16 offsets within a line.
// Node ids are preorder indices (0 = Document) and are only valid until the next update().

#include "text/changeset.h"
#include "text/text.h"

#include <QChar>
#include <QList>
#include <QString>

#include <memory>

namespace oc::md {

enum class NodeType : quint8 {
    Document = 0,
    // container blocks
    BlockQuote,
    Callout,      // a blockquote whose first line is "[!type]..." (replaces the BlockQuote node)
    BulletList,
    OrderedList,
    ListItem,
    FootnoteDef,  // "[^label]: ..." with 4-column continuation indent
    // leaf blocks
    Paragraph,
    AtxHeading,
    SetextHeading,
    ThematicBreak,
    FencedCode,
    IndentedCode,
    HtmlBlock,
    LinkRefDef,   // a paragraph that consisted only of link reference definitions
    Table,
    MathBlock,    // $$ ... $$
    CommentBlock, // %% ... %%
    Frontmatter,  // YAML between '---' lines at the very start of the document
};

bool isContainer(NodeType t);
bool isLeaf(NodeType t);
const char* nodeTypeName(NodeType t);

enum BlockFlag : quint16 {
    FlagUnclosed = 1 << 0,   // fence/math/comment ran to the end of its container/document
    FlagTight = 1 << 1,      // BulletList/OrderedList: tight list
    FlagTask = 1 << 2,       // ListItem: starts with a task checkbox "[c]"
    FlagTaskChecked = 1 << 3,// ListItem: checkbox char is not ' '
    FlagFoldable = 1 << 4,   // Callout: has '+' or '-'
    FlagFolded = 1 << 5,     // Callout: '-' (collapsed by default)
    FlagEmptyItem = 1 << 6,  // ListItem: marker only, no content on the first line
};

struct BlockNode {
    NodeType type = NodeType::Document;
    quint8 depth = 0;     // Document = 0
    quint16 flags = 0;
    qint32 fromLine = 0;  // first line, inclusive
    qint32 toLine = 0;    // exclusive
    qint32 data = -1;     // index into the type's side table (internal)
};

// What a line is, within the innermost leaf block covering it.
enum class LineRole : quint8 {
    Blank,
    Paragraph,
    AtxHeading,
    SetextText,
    SetextUnderline,
    ThematicBreak,
    FenceOpen,
    FenceBody,
    FenceClose,
    IndentedCode,
    Html,
    TableHeader,
    TableDelimiter,
    TableRow,
    MathOpen,     // "$$" line opening a math block (may carry content after $$)
    MathBody,
    MathClose,
    MathOneLine,  // "$$ ... $$" complete on one line as a block
    CommentOpen,
    CommentBody,
    CommentClose,
    FrontmatterOpen,
    FrontmatterBody,
    FrontmatterClose,
    LinkRefDef,
    CalloutHeader, // first line of a callout ("> [!type]+ Title")
};

const char* lineRoleName(LineRole r);

// Container syntax on a line, outermost first.
struct ContainerMarker {
    enum class Kind : quint8 {
        Quote,          // '>' plus one optional following space/tab column (from..to)
        ListMarker,     // bullet/number + delimiter + following spaces on an item's first line
        ListIndent,     // indentation consumed by a list item continuation line
        FootnoteLabel,  // "[^label]: " on a footnote definition's first line
        FootnoteIndent, // continuation indent of a footnote definition
    };
    Kind kind = Kind::Quote;
    qint32 from = 0;   // column
    qint32 to = 0;     // column, exclusive
    qint32 node = -1;  // container node id
};

struct HeadingInfo {
    int level = 0;          // 1..6
    qint32 markerFrom = 0;  // ATX: column of first '#'; setext: 0
    qint32 markerTo = 0;    // ATX: column after the '#'s and following whitespace
    qint32 closeFrom = -1;  // ATX: column where an optional closing '#' sequence (and preceding spaces) starts, else -1
    qint32 textFrom = 0;    // column of the heading text (on its line; setext: first text line)
    qint32 textTo = 0;      // end column of the heading text (trailing spaces / closing #s excluded)
};

struct FenceInfo {
    QChar ch;               // '`' or '~'
    int length = 3;
    int indent = 0;         // indentation of the opening fence (columns)
    QString info;           // full info string (trimmed)
    QString language;       // first word of info, lowercased ("" when none)
    qint32 infoFrom = 0;    // column range of the info string on the opening line
    qint32 infoTo = 0;
    bool closed = false;
};

struct ListInfo {
    bool ordered = false;
    bool tight = true;
    int start = 1;          // ordered lists
    QChar delimiter;        // ordered: '.' or ')'; bullet: '-', '*' or '+'
};

struct ListItemInfo {
    QChar bullet;           // '-', '*', '+' or the delimiter ('.' / ')') for ordered items
    int number = 0;         // ordered items
    qint32 markerFrom = 0;  // column of the marker on the item's first line
    qint32 markerTo = 0;    // column just after the marker (before the spaces)
    qint32 contentCol = 0;  // content indent column (CommonMark "W + N")
    QChar taskChar;         // ' ' / 'x' / any char, when FlagTask
    qint32 taskFrom = -1;   // column of '[' of the checkbox
};

struct CalloutInfo {
    QString type;           // lowercased, e.g. "note", "tip"
    QString metadata;       // text after '|' inside the brackets
    QChar fold;             // '+', '-' or null
    qint32 markerFrom = 0;  // column of '[' on the header line
    qint32 markerTo = 0;    // column after "]" and the fold char
    qint32 titleFrom = 0;   // title text (may be empty)
    qint32 titleTo = 0;
};

struct TableInfo {
    int columns = 0;
    QList<Qt::Alignment> align; // per column: Qt::AlignLeft / AlignHCenter / AlignRight, or 0 = default
};

struct TableCell {
    qint32 from = 0;        // column range of the cell content (trimmed)
    qint32 to = 0;
};

struct FootnoteDefInfo {
    QString label;
};

struct LinkRefDefinition {
    QString label;          // normalized (case-folded, whitespace collapsed)
    QString destination;
    QString title;
    int line = 0;
};

// Budget for incremental parsing. The parser always parses at least through `ensureLine` (e.g. the
// bottom of the viewport + 2 screens) and otherwise stops after `maxNanos`, leaving the old (shifted)
// structure below validUpTo() until resume() finishes it.
struct ParseBudget {
    int ensureLine = -1;
    qint64 maxNanos = 3'000'000;
};

struct TreeUpdate {
    int fromLine = 0;       // first line whose structure may differ (new coordinates)
    int toLine = 0;         // end of the re-parsed range (new coordinates, exclusive)
    int oldToLine = 0;      // the same end in old coordinates
    int lineDelta = 0;      // newLineCount - oldLineCount
    bool structural = false;// nodes were added/removed/retyped (not only line content)
    bool headingsChanged = false;
    bool complete = true;   // false when parsing paused before the end of the document
};

class SyntaxTree {
public:
    SyntaxTree();  // tree of an empty document
    ~SyntaxTree();
    SyntaxTree(const SyntaxTree&);
    SyntaxTree& operator=(const SyntaxTree&);

    // Parse everything.
    void parse(const Text& text);
    // Incremental update after `change` turned the previously parsed text into `newText`.
    TreeUpdate update(const Text& newText, const ChangeDesc& change, const ParseBudget& budget = {});
    // Continue a budget-limited parse. Returns true while work remains.
    bool resume(const Text& text, qint64 maxNanos, TreeUpdate* out = nullptr);
    int validUpTo() const;  // lines [0, validUpTo) are exact for the current text
    int lineCount() const;

    // --- lines -------------------------------------------------------------------------------
    LineRole role(int line) const;
    QList<ContainerMarker> containerMarkers(int line) const;
    qint32 contentColumn(int line) const;  // column where leaf content starts (after container markers)
    int leafAt(int line) const;            // leaf node covering line, or -1 (blank line between blocks)

    // --- nodes ------------------------------------------------------------------------------
    int nodeCount() const;
    const BlockNode& node(int id) const;
    int parent(int id) const;
    int firstChild(int id) const;          // -1 if none
    int nextSibling(int id) const;         // -1 if none
    int deepestAt(int line) const;         // deepest node covering the line (Document if none)
    QList<int> ancestorsAt(int line) const;// Document ... deepest
    // First node (preorder) whose fromLine >= line, or nodeCount().
    int firstNodeAtOrAfter(int line) const;
    QList<int> headings() const;           // AtxHeading/SetextHeading ids in document order

    // --- per-type data (id must be of the matching type) --------------------------------------
    HeadingInfo heading(int id) const;
    FenceInfo fence(int id) const;
    ListInfo list(int id) const;
    ListItemInfo listItem(int id) const;
    CalloutInfo callout(int id) const;
    TableInfo table(int id) const;
    QList<TableCell> tableCells(int line, const Text& text) const; // for TableHeader/TableRow lines
    FootnoteDefInfo footnoteDef(int id) const;
    int htmlBlockKind(int id) const;       // CommonMark HTML block start condition 1..7
    const QList<LinkRefDefinition>& linkRefDefs() const;
    int frontmatter() const;               // Frontmatter node id or -1

    // --- debugging / tests ------------------------------------------------------------------------
    QString dump(const Text& text) const;  // S-expression of nodes + line roles
    static bool equivalent(const SyntaxTree& a, const SyntaxTree& b, QString* why = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace oc::md
