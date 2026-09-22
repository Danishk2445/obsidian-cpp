#pragma once

// Internal (not frozen): pure lexical rules of the inline parser (inline_parser.cpp), exposed for unit
// tests and for renderers that need the same decoding helpers (entities, escapes, link labels).
//
// All scanners work on UTF-16 code units and return the end offset (exclusive) of the construct that
// starts at `i`, or -1 when there is none. Rules that mirror Obsidian (1.13) rather than CommonMark /
// GFM say so; see tests/markdown/inline_obsidian_test.cpp for the verified and uncertain cases.

#include "markdown/inline_tree.h"

#include <QString>
#include <QStringView>

namespace oc::md::rules {

// InlineNode::flags set by parseInline() (internal; the frozen header leaves flags unspecified).
enum InlineNodeFlag : quint16 {
    FlagUrlWww = 1 << 0,   // BareUrl "www.x": the href is "http://" + text
    FlagUrlEmail = 1 << 1, // BareUrl / Autolink holding a bare email address: the href is "mailto:" + text
};

// ---- character classes ------------------------------------------------------------------------
inline bool isAsciiAlpha(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
inline bool isAsciiDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
inline bool isAsciiAlnum(char16_t c) { return isAsciiAlpha(c) || isAsciiDigit(c); }
inline bool isAsciiPunct(char16_t c)
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}
// JavaScript's \s (what Obsidian's tokenizers use for tags, math and block ids).
bool isJsSpace(char16_t c);
// CommonMark 0.31.2 "Unicode whitespace" (Zs + tab, LF, FF, CR) / "Unicode punctuation" (P* or S*).
bool isUnicodeWhitespace(char32_t c);
bool isUnicodePunctuation(char32_t c);
// Code point before / at a UTF-16 offset (surrogate pairs combined). Out of range -> '\n'.
char32_t codePointBefore(QStringView s, qsizetype i);
char32_t codePointAt(QStringView s, qsizetype i);

// ---- Obsidian constructs ----------------------------------------------------------------------
// Tag body character (Obsidian: anything but whitespace, U+2000-206F, U+2E00-2E7F and ASCII
// punctuation other than '-', '_' and '/').
bool isTagChar(char16_t c);
// "#tag" at i (s[i] == '#'). The caller checks the preceding character (start of content or JS
// whitespace). Needs >= 1 tag char and at least one character that is not an ASCII digit.
qsizetype scanTag(QStringView s, qsizetype i);

// "$...$" at i (Obsidian editor / HyperMD rule): the opening '$' must be followed by a character that
// is neither whitespace nor '$'; the first '$' after it on the same line that is not preceded by a
// backslash is the only closing candidate, and it must not be preceded by whitespace nor followed by an
// ASCII digit. Returns the end of the closing '$'.
qsizetype scanInlineMath(QStringView s, qsizetype i);
// "$$...$$" at i: up to the next "$$" (backslash-escaped characters skipped); may span lines.
qsizetype scanDisplayMath(QStringView s, qsizetype i);

// "[[...]]" / "![[...]]" at i: single line, raw inner text of >= 1 character up to the first "]]", no
// "[[" inside. On success fills the parts (offsets into s).
struct WikiParts {
    qsizetype open = 0;       // "[[" or "![[" start
    qsizetype innerFrom = 0;  // after the opening brackets
    qsizetype innerTo = 0;    // before "]]"
    qsizetype pipeFrom = -1;  // alias separator: '|' or "\|" (table cells), -1 when there is no alias
    qsizetype pipeTo = -1;
    qsizetype end = 0;        // after "]]"
};
qsizetype scanWikiLink(QStringView s, qsizetype i, WikiParts* parts);
// Obsidian's split of the inner text: first '|' (not at the start of the trimmed text) = alias; a '\'
// right before it is dropped; first '#' = subpath; target trimmed (NBSP -> space, NFC).
WikiLinkInfo wikiLinkInfo(QStringView s, const WikiParts& parts, bool embed);
// Obsidian image size "100" / "100x200" (spaces allowed around the numbers).
bool parseImageSize(QStringView s, int* width, int* height);
// Size suffix of an alias / alt text: "...|100x200" or a whole "100x200".
bool parseSizeSuffix(QStringView alt, int* width, int* height);

// ---- GFM extended autolinks (cmark-gfm autolink extension rules) --------------------------------
// "www." at i; the caller checks the preceding character (start, whitespace or one of "*_~(").
qsizetype scanWwwAutolink(QStringView s, qsizetype i);
// "http://", "https://" or "ftp://" (any case) at i; the caller checks that s[i-1] is not a letter.
qsizetype scanSchemeAutolink(QStringView s, qsizetype i);
// Email local part char: ASCII alnum or one of ".+-_".
inline bool isEmailLocalChar(char16_t c) { return isAsciiAlnum(c) || c == u'.' || c == u'+' || c == u'-' || c == u'_'; }
// Email autolink starting at i (optionally "mailto:" / "xmpp:" prefixed); the caller ensures i is the
// start of the local part run.
qsizetype scanEmailAutolink(QStringView s, qsizetype i);

// ---- CommonMark ---------------------------------------------------------------------------------
// "<scheme:...>" or "<email>" at i. `*email` tells which.
qsizetype scanAutolink(QStringView s, qsizetype i, bool* email);
// Raw HTML (open/closing tag, comment, processing instruction, declaration, CDATA) at i. `memo` (optional,
// one per content string, calls with increasing i) remembers terminators that do not occur any more so
// repeated unterminated constructs stay linear.
struct HtmlScanMemo {
    qsizetype noCommentEnd = -1; // "-->" absent from this offset on (-1 = unknown)
    qsizetype noPiEnd = -1;      // "?>"
    qsizetype noCdataEnd = -1;   // "]]>"
    qsizetype noGreater = -1;    // ">"
    qsizetype noDquote = -1;     // '"'
    qsizetype noSquote = -1;     // '\''
};
qsizetype scanHtmlTag(QStringView s, qsizetype i, HtmlTagInfo* info, HtmlScanMemo* memo = nullptr);
// "&name;", "&#123;", "&#x1F;" at i (named ones must be HTML5 entities).
qsizetype scanEntity(QStringView s, qsizetype i);
// Decoded text of an entity reference ("&amp;" -> "&"); invalid numeric references -> U+FFFD.
QString decodeEntity(QStringView ref);
bool lookupEntity(QStringView name, char32_t* cp1, char32_t* cp2);
// Process backslash escapes and entity references (link destinations and titles).
QString unescapeString(QStringView s);

// Inline link destination at i (after optional whitespace): "<...>" or a raw destination with balanced
// parentheses. Returns the end; `from/to` delimit the destination text (without '<' '>').
qsizetype scanLinkDestination(QStringView s, qsizetype i, qsizetype* from, qsizetype* to, bool* pointy);
// Link title ("..." / '...' / (...)) at i. Returns the end; `from/to` delimit the text.
qsizetype scanLinkTitle(QStringView s, qsizetype i, qsizetype* from, qsizetype* to);
// Link label "[...]" at i (<= 999 chars, no unescaped brackets). Returns the end; `from/to` delimit
// the raw label text.
qsizetype scanLinkLabel(QStringView s, qsizetype i, qsizetype* from, qsizetype* to);
// CommonMark label matching key: Unicode full case fold, whitespace runs collapsed to one space, trimmed.
// LinkRefDefinition::label should be normalized with this function.
QString normalizeLinkLabel(QStringView label);

} // namespace oc::md::rules
