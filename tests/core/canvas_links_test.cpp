// Canvas files as link sources in the metadata cache (Obsidian's canvas index): file cards, group
// backgrounds and text-card links count in resolvedLinks / unresolvedLinks and backlinks.
#include "core/canvas/canvas_links.h"
#include "core/metadata/metadata_cache.h"

#include <QHash>
#include <QTest>

#include <gtest/gtest.h>

using namespace oc;

namespace {
const QByteArray kCanvas = R"json({
	"nodes":[
		{"id":"f","type":"file","file":"notes/Note A.md","subpath":"#Heading","x":0,"y":0,"width":400,"height":400},
		{"id":"t","type":"text","text":"See [[Note B]] and [[Missing note]], ![[pic.png]] and [md](notes/Note%20A.md)","x":0,"y":500,"width":250,"height":60},
		{"id":"g","type":"group","background":"pic.png","x":-50,"y":-50,"width":1000,"height":1000},
		{"id":"l","type":"link","url":"https://example.com","x":500,"y":0,"width":400,"height":400}
	],
	"edges":[]
})json";

class CanvasLinksCache : public ::testing::Test {
protected:
    void SetUp() override
    {
        files = {{"notes/Note A.md", "# Heading\ntext"}, {"Note B.md", "b"}, {"pic.png", "png"}, {"board.canvas", kCanvas}};
        cache.setContentProvider([this](const QString& p) -> std::optional<QByteArray> {
            if (!files.contains(p))
                return std::nullopt;
            return files.value(p);
        });
        cache.indexAll(files.keys());
        ASSERT_TRUE(QTest::qWaitFor([&] { return cache.isReady() && cache.isClean(); }, 10000));
    }
    QHash<QString, QByteArray> files;
    MetadataCache cache;
};
} // namespace

TEST(CanvasLinks, Extraction)
{
    const canvas::CanvasLinks links = canvas::canvasLinks(kCanvas);
    ASSERT_EQ(links.embeds.size(), 2);
    EXPECT_EQ(links.embeds[0].link, "notes/Note A.md#Heading");
    EXPECT_TRUE(links.embeds[0].embed);
    EXPECT_EQ(links.embeds[1].link, "pic.png");
    ASSERT_EQ(links.textCards.size(), 1);
    EXPECT_EQ(links.textCards[0].nodeId, "t");
    EXPECT_EQ(links.textCards[0].meta.links.size(), 3); // two wikilinks + the markdown link
    EXPECT_EQ(links.textCards[0].meta.embeds.size(), 1);
    EXPECT_EQ(links.all().size(), 6);
    EXPECT_TRUE(canvas::canvasLinks("not json").all().isEmpty());
    EXPECT_TRUE(canvas::canvasLinks("").all().isEmpty());
}

TEST_F(CanvasLinksCache, CanvasLinksCountInTheLinkGraph)
{
    EXPECT_EQ(cache.fileCache("board.canvas"), nullptr); // no markdown metadata
    EXPECT_FALSE(cache.markdownFiles().contains("board.canvas"));
    EXPECT_EQ(cache.canvasFiles(), QStringList{"board.canvas"});
    const QHash<QString, int> resolved = cache.resolvedLinks().value("board.canvas");
    EXPECT_EQ(resolved.value("notes/Note A.md"), 2); // file card + markdown link
    EXPECT_EQ(resolved.value("Note B.md"), 1);
    EXPECT_EQ(resolved.value("pic.png"), 2); // embed in the text + group background
    EXPECT_EQ(cache.unresolvedLinks().value("board.canvas").value("Missing note"), 1);
    EXPECT_TRUE(cache.backlinksOf("Note B.md").contains("board.canvas"));
    EXPECT_EQ(cache.backlinkCount("notes/Note A.md"), 2);
    EXPECT_EQ(cache.fileLinks("board.canvas").size(), 6);
}

TEST_F(CanvasLinksCache, FollowsCreateModifyRenameDelete)
{
    // The missing note appears: the canvas link resolves.
    files.insert("Missing note.md", "now here");
    cache.fileCreated("Missing note.md");
    EXPECT_EQ(cache.resolvedLinks().value("board.canvas").value("Missing note.md"), 1);
    // The canvas changes.
    files.insert("board.canvas", R"json({"nodes":[{"id":"t","type":"text","text":"[[Note B]]","x":0,"y":0,"width":10,"height":10}]})json");
    cache.fileModified("board.canvas");
    ASSERT_TRUE(QTest::qWaitFor([&] { return cache.isClean(); }, 10000));
    QTest::qWait(0);
    EXPECT_EQ(cache.resolvedLinks().value("board.canvas"), (QHash<QString, int>{{"Note B.md", 1}}));
    EXPECT_FALSE(cache.backlinksOf("notes/Note A.md").contains("board.canvas"));
    // Renamed canvas keeps its links.
    files.insert("boards/b.canvas", files.take("board.canvas"));
    cache.fileRenamed("board.canvas", "boards/b.canvas");
    EXPECT_TRUE(cache.backlinksOf("Note B.md").contains("boards/b.canvas"));
    EXPECT_FALSE(cache.resolvedLinks().contains("board.canvas"));
    // Deleted canvas: gone from the graph.
    cache.fileDeleted("boards/b.canvas");
    EXPECT_FALSE(cache.backlinksOf("Note B.md").contains("boards/b.canvas"));
    EXPECT_TRUE(cache.canvasFiles().isEmpty());
}
