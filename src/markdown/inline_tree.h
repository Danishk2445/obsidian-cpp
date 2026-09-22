#pragma once

// FROZEN INTERFACE (M2) — inline structure of a leaf block (Obsidian-flavoured Markdown).
//
// Inline parsing is lazy and per leaf: SyntaxTree::inlineTree(leaf, text) parses the leaf's *content*
// (its lines after container markers / leaf markers, joined with '\n') and caches the result by content
// hash, so moving a block around doesn't invalidate it. Node offsets are CONTENT offsets; convert with
// LeafContent::toDoc(). Hideable syntax (delimiters, brackets, targets...) is represented as child nodes
// of type Mark so live preview can hide/reveal it precisely.
//
// Precedence (earliest start wins; at the same start, in this order): backslash escape > code span >
// %%comment%% > [[wikilink]] / ![[embed]] > $$display math$$ > $inline math$ > <autolink> / raw HTML >
// bare URL > #tag; then CommonMark emphasis (* _), plus ~~strike~~ and ==highlight== (exactly two
// delimiters), links/images (inline + reference), footnote refs [^x], inline footnotes ^[..], block id
// " ^id" at the very end of the leaf, hard/soft breaks, entities.

#include "text/text.h"

#include <QList>
#include <QString>

#include <memory>

namespace oc::md {

enum class InlineType : quint8 {
    Text,
    Emphasis,
    Strong,
    Strikethrough,
    Highlight,
    CodeSpan,
    InlineMath,     // $...$
    DisplayMath,    // $$...$$ inside a paragraph
    Comment,        // %%...%%
    WikiLink,       // [[target#subpath|alias]]
    Embed,          // ![[target#subpath|alias-or-size]]
    Link,           // [text](dest "title") / [text][ref] / [text]
    Image,          // ![alt](dest) / ![alt|100x50](dest)
    Autolink,       // <https://x> / <a@b.c>
    BareUrl,        // https://x, www.x (GFM extended autolink rules)
    Tag,            // #tag or #nested/tag
    FootnoteRef,    // [^label]
    InlineFootnote, // ^[text]
    BlockId,        // ^block-id (end of the leaf)
    HtmlTag,        // <kbd>, </kbd>, <br/>, <!-- ... -->
    Escape,         // \* (child Mark = the backslash)
    Entity,         // &amp; &#123;
    HardBreak,      // two spaces / backslash before a line break
    SoftBreak,      // line break inside a paragraph
    Mark,           // hideable syntax, see MarkKind
};

enum class MarkKind : quint8 {
    None,
    EmphasisDelim,  // * _ ** __ ~~ ==
    CodeDelim,      // backtick run
    MathDelim,      // $ or $$
    CommentDelim,   // %%
    WikiOpen,       // [[  (for embeds including the '!')
    WikiClose,      // ]]
    WikiTarget,     // "target#subpath" text (hidden when an alias is displayed)
    WikiPipe,       // |
    LinkOpen,       // [ or ![
    LinkClose,      // from ']' through the closing ')' (or ']' of a reference)
    LinkUrl,        // the destination (inside LinkClose)
    EscapeChar,     // the backslash
    FootnoteDelim,  // "[^" + "]" or "^[" + "]"
    TagHash,        // '#'
    AutolinkDelim,  // < and >
    BlockIdCaret,   // '^'
};

struct InlineNode {
    InlineType type = InlineType::Text;
    MarkKind mark = MarkKind::None;
    quint16 flags = 0;
    qint32 from = 0;       // content offsets
    qint32 to = 0;
    qint32 parent = -1;    // -1 = top level
    qint32 firstChild = -1;
    qint32 next = -1;      // next sibling
    qint32 data = -1;      // index into the matching info list
};

struct WikiLinkInfo {
    QString target;        // "Note" / "folder/Note" / "image.png" (no subpath); empty for "[[#Heading]]"
    QString subpath;       // "#Heading", "#Heading#Sub", "#^block-id" or ""
    QString alias;         // text after the first '|' ("" if none); for image embeds may be a size
    int width = -1;        // embeds: "|100" or "|100x200"
    int height = -1;
    QString displayText() const; // alias, else "target > heading" style per Obsidian, else target
};

struct LinkInfo {
    QString destination;   // unescaped, '<>' stripped (percent-encoding kept)
    QString title;
    bool reference = false;
    QString label;         // for reference links
    int width = -1;        // images: "![alt|100x200](x)"
    int height = -1;
};

struct TagInfo {
    QString tag;           // without '#', e.g. "nested/tag"
};

struct FootnoteInfo {
    QString label;         // FootnoteRef label ("" for inline footnotes)
};

struct HtmlTagInfo {
    QString name;          // lowercase, "" for comments
    bool closing = false;
    bool selfClosing = false;
    bool comment = false;
};

class InlineTree {
public:
    int size() const { return int(m_nodes.size()); }
    const InlineNode& node(int i) const { return m_nodes[i]; }
    const QList<InlineNode>& nodes() const { return m_nodes; }
    int firstRoot() const { return m_nodes.isEmpty() ? -1 : 0; } // roots are chained via `next`
    const QString& content() const { return m_content; }

    const WikiLinkInfo& wikiLink(int id) const { return m_wikiLinks[m_nodes[id].data]; }
    const LinkInfo& link(int id) const { return m_links[m_nodes[id].data]; }
    const TagInfo& tag(int id) const { return m_tags[m_nodes[id].data]; }
    const FootnoteInfo& footnote(int id) const { return m_footnotes[m_nodes[id].data]; }
    const HtmlTagInfo& htmlTag(int id) const { return m_html[m_nodes[id].data]; }
    QString blockId(int id) const { return m_content.mid(m_nodes[id].from + 1, m_nodes[id].to - m_nodes[id].from - 1); }

    // Build interface (used by the parser).
    QList<InlineNode>& mutableNodes() { return m_nodes; }
    QString& mutableContent() { return m_content; }
    QList<WikiLinkInfo>& mutableWikiLinks() { return m_wikiLinks; }
    QList<LinkInfo>& mutableLinks() { return m_links; }
    QList<TagInfo>& mutableTags() { return m_tags; }
    QList<FootnoteInfo>& mutableFootnotes() { return m_footnotes; }
    QList<HtmlTagInfo>& mutableHtml() { return m_html; }

private:
    QList<InlineNode> m_nodes; // preorder
    QString m_content;
    QList<WikiLinkInfo> m_wikiLinks;
    QList<LinkInfo> m_links;
    QList<TagInfo> m_tags;
    QList<FootnoteInfo> m_footnotes;
    QList<HtmlTagInfo> m_html;
};

using InlineTreePtr = std::shared_ptr<const InlineTree>;

// Content of a leaf (or of a table cell / callout title): the text inline offsets refer to, with a
// mapping back to document positions.
struct LeafContent {
    QString text;
    struct Segment {
        int line = 0;        // document line
        int col = 0;         // column where this segment starts in that line
        int contentFrom = 0; // offset in `text`
        int length = 0;      // chars (the '\n' joining segments is not part of any segment)
    };
    QList<Segment> segments;

    qsizetype toDoc(int contentOffset, const Text& doc) const;
    int fromDoc(qsizetype docOffset, const Text& doc) const; // -1 if not inside this content
    int lineOf(int contentOffset) const;                     // document line of a content offset
};

} // namespace oc::md
