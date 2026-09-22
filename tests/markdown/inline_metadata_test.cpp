// CachedMetadata extraction (markdown/metadata.cpp) over the real inline parser: links, embeds, tags,
// footnote refs and block ids, with raw constructs (code, comments, math) hiding what they contain.
// Only paragraphs and ATX headings are used, so the test does not depend on other block features.
#include "markdown/metadata.h"
#include "markdown/syntax_tree.h"

#include <gtest/gtest.h>

using namespace oc;
using namespace oc::md;

namespace {

CachedMetadata extract(const QString& s)
{
    const Text t = Text::fromString(s);
    SyntaxTree tree;
    tree.parse(t);
    return extractMetadata(tree, t);
}

QStringList linkTargets(const QList<LinkCache>& links)
{
    QStringList out;
    for (const LinkCache& l : links)
        out << l.link;
    return out;
}

} // namespace

TEST(InlineMetadata, LinksEmbedsTagsFootnotesBlockIds)
{
    const QString doc = QStringLiteral(
        "# Heading with [[Link In Heading]] #htag\n"
        "\n"
        "See [[Note#Sec|alias]], [[Other#A#B]], ![[img.png|100x50]] and [text](Folder/My%20Note.md \"t\").\n"
        "\n"
        "External [x](https://example.com) and #tag/sub plus `[[not a link]]` and %%[[hidden]]%%.\n"
        "\n"
        "A footnote[^1] and $[[math]]$ and \\[[escaped]] ^blk-1\n");
    const CachedMetadata md = extract(doc);

    EXPECT_EQ(linkTargets(md.links), (QStringList{"Link In Heading", "Note#Sec", "Other#A#B", "Folder/My Note.md"}));
    ASSERT_EQ(md.links.size(), 4);
    EXPECT_EQ(md.links[1].original, "[[Note#Sec|alias]]");
    EXPECT_EQ(md.links[1].displayText, "alias");
    EXPECT_EQ(md.links[2].displayText, "Other > A > B");
    EXPECT_FALSE(md.links[2].markdown);
    EXPECT_TRUE(md.links[3].markdown);
    EXPECT_EQ(md.links[3].displayText, "text");
    EXPECT_EQ(md.links[3].original, "[text](Folder/My%20Note.md \"t\")");
    // positions are document positions
    const qsizetype at = doc.indexOf(QStringLiteral("[[Note#Sec"));
    EXPECT_EQ(md.links[1].position.start.offset, at);
    EXPECT_EQ(md.links[1].position.end.offset, at + md.links[1].original.size());
    EXPECT_EQ(md.links[1].position.start.line, 2);
    EXPECT_EQ(md.links[1].position.start.col, 4);

    ASSERT_EQ(md.embeds.size(), 1);
    EXPECT_EQ(md.embeds[0].link, "img.png");
    EXPECT_TRUE(md.embeds[0].embed);
    EXPECT_EQ(md.embeds[0].original, "![[img.png|100x50]]");

    QStringList tags;
    for (const TagCache& t : md.tags)
        tags << t.tag;
    EXPECT_EQ(tags, (QStringList{"#htag", "#tag/sub"}));

    ASSERT_EQ(md.footnoteRefs.size(), 1);
    EXPECT_EQ(md.footnoteRefs[0].id, "1");
    EXPECT_TRUE(md.blocks.contains("blk-1"));
    EXPECT_EQ(md.blocks.value("blk-1").id, "blk-1");
}

TEST(InlineMetadata, ShellTextAndTablePipes)
{
    // A bash test line outside a fence is an (unresolved) wikilink in Obsidian; money is not math.
    const CachedMetadata md = extract(QStringLiteral("if [[ -f \"$f\" ]]; then echo $5 and $10; fi\n"));
    EXPECT_EQ(linkTargets(md.links), (QStringList{"-f \"$f\""}));
    // "\|" inside a wikilink is the alias separator (table cell syntax), the backslash is dropped
    const CachedMetadata cell = extract(QStringLiteral("[[Note\\|alias]]\n"));
    ASSERT_EQ(cell.links.size(), 1);
    EXPECT_EQ(cell.links[0].link, "Note");
    EXPECT_EQ(cell.links[0].displayText, "alias");
}
