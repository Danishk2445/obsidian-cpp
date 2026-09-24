// LeafContent: content offsets <-> document offsets for multi-line leaves with per-line columns.
#include "markdown/inline_tree.h"
#include "text/text.h"

#include <gtest/gtest.h>

using namespace oc;
using namespace oc::md;

namespace {

// "> > first line\n> > second\n> >   third" : a paragraph inside two quotes, content starts at column 4
// (the third line keeps its extra indentation in the content).
struct Fixture {
    Text doc = Text::fromString(u"# Title\n> > first line\n> > second\n> >   third\nafter");
    LeafContent content;
    Fixture()
    {
        content.text = QStringLiteral("first line\nsecond\n  third");
        content.segments = {{1, 4, 0, 10}, {2, 4, 11, 6}, {3, 4, 18, 7}};
    }
};

} // namespace

TEST(InlineLeafContent, ToDocMapsEachLine)
{
    Fixture f;
    const Text& d = f.doc;
    EXPECT_EQ(f.content.toDoc(0, d), d.lineStart(1) + 4);
    EXPECT_EQ(f.content.toDoc(5, d), d.lineStart(1) + 9);
    EXPECT_EQ(f.content.toDoc(10, d), d.lineEnd(1)); // the joining '\n' maps to the end of its line
    EXPECT_EQ(f.content.toDoc(11, d), d.lineStart(2) + 4);
    EXPECT_EQ(f.content.toDoc(17, d), d.lineEnd(2));
    EXPECT_EQ(f.content.toDoc(18, d), d.lineStart(3) + 4);
    EXPECT_EQ(f.content.toDoc(25, d), d.lineEnd(3));
    // clamped outside the content
    EXPECT_EQ(f.content.toDoc(-3, d), d.lineStart(1) + 4);
    EXPECT_EQ(f.content.toDoc(99, d), d.lineEnd(3));
    EXPECT_EQ(d.slice(f.content.toDoc(11, d), f.content.toDoc(17, d)), QStringLiteral("second"));
    // a range crossing lines keeps the document's own separators
    EXPECT_EQ(d.slice(f.content.toDoc(6, d), f.content.toDoc(17, d)), QStringLiteral("line\n> > second"));
}

TEST(InlineLeafContent, FromDocInvertsToDoc)
{
    Fixture f;
    const Text& d = f.doc;
    for (int off = 0; off <= int(f.content.text.size()); ++off)
        EXPECT_EQ(f.content.fromDoc(f.content.toDoc(off, d), d), off) << off;
    // container markers and other lines are outside the content
    EXPECT_EQ(f.content.fromDoc(d.lineStart(1), d), -1);
    EXPECT_EQ(f.content.fromDoc(d.lineStart(2) + 2, d), -1);
    EXPECT_EQ(f.content.fromDoc(d.lineStart(0) + 3, d), -1);
    EXPECT_EQ(f.content.fromDoc(d.lineStart(4) + 1, d), -1);
    EXPECT_EQ(f.content.fromDoc(-1, d), -1);
    EXPECT_EQ(f.content.fromDoc(d.length() + 1, d), -1);
}

TEST(InlineLeafContent, LineOf)
{
    Fixture f;
    EXPECT_EQ(f.content.lineOf(0), 1);
    EXPECT_EQ(f.content.lineOf(10), 1); // '\n' belongs to the line it ends
    EXPECT_EQ(f.content.lineOf(11), 2);
    EXPECT_EQ(f.content.lineOf(20), 3);
    EXPECT_EQ(f.content.lineOf(1000), 3);
    LeafContent empty;
    EXPECT_EQ(empty.lineOf(3), 0);
    EXPECT_EQ(empty.toDoc(3, f.doc), 0);
    EXPECT_EQ(empty.fromDoc(3, f.doc), -1);
}

TEST(InlineLeafContent, InlineNodesMapToDocument)
{
    Fixture f;
    f.doc = Text::fromString(u"> **bold\n> text** and [[Note|alias]]");
    f.content.text = QStringLiteral("**bold\ntext** and [[Note|alias]]");
    f.content.segments = {{0, 2, 0, 6}, {1, 2, 7, 25}};
    const InlineTreePtr t = parseInline(f.content.text);
    ASSERT_GE(t->size(), 1);
    const InlineNode& strong = t->node(0);
    ASSERT_EQ(strong.type, InlineType::Strong);
    EXPECT_EQ(f.doc.slice(f.content.toDoc(strong.from, f.doc), f.content.toDoc(strong.to, f.doc)), QStringLiteral("**bold\n> text**"));
    int wiki = -1;
    for (int i = 0; i < t->size(); ++i)
        if (t->node(i).type == InlineType::WikiLink)
            wiki = i;
    ASSERT_GE(wiki, 0);
    EXPECT_EQ(f.doc.slice(f.content.toDoc(t->node(wiki).from, f.doc), f.content.toDoc(t->node(wiki).to, f.doc)), QStringLiteral("[[Note|alias]]"));
    EXPECT_EQ(f.content.lineOf(t->node(wiki).from), 1);
}
