// Golden tests of md::parseInline() for Obsidian-flavoured Markdown (S-expression dumps, see
// inline_html.h), the Obsidian rule predicates and tree invariants.
//
// Reference behaviour: Obsidian 1.13.7. Its editor (CodeMirror + HyperMD tokenizer, what Live Preview
// shows) and its reading view / metadata cache (remark tokenizers) do not always agree; one InlineTree
// serves both here, so each rule below says which side it follows.
//
// '$' math (verified against the Obsidian 1.13.7 editor tokenizer; UNVERIFIED items are marked):
//  * an opening '$' must be followed by a character that is neither whitespace nor '$';
//  * the FIRST later '$' on the same line that is not preceded by '\' is the only closing candidate
//    (editor rule). It must not be preceded by whitespace and not be followed by an ASCII digit,
//    otherwise this '$' opens nothing. (Obsidian's reading view instead skips invalid candidates, so
//    "$a $b$" renders "a $b" as math there, while the editor, and this parser, render "$a " + math "b".)
//  * inline math never spans lines (editor rule; reading view allows a single line break);
//  * "$$...$$" inside a paragraph is display math up to the next "$$" and may span lines;
//  * '\$' never delimits; math inside a [[wikilink]] or `code` is never seen (they start earlier).
//  Shell text therefore stays text: "$5 and $10", "echo $HOME and $PATH", "\"$@\"", "${x}", "$(cmd)",
//  "$1$2" (closer followed by a digit), while "PATH=$PATH:$HOME/bin" is math "PATH:" exactly as in
//  Obsidian. UNVERIFIED: '$' inside link text (the editor disables math there, reading view does not;
//  this parser allows it), table cells where '|' ends math in the editor.
// Tags: '#' at the start of the content or after whitespace (JS \s) - also not after '(' (Obsidian's
//  editor and reading view both require whitespace); body = characters other than whitespace,
//  U+2000-206F, U+2E00-2E7F and ASCII punctuation except '-' '_' '/'; at least one non-digit.
//  (Reading view also accepts a tag glued to a preceding construct, e.g. "**b**#tag"; the editor, and
//  this parser, do not.)
// Wikilinks (reading view / metadata rules): "[[" + at least one character + first "]]" on the same
//  line, no "[[" inside; alias = after the first '|' unless that '|' starts the trimmed text; "\|"
//  (table cells) is a separator whose backslash is dropped; target = before the first '#'.
// Comments: %%...%% may span lines (editor rule; reading view only matches within one line).
#include "markdown/inline_html.h"
#include "markdown/inline_rules.h"
#include "markdown/inline_tree.h"

#include <QRandomGenerator>
#include <gtest/gtest.h>

using namespace oc;
using namespace oc::md;
using test::dumpSexpr;

namespace {

// Structural invariants every tree must satisfy.
void checkTree(const InlineTree& t, const QString& input)
{
    const int n = int(t.content().size());
    ASSERT_EQ(t.content(), input);
    int prevFrom = -1;
    std::vector<int> childCount(size_t(t.size()), 0);
    for (int i = 0; i < t.size(); ++i) {
        const InlineNode& nd = t.node(i);
        ASSERT_LE(0, nd.from) << i;
        ASSERT_LT(nd.from, nd.to) << "empty node " << i << " in " << qPrintable(input);
        ASSERT_LE(nd.to, n) << i;
        ASSERT_GE(nd.from, prevFrom) << "not preorder at " << i;
        prevFrom = nd.from;
        ASSERT_NE(nd.type, InlineType::Text) << "no Text nodes are emitted";
        ASSERT_EQ(nd.type == InlineType::Mark, nd.mark != MarkKind::None) << i;
        if (nd.parent >= 0) {
            ASSERT_LT(nd.parent, i);
            const InlineNode& p = t.node(nd.parent);
            ASSERT_LE(p.from, nd.from);
            ASSERT_LE(nd.to, p.to);
        }
        // data indices
        switch (nd.type) {
        case InlineType::WikiLink:
        case InlineType::Embed:
            ASSERT_TRUE(nd.data >= 0 && nd.data < const_cast<InlineTree&>(t).mutableWikiLinks().size());
            break;
        case InlineType::Link:
        case InlineType::Image:
            ASSERT_TRUE(nd.data >= 0 && nd.data < const_cast<InlineTree&>(t).mutableLinks().size());
            break;
        case InlineType::Tag:
            ASSERT_TRUE(nd.data >= 0 && nd.data < const_cast<InlineTree&>(t).mutableTags().size());
            break;
        case InlineType::FootnoteRef:
        case InlineType::InlineFootnote:
            ASSERT_TRUE(nd.data >= 0 && nd.data < const_cast<InlineTree&>(t).mutableFootnotes().size());
            break;
        case InlineType::HtmlTag:
            ASSERT_TRUE(nd.data >= 0 && nd.data < const_cast<InlineTree&>(t).mutableHtml().size());
            break;
        default:
            ASSERT_EQ(nd.data, -1);
            break;
        }
    }
    // sibling chains: ordered, non-overlapping, parents consistent
    auto checkChain = [&](int first, int parent) {
        int prevTo = parent < 0 ? 0 : t.node(parent).from;
        for (int c = first; c >= 0; c = t.node(c).next) {
            ASSERT_EQ(t.node(c).parent, parent);
            ASSERT_GE(t.node(c).from, prevTo) << "overlapping siblings in " << qPrintable(input);
            prevTo = t.node(c).to;
            if (parent >= 0)
                ++childCount[size_t(parent)];
        }
    };
    checkChain(t.firstRoot(), -1);
    for (int i = 0; i < t.size(); ++i)
        checkChain(t.node(i).firstChild, i);
    for (int i = 0; i < t.size(); ++i) {
        int direct = 0;
        for (int j = i + 1; j < t.size(); ++j)
            if (t.node(j).parent == i)
                ++direct;
        ASSERT_EQ(direct, childCount[size_t(i)]) << "child list of " << i << " incomplete";
    }
}

QString dumpChecked(const QString& input, const QList<LinkRefDefinition>& refs = {})
{
    const InlineTreePtr t = parseInline(input, refs);
    checkTree(*t, input);
    return dumpSexpr(*t);
}

#define EXPECT_DUMP(input, expected) EXPECT_EQ(dumpChecked(QStringLiteral(input)).toStdString(), std::string(expected)) << "input: " << input

LinkRefDefinition ref(const char* label, const char* dest, const char* title = "")
{
    LinkRefDefinition d;
    d.label = rules::normalizeLinkLabel(QString::fromUtf8(label));
    d.destination = QString::fromUtf8(dest);
    d.title = QString::fromUtf8(title);
    return d;
}

const InlineNode* findNode(const InlineTree& t, InlineType type, int nth = 0)
{
    for (int i = 0; i < t.size(); ++i)
        if (t.node(i).type == type && nth-- == 0)
            return &t.node(i);
    return nullptr;
}

} // namespace

// ---- emphasis family ----------------------------------------------------------------------------

TEST(InlineObsidian, EmphasisStrongStrikeHighlight)
{
    EXPECT_DUMP("**bold** and *em*",
                R"x((Strong (EmphasisDelim "**") "bold" (EmphasisDelim "**")) " and " (Emphasis (EmphasisDelim "*") "em" (EmphasisDelim "*")))x");
    EXPECT_DUMP("__b__ _e_", R"x((Strong (EmphasisDelim "__") "b" (EmphasisDelim "__")) " " (Emphasis (EmphasisDelim "_") "e" (EmphasisDelim "_")))x");
    EXPECT_DUMP("***both***", R"x((Emphasis (EmphasisDelim "*") (Strong (EmphasisDelim "**") "both" (EmphasisDelim "**")) (EmphasisDelim "*")))x");
    EXPECT_DUMP("~~strike~~ ==hl==",
                R"x((Strikethrough (EmphasisDelim "~~") "strike" (EmphasisDelim "~~")) " " (Highlight (EmphasisDelim "==") "hl" (EmphasisDelim "==")))x");
    // exactly two delimiters
    EXPECT_DUMP("~single~ ~~~three~~~ ===no===", R"x("~single~ ~~~three~~~ ===no===")x");
    EXPECT_DUMP("a == b and c ~~ d", R"x("a == b and c ~~ d")x");
    EXPECT_DUMP("x==y==z", R"x("x" (Highlight (EmphasisDelim "==") "y" (EmphasisDelim "==")) "z")x");
    EXPECT_DUMP("**==bold highlight==**",
                R"x((Strong (EmphasisDelim "**") (Highlight (EmphasisDelim "==") "bold highlight" (EmphasisDelim "==")) (EmphasisDelim "**")))x");
    EXPECT_DUMP("==**a**==", R"x((Highlight (EmphasisDelim "==") (Strong (EmphasisDelim "**") "a" (EmphasisDelim "**")) (EmphasisDelim "==")))x");
    EXPECT_DUMP("~~a ==b~~ c==", R"x((Strikethrough (EmphasisDelim "~~") "a ==b" (EmphasisDelim "~~")) " c==")x");
    EXPECT_DUMP("snake_case_word", R"x("snake_case_word")x");
    EXPECT_DUMP("**unclosed", R"x("**unclosed")x");
    EXPECT_DUMP("*a **b** c*",
                R"x((Emphasis (EmphasisDelim "*") "a " (Strong (EmphasisDelim "**") "b" (EmphasisDelim "**")) " c" (EmphasisDelim "*")))x");
}

TEST(InlineObsidian, CodeSpans)
{
    EXPECT_DUMP("`code` and ``a`b``",
                R"x((CodeSpan (CodeDelim "`") "code" (CodeDelim "`")) " and " (CodeSpan (CodeDelim "``") "a`b" (CodeDelim "``")))x");
    EXPECT_DUMP("`unclosed and ``two", R"x("`unclosed and ``two")x");
    EXPECT_DUMP("`a and ``b`", R"x((CodeSpan (CodeDelim "`") "a and ``b" (CodeDelim "`")))x");
    EXPECT_DUMP("`**not bold** [[not link]] $not math$`", R"x((CodeSpan (CodeDelim "`") "**not bold** [[not link]] $not math$" (CodeDelim "`")))x");
    EXPECT_DUMP("`multi\nline`", R"x((CodeSpan (CodeDelim "`") "multi\nline" (CodeDelim "`")))x");
    EXPECT_DUMP("\\``code`", R"x((Escape (EscapeChar "\\") "`") (CodeSpan (CodeDelim "`") "code" (CodeDelim "`")))x");
}

// ---- wikilinks / embeds ---------------------------------------------------------------------------

TEST(InlineObsidian, WikiLinks)
{
    EXPECT_DUMP("[[Note]]", R"x((WikiLink{target="Note"} (WikiOpen "[[") (WikiTarget "Note") (WikiClose "]]")))x");
    EXPECT_DUMP("[[Note#Heading|alias]]",
                R"x((WikiLink{target="Note" subpath="#Heading" alias="alias"} (WikiOpen "[[") (WikiTarget "Note#Heading") (WikiPipe "|") "alias" (WikiClose "]]")))x");
    EXPECT_DUMP("[[#Heading]]", R"x((WikiLink{target="" subpath="#Heading"} (WikiOpen "[[") (WikiTarget "#Heading") (WikiClose "]]")))x");
    EXPECT_DUMP("[[Note#^block-id]]", R"x((WikiLink{target="Note" subpath="#^block-id"} (WikiOpen "[[") (WikiTarget "Note#^block-id") (WikiClose "]]")))x");
    EXPECT_DUMP("[[folder/Note#H1#H2]]", R"x((WikiLink{target="folder/Note" subpath="#H1#H2"} (WikiOpen "[[") (WikiTarget "folder/Note#H1#H2") (WikiClose "]]")))x");
    EXPECT_DUMP("[[ spaced  |  alias ]]",
                R"x((WikiLink{target="spaced" alias="alias"} (WikiOpen "[[") (WikiTarget " spaced  ") (WikiPipe "|") "  alias " (WikiClose "]]")))x");
    EXPECT_DUMP("[[a|b|c]]", R"x((WikiLink{target="a" alias="b|c"} (WikiOpen "[[") (WikiTarget "a") (WikiPipe "|") "b|c" (WikiClose "]]")))x");
    // a '|' starting the (trimmed) text is not a separator (Obsidian)
    EXPECT_DUMP("[[|x]]", R"x((WikiLink{target="|x"} (WikiOpen "[[") (WikiTarget "|x") (WikiClose "]]")))x");
    // inner text is raw
    EXPECT_DUMP("[[**not bold** `x`]]", R"x((WikiLink{target="**not bold** `x`"} (WikiOpen "[[") (WikiTarget "**not bold** `x`") (WikiClose "]]")))x");
    // table cells: "\|" is the separator, its backslash belongs to the pipe
    EXPECT_DUMP("[[Note\\|alias]]", R"x((WikiLink{target="Note" alias="alias"} (WikiOpen "[[") (WikiTarget "Note") (WikiPipe "\\|") "alias" (WikiClose "]]")))x");
    EXPECT_DUMP("[[Note#Sec\\|alias]] | x",
                R"x((WikiLink{target="Note" subpath="#Sec" alias="alias"} (WikiOpen "[[") (WikiTarget "Note#Sec") (WikiPipe "\\|") "alias" (WikiClose "]]")) " | x")x");
    // no nesting, single line, non-empty
    EXPECT_DUMP("[[a [[b]]", R"x("[[a " (WikiLink{target="b"} (WikiOpen "[[") (WikiTarget "b") (WikiClose "]]")))x");
    EXPECT_DUMP("[[a]]]", R"x((WikiLink{target="a"} (WikiOpen "[[") (WikiTarget "a") (WikiClose "]]")) "]")x");
    EXPECT_DUMP("[[]] and [[a\nb]]", R"x("[[]] and [[a" (SoftBreak "\n") "b]]")x");
    EXPECT_DUMP("[[]]]", R"x((WikiLink{target="]"} (WikiOpen "[[") (WikiTarget "]") (WikiClose "]]")))x");
    EXPECT_DUMP("\\[[not wiki]]", R"x((Escape (EscapeChar "\\") "[") "[not wiki]]")x");
}

TEST(InlineObsidian, Embeds)
{
    EXPECT_DUMP("![[image.png]]", R"x((Embed{target="image.png"} (WikiOpen "![[") (WikiTarget "image.png") (WikiClose "]]")))x");
    EXPECT_DUMP("![[image.png|200]]", R"x((Embed{target="image.png" alias="200" w=200} (WikiOpen "![[") (WikiTarget "image.png") (WikiPipe "|") "200" (WikiClose "]]")))x");
    EXPECT_DUMP("![[image.png|200x100]]",
                R"x((Embed{target="image.png" alias="200x100" w=200 h=100} (WikiOpen "![[") (WikiTarget "image.png") (WikiPipe "|") "200x100" (WikiClose "]]")))x");
    EXPECT_DUMP("![[image.png|my caption|300]]",
                R"x((Embed{target="image.png" alias="my caption|300" w=300} (WikiOpen "![[") (WikiTarget "image.png") (WikiPipe "|") "my caption|300" (WikiClose "]]")))x");
    EXPECT_DUMP("![[image.png|caption]]",
                R"x((Embed{target="image.png" alias="caption"} (WikiOpen "![[") (WikiTarget "image.png") (WikiPipe "|") "caption" (WikiClose "]]")))x");
    EXPECT_DUMP("![[doc.pdf#page=3]]", R"x((Embed{target="doc.pdf" subpath="#page=3"} (WikiOpen "![[") (WikiTarget "doc.pdf#page=3") (WikiClose "]]")))x");
    EXPECT_DUMP("![[Note#Section]] text",
                R"x((Embed{target="Note" subpath="#Section"} (WikiOpen "![[") (WikiTarget "Note#Section") (WikiClose "]]")) " text")x");
    // a wikilink after an escaped '!' is not an embed
    EXPECT_DUMP("\\![[a]]", R"x((Escape (EscapeChar "\\") "!") (WikiLink{target="a"} (WikiOpen "[[") (WikiTarget "a") (WikiClose "]]")))x");
}

TEST(InlineObsidian, WikiLinkDisplayText)
{
    auto info = [](const char* s) { return parseInline(QString::fromUtf8(s))->wikiLink(0); };
    EXPECT_EQ(info("[[Note]]").displayText(), QStringLiteral("Note"));
    EXPECT_EQ(info("[[Note|Alias]]").displayText(), QStringLiteral("Alias"));
    EXPECT_EQ(info("[[Note#Heading]]").displayText(), QStringLiteral("Note > Heading"));
    EXPECT_EQ(info("[[Note#Heading#Sub]]").displayText(), QStringLiteral("Note > Heading > Sub"));
    EXPECT_EQ(info("[[#Heading]]").displayText(), QStringLiteral("Heading"));
    EXPECT_EQ(info("[[Note#^blk]]").displayText(), QStringLiteral("Note > ^blk"));
    EXPECT_EQ(info("[[folder/Note]]").displayText(), QStringLiteral("folder/Note"));
    WikiLinkInfo w;
    w.target = QStringLiteral("Note");
    EXPECT_EQ(w.displayText(), QStringLiteral("Note"));
    w.subpath = QStringLiteral("##A");
    EXPECT_EQ(w.displayText(), QStringLiteral("Note > A"));
}

// ---- math ---------------------------------------------------------------------------------------

TEST(InlineObsidian, Math)
{
    EXPECT_DUMP("$x^2$", R"x((InlineMath (MathDelim "$") "x^2" (MathDelim "$")))x");
    EXPECT_DUMP("area $\\pi r^2$.", R"x("area " (InlineMath (MathDelim "$") "\\pi r^2" (MathDelim "$")) ".")x");
    EXPECT_DUMP("$$\\int_0^1 x$$", R"x((DisplayMath (MathDelim "$$") "\\int_0^1 x" (MathDelim "$$")))x");
    EXPECT_DUMP("a $$x\ny$$ b", R"x("a " (DisplayMath (MathDelim "$$") "x\ny" (MathDelim "$$")) " b")x");
    EXPECT_DUMP("$$$$", R"x((DisplayMath (MathDelim "$$") (MathDelim "$$")))x");
    // opener / closer whitespace rules
    EXPECT_DUMP("$ x$ and $x $", R"x("$ x$ and $x $")x");
    // the first '$' is the only closing candidate (Obsidian editor)
    EXPECT_DUMP("$a $b$", R"x("$a " (InlineMath (MathDelim "$") "b" (MathDelim "$")))x");
    // closer followed by a digit
    EXPECT_DUMP("$x$5 $1$2", R"x("$x$5 $1$2")x");
    // escaped dollars never delimit
    EXPECT_DUMP("\\$x$", R"x((Escape (EscapeChar "\\") "$") "x$")x");
    EXPECT_DUMP("$x\\$y$", R"x((InlineMath (MathDelim "$") "x\\$y" (MathDelim "$")))x");
    // an unclosed "$$" leaves the second '$' free to open inline math (reading view)
    EXPECT_DUMP("a $$x$ b", R"x("a $" (InlineMath (MathDelim "$") "x" (MathDelim "$")) " b")x");
    // inline math does not cross lines; display math does
    EXPECT_DUMP("$a\nb$", R"x("$a" (SoftBreak "\n") "b$")x");
    // raw content
    EXPECT_DUMP("$**a**$", R"x((InlineMath (MathDelim "$") "**a**" (MathDelim "$")))x");
}

TEST(InlineObsidian, ShellDollarsStayText)
{
    EXPECT_DUMP("It costs $5, or $10 with tax", R"x("It costs $5, or $10 with tax")x");
    EXPECT_DUMP("echo $HOME and $PATH", R"x("echo $HOME and $PATH")x");
    EXPECT_DUMP("echo \"$@\"", R"x("echo \"$@\"")x");
    // ...but a later "$f" closes it (Obsidian renders this math too)
    EXPECT_DUMP("for f in \"$@\"; do echo \"$f\"", R"x("for f in \"" (InlineMath (MathDelim "$") "@\"; do echo \"" (MathDelim "$")) "f\"")x");
    EXPECT_DUMP("${x} and $(cmd) and $1$2", R"x("${x} and $(cmd) and $1$2")x");
    EXPECT_DUMP("price: $5-$10", R"x("price: $5-$10")x");
    // what Obsidian renders as math too
    EXPECT_DUMP("PATH=$PATH:$HOME/bin", R"x("PATH=" (InlineMath (MathDelim "$") "PATH:" (MathDelim "$")) "HOME/bin")x");
    // bash test inside [[ ]] is an (unresolved) wikilink, its '$' is not math
    EXPECT_DUMP("if [[ -f \"$f\" ]]; then",
                R"x("if " (WikiLink{target="-f \"$f\""} (WikiOpen "[[") (WikiTarget " -f \"$f\" ") (WikiClose "]]")) "; then")x");
    EXPECT_DUMP("while [[ $i -lt 10 ]]; do", R"x("while " (WikiLink{target="$i -lt 10"} (WikiOpen "[[") (WikiTarget " $i -lt 10 ") (WikiClose "]]")) "; do")x");
}

// ---- comments, tags, footnotes, block ids -----------------------------------------------------------

TEST(InlineObsidian, Comments)
{
    EXPECT_DUMP("a %%hidden%% b", R"x("a " (Comment (CommentDelim "%%") "hidden" (CommentDelim "%%")) " b")x");
    EXPECT_DUMP("%%multi\nline%% after", R"x((Comment (CommentDelim "%%") "multi\nline" (CommentDelim "%%")) " after")x");
    EXPECT_DUMP("%%[[not a link]] **raw**%%", R"x((Comment (CommentDelim "%%") "[[not a link]] **raw**" (CommentDelim "%%")))x");
    EXPECT_DUMP("100%% sure", R"x("100%% sure")x");
    EXPECT_DUMP("%%%%", R"x((Comment (CommentDelim "%%") (CommentDelim "%%")))x");
}

TEST(InlineObsidian, Tags)
{
    EXPECT_DUMP("#tag #nested/tag", R"x((Tag{tag="tag"} (TagHash "#") "tag") " " (Tag{tag="nested/tag"} (TagHash "#") "nested/tag"))x");
    EXPECT_DUMP("#123 a#b C# (#paren) #1a",
                R"x("#123 a#b C# (#paren) " (Tag{tag="1a"} (TagHash "#") "1a"))x");
    EXPECT_DUMP("#tag-with_under #tag. #tag,x",
                R"x((Tag{tag="tag-with_under"} (TagHash "#") "tag-with_under") " " (Tag{tag="tag"} (TagHash "#") "tag") ". " (Tag{tag="tag"} (TagHash "#") "tag") ",x")x");
    EXPECT_DUMP("#Über #日本語 #emoji😀x", R"x((Tag{tag="Über"} (TagHash "#") "Über") " " (Tag{tag="日本語"} (TagHash "#") "日本語") " " (Tag{tag="emoji😀x"} (TagHash "#") "emoji😀x"))x");
    EXPECT_DUMP("#a—b #«q»", R"x((Tag{tag="a"} (TagHash "#") "a") "—b " (Tag{tag="«q»"} (TagHash "#") "«q»"))x");
    EXPECT_DUMP("line\n#tag", R"x("line" (SoftBreak "\n") (Tag{tag="tag"} (TagHash "#") "tag"))x");
    EXPECT_DUMP("**b**#glued ##double #", R"x((Strong (EmphasisDelim "**") "b" (EmphasisDelim "**")) "#glued ##double #")x");
    EXPECT_DUMP("#include <stdio.h>", R"x((Tag{tag="include"} (TagHash "#") "include") " <stdio.h>")x");
    EXPECT_DUMP("*#tag*", R"x((Emphasis (EmphasisDelim "*") "#tag" (EmphasisDelim "*")))x");
    EXPECT_DUMP("#tag*", R"x((Tag{tag="tag"} (TagHash "#") "tag") "*")x");
}

TEST(InlineObsidian, Footnotes)
{
    EXPECT_DUMP("text[^1] more", R"x("text" (FootnoteRef{label="1"} (FootnoteDelim "[^") "1" (FootnoteDelim "]")) " more")x");
    EXPECT_DUMP("x[^note-a]", R"x("x" (FootnoteRef{label="note-a"} (FootnoteDelim "[^") "note-a" (FootnoteDelim "]")))x");
    EXPECT_DUMP("[^a b] [^]", R"x("[^a b] [^]")x");
    EXPECT_DUMP("inline^[note with *em*] end",
                R"x("inline" (InlineFootnote (FootnoteDelim "^[") "note with " (Emphasis (EmphasisDelim "*") "em" (EmphasisDelim "*")) (FootnoteDelim "]")) " end")x");
    EXPECT_DUMP("^[a [b] c]", R"x((InlineFootnote (FootnoteDelim "^[") "a [b] c" (FootnoteDelim "]")))x");
    EXPECT_DUMP("^[see [[Note]]]", R"x((InlineFootnote (FootnoteDelim "^[") "see " (WikiLink{target="Note"} (WikiOpen "[[") (WikiTarget "Note") (WikiClose "]]")) (FootnoteDelim "]")))x");
    EXPECT_DUMP("^[unclosed", R"x("^[unclosed")x");
}

TEST(InlineObsidian, BlockIds)
{
    EXPECT_DUMP("text ^abc-123", R"x("text " (BlockId{id="abc-123"} (BlockIdCaret "^") "abc-123"))x");
    EXPECT_DUMP("^abc", R"x((BlockId{id="abc"} (BlockIdCaret "^") "abc"))x");
    EXPECT_DUMP("text ^abc  ", R"x("text " (BlockId{id="abc"} (BlockIdCaret "^") "abc") "  ")x");
    EXPECT_DUMP("line\n^id", R"x("line" (SoftBreak "\n") (BlockId{id="id"} (BlockIdCaret "^") "id"))x");
    EXPECT_DUMP("text^abc", R"x("text^abc")x");
    EXPECT_DUMP("text ^abc more", R"x("text ^abc more")x");
    EXPECT_DUMP("x ^a_b", R"x("x ^a_b")x");
    EXPECT_DUMP("`code ^abc`", R"x((CodeSpan (CodeDelim "`") "code ^abc" (CodeDelim "`")))x");
    const InlineTreePtr t = parseInline(QStringLiteral("Some paragraph ^my-id"));
    const InlineNode* b = findNode(*t, InlineType::BlockId);
    ASSERT_TRUE(b);
    EXPECT_EQ(t->blockId(int(b - &t->node(0))), QStringLiteral("my-id"));
}

// ---- CommonMark constructs with Obsidian content -----------------------------------------------------

TEST(InlineObsidian, Links)
{
    EXPECT_DUMP("[text](http://x.com \"title\")",
                R"x((Link{dest="http://x.com" title="title"} (LinkOpen "[") "text" (LinkClose "](" (LinkUrl "http://x.com") " \"title\")")))x");
    EXPECT_DUMP("[a *b* c](u)",
                R"x((Link{dest="u"} (LinkOpen "[") "a " (Emphasis (EmphasisDelim "*") "b" (EmphasisDelim "*")) " c" (LinkClose "](" (LinkUrl "u") ")")))x");
    EXPECT_DUMP("*[a](u)*", R"x((Emphasis (EmphasisDelim "*") (Link{dest="u"} (LinkOpen "[") "a" (LinkClose "](" (LinkUrl "u") ")")) (EmphasisDelim "*")))x");
    EXPECT_DUMP("[**bold** link](Note%20Name.md)",
                R"x((Link{dest="Note%20Name.md"} (LinkOpen "[") (Strong (EmphasisDelim "**") "bold" (EmphasisDelim "**")) " link" (LinkClose "](" (LinkUrl "Note%20Name.md") ")")))x");
    EXPECT_DUMP("[a](<b c>)", R"x((Link{dest="b c"} (LinkOpen "[") "a" (LinkClose "](" (LinkUrl "<b c>") ")")))x");
    EXPECT_DUMP("[a]()", R"x((Link{dest=""} (LinkOpen "[") "a" (LinkClose "]()")))x");
    EXPECT_DUMP("[a](b", R"x("[a](b")x");
    EXPECT_DUMP("[a](b\\)c)", R"x((Link{dest="b)c"} (LinkOpen "[") "a" (LinkClose "](" (LinkUrl "b\\)c") ")")))x");
    EXPECT_DUMP("[link [[wiki]]](u)",
                R"x((Link{dest="u"} (LinkOpen "[") "link " (WikiLink{target="wiki"} (WikiOpen "[[") (WikiTarget "wiki") (WikiClose "]]")) (LinkClose "](" (LinkUrl "u") ")")))x");
    EXPECT_DUMP("[a [b](c) d](e)", R"x("[a " (Link{dest="c"} (LinkOpen "[") "b" (LinkClose "](" (LinkUrl "c") ")")) " d](e)")x");
}

TEST(InlineObsidian, ReferenceLinks)
{
    const QList<LinkRefDefinition> refs = {ref("Ref", "/url", "T"), ref("other one", "/o")};
    auto dump = [&](const char* s) { return dumpChecked(QString::fromUtf8(s), refs).toStdString(); };
    EXPECT_EQ(dump("[text][ref]"), R"x((Link{dest="/url" title="T" ref="ref"} (LinkOpen "[") "text" (LinkClose "][ref]")))x");
    EXPECT_EQ(dump("[REF][]"), R"x((Link{dest="/url" title="T" ref="ref"} (LinkOpen "[") "REF" (LinkClose "][]")))x");
    EXPECT_EQ(dump("[Other   One]"), R"x((Link{dest="/o" ref="other one"} (LinkOpen "[") "Other   One" (LinkClose "]")))x");
    EXPECT_EQ(dump("[undefined] and [x][undefined]"), R"x("[undefined] and [x][undefined]")x");
    EXPECT_EQ(dump("![alt][ref]"), R"x((Image{dest="/url" title="T" ref="ref"} (LinkOpen "![") "alt" (LinkClose "][ref]")))x");
}

TEST(InlineObsidian, Images)
{
    EXPECT_DUMP("![alt|100x200](img.png)", R"x((Image{dest="img.png" w=100 h=200} (LinkOpen "![") "alt|100x200" (LinkClose "](" (LinkUrl "img.png") ")")))x");
    EXPECT_DUMP("![alt|300](img.png)", R"x((Image{dest="img.png" w=300} (LinkOpen "![") "alt|300" (LinkClose "](" (LinkUrl "img.png") ")")))x");
    EXPECT_DUMP("![100](img.png)", R"x((Image{dest="img.png" w=100} (LinkOpen "![") "100" (LinkClose "](" (LinkUrl "img.png") ")")))x");
    EXPECT_DUMP("![a|b](img.png)", R"x((Image{dest="img.png"} (LinkOpen "![") "a|b" (LinkClose "](" (LinkUrl "img.png") ")")))x");
    EXPECT_DUMP("![](https://x.com/a.png)", R"x((Image{dest="https://x.com/a.png"} (LinkOpen "![") (LinkClose "](" (LinkUrl "https://x.com/a.png") ")")))x");
    EXPECT_DUMP("[![img](a.png)](b)",
                R"x((Link{dest="b"} (LinkOpen "[") (Image{dest="a.png"} (LinkOpen "![") "img" (LinkClose "](" (LinkUrl "a.png") ")")) (LinkClose "](" (LinkUrl "b") ")")))x");
}

TEST(InlineObsidian, HtmlAndAutolinks)
{
    EXPECT_DUMP("<kbd>Ctrl</kbd>+<kbd>C</kbd>",
                R"x((HtmlTag{name="kbd"} "<kbd>") "Ctrl" (HtmlTag{name="kbd" closing} "</kbd>") "+" (HtmlTag{name="kbd"} "<kbd>") "C" (HtmlTag{name="kbd" closing} "</kbd>"))x");
    EXPECT_DUMP("H<sub>2</sub>O", R"x("H" (HtmlTag{name="sub"} "<sub>") "2" (HtmlTag{name="sub" closing} "</sub>") "O")x");
    EXPECT_DUMP("a<br>b<br/>c<BR />", R"x("a" (HtmlTag{name="br"} "<br>") "b" (HtmlTag{name="br" selfClosing} "<br/>") "c" (HtmlTag{name="br" selfClosing} "<BR />"))x");
    EXPECT_DUMP("<!-- note -->x", R"x((HtmlTag{comment} "<!-- note -->") "x")x");
    EXPECT_DUMP("<span style=\"color: red\">r</span>", R"x((HtmlTag{name="span"} "<span style=\"color: red\">") "r" (HtmlTag{name="span" closing} "</span>"))x");
    EXPECT_DUMP("<https://x.com>", R"x((Autolink (AutolinkDelim "<") "https://x.com" (AutolinkDelim ">")))x");
    EXPECT_DUMP("<me@x.com>", R"x((Autolink{email} (AutolinkDelim "<") "me@x.com" (AutolinkDelim ">")))x");
    EXPECT_DUMP("a < b > c <3", R"x("a < b > c <3")x");
}

TEST(InlineObsidian, BareUrls)
{
    EXPECT_DUMP("see https://example.com/path.", R"x("see " (BareUrl "https://example.com/path") ".")x");
    EXPECT_DUMP("www.example.com/x?y=1", R"x((BareUrl{www} "www.example.com/x?y=1"))x");
    EXPECT_DUMP("(https://x.com/a_(b))", R"x("(" (BareUrl "https://x.com/a_(b)") ")")x");
    EXPECT_DUMP("mail me@x.com now", R"x("mail " (BareUrl{email} "me@x.com") " now")x");
    EXPECT_DUMP("mailto:me@x.com", R"x((BareUrl "mailto:me@x.com"))x");
    EXPECT_DUMP("https://x.com/#tag $a$", R"x((BareUrl "https://x.com/#tag") " " (InlineMath (MathDelim "$") "a" (MathDelim "$")))x");
    EXPECT_DUMP("**https://x.com**", R"x((Strong (EmphasisDelim "**") (BareUrl "https://x.com") (EmphasisDelim "**")))x");
    EXPECT_DUMP("_a@b.co_", R"x((Emphasis (EmphasisDelim "_") (BareUrl{email} "a@b.co") (EmphasisDelim "_")))x");
    // GFM: no extended autolinks inside link text (brackets)
    EXPECT_DUMP("[see https://x.com and a@b.co](u)",
                R"x((Link{dest="u"} (LinkOpen "[") "see https://x.com and a@b.co" (LinkClose "](" (LinkUrl "u") ")")))x");
    EXPECT_DUMP("xhttps://x.com notwww.x.com", R"x("xhttps://x.com notwww.x.com")x");
    EXPECT_DUMP("http:// and www.x and www.", R"x("http:// and " (BareUrl{www} "www.x") " and www.")x");
}

TEST(InlineObsidian, EscapesEntitiesBreaks)
{
    EXPECT_DUMP("\\*not em\\*", R"x((Escape (EscapeChar "\\") "*") "not em" (Escape (EscapeChar "\\") "*"))x");
    EXPECT_DUMP("\\a \\\\", R"x("\\a " (Escape (EscapeChar "\\") "\\"))x");
    EXPECT_DUMP("&amp; &copy; &#35; &#x1F600; &bogus; &#;",
                R"x((Entity "&amp;") " " (Entity "&copy;") " " (Entity "&#35;") " " (Entity "&#x1F600;") " &bogus; &#;")x");
    EXPECT_DUMP("a  \nb", R"x("a" (HardBreak "  \n") "b")x");
    EXPECT_DUMP("a\\\nb", R"x("a" (HardBreak "\\\n") "b")x");
    EXPECT_DUMP("a\nb", R"x("a" (SoftBreak "\n") "b")x");
    EXPECT_DUMP("a \n  b", R"x("a" (SoftBreak " \n  ") "b")x");
    EXPECT_DUMP("a\t\nb", R"x("a" (SoftBreak "\t\n") "b")x");
    EXPECT_DUMP("end\\", R"x("end\\")x");
}

// ---- precedence -------------------------------------------------------------------------------------

TEST(InlineObsidian, PrecedenceClashes)
{
    EXPECT_DUMP("`[[not a link]]`", R"x((CodeSpan (CodeDelim "`") "[[not a link]]" (CodeDelim "`")))x");
    EXPECT_DUMP("[[a `b]] c`", R"x((WikiLink{target="a `b"} (WikiOpen "[[") (WikiTarget "a `b") (WikiClose "]]")) " c`")x");
    EXPECT_DUMP("`a $b` c$", R"x((CodeSpan (CodeDelim "`") "a $b" (CodeDelim "`")) " c$")x");
    EXPECT_DUMP("$a `b$ c`", R"x((InlineMath (MathDelim "$") "a `b" (MathDelim "$")) " c`")x");
    EXPECT_DUMP("$[[x]]$", R"x((InlineMath (MathDelim "$") "[[x]]" (MathDelim "$")))x");
    EXPECT_DUMP("[[a$b]] $c$", R"x((WikiLink{target="a$b"} (WikiOpen "[[") (WikiTarget "a$b") (WikiClose "]]")) " " (InlineMath (MathDelim "$") "c" (MathDelim "$")))x");
    EXPECT_DUMP("%%a [[b]]%%", R"x((Comment (CommentDelim "%%") "a [[b]]" (CommentDelim "%%")))x");
    EXPECT_DUMP("[[a %% b]] %%", R"x((WikiLink{target="a %% b"} (WikiOpen "[[") (WikiTarget "a %% b") (WikiClose "]]")) " %%")x");
    EXPECT_DUMP("`%%` not %% comment", R"x((CodeSpan (CodeDelim "`") "%%" (CodeDelim "`")) " not %% comment")x");
    EXPECT_DUMP("#tag`code`", R"x((Tag{tag="tag"} (TagHash "#") "tag") (CodeSpan (CodeDelim "`") "code" (CodeDelim "`")))x");
    EXPECT_DUMP("`#nottag` #tag", R"x((CodeSpan (CodeDelim "`") "#nottag" (CodeDelim "`")) " " (Tag{tag="tag"} (TagHash "#") "tag"))x");
    EXPECT_DUMP("#tag$x$", R"x((Tag{tag="tag"} (TagHash "#") "tag") (InlineMath (MathDelim "$") "x" (MathDelim "$")))x");
    EXPECT_DUMP("*a [[b*]] c*",
                R"x((Emphasis (EmphasisDelim "*") "a " (WikiLink{target="b*"} (WikiOpen "[[") (WikiTarget "b*") (WikiClose "]]")) " c" (EmphasisDelim "*")))x");
    EXPECT_DUMP("**a `**` b**", R"x((Strong (EmphasisDelim "**") "a " (CodeSpan (CodeDelim "`") "**" (CodeDelim "`")) " b" (EmphasisDelim "**")))x");
    EXPECT_DUMP("<https://x.com/[[a]]>", R"x((Autolink (AutolinkDelim "<") "https://x.com/[[a]]" (AutolinkDelim ">")))x");
    EXPECT_DUMP("[[a]]#tag", R"x((WikiLink{target="a"} (WikiOpen "[[") (WikiTarget "a") (WikiClose "]]")) "#tag")x");
    EXPECT_DUMP("[x](y) #t", R"x((Link{dest="y"} (LinkOpen "[") "x" (LinkClose "](" (LinkUrl "y") ")")) " " (Tag{tag="t"} (TagHash "#") "t"))x");
}

TEST(InlineObsidian, CalloutTitleContent)
{
    // callout titles / table cells are parsed as standalone contents
    EXPECT_DUMP("Read [[Guide#Setup|the guide]] **now**",
                R"x("Read " (WikiLink{target="Guide" subpath="#Setup" alias="the guide"} (WikiOpen "[[") (WikiTarget "Guide#Setup") (WikiPipe "|") "the guide" (WikiClose "]]")) " " (Strong (EmphasisDelim "**") "now" (EmphasisDelim "**")))x");
    EXPECT_DUMP("Tip: see [docs](https://x.com) #todo",
                R"x("Tip: see " (Link{dest="https://x.com"} (LinkOpen "[") "docs" (LinkClose "](" (LinkUrl "https://x.com") ")")) " " (Tag{tag="todo"} (TagHash "#") "todo"))x");
}

// ---- rule predicates ----------------------------------------------------------------------------------

TEST(InlineRules, MathScanner)
{
    auto m = [](const char* s) { return rules::scanInlineMath(QString::fromUtf8(s), 0); };
    EXPECT_EQ(m("$x$"), 3);
    EXPECT_EQ(m("$x$."), 3);
    EXPECT_EQ(m("$x$a"), 3);
    EXPECT_EQ(m("$x$1"), -1);
    EXPECT_EQ(m("$ x$"), -1);
    EXPECT_EQ(m("$x $"), -1);
    EXPECT_EQ(m("$$"), -1);
    EXPECT_EQ(m("$ x$"), -1); // NBSP counts as whitespace (JS \s)
    EXPECT_EQ(m("$x\\$ y$"), 7);
    EXPECT_EQ(m("$x\ny$"), -1);
    EXPECT_EQ(m("$x"), -1);
    EXPECT_EQ(rules::scanDisplayMath(QStringLiteral("$$a\\$$b$$"), 0), 9);
    EXPECT_EQ(rules::scanDisplayMath(QStringLiteral("$$ab"), 0), -1);
}

TEST(InlineRules, TagScanner)
{
    auto t = [](const char* s) { return rules::scanTag(QString::fromUtf8(s), 0); };
    EXPECT_EQ(t("#a"), 2);
    EXPECT_EQ(t("#a/b-c_d"), 8);
    EXPECT_EQ(t("#123"), -1);
    EXPECT_EQ(t("#12a"), 4);
    EXPECT_EQ(t("#"), -1);
    EXPECT_EQ(t("# a"), -1);
    EXPECT_EQ(t("#a.b"), 2);
    EXPECT_EQ(t("#a'b"), 2);
    EXPECT_EQ(t("#a’b"), 2); // U+2019 is in U+2000-206F
    EXPECT_EQ(t("#a⺀b"), 4); // CJK radical: allowed (outside the excluded blocks)
}

TEST(InlineRules, ImageSize)
{
    int w = 0, h = 0;
    EXPECT_TRUE(rules::parseImageSize(QStringLiteral("200"), &w, &h));
    EXPECT_EQ(w, 200);
    EXPECT_EQ(h, -1);
    EXPECT_TRUE(rules::parseImageSize(QStringLiteral(" 200 x 100 "), &w, &h));
    EXPECT_EQ(w, 200);
    EXPECT_EQ(h, 100);
    EXPECT_FALSE(rules::parseImageSize(QStringLiteral("200X100"), &w, &h));
    EXPECT_FALSE(rules::parseImageSize(QStringLiteral("x100"), &w, &h));
    EXPECT_FALSE(rules::parseImageSize(QStringLiteral("abc"), &w, &h));
    EXPECT_TRUE(rules::parseSizeSuffix(QStringLiteral("caption|640x480"), &w, &h));
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 480);
    EXPECT_FALSE(rules::parseSizeSuffix(QStringLiteral("640|caption"), &w, &h));
}

TEST(InlineRules, EntitiesAndLabels)
{
    EXPECT_EQ(rules::decodeEntity(u"&amp;"), QStringLiteral("&"));
    EXPECT_EQ(rules::decodeEntity(u"&ngE;"), QString::fromUtf8("≧̸"));
    EXPECT_EQ(rules::decodeEntity(u"&#0;"), QString(QChar(0xFFFD)));
    EXPECT_EQ(rules::decodeEntity(u"&#x1F600;"), QString::fromUtf8("😀"));
    EXPECT_EQ(rules::scanEntity(u"&#87654321;", 0), -1);
    EXPECT_EQ(rules::scanEntity(u"&MadeUpEntity;", 0), -1);
    EXPECT_EQ(rules::normalizeLinkLabel(u"  Foo \t\n BAR  "), QStringLiteral("foo bar"));
    EXPECT_EQ(rules::normalizeLinkLabel(QString::fromUtf8("ẞ")), rules::normalizeLinkLabel(u"SS")); // full case folding
    EXPECT_EQ(rules::unescapeString(u"a\\*b&amp;c\\q"), QStringLiteral("a*b&c\\q"));
}

// ---- robustness ---------------------------------------------------------------------------------------

// Development helper: OC_INLINE_DEBUG='text with \n escapes' prints the tree (skipped otherwise).
TEST(InlineObsidian, DebugDump)
{
    const QByteArray env = qgetenv("OC_INLINE_DEBUG");
    if (env.isEmpty())
        GTEST_SKIP() << "set OC_INLINE_DEBUG to dump a tree";
    QString s = QString::fromUtf8(env);
    s.replace(QStringLiteral("\\n"), QStringLiteral("\n")).replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    const InlineTreePtr t = parseInline(s);
    std::printf("%s\n", qPrintable(dumpSexpr(*t)));
    for (int i = 0; i < t->size(); ++i) {
        const InlineNode& n = t->node(i);
        std::printf("%3d %-14s %-13s [%d,%d) parent=%d first=%d next=%d\n", i, test::typeName(n.type), test::markName(n.mark), n.from, n.to, n.parent,
                    n.firstChild, n.next);
    }
    std::printf("%s\n", qPrintable(test::renderHtml(*t)));
}

TEST(InlineObsidian, EmptyAndTrivial)
{
    EXPECT_EQ(parseInline(QString())->size(), 0);
    EXPECT_EQ(parseInline(QString())->firstRoot(), -1);
    EXPECT_DUMP("plain text", R"x("plain text")x");
    EXPECT_DUMP("[", R"x("[")x");
    EXPECT_DUMP("]", R"x("]")x");
    EXPECT_DUMP("!", R"x("!")x");
    EXPECT_DUMP("$", R"x("$")x");
    EXPECT_DUMP("\\", R"x("\\")x");
    EXPECT_DUMP("^", R"x("^")x");
}

TEST(InlineObsidian, RandomInputsKeepTreeInvariants)
{
    const QString alphabet = QStringLiteral("ab #*_~=`$%[]()!^<>&;:/\\|@.\n-'\"\t1wh");
    const QStringList chunks = {QStringLiteral("[["), QStringLiteral("]]"), QStringLiteral("**"), QStringLiteral("~~"), QStringLiteral("=="),
                                QStringLiteral("$$"), QStringLiteral("%%"), QStringLiteral("http://a.b"), QStringLiteral("www.x.y"),
                                QStringLiteral("<a href=\"x\">"), QStringLiteral("</b>"), QStringLiteral("&amp;"), QStringLiteral("[^1]"),
                                QStringLiteral("^["), QStringLiteral("](u \"t\")"), QStringLiteral("a@b.co"), QStringLiteral(" ^id"), QStringLiteral("😀"),
                                QStringLiteral("<!--"), QStringLiteral("-->"), QStringLiteral("``")};
    QRandomGenerator r(1234);
    const QList<LinkRefDefinition> refs = {ref("a", "/a"), ref("b", "/b", "t")};
    for (int iter = 0; iter < 20000; ++iter) {
        QString s;
        const int len = r.bounded(40);
        for (int k = 0; k < len; ++k) {
            if (r.bounded(4) == 0)
                s += chunks[r.bounded(int(chunks.size()))];
            else
                s += alphabet[r.bounded(int(alphabet.size()))];
        }
        const InlineTreePtr t = parseInline(s, refs);
        checkTree(*t, s);
        if (HasFatalFailure()) {
            QString esc = s;
            esc.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\n'), QStringLiteral("\\n")).replace(QLatin1Char('\t'), QStringLiteral("\\t"));
            ADD_FAILURE() << "input (escaped): " << qPrintable(esc) << "\ndump: " << qPrintable(dumpSexpr(*t));
            return;
        }
        // rendering never crashes and every tree is reproducible
        (void)test::renderHtml(*t);
        ASSERT_EQ(dumpSexpr(*t), dumpSexpr(*parseInline(s, refs)));
    }
}
