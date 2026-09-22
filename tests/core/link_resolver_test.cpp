#include "core/metadata/link_resolver.h"

#include <gtest/gtest.h>

using namespace oc;

namespace {
LinkResolver makeResolver()
{
    LinkResolver r;
    r.setFiles({"Welcome.md", "Projects/Alpha.md", "Projects/Beta.md", "Projects/Notes.md", "Archive/Notes.md", "attachments/image.png",
                "Deep/a/b/Notes.md", "Daily/2026-09-22.md", "Some Note With Spaces.md", "Case/UPPER.md"});
    return r;
}
} // namespace

TEST(LinkResolver, BasicResolution)
{
    const LinkResolver r = makeResolver();
    EXPECT_EQ(r.resolve("Welcome", "Projects/Alpha.md"), "Welcome.md");
    EXPECT_EQ(r.resolve("welcome", "x.md"), "Welcome.md"); // case-insensitive
    EXPECT_EQ(r.resolve("Welcome.md", "x.md"), "Welcome.md");
    EXPECT_EQ(r.resolve("Projects/Beta", "x.md"), "Projects/Beta.md");
    EXPECT_EQ(r.resolve("image.png", "x.md"), "attachments/image.png");
    EXPECT_EQ(r.resolve("Some Note With Spaces", "x.md"), "Some Note With Spaces.md");
    EXPECT_EQ(r.resolve("upper", "x.md"), "Case/UPPER.md");
    EXPECT_EQ(r.resolve("Does not exist", "x.md"), "");
    EXPECT_EQ(r.resolve("", "Projects/Alpha.md"), "Projects/Alpha.md"); // [[#heading]]
}

TEST(LinkResolver, DuplicateNamesPreferSameFolderThenShortest)
{
    const LinkResolver r = makeResolver();
    EXPECT_EQ(r.resolve("Notes", "Projects/Alpha.md"), "Projects/Notes.md");
    EXPECT_EQ(r.resolve("Notes", "Archive/x.md"), "Archive/Notes.md");
    // from elsewhere: fewest path segments, then alphabetical
    EXPECT_EQ(r.resolve("Notes", "Welcome.md"), "Archive/Notes.md");
    EXPECT_EQ(r.resolve("b/Notes", "Welcome.md"), "Deep/a/b/Notes.md"); // path suffix disambiguates
    EXPECT_EQ(r.candidates("Notes", "Welcome.md").size(), 3);
}

TEST(LinkResolver, RelativeAndMarkdownLinks)
{
    const LinkResolver r = makeResolver();
    EXPECT_EQ(r.resolve("./Beta", "Projects/Alpha.md"), "Projects/Beta.md");
    EXPECT_EQ(r.resolve("../Welcome.md", "Projects/Alpha.md", true), "Welcome.md");
    EXPECT_EQ(r.resolve("Beta.md", "Projects/Alpha.md", true), "Projects/Beta.md");
    EXPECT_EQ(r.resolve("./Missing", "Projects/Alpha.md"), "");
}

TEST(LinkResolver, SubpathSplitAndLinkText)
{
    EXPECT_EQ(LinkResolver::splitSubpath("Note#Heading#Sub"), (QPair<QString, QString>{"Note", "#Heading#Sub"}));
    EXPECT_EQ(LinkResolver::splitSubpath("#^block"), (QPair<QString, QString>{"", "#^block"}));
    const LinkResolver r = makeResolver();
    EXPECT_EQ(r.linkText("Projects/Beta.md", "Welcome.md", LinkResolver::LinkFormat::Shortest), "Beta");
    EXPECT_EQ(r.linkText("Projects/Notes.md", "Welcome.md", LinkResolver::LinkFormat::Shortest), "Projects/Notes");
    EXPECT_EQ(r.linkText("attachments/image.png", "Welcome.md", LinkResolver::LinkFormat::Shortest), "image.png");
    EXPECT_EQ(r.linkText("Projects/Beta.md", "Daily/2026-09-22.md", LinkResolver::LinkFormat::Relative), "../Projects/Beta");
    EXPECT_EQ(r.linkText("Projects/Beta.md", "Projects/Alpha.md", LinkResolver::LinkFormat::Relative), "Beta");
    EXPECT_EQ(r.linkText("Projects/Beta.md", "x.md", LinkResolver::LinkFormat::Absolute), "Projects/Beta");
}

TEST(LinkResolver, IncrementalAddRemove)
{
    LinkResolver r = makeResolver();
    EXPECT_EQ(r.resolve("Gamma", "x.md"), "");
    r.addFile("New/Gamma.md");
    EXPECT_EQ(r.resolve("Gamma", "x.md"), "New/Gamma.md");
    r.removeFile("New/Gamma.md");
    EXPECT_EQ(r.resolve("Gamma", "x.md"), "");
}
