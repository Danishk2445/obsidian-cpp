#pragma once

// Inline raw HTML (`<kbd>Ctrl</kbd>`, `H<sub>2</sub>O`, `line<br>break`, `<span style="color:red">`...)
// rendered with decorations instead of a nested document, so wrapping, hit-testing and selection keep
// working: tags of a leaf are paired, the tag text is hidden and the element's content gets the
// element's formatting. Obsidian's live preview replaces an HTML element (opening tag through matching
// closing tag) by a rendered widget and shows its source while the selection touches it; here the
// outermost element of each nest is that unit.

#include "editor/html/html_parse.h"
#include "markdown/inline_tree.h"
#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"

#include <functional>
#include <optional>

namespace oc {

class Text;

namespace html {

// One element of a leaf's inline HTML. Node ids are InlineTree ids.
struct InlineElement {
    int open = -1;          // opening (or void / self-closing) tag node
    int close = -1;         // matching closing tag node; -1: void, self-closing or unclosed
    int parent = -1;        // index of the enclosing element (-1 = outermost)
    bool unclosed = false;  // an opening tag whose closing tag isn't in the leaf
    int contentFrom = 0;    // content offsets of the element's content
    int contentTo = 0;      // closing tag start, an enclosing element's closing tag, or the content end
    Tag tag;                // the parsed opening tag
};

struct InlineStructure {
    QList<InlineElement> elements; // in document order of their opening tags
    QList<int> strayClosing;       // closing tags without an opener
    QList<int> comments;           // <!-- --> and other non-element markup
};

// HtmlTag nodes of a tree.
QList<int> htmlTagNodes(const md::InlineTree& tree);
// HTML-style pairing: a closing tag closes the nearest open element with its name (and the unclosed
// ones opened after it); void and self-closing tags are complete elements.
InlineStructure pairTags(const md::InlineTree& tree, const QList<int>& htmlNodes);

struct InlineContext {
    md::InlineTreePtr tree;
    const md::LeafContent* content = nullptr;
    const Text* text = nullptr;
    qsizetype lineFrom = 0;  // the window being decorated (document offsets, one editor line / cell)
    qsizetype lineTo = 0;
    qsizetype colBase = 0;   // decoration columns are relative to this document offset
    bool reading = false;    // Reading view / PDF (browser semantics for unclosed and stray tags)
    bool forceInactive = false;
    std::function<bool(qsizetype from, qsizetype to)> touches; // the selection touches [from, to]
    const StyleSheet* styles = nullptr;
    StyleId lineStyle = StyleId::Normal;
    qreal contentWidth = 700;
    std::function<QString(const QString& linkpath)> resolveAttachment;
};

// Adds the marks / hidden ranges / substitutions / widgets of the inline HTML `htmlNodes` (from
// htmlTagNodes()) to `d`.
void decorateInline(const InlineContext& ctx, const QList<int>& htmlNodes, LineDecoration& d);

// <a href> around a content offset (the innermost one). from/to: the range Live Preview reveals with it (its
// outermost element, widened to whole lines when alone on them).
struct AnchorHit {
    QString href;
    bool external = false; // href has a URL scheme
    qsizetype from = 0;
    qsizetype to = 0;
};
std::optional<AnchorHit> anchorAt(const md::InlineTreePtr& tree, const md::LeafContent& content, const Text& text, int contentOffset);

// "https:", "mailto:", "obsidian:"...
bool hasUrlScheme(const QString& url);

// The content offsets [first, last] of a content's part inside the document range [from, to] (first > last when
// none): nodes outside it can be skipped without mapping them to the document.
std::pair<int, int> contentWindow(const md::LeafContent& content, const Text& text, qsizetype from, qsizetype to);

// Inline content of an HTML block leaf (its lines after container markers), for rendering an HTML block
// that isn't a block widget (inside a list or a quote) with inline decorations.
md::LeafContent blockContent(const md::SyntaxTree& tree, int leaf, const Text& text);

} // namespace html
} // namespace oc
