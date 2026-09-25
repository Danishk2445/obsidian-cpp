// Canvas edit operations and Obsidian's snapshot undo history.
#include "core/canvas/canvas_document.h"

#include <gtest/gtest.h>

using namespace oc::canvas;

namespace {
Node textNode(const QString& id, QRectF r, const QString& text = {})
{
    Node n;
    n.id = id;
    n.type = NodeType::Text;
    n.text = text;
    n.setRect(r);
    return n;
}

Node groupNode(const QString& id, QRectF r)
{
    Node n;
    n.id = id;
    n.type = NodeType::Group;
    n.setRect(r);
    return n;
}

Edge edge(const QString& id, const QString& from, const QString& to)
{
    Edge e;
    e.id = id;
    e.fromNode = from;
    e.toNode = to;
    return e;
}

QStringList ids(const CanvasData& d)
{
    QStringList out;
    for (const Node& n : d.nodes)
        out << n.id;
    return out;
}

CanvasDocument threeCards()
{
    CanvasData d;
    d.nodes = {textNode("a", {0, 0, 100, 50}), textNode("b", {200, 0, 100, 50}), textNode("c", {0, 200, 100, 50})};
    d.edges = {edge("ab", "a", "b"), edge("bc", "b", "c")};
    return CanvasDocument(d);
}
} // namespace

TEST(CanvasDocument, CommitUndoRedo)
{
    CanvasDocument doc = threeCards();
    EXPECT_FALSE(doc.canUndo());
    EXPECT_FALSE(doc.commit()); // nothing changed
    doc.setNodeRect(u"a", QRectF(10, 10, 100, 50));
    EXPECT_TRUE(doc.hasUncommittedChanges());
    EXPECT_TRUE(doc.commit());
    doc.setText(u"b", "hello");
    EXPECT_TRUE(doc.commit());
    EXPECT_EQ(doc.stepCount(), 3);
    EXPECT_TRUE(doc.undo());
    EXPECT_EQ(doc.data().node(u"b")->text, "");
    EXPECT_EQ(doc.data().node(u"a")->x, 10);
    EXPECT_TRUE(doc.undo());
    EXPECT_EQ(doc.data().node(u"a")->x, 0);
    EXPECT_FALSE(doc.undo());
    EXPECT_TRUE(doc.redo());
    EXPECT_TRUE(doc.redo());
    EXPECT_EQ(doc.data().node(u"b")->text, "hello");
    EXPECT_FALSE(doc.redo());
    // A new edit drops the redo steps.
    doc.undo();
    doc.setColor({"c"}, "3");
    doc.commit();
    EXPECT_FALSE(doc.canRedo());
    EXPECT_EQ(doc.data().node(u"b")->text, "");
    EXPECT_EQ(doc.data().node(u"c")->color, "3");
}

TEST(CanvasDocument, UndoRecordsAPendingEditFirst)
{
    CanvasDocument doc = threeCards();
    doc.setText(u"a", "typed");
    EXPECT_TRUE(doc.undo()); // commits "typed", then goes back before it
    EXPECT_EQ(doc.data().node(u"a")->text, "");
    EXPECT_TRUE(doc.redo());
    EXPECT_EQ(doc.data().node(u"a")->text, "typed");
}

TEST(CanvasDocument, HistoryIsCapped)
{
    CanvasDocument doc = threeCards();
    for (int i = 1; i <= 150; ++i) {
        doc.setNodeRect(u"a", QRectF(i, 0, 100, 50));
        doc.commit();
    }
    EXPECT_EQ(doc.stepCount(), CanvasDocument::kMaxSteps - 1); // Obsidian's history keeps 99
    int undos = 0;
    while (doc.undo())
        ++undos;
    EXPECT_EQ(undos, CanvasDocument::kMaxSteps - 2);
    EXPECT_EQ(doc.data().node(u"a")->x, 150 - undos);
}

TEST(CanvasDocument, RemoveNodesTakesTheirEdges)
{
    CanvasDocument doc = threeCards();
    doc.removeItems({"b"});
    EXPECT_EQ(ids(doc.data()), QStringList({"a", "c"}));
    EXPECT_TRUE(doc.data().edges.isEmpty());
    doc.undo();
    doc.removeItems({"ab"});
    EXPECT_EQ(doc.data().edges.size(), 1);
    EXPECT_EQ(doc.data().edges[0].id, "bc");
}

TEST(CanvasDocument, AddNodeZOrder)
{
    CanvasDocument doc = threeCards();
    const QString id = doc.addNode(textNode({}, {1.4, 2.6, 250, 60}));
    EXPECT_EQ(id.size(), 16);
    EXPECT_EQ(ids(doc.data()).last(), id);
    EXPECT_EQ(doc.data().node(id)->rect(), QRectF(1, 3, 250, 60));
    // Groups go below the cards (Obsidian writes them first, larger first).
    doc.addNode(groupNode("small", {0, 0, 100, 100}));
    doc.addNode(groupNode("big", {0, 0, 1000, 1000}));
    EXPECT_EQ(ids(doc.data()).mid(0, 3), QStringList({"big", "small", "a"}));
    // Duplicate ids are replaced.
    const QString dup = doc.addNode(textNode("a", {0, 0, 10, 10}));
    EXPECT_NE(dup, "a");
}

TEST(CanvasDocument, RaiseMovesCardsToTheTop)
{
    CanvasDocument doc = threeCards();
    doc.addNode(groupNode("g", {0, 0, 500, 500}));
    doc.raise({"a", "g"});
    EXPECT_EQ(ids(doc.data()), QStringList({"g", "b", "c", "a"}));
}

TEST(CanvasDocument, ClipboardCloneRemapsIds)
{
    CanvasDocument doc = threeCards();
    doc.node(u"a")->color = "2";
    const Json clip = doc.selectionData({"a", "b"});
    // Only the edge between two selected cards is copied; the center is the selection's.
    EXPECT_EQ(clip.value(u"nodes").size(), 2);
    EXPECT_EQ(clip.value(u"edges").size(), 1);
    EXPECT_EQ(clip.value(u"center").value(u"x").toDouble(), 150);
    EXPECT_EQ(clip.value(u"center").value(u"y").toDouble(), 25);
    const QByteArray text = stringifyCompact(clip);
    const std::optional<Json> back = parseJson(text);
    ASSERT_TRUE(back);
    const QStringList added = doc.importClone(*back, QPointF(1000, 500.5));
    ASSERT_EQ(added.size(), 2);
    const Node* a2 = doc.data().node(added[0]);
    ASSERT_TRUE(a2);
    EXPECT_NE(a2->id, "a");
    EXPECT_EQ(a2->rect(), QRectF(1000, 501, 100, 50));
    EXPECT_EQ(a2->color, "2");
    ASSERT_EQ(doc.data().edges.size(), 3);
    const Edge& e = doc.data().edges.last();
    EXPECT_EQ(e.fromNode, added[0]);
    EXPECT_EQ(e.toNode, added[1]);
    EXPECT_TRUE(e.fromSideSet);
    EXPECT_EQ(e.fromSide, Side::Right);
    // Unknown members survive a copy (Obsidian's unknownData).
    const std::optional<CanvasData> withExtra =
        parseCanvas(QByteArray(R"({"nodes":[{"id":"q","type":"text","text":"t","x":0,"y":0,"width":10,"height":10,"meta":"kept"}]})"));
    CanvasDocument doc2(*withExtra);
    const QStringList pasted = doc2.importClone(doc2.selectionData({"q"}), QPointF(5, 5));
    ASSERT_EQ(pasted.size(), 1);
    const QByteArray line = stringifyCompact(nodeToJson(*doc2.data().node(pasted[0])));
    EXPECT_EQ(line, "{\"id\":\"" + pasted[0].toUtf8() + "\",\"type\":\"text\",\"text\":\"t\",\"x\":5,\"y\":5,\"width\":10,\"height\":10,\"meta\":\"kept\"}");
}

TEST(CanvasDocument, AlignDistributeJustifyArrange)
{
    CanvasData d;
    d.nodes = {textNode("a", {0, 0, 100, 50}), textNode("b", {300, 40, 50, 50}), textNode("c", {130, 100, 60, 20})};
    CanvasDocument doc(d);
    const QStringList all{"a", "b", "c"};
    const QRectF box = itemsBounds(doc.data(), {"a", "b", "c"});
    EXPECT_EQ(box, QRectF(0, 0, 350, 120));
    doc.align(all, Align::Right, box);
    EXPECT_EQ(doc.data().node(u"a")->x, 250);
    EXPECT_EQ(doc.data().node(u"c")->x, 290);
    doc.align(all, Align::Middle, box);
    EXPECT_EQ(doc.data().node(u"a")->y, 35);
    EXPECT_EQ(doc.data().node(u"c")->y, 50);

    CanvasDocument dist(d);
    dist.distribute(all, Qt::Horizontal, box); // widths 210 in 350: gaps of 70
    EXPECT_EQ(dist.data().node(u"a")->x, 0);
    EXPECT_EQ(dist.data().node(u"c")->x, 170);
    EXPECT_EQ(dist.data().node(u"b")->x, 300);

    CanvasDocument just(d);
    just.justify(all, Qt::Vertical, box);
    EXPECT_EQ(just.data().node(u"b")->rect(), QRectF(300, 0, 50, 120));

    CanvasDocument arr(d);
    arr.arrange(all, Arrange::Row, box, 20);
    // Rows by y: a (0..50) and b (40, above a's bottom) share a row, c starts a new one.
    EXPECT_EQ(arr.data().node(u"a")->rect().topLeft(), QPointF(0, 0));
    EXPECT_EQ(arr.data().node(u"b")->rect().topLeft(), QPointF(120, 0));
    EXPECT_EQ(arr.data().node(u"c")->rect().topLeft(), QPointF(190, 0));
    CanvasDocument grid(d);
    grid.arrange(all, Arrange::Grid, box, 20);
    EXPECT_EQ(grid.data().node(u"b")->rect().topLeft(), QPointF(120, 0));
    EXPECT_EQ(grid.data().node(u"c")->rect().topLeft(), QPointF(0, 70));
}

TEST(CanvasDocument, NudgeAlignsToTheGridFirst)
{
    CanvasData d;
    d.nodes = {textNode("a", {5, 0, 100, 50})};
    CanvasDocument doc(d);
    doc.nudge({"a"}, Side::Right, false, 20, true); // 5 is off the grid: to the next line, 20
    EXPECT_EQ(doc.data().node(u"a")->x, 20);
    doc.nudge({"a"}, Side::Right, false, 20, true);
    EXPECT_EQ(doc.data().node(u"a")->x, 40);
    doc.nudge({"a"}, Side::Left, true, 20, true); // Shift: 5 steps
    EXPECT_EQ(doc.data().node(u"a")->x, -60);
    doc.nudge({"a"}, Side::Bottom, false, 20, false);
    EXPECT_EQ(doc.data().node(u"a")->y, 20);
}

TEST(CanvasDocument, ReconnectAndEnds)
{
    CanvasDocument doc = threeCards();
    doc.reconnectEdge(u"ab", false, "c", Side::Top);
    const Edge* e = doc.data().edge(u"ab");
    EXPECT_EQ(e->toNode, "c");
    EXPECT_EQ(e->toSide, Side::Top);
    doc.setEdgeEnds({"ab", "bc"}, EdgeEnd::Arrow, EdgeEnd::Arrow);
    EXPECT_EQ(doc.data().edge(u"bc")->fromEnd, EdgeEnd::Arrow);
    doc.setEdgeLabel(u"bc", "label");
    EXPECT_EQ(doc.data().edge(u"bc")->label, "label");
    doc.retargetEdges("c", "a");
    EXPECT_EQ(doc.data().edge(u"bc")->toNode, "a");
}
