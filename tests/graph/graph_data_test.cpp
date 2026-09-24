// GraphData, GraphFilter (filters, colour groups, timelapse steps), local graph extraction,
// GraphSettings JSON and the fallback query matcher.
#include "graph/graph_data.h"
#include "graph/graph_settings.h"
#include "graph/simple_query.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <gtest/gtest.h>

#include <algorithm>

using namespace oc::graph;

namespace {

QStringList ids(const GraphData& g)
{
    QStringList out;
    for (const GraphNode& n : g.nodes())
        out << n.id;
    return out;
}

QStringList sortedIds(const GraphData& g)
{
    QStringList out = ids(g);
    out.sort();
    return out;
}

QStringList edgeList(const GraphData& g)
{
    QStringList out;
    for (const GraphEdge& e : g.edges())
        out << g.node(e.source).id + QStringLiteral("->") + g.node(e.target).id;
    out.sort();
    return out;
}

// A.md -> B.md, A.md -> C.md, B.md -> C.md, C.md -> A.md, D.md (orphan), E.md -> img.png,
// A.md -> #tag, B.md -> #tag, B.md -> "Missing"
GraphData sample()
{
    GraphData g;
    g.addNode("A.md", NodeKind::File, {}, 100);
    g.addNode("B.md", NodeKind::File, {}, 200);
    g.addNode("sub/C.md", NodeKind::File, {}, 300);
    g.addNode("D.md", NodeKind::File, {}, 400);
    g.addNode("E.md", NodeKind::File, {}, 500);
    g.addNode("img.png", NodeKind::Attachment);
    g.addNode("#tag", NodeKind::Tag);
    g.addNode("Missing", NodeKind::Unresolved);
    g.addEdge("A.md", "B.md");
    g.addEdge("A.md", "sub/C.md");
    g.addEdge("A.md", "#tag");
    g.addEdge("B.md", "sub/C.md");
    g.addEdge("B.md", "#tag");
    g.addEdge("B.md", "Missing");
    g.addEdge("sub/C.md", "A.md");
    g.addEdge("E.md", "img.png");
    g.finalize();
    return g;
}

} // namespace

TEST(GraphData, BuildDedupAndAdjacency)
{
    GraphData g;
    EXPECT_EQ(g.addNode("a.md"), 0);
    EXPECT_EQ(g.addNode("b.md"), 1);
    EXPECT_EQ(g.addNode("a.md", NodeKind::Tag), 0); // existing node untouched
    EXPECT_EQ(g.node(0).kind, NodeKind::File);
    EXPECT_TRUE(g.addEdge(0, 1));
    EXPECT_FALSE(g.addEdge(0, 1)); // duplicate
    EXPECT_TRUE(g.addEdge(1, 0));  // reverse direction is a separate link
    EXPECT_TRUE(g.addEdge(0, 0));  // self link kept
    EXPECT_FALSE(g.addEdge("a.md", "nope.md"));
    g.finalize();
    EXPECT_EQ(g.edgeCount(), 3);
    EXPECT_EQ(g.node(0).weight, 4); // forward {b, a} + reverse {b, a}
    EXPECT_EQ(g.node(1).weight, 2);
    ASSERT_EQ(g.neighbors(0).size(), 1u);
    EXPECT_EQ(g.neighbors(0)[0], 1);
    EXPECT_TRUE(g.hasEdge(1, 0));
    EXPECT_FALSE(g.hasEdge(1, 1));
}

TEST(GraphData, DefaultLabel)
{
    EXPECT_EQ(GraphData::defaultLabel("Folder/Note.md"), "Note");
    EXPECT_EQ(GraphData::defaultLabel("Note.MD"), "Note");
    EXPECT_EQ(GraphData::defaultLabel("a/b/photo.png"), "photo.png");
    EXPECT_EQ(GraphData::defaultLabel("#tag"), "#tag");
    EXPECT_EQ(GraphData::defaultLabel("#parent/child"), "child"); // Obsidian shows the last segment
    EXPECT_EQ(GraphData::defaultLabel(".md"), ".md");
    EXPECT_EQ(GraphData::defaultLabel("Some link"), "Some link");
}

TEST(GraphFilter, DefaultsHideTagsAndAttachments)
{
    const GraphData g = sample();
    GraphFilter f; // Obsidian defaults: no tags, no attachments, unresolved + orphans shown
    const GraphData out = f.apply(g);
    EXPECT_EQ(sortedIds(out), QStringList({"A.md", "B.md", "D.md", "E.md", "Missing", "sub/C.md"}));
    EXPECT_EQ(edgeList(out), QStringList({"A.md->B.md", "A.md->sub/C.md", "B.md->Missing", "B.md->sub/C.md", "sub/C.md->A.md"}));
    EXPECT_EQ(out.node(out.indexOf("A.md")).weight, 3);
    EXPECT_EQ(out.node(out.indexOf("D.md")).weight, 0);
    // Unresolved targets are inserted right after the file that links to them (Obsidian's order).
    EXPECT_EQ(ids(out), QStringList({"A.md", "B.md", "Missing", "sub/C.md", "D.md", "E.md"}));
}

TEST(GraphFilter, TogglesAndOrphans)
{
    const GraphData g = sample();
    GraphFilter f;
    f.showTags = true;
    f.showAttachments = true;
    f.hideUnresolved = true;
    f.showOrphans = false;
    const GraphData out = f.apply(g);
    EXPECT_EQ(sortedIds(out), QStringList({"#tag", "A.md", "B.md", "E.md", "img.png", "sub/C.md"}));
    EXPECT_TRUE(out.node(out.indexOf("#tag")).kind == NodeKind::Tag);
    EXPECT_EQ(out.node(out.indexOf("#tag")).weight, 2);
    EXPECT_EQ(out.node(out.indexOf("img.png")).weight, 1);

    // A node whose only link is a self link is still an orphan.
    GraphData s;
    s.addNode("self.md");
    s.addNode("x.md");
    s.addEdge(0, 0);
    s.finalize();
    GraphFilter noOrphans;
    noOrphans.showOrphans = false;
    EXPECT_EQ(noOrphans.apply(s).nodeCount(), 0);
}

TEST(GraphFilter, SearchAndColorGroups)
{
    const GraphData g = sample();
    GraphFilter f;
    f.showTags = true;
    f.search = [](const GraphData& gr, int i) { return gr.node(i).id != "B.md" && gr.node(i).kind != NodeKind::Tag; };
    f.colorGroups.push_back({[](const GraphData& gr, int i) { return gr.node(i).id.startsWith("sub/"); }, GraphColor{0xff0000, 1}});
    f.colorGroups.push_back({[](const GraphData&, int) { return true; }, GraphColor{0x00ff00, 1}});
    GraphData out = f.apply(g);
    // B.md filtered out, so are its unresolved link and the tag (search rejects tags here).
    EXPECT_EQ(sortedIds(out), QStringList({"A.md", "D.md", "E.md", "sub/C.md"}));
    ASSERT_TRUE(out.node(out.indexOf("sub/C.md")).color.has_value());
    EXPECT_EQ(out.node(out.indexOf("sub/C.md")).color->rgb, 0xff0000u); // first match wins
    EXPECT_EQ(out.node(out.indexOf("A.md")).color->rgb, 0x00ff00u);

    // Colour groups colour files only.
    f.search = {};
    f.showAttachments = true;
    out = f.apply(g);
    EXPECT_FALSE(out.node(out.indexOf("#tag")).color.has_value());
    EXPECT_FALSE(out.node(out.indexOf("img.png")).color.has_value());
    EXPECT_FALSE(out.node(out.indexOf("Missing")).color.has_value());

    // The local graph's file always passes the search and gets no group colour.
    f.search = [](const GraphData&, int) { return false; };
    f.alwaysInclude = "A.md";
    out = f.apply(g);
    EXPECT_EQ(sortedIds(out), QStringList({"A.md"}));
    EXPECT_FALSE(out.node(0).color.has_value());
}

TEST(GraphFilter, ProgressionFollowsCreationOrder)
{
    GraphData g;
    g.addNode("late.md", NodeKind::File, {}, 3000);
    g.addNode("early.md", NodeKind::File, {}, 1000);
    g.addNode("mid.md", NodeKind::File, {}, 2000);
    g.addEdge("early.md", "mid.md");
    g.addEdge("early.md", "late.md");
    g.addEdge("mid.md", "late.md");
    g.finalize();
    GraphFilter f;
    int total = 0;
    f.applyProgression(g, -1, &total);
    EXPECT_EQ(total, 6); // 3 files (orphan steps) + 3 links

    // Step 1: only the earliest file.
    GraphData p = f.applyProgression(g, 1, nullptr);
    EXPECT_EQ(ids(p), QStringList({"early.md"}));
    EXPECT_EQ(p.edgeCount(), 0);
    // Step 2 adds early's first link, but its target is created later than the cut file.
    p = f.applyProgression(g, 2, nullptr);
    EXPECT_EQ(ids(p), QStringList({"early.md"}));
    p = f.applyProgression(g, 4, nullptr);
    EXPECT_EQ(ids(p), QStringList({"early.md", "mid.md"}));
    EXPECT_EQ(edgeList(p), QStringList({"early.md->mid.md"}));
    p = f.applyProgression(g, 6, nullptr);
    EXPECT_EQ(p.nodeCount(), 3);
    EXPECT_EQ(p.edgeCount(), 3);
}

TEST(LocalGraph, DepthAndDirections)
{
    // chain: a -> b -> c -> d ; e -> a ; b -> e (so e is also reachable forward at depth 2)
    GraphData g;
    for (const char* id : {"a.md", "b.md", "c.md", "d.md", "e.md"})
        g.addNode(id);
    g.addEdge("a.md", "b.md");
    g.addEdge("b.md", "c.md");
    g.addEdge("c.md", "d.md");
    g.addEdge("e.md", "a.md");
    g.addEdge("b.md", "e.md");
    g.finalize();

    LocalGraphOptions o; // depth 1, both directions, no neighbour links
    GraphData l = extractLocalGraph(g, "a.md", o);
    EXPECT_EQ(ids(l), QStringList({"a.md", "b.md", "e.md"}));
    EXPECT_EQ(edgeList(l), QStringList({"a.md->b.md", "e.md->a.md"}));
    EXPECT_EQ(l.node(0).weight, 30);
    EXPECT_EQ(l.node(1).weight, 0);

    o.neighborLinks = true; // b -> e is a link between neighbours
    l = extractLocalGraph(g, "a.md", o);
    EXPECT_EQ(edgeList(l), QStringList({"a.md->b.md", "b.md->e.md", "e.md->a.md"}));

    o = {};
    o.incoming = false;
    o.depth = 3;
    l = extractLocalGraph(g, "a.md", o);
    EXPECT_EQ(ids(l), QStringList({"a.md", "b.md", "c.md", "e.md", "d.md"}));
    EXPECT_EQ(l.node(l.indexOf("b.md")).weight, 20);
    EXPECT_EQ(l.node(l.indexOf("c.md")).weight, 10);
    EXPECT_EQ(l.node(l.indexOf("d.md")).weight, 0);
    EXPECT_EQ(edgeList(l), QStringList({"a.md->b.md", "b.md->c.md", "b.md->e.md", "c.md->d.md"}));

    o = {};
    o.outgoing = false;
    l = extractLocalGraph(g, "a.md", o);
    EXPECT_EQ(ids(l), QStringList({"a.md", "e.md"}));

    // Unknown centre: a lone node.
    l = extractLocalGraph(g, "zzz.md", o);
    EXPECT_EQ(ids(l), QStringList({"zzz.md"}));
    EXPECT_EQ(l.node(0).weight, 30);
}

TEST(LocalGraph, TagsAreDeadEnds)
{
    GraphData g;
    g.addNode("a.md");
    g.addNode("b.md");
    g.addNode("#t", NodeKind::Tag);
    g.addEdge("a.md", "#t");
    g.addEdge("b.md", "#t");
    g.finalize();
    LocalGraphOptions o;
    o.depth = 3;
    const GraphData l = extractLocalGraph(g, "a.md", o);
    EXPECT_EQ(ids(l), QStringList({"a.md", "#t"})); // b.md is not reached through the tag
}

TEST(GraphSettings, JsonRoundTripExactKeys)
{
    GraphSettings s = GraphSettings::userDefaults();
    s.search = "path:Projects";
    s.colorGroups.append(ColorGroupSetting{"tag:#idea", GraphColor{14701138, 1}});
    s.showArrow = true;
    s.textFadeMultiplier = -1.5;
    s.close = true;
    const QJsonObject j = s.toJson();
    QStringList keys = j.keys();
    keys.sort();
    QStringList expected = {"collapse-filter", "search", "showTags", "showAttachments", "hideUnresolved", "showOrphans",
                            "collapse-color-groups", "colorGroups", "collapse-display", "showArrow", "textFadeMultiplier",
                            "nodeSizeMultiplier", "lineSizeMultiplier", "collapse-forces", "centerStrength", "repelStrength",
                            "linkStrength", "linkDistance", "scale", "close"};
    expected.sort();
    EXPECT_EQ(keys, expected);
    const QJsonObject group = j.value("colorGroups").toArray().at(0).toObject();
    EXPECT_EQ(group.value("query").toString(), "tag:#idea");
    EXPECT_EQ(group.value("color").toObject().value("rgb").toInteger(), 14701138);
    EXPECT_EQ(group.value("color").toObject().value("a").toDouble(), 1.0);
    EXPECT_EQ(GraphSettings::fromJson(j), s);

    // Local graph view state: no showOrphans, plus the four local keys.
    const QJsonObject lj = s.toJson(GraphSettings::Variant::Local);
    EXPECT_FALSE(lj.contains("showOrphans"));
    for (const char* k : {"localJumps", "localBacklinks", "localForelinks", "localInterlinks"})
        EXPECT_TRUE(lj.contains(QLatin1String(k))) << k;
    EXPECT_EQ(lj.size(), j.size() - 1 + 4);
}

TEST(GraphSettings, ParsesObsidianGraphJson)
{
    const QByteArray json = R"({
  "collapse-filter": true, "search": "", "showTags": false, "showAttachments": false,
  "hideUnresolved": false, "showOrphans": true, "collapse-color-groups": true, "colorGroups": [],
  "collapse-display": true, "showArrow": false, "textFadeMultiplier": 0, "nodeSizeMultiplier": 1,
  "lineSizeMultiplier": 1, "collapse-forces": true, "centerStrength": 0.518713248970312,
  "repelStrength": 10, "linkStrength": 1, "linkDistance": 250, "scale": 0.5443310539518174, "close": false
})";
    const GraphSettings s = GraphSettings::fromJson(QJsonDocument::fromJson(json).object());
    EXPECT_EQ(s, GraphSettings::userDefaults());
    EXPECT_NEAR(s.forces().d3CenterStrength(), 0.1, 1e-12);
    EXPECT_EQ(s.forces().d3ChargeStrength(), -1000);
    EXPECT_NEAR(s.forces().d3LinkStrengthScale(), 1.0, 1e-12);
    // Missing keys fall back to defaults; wrong types are ignored.
    const GraphSettings d = GraphSettings::fromJson(QJsonDocument::fromJson(R"({"scale": "big", "showTags": 1})").object());
    EXPECT_EQ(d, GraphSettings());
}

TEST(GraphSettings, RestoreDefaultsKeepsSearchAndView)
{
    GraphSettings s;
    s.search = "foo";
    s.showTags = true;
    s.localJumps = 4;
    s.colorGroups.append(ColorGroupSetting{"x", {}});
    s.repelStrength = 3;
    s.scale = 2;
    s.collapseForces = false;
    s.restoreDefaults();
    EXPECT_EQ(s.search, "foo");
    EXPECT_FALSE(s.showTags);
    EXPECT_EQ(s.localJumps, 1);
    EXPECT_TRUE(s.colorGroups.isEmpty());
    EXPECT_EQ(s.repelStrength, 10);
    EXPECT_EQ(s.scale, 2);
    EXPECT_FALSE(s.collapseForces);
}

TEST(SimpleQuery, Operators)
{
    const GraphData g = sample();
    auto matches = [&](const QString& q) {
        const NodePredicate p = compileSimpleQuery(q);
        QStringList out;
        for (int i = 0; i < g.nodeCount(); ++i)
            if (p(g, i))
                out << g.node(i).id;
        out.sort();
        return out;
    };
    EXPECT_FALSE(compileSimpleQuery("   "));
    EXPECT_EQ(matches("path:sub/"), QStringList({"sub/C.md"}));
    EXPECT_EQ(matches("file:c.md"), QStringList({"sub/C.md"}));
    EXPECT_EQ(matches("tag:#tag"), QStringList({"#tag", "A.md", "B.md"}));
    EXPECT_EQ(matches("tag:tag -b"), QStringList({"#tag", "A.md"}));
    EXPECT_EQ(matches("a.md OR d.md"), QStringList({"A.md", "D.md"}));
    EXPECT_EQ(matches("\"sub/c\""), QStringList({"sub/C.md"}));
}
