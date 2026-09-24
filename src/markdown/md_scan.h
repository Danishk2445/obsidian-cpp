#pragma once

// Line-level scanners for the block parser (private to oc_markdown).
//
// All functions take a line (without terminator) and a start position `pos` (a UTF-16 offset,
// normally the first non-space character after the container prefix). They never allocate unless
// they return strings. "End of line" plays the role of cmark's '\n'.

#include "markdown/syntax_tree.h"

#include <QStringView>

namespace oc::md::scan {

inline bool isSpaceOrTab(QChar c) { return c == u' ' || c == u'\t'; }
inline bool isAsciiPunct(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x21 && u <= 0x2F) || (u >= 0x3A && u <= 0x40) || (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
}
inline bool isAsciiLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}
inline bool isAsciiDigit(QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; }

// True if s[from..] is only spaces/tabs.
bool isBlankFrom(QStringView s, int from);
// First position >= from that is not a space/tab (s.size() if none).
int skipSpaces(QStringView s, int from);
// End position after trimming trailing spaces/tabs from s[from..to).
int rtrim(QStringView s, int from, int to);

// Thematic break (***, ---, ___ with optional spaces/tabs) starting at pos.
bool thematicBreak(QStringView s, int pos);
// ATX heading opening: returns level 1..6 (0 if none); *after = position after the #s and the
// following spaces/tabs.
int atxStart(QStringView s, int pos, int* after = nullptr);
// Fenced code opening (``` or ~~~, backtick info must not contain backticks). Returns fence length.
int openFence(QStringView s, int pos, QChar* ch);
// Closing fence of char ch: returns length of the fence run when the rest is only spaces/tabs.
int closeFence(QStringView s, int pos, QChar ch);
// Setext underline: 1 for '=', 2 for '-', 0 otherwise.
int setextLine(QStringView s, int pos);
// HTML block start condition 1..7 (0 = none). allow7: kind 7 may start (not interrupting a paragraph).
int htmlBlockStart(QStringView s, int pos, bool allow7);
// End condition of HTML kinds 1..5 on this line (from pos).
bool htmlBlockEnd(int kind, QStringView s, int pos);

struct ListMarker {
    bool ordered = false;
    QChar ch;       // bullet char, or delimiter '.' / ')'
    int start = 0;  // ordered start number
    int length = 0; // marker length in chars
};
// cmark parse_list_marker: returns the marker length (0 if none).
int listMarker(QStringView s, int pos, bool interruptsParagraph, ListMarker* out);

// "[^label]:" + following spaces/tabs. Returns total length (0 if none).
int footnoteDef(QStringView s, int pos, int* labelFrom = nullptr, int* labelTo = nullptr);

struct CalloutScan {
    int markerFrom = 0, markerTo = 0; // "[!...]" plus fold char
    int typeFrom = 0, typeTo = 0;     // inside the brackets, before '|'
    int metaFrom = -1, metaTo = -1;   // after '|'
    QChar fold;
    int titleFrom = 0, titleTo = 0;
};
// Obsidian callout header ^\[!([^\]]+)\]([+-]?)(?:\s|$) at pos.
bool calloutHeader(QStringView s, int pos, CalloutScan* out = nullptr);

enum class DelimBlock { None, Open, OneLine };
// Obsidian "$$" math block opener at pos.
DelimBlock mathStart(QStringView s, int pos);
// Obsidian "%%" comment block opener at pos (never OneLine: a closed "%% .. %%" line is inline).
DelimBlock commentStart(QStringView s, int pos);
// Position of the first "cc" pair at or after from, or -1.
int findPair(QStringView s, int from, char16_t c);

// Obsidian frontmatter delimiter: exactly "---" plus optional trailing spaces/tabs.
bool isFrontmatterDelim(QStringView s);

// GFM tables (cmark-gfm row_from_string semantics). Cells split on '|' not preceded by '\'.
int tableRowCells(QStringView s, int pos);                   // 0 = not a row
bool tableDelimiterRow(QStringView s, int pos, int* columns); // scan_table_start + column count
void tableCellRanges(QStringView s, int pos, QList<TableCell>* out);
void tableAlignments(QStringView s, int pos, QList<Qt::Alignment>* out);

// Task marker "[c]" + space/tab/EOL at pos. Returns the char (null if none).
QChar taskMarker(QStringView s, int pos);

// ATX heading geometry on a line whose content (after container markers) starts at `col`.
HeadingInfo atxInfo(QStringView s, int col);

// Normalize a link label (case fold, trim, collapse internal whitespace).
QString normalizeLabel(QStringView raw);
// Remove backslash escapes before ASCII punctuation.
QString unescapeBackslashes(QStringView s);

} // namespace oc::md::scan
