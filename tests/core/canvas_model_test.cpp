// Canvas file model: reading JSON Canvas files, byte-stable writing in Obsidian's layout (golden
// round trips, unknown keys and items, key order, defaults) and edge side resolution.
#include "core/canvas/canvas_model.h"

#include <QFile>

#include <gtest/gtest.h>

using namespace oc::canvas;

namespace {
QByteArray fixture(const char* name)
{
    QFile f(QStringLiteral(OC_FIXTURES_DIR "/canvas/") + QLatin1String(name));
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << name;
    return f.readAll();
}

CanvasData parsed(const QByteArray& bytes)
{
    QString err;
    std::optional<CanvasData> d = parseCanvas(bytes, &err);
    EXPECT_TRUE(d.has_value()) << err.toStdString();
    return d ? *d : CanvasData{};
}

// The line of the output that contains `needle`.
QByteArray lineWith(const QByteArray& text, const QByteArray& needle)
{
    for (const QByteArray& l : text.split('\n'))
        if (l.contains(needle))
            return l;
    return {};
}
} // namespace

TEST(CanvasModel, ReadsEveryNodeType)
{
    const CanvasData d = parsed(fixture("obsidian-layout.canvas"));
    ASSERT_EQ(d.nodes.size(), 7);
    const Node* g = d.node(u"g1");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->type, NodeType::Group);
    EXPECT_EQ(g->label, "Ideas");
    EXPECT_EQ(g->color, "4");
    EXPECT_EQ(g->rect(), QRectF(-420, -300, 900, 620));
    const Node* t = d.node(u"t1");
    ASSERT_TRUE(t);
    EXPECT_EQ(t->type, NodeType::Text);
    EXPECT_TRUE(t->text.startsWith("# Hello\n\nSome **bold**"));
    const Node* f2 = d.node(u"f2");
    ASSERT_TRUE(f2);
    EXPECT_EQ(f2->file, "Note B.md");
    EXPECT_EQ(f2->subpath, "#Section");
    const Node* l = d.node(u"l1");
    ASSERT_TRUE(l);
    EXPECT_EQ(l->type, NodeType::Link);
    EXPECT_EQ(l->url, "https://obsidian.md");
    // An unknown type is kept but not shown.
    EXPECT_EQ(d.nodes[5].type, NodeType::Unknown);
    EXPECT_FALSE(d.node(u"x1"));
    ASSERT_EQ(d.edges.size(), 4);
    const Edge* e2 = d.edge(u"e2");
    ASSERT_TRUE(e2);
    EXPECT_EQ(e2->fromSide, Side::Bottom);
    EXPECT_EQ(e2->toSide, Side::Top);
    EXPECT_EQ(e2->fromEnd, EdgeEnd::Arrow);
    EXPECT_EQ(e2->toEnd, EdgeEnd::Arrow);
    EXPECT_EQ(e2->color, "1");
    EXPECT_EQ(e2->label, "relates to");
    // Defaults and computed sides.
    const Edge* e3 = d.edge(u"e3");
    ASSERT_TRUE(e3);
    EXPECT_EQ(e3->fromEnd, EdgeEnd::None);
    EXPECT_EQ(e3->toEnd, EdgeEnd::None);
    EXPECT_FALSE(e3->fromSideSet);
    EXPECT_EQ(e3->fromSide, Side::Right); // f1 (20..420) -> l1 (520..920) on the right
    EXPECT_EQ(e3->toSide, Side::Left);
    // A dangling edge is kept but not shown.
    EXPECT_FALSE(d.isEdgeShown(*d.edge(u"e4")));
    EXPECT_TRUE(d.isEdgeShown(*d.edge(u"e1")));
}

TEST(CanvasModel, ObsidianFileRoundTripsByteExactly)
{
    const QByteArray bytes = fixture("obsidian-layout.canvas");
    const CanvasData d = parsed(bytes);
    EXPECT_EQ(serializeCanvas(d, FileFormat::detect(bytes)), bytes);
}

TEST(CanvasModel, EditTouchesOnlyTheEditedNode)
{
    const QByteArray bytes = fixture("obsidian-layout.canvas");
    CanvasData d = parsed(bytes);
    d.node(u"t2")->setRect(QRectF(640, 220.4, 250, 80));
    const QByteArray out = serializeCanvas(d, FileFormat::detect(bytes));
    const QList<QByteArray> a = bytes.split('\n'), b = out.split('\n');
    ASSERT_EQ(a.size(), b.size());
    int changed = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) {
            ++changed;
            // Key order and unknown members stay; only the values change.
            EXPECT_EQ(b[i], "\t\t{\"id\":\"t2\",\"x\":640,\"y\":220,\"width\":250,\"height\":80,\"type\":\"text\",\"text\":\"Created in a session: "
                            "order id,x,y,w,h,type,text\",\"cardMeta\":\"kept\"}");
        }
    EXPECT_EQ(changed, 1);
}

TEST(CanvasModel, MembersAddedAndRemoved)
{
    CanvasData d = parsed(fixture("obsidian-layout.canvas"));
    d.node(u"f1")->color.clear();      // removed
    d.node(u"t1")->color = "#ff0000";  // appended at the end
    d.node(u"f2")->subpath.clear();    // removed
    d.node(u"g1")->label.clear();      // removed
    d.edge(u"e2")->label.clear();      // removed
    d.edge(u"e2")->fromEnd = EdgeEnd::None; // default: removed
    d.edge(u"e2")->toEnd = EdgeEnd::Arrow;  // unchanged: explicit "arrow" kept
    d.edge(u"e3")->toEnd = EdgeEnd::Arrow;  // back to the default: "toEnd":"none" removed
    const QByteArray out = serializeCanvas(d);
    EXPECT_EQ(lineWith(out, "\"f1\""), "\t\t{\"id\":\"f1\",\"type\":\"file\",\"file\":\"Note A.md\",\"x\":20,\"y\":-240,\"width\":400,\"height\":400},");
    EXPECT_TRUE(lineWith(out, "\"t1\"").endsWith(",\"height\":180,\"color\":\"#ff0000\"},"));
    EXPECT_EQ(lineWith(out, "\"f2\""), "\t\t{\"id\":\"f2\",\"type\":\"file\",\"file\":\"Note B.md\",\"x\":-380,\"y\":0,\"width\":300,\"height\":200},");
    EXPECT_EQ(lineWith(out, "\"g1\""), "\t\t{\"id\":\"g1\",\"type\":\"group\",\"x\":-420,\"y\":-300,\"width\":900,\"height\":620,\"color\":\"4\"},");
    EXPECT_EQ(lineWith(out, "\"e2\""), "\t\t{\"id\":\"e2\",\"fromNode\":\"f1\",\"fromSide\":\"bottom\",\"toNode\":\"f2\",\"toSide\":\"top\",\"toEnd\":\"arrow\",\"color\":\"1\"},");
    // e3's sides were missing: still not written (they were computed for display only).
    EXPECT_EQ(lineWith(out, "\"e3\""), "\t\t{\"id\":\"e3\",\"fromNode\":\"f1\",\"toNode\":\"l1\"},");
}

TEST(CanvasModel, ReconnectedEdgeWritesItsSide)
{
    CanvasData d = parsed(fixture("obsidian-layout.canvas"));
    Edge* e = d.edge(u"e3");
    e->toNode = "t1";
    e->toSide = Side::Bottom;
    e->toSideSet = true;
    EXPECT_EQ(lineWith(serializeCanvas(d), "\"e3\""), "\t\t{\"id\":\"e3\",\"fromNode\":\"f1\",\"toNode\":\"t1\",\"toEnd\":\"none\",\"toSide\":\"bottom\"},");
}

TEST(CanvasModel, UnknownItemsAndTopLevelKeysAreKept)
{
    const QByteArray src = "{\"version\":2,\"nodes\":[\"junk\",{\"id\":\"a\",\"type\":\"text\",\"text\":\"x\",\"x\":0,\"y\":0,\"width\":10,\"height\":10},"
                           "{\"id\":\"a\",\"type\":\"text\",\"text\":\"dup\",\"x\":5,\"y\":5,\"width\":10,\"height\":10}],"
                           "\"edges\":[7,{\"id\":\"e\",\"fromNode\":\"a\",\"toNode\":\"a\",\"extra\":{\"deep\":[null]}}],\"tail\":true}";
    CanvasData d = parsed(src);
    ASSERT_EQ(d.nodes.size(), 3);
    EXPECT_FALSE(d.nodes[0].isShown());
    EXPECT_TRUE(d.nodes[1].isShown());
    EXPECT_FALSE(d.nodes[2].isShown()); // duplicate id
    EXPECT_EQ(d.node(u"a")->text, "x");
    EXPECT_TRUE(d.edges[0].opaque);
    d.node(u"a")->text = "changed";
    const QByteArray out = serializeCanvas(d);
    EXPECT_EQ(out, "{\n\t\"version\":2,\n\t\"nodes\":[\n\t\t\"junk\",\n\t\t{\"id\":\"a\",\"type\":\"text\",\"text\":\"changed\",\"x\":0,\"y\":0,\"width\":10,\"height\":10},\n"
                   "\t\t{\"id\":\"a\",\"type\":\"text\",\"text\":\"dup\",\"x\":5,\"y\":5,\"width\":10,\"height\":10}\n\t],\n"
                   "\t\"edges\":[\n\t\t7,\n\t\t{\n\t\t\t\"id\":\"e\",\n\t\t\t\"fromNode\":\"a\",\n\t\t\t\"toNode\":\"a\",\n\t\t\t\"extra\":{\n"
                   "\t\t\t\t\"deep\":[\n\t\t\t\t\tnull\n\t\t\t\t]\n\t\t\t}\n\t\t}\n\t],\n\t\"tail\":true\n}");
}

TEST(CanvasModel, PrettyFileIsRewrittenInObsidianLayoutKeepingValues)
{
    const QByteArray bytes = fixture("pretty.canvas");
    CanvasData d = parsed(bytes);
    const Node* a = d.node(u"a");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->text, QString::fromUtf8("café / slash"));
    EXPECT_EQ(a->x, 11); // Math.round, like Obsidian
    EXPECT_FALSE(d.edges[0].fromSideSet);
    const QByteArray out = serializeCanvas(d, FileFormat::detect(bytes));
    // Unchanged values keep their source text (10.6, the escapes); the final newline is kept.
    EXPECT_EQ(out, "{\n\t\"nodes\":[\n\t\t{\"id\":\"a\",\"type\":\"text\",\"text\":\"caf\\u00e9 \\/ slash\",\"x\":10.6,\"y\":-20,\"width\":250,\"height\":60},\n"
                   "\t\t{\"id\":\"b\",\"type\":\"file\",\"file\":\"Pic.png\",\"x\":300,\"y\":0,\"width\":400,\"height\":300}\n\t],\n"
                   "\t\"edges\":[\n\t\t{\"id\":\"e\",\"fromNode\":\"a\",\"toNode\":\"b\"}\n\t]\n}\n");
    d.node(u"a")->setRect(QRectF(20, -20, 250, 60));
    EXPECT_TRUE(lineWith(serializeCanvas(d), "\"a\"").contains("\"x\":20,"));
}

TEST(CanvasModel, NewNodesUseObsidiansMemberOrder)
{
    CanvasData d;
    Node t;
    t.id = "t";
    t.type = NodeType::Text;
    t.text = "hi";
    t.setRect(QRectF(-125, -30, 250, 60));
    Node f;
    f.id = "f";
    f.type = NodeType::File;
    f.file = "a/b.md";
    f.subpath = "#^blk";
    f.color = "3";
    f.setRect(QRectF(0.4, 0.5, 400, 400));
    Node g;
    g.id = "g";
    g.type = NodeType::Group;
    g.label = "G";
    g.background = "bg.png";
    g.backgroundStyle = "repeat";
    g.setRect(QRectF(-2.5, 0, 10, 10));
    Node l;
    l.id = "l";
    l.type = NodeType::Link;
    l.url = "https://example.com";
    d.nodes = {g, t, f, l};
    Edge e;
    e.id = "e";
    e.fromNode = "t";
    e.toNode = "f";
    e.fromSide = Side::Right;
    e.toSide = Side::Left;
    e.fromEnd = EdgeEnd::Arrow;
    e.label = "x";
    d.edges = {e};
    EXPECT_EQ(serializeCanvas(d),
              "{\n\t\"nodes\":[\n"
              "\t\t{\"id\":\"g\",\"x\":-2,\"y\":0,\"width\":10,\"height\":10,\"type\":\"group\",\"label\":\"G\",\"background\":\"bg.png\",\"backgroundStyle\":\"repeat\"},\n"
              "\t\t{\"id\":\"t\",\"x\":-125,\"y\":-30,\"width\":250,\"height\":60,\"type\":\"text\",\"text\":\"hi\"},\n"
              "\t\t{\"id\":\"f\",\"x\":0,\"y\":1,\"width\":400,\"height\":400,\"color\":\"3\",\"type\":\"file\",\"file\":\"a/b.md\",\"subpath\":\"#^blk\"},\n"
              "\t\t{\"id\":\"l\",\"x\":0,\"y\":0,\"width\":250,\"height\":60,\"type\":\"link\",\"url\":\"https://example.com\"}\n"
              "\t],\n\t\"edges\":[\n"
              "\t\t{\"id\":\"e\",\"fromNode\":\"t\",\"fromSide\":\"right\",\"toNode\":\"f\",\"toSide\":\"left\",\"fromEnd\":\"arrow\",\"label\":\"x\"}\n"
              "\t]\n}");
}

TEST(CanvasModel, EmptyAndInvalidFiles)
{
    const std::optional<CanvasData> empty = parseCanvas(QByteArray(" \n"));
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->isEmpty());
    EXPECT_EQ(serializeCanvas(*empty), "{\n\t\"nodes\":[],\n\t\"edges\":[]\n}");
    QString err;
    EXPECT_FALSE(parseCanvas(QByteArray("{\"nodes\":["), &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(parseCanvas(QByteArray("[1,2]"), &err).has_value());
    // No nodes/edges members: an empty canvas that keeps its other members.
    const std::optional<CanvasData> bare = parseCanvas(QByteArray("{\"a\":1}"));
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(serializeCanvas(*bare), "{\n\t\"a\":1,\n\t\"nodes\":[],\n\t\"edges\":[]\n}");
}

TEST(CanvasModel, FileFormatIsKept)
{
    const QByteArray src = "\xEF\xBB\xBF{\r\n\t\"nodes\":[\r\n\t\t{\"id\":\"a\",\"type\":\"text\",\"text\":\"x\",\"x\":0,\"y\":0,\"width\":10,\"height\":10}\r\n\t],\r\n"
                           "\t\"edges\":[]\r\n}\r\n";
    const FileFormat f = FileFormat::detect(src);
    EXPECT_TRUE(f.bom);
    EXPECT_TRUE(f.crlf);
    EXPECT_TRUE(f.finalNewline);
    EXPECT_EQ(serializeCanvas(parsed(src), f), src);
}

TEST(CanvasModel, IdsAndGeometryDefaults)
{
    CanvasData d = parsed("{\"nodes\":[{\"type\":\"text\",\"text\":\"no id\"},{\"id\":7,\"type\":\"group\",\"x\":\"12\",\"width\":\" 50 \"}],"
                          "\"edges\":[{\"fromNode\":7,\"toNode\":\"x\"}]}");
    ASSERT_EQ(d.nodes.size(), 2);
    EXPECT_EQ(d.nodes[0].id.size(), 16);           // a new id
    EXPECT_EQ(d.nodes[0].rect(), QRectF(0, 0, 100, 100)); // Obsidian's defaults for missing geometry
    EXPECT_EQ(d.nodes[1].id, "7");
    EXPECT_EQ(d.nodes[1].x, 12);                    // numeric strings are numbers (Math.round("12"))
    EXPECT_EQ(d.nodes[1].width, 50);
    EXPECT_EQ(d.edges[0].fromNode, "7");
    // Untouched: id-less node gets its id written in front, the numeric id stays a number.
    const QByteArray out = serializeCanvas(d);
    EXPECT_TRUE(lineWith(out, "no id").startsWith(QByteArray("\t\t{\"id\":\"" + d.nodes[0].id.toUtf8() + "\",\"type\":\"text\"")));
    EXPECT_EQ(lineWith(out, "group"), "\t\t{\"id\":7,\"type\":\"group\",\"x\":\"12\",\"width\":\" 50 \"}");
}

TEST(CanvasModel, AutoSide)
{
    const QRectF a(0, 0, 100, 100);
    EXPECT_EQ(autoSide(a, QRectF(300, 0, 100, 100)), Side::Right);
    EXPECT_EQ(autoSide(a, QRectF(-300, 0, 100, 100)), Side::Left);
    EXPECT_EQ(autoSide(a, QRectF(0, 300, 100, 100)), Side::Bottom);
    EXPECT_EQ(autoSide(a, QRectF(0, -300, 100, 100)), Side::Top);
    // Wide cards: the diagonal of the card decides.
    EXPECT_EQ(autoSide(QRectF(0, 0, 400, 50), QRectF(150, 200, 100, 100)), Side::Bottom);
    EXPECT_EQ(autoSide(QRectF(0, 0, 400, 50), QRectF(600, 100, 100, 100)), Side::Bottom); // atan2(125, 450) > atan2(50, 400)
    EXPECT_EQ(autoSide(QRectF(0, 0, 400, 50), QRectF(600, 0, 100, 50)), Side::Right);
}

TEST(CanvasModel, JsRound)
{
    EXPECT_EQ(jsRound(2.5), 3);
    EXPECT_EQ(jsRound(-2.5), -2);
    EXPECT_EQ(jsRound(-2.6), -3);
    EXPECT_EQ(jsRound(0.49999), 0);
}
