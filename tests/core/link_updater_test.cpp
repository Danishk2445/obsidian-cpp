// Link updating on rename/move (Obsidian's "Automatically update internal links").
#include "core/metadata/link_updater.h"
#include "markdown/syntax_tree.h"
#include "text/codec.h"

#include <gtest/gtest.h>

using namespace oc;

namespace {
using Files = QList<QPair<QString, QString>>; // path -> content ("" for attachments)

// Links as the metadata cache sees them: block + inline parser, then extractMetadata().
md::CachedMetadata extract(const Text& t)
{
    md::SyntaxTree tree;
    tree.parse(t);
    return md::extractMetadata(tree, t);
}

struct Vault {
    explicit Vault(const Files& files)
    {
        for (const auto& [p, c] : files) {
            allPaths << p;
            if (p.endsWith(".md")) {
                texts.insert(p, Text::fromString(c));
                meta.insert(p, extract(texts[p]));
            }
        }
        resolver.setFiles(allPaths);
    }
    // Rename (file or folder) and return the rewritten texts of the files that changed.
    QHash<QString, QString> rename(const QString& from, const QString& to, LinkResolver::LinkFormat format = LinkResolver::LinkFormat::Shortest)
    {
        const QList<LinkUpdater::Reference> refs =
            LinkUpdater::snapshot(resolver, meta.keys(), [this](const QString& p) { return meta.contains(p) ? &meta[p] : nullptr; });
        const LinkUpdater::PathMap moved = LinkUpdater::renameMap(from, to);
        QHash<QString, Text> after;
        for (auto it = texts.cbegin(); it != texts.cend(); ++it)
            after.insert(moved(it.key()), it.value());
        texts = after;
        for (QString& p : allPaths) {
            if (moved(p) != p) {
                resolver.removeFile(p);
                resolver.addFile(moved(p));
            }
            p = moved(p);
        }
        QHash<QString, QString> out;
        const auto edits = LinkUpdater::edits(refs, resolver, moved, format);
        for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
            Text& t = texts[it.key()];
            if (LinkUpdater::apply(t, it.value()) > 0)
                out.insert(it.key(), t.toString());
        }
        meta.clear();
        for (auto it = texts.cbegin(); it != texts.cend(); ++it)
            meta.insert(it.key(), extract(it.value()));
        return out;
    }
    LinkResolver resolver;
    QHash<QString, Text> texts;
    QHash<QString, md::CachedMetadata> meta;
    QStringList allPaths;
};

md::LinkCache wiki(const QString& original, const QString& link)
{
    md::LinkCache l;
    l.original = original;
    l.link = link;
    return l;
}
md::LinkCache mdlink(const QString& original, const QString& link)
{
    md::LinkCache l = wiki(original, link);
    l.markdown = true;
    return l;
}
} // namespace

TEST(LinkUpdaterRewrite, Wikilinks)
{
    EXPECT_EQ(LinkUpdater::rewrite(wiki("[[Old]]", "Old"), "New"), "[[New]]");
    EXPECT_EQ(LinkUpdater::rewrite(wiki("![[Old]]", "Old"), "New"), "![[New]]");
    EXPECT_EQ(LinkUpdater::rewrite(wiki("[[Old#H|alias]]", "Old#H"), "New#H"), "[[New#H|alias]]");
    // escaped pipe inside tables is kept
    EXPECT_EQ(LinkUpdater::rewrite(wiki("[[Old\\|alias]]", "Old"), "New"), "[[New\\|alias]]");
    // an alias repeating the old file name of a path link follows the rename
    EXPECT_EQ(LinkUpdater::rewrite(wiki("[[a/Old|Old]]", "a/Old"), "b/New"), "[[b/New|New]]");
    EXPECT_EQ(LinkUpdater::rewrite(wiki("[[Old|Old]]", "Old"), "New"), "[[New|Old]]"); // no '/': alias kept
}

TEST(LinkUpdaterRewrite, MarkdownLinks)
{
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[text](Old.md)", "Old.md"), "New Note.md"), "[text](New%20Note.md)");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[Old](Old.md)", "Old.md"), "New.md"), "[New](New.md)");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[a/Old](a/Old.md)", "a/Old.md"), "b/New.md"), "[b/New](b/New.md)");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[t](<Old one.md>)", "Old one.md"), "New one.md"), "[t](<New one.md>)");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[t](Old.md \"title\")", "Old.md"), "New.md"), "[t](New.md \"title\")");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("![](Old.png)", "Old.png"), "img/Old.png"), "![](img/Old.png)");
    EXPECT_EQ(LinkUpdater::rewrite(mdlink("[t](Old.md#Sec)", "Old.md#Sec"), "x\\y.md#Sec"), "[t](x%5Cy.md#Sec)");
}

TEST(LinkUpdater, RenameRewritesLinksToTheFile)
{
    Vault v({{"Welcome.md", "See [[Other#Head|alias]] and [[Other]].\n![[Other]]\n[md](Other.md)\n"},
                    {"Other.md", "# Head\n"},
                    {"Unrelated.md", "[[Welcome]] [[Missing]]\n"}});
    const auto changed = v.rename("Other.md", "Renamed.md");
    ASSERT_EQ(changed.size(), 1);
    EXPECT_EQ(changed.value("Welcome.md"), "See [[Renamed#Head|alias]] and [[Renamed]].\n![[Renamed]]\n[md](Renamed.md)\n");
    EXPECT_EQ(v.resolver.resolve("Renamed", "Welcome.md"), "Renamed.md");
}

TEST(LinkUpdater, MoveKeepsShortestLinksAndFixesPathLinks)
{
    Vault v({{"Welcome.md", "[[Note]] [[Folder/Note|Note]] [x](Note.md)\n"},
                    {"Folder/Note.md", "hi\n"},
                    {"Folder/Deep/child.md", "[up](../Note.md) [[Note]]\n"}});
    const auto changed = v.rename("Folder", "Dir");
    // [[Note]] and [x](Note.md) still mean the same file with the shortest form: untouched.
    EXPECT_EQ(changed.value("Welcome.md"), "[[Note]] [[Note|Note]] [x](Note.md)\n");
    // The moved file's relative link is rewritten in the configured (shortest) format.
    EXPECT_EQ(changed.value("Dir/Deep/child.md"), "[up](Note.md) [[Note]]\n");
}

TEST(LinkUpdater, AmbiguousNamesGetFullPaths)
{
    Vault v({{"a.md", "[[Target]]\n"}, {"x/Target.md", ""}, {"y/Other.md", ""}});
    // Renaming Other -> Target makes the bare name ambiguous: the link keeps pointing at x/Target.
    const auto changed = v.rename("y/Other.md", "y/Target.md");
    EXPECT_EQ(changed.value("a.md"), "[[x/Target]]\n");
}

TEST(LinkUpdater, AbsoluteAndRelativeFormats)
{
    {
        Vault v({{"n/a.md", "[[b]]\n"}, {"n/b.md", ""}});
        EXPECT_EQ(v.rename("n/b.md", "m/c.md", LinkResolver::LinkFormat::Absolute).value("n/a.md"), "[[m/c]]\n");
    }
    {
        Vault v({{"n/a.md", "[[b]]\n"}, {"n/b.md", ""}});
        EXPECT_EQ(v.rename("n/b.md", "m/c.md", LinkResolver::LinkFormat::Relative).value("n/a.md"), "[[../m/c]]\n");
    }
}

TEST(LinkUpdater, PropertyLinksAndAttachments)
{
    Vault v({{"a.md", "---\nrelated: \"[[Pic]]\"\nup: \"[[b]]\"\n---\n![[pic.png]]\n"}, {"b.md", ""}, {"pic.png", ""}});
    const auto changed = v.rename("b.md", "c.md");
    EXPECT_EQ(changed.value("a.md"), "---\nrelated: \"[[Pic]]\"\nup: \"[[c]]\"\n---\n![[pic.png]]\n");
    const auto changed2 = v.rename("pic.png", "img/photo.png");
    EXPECT_EQ(changed2.value("a.md"), "---\nrelated: \"[[Pic]]\"\nup: \"[[c]]\"\n---\n![[photo.png]]\n");
}

TEST(LinkUpdater, ApplyIsByteExactAndSkipsStaleEdits)
{
    DecodedFile f = decodeFile(QByteArray("a [[x]] b\r\nc [[x]]  \r\n\tend"));
    Text t = f.text;
    QList<LinkUpdater::Edit> edits{{2, 7, "[[x]]", "[[yy]]"}, {12, 17, "[[x]]", "[[zz]]"}, {0, 1, "Q", "nope"}};
    EXPECT_EQ(LinkUpdater::apply(t, edits), 2);
    EXPECT_EQ(encodeFile(t, f.bom), QByteArray("a [[yy]] b\r\nc [[zz]]  \r\n\tend"));
}
