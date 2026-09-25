// Canvas geometry: viewport math, zoom-to-fit, edge routing, hit testing, snapping and resizing
// (values worked out from Obsidian's canvas code).
#include "core/canvas/canvas_geometry.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace oc::canvas;

namespace {
Node card(const QString& id, QRectF r, NodeType type = NodeType::Text)
{
    Node n;
    n.id = id;
    n.type = type;
    n.setRect(r);
    return n;
}

void expectNear(const QPointF& a, const QPointF& b, double eps = 1e-9)
{
    EXPECT_NEAR(a.x(), b.x(), eps);
    EXPECT_NEAR(a.y(), b.y(), eps);
}
} // namespace

TEST(CanvasGeometry, ViewportMapping)
{
    Viewport v{100, 50, 1, QSizeF(800, 600)};
    EXPECT_EQ(v.scale(), 2);
    EXPECT_NEAR(v.zoomMultiplier(), std::sqrt(0.5), 1e-12);
    expectNear(v.toView(QPointF(100, 50)), QPointF(400, 300));
    expectNear(v.toView(QPointF(110, 60)), QPointF(420, 320));
    expectNear(v.toCanvas(QPointF(0, 0)), QPointF(-100, -100));
    const QRectF vis = v.visibleRect();
    EXPECT_EQ(vis, QRectF(-100, -100, 400, 300));
    // Zooming around a point keeps the canvas point under it.
    const QPointF anchor(650, 120);
    const QPointF before = v.toCanvas(anchor);
    v.zoomAround(-1.5, anchor);
    EXPECT_EQ(v.zoom, -0.5);
    expectNear(v.toCanvas(anchor), before);
    // Limits: 1/16 .. 2.
    v.zoomAround(-10, std::nullopt);
    EXPECT_EQ(v.zoom, kMinZoom);
    v.zoomAround(20, std::nullopt);
    EXPECT_EQ(v.zoom, kMaxZoom);
}

TEST(CanvasGeometry, ZoomToFit)
{
    // Fits with a 10% margin.
    auto z = zoomToRect(QRectF(0, 0, 4000, 1000), QSizeF(1100, 550));
    ASSERT_TRUE(z);
    EXPECT_EQ(z->center, QPointF(2000, 500));
    EXPECT_DOUBLE_EQ(z->zoom, std::log2(1100.0 / 4400.0)); // -2
    // Never zooms in beyond 100%.
    z = zoomToRect(QRectF(10, 10, 50, 20), QSizeF(1100, 550));
    EXPECT_EQ(z->zoom, 0);
    // Clamped at 1/16.
    z = zoomToRect(QRectF(0, 0, 1e6, 10), QSizeF(1000, 800));
    EXPECT_EQ(z->zoom, kMinZoom);
    // Degenerate view.
    EXPECT_FALSE(zoomToRect(QRectF(0, 0, 10, 10), QSizeF(0, 100)));
    // A single point (zero-size box).
    z = zoomToRect(QRectF(5, 5, 0, 0), QSizeF(800, 600));
    EXPECT_EQ(z->zoom, 0);
}

TEST(CanvasGeometry, GridSpacingAndSnapDistance)
{
    EXPECT_EQ(gridSpacing(0), 20);
    EXPECT_EQ(gridSpacing(-0.9), 20);
    EXPECT_EQ(gridSpacing(-0.92), 40);
    EXPECT_EQ(gridSpacing(-2.2), 80);
    EXPECT_EQ(gridSpacing(-3.5), 160);
    EXPECT_EQ(snapDistance(1), 15);
    EXPECT_EQ(snapDistance(0.5), 30);
    EXPECT_EQ(snapDistance(2), 8);
}

TEST(CanvasGeometry, EdgeRouting)
{
    const QRectF a(0, 0, 100, 100), b(300, 0, 100, 100);
    const EdgeGeometry g = edgeGeometry(a, Side::Right, b, Side::Left);
    EXPECT_EQ(g.fromAnchor, QPointF(100, 50));
    EXPECT_EQ(g.toAnchor, QPointF(300, 50));
    EXPECT_EQ(g.from, QPointF(107, 50));
    EXPECT_EQ(g.to, QPointF(293, 50));
    // Control points: half the distance (186 / 2 = 93), clamped to 70..150, along the side normals.
    EXPECT_EQ(g.cp1, QPointF(200, 50));
    EXPECT_EQ(g.cp2, QPointF(200, 50));
    expectNear(g.center(), QPointF(200, 50));
    EXPECT_TRUE(g.toArrow);
    EXPECT_FALSE(g.fromArrow);
    // Close cards: at least 70; far: at most 150.
    const EdgeGeometry close = edgeGeometry(a, Side::Bottom, QRectF(0, 130, 100, 100), Side::Top);
    EXPECT_EQ(close.cp1, QPointF(50, 107 + 70));
    EXPECT_EQ(close.cp2, QPointF(50, 123 - 70));
    const EdgeGeometry far = edgeGeometry(a, Side::Top, QRectF(1000, 1000, 10, 10), Side::Left);
    EXPECT_EQ(far.cp1, QPointF(50, -7 - 150));
    EXPECT_EQ(far.cp2, QPointF(993 - 150, 1005));
    EXPECT_EQ(edgeBox(g), QRectF(100, 50, 200, 0));
}

TEST(CanvasGeometry, ArrowHeads)
{
    // The tip is at the anchor; the base (10.4 long) points away from the node.
    const QPolygonF bottom = arrowHead(QPointF(10, 20), Side::Bottom);
    expectNear(bottom[0], QPointF(10, 20));
    expectNear(bottom[1], QPointF(16.5, 30.4));
    expectNear(bottom[2], QPointF(3.5, 30.4));
    const QPolygonF top = arrowHead(QPointF(0, 0), Side::Top, 2);
    expectNear(top[1], QPointF(-13, -20.8));
    const QPolygonF left = arrowHead(QPointF(0, 0), Side::Left);
    expectNear((left[1] + left[2]) / 2, QPointF(-10.4, 0));
    const QPolygonF right = arrowHead(QPointF(0, 0), Side::Right);
    expectNear((right[1] + right[2]) / 2, QPointF(10.4, 0));
}

TEST(CanvasGeometry, EdgeDistance)
{
    const EdgeGeometry g = edgeGeometry(QRectF(0, 0, 100, 100), Side::Right, QRectF(300, 0, 100, 100), Side::Left);
    EXPECT_NEAR(distanceToEdge(g, QPointF(200, 50)), 0, 1e-6);
    EXPECT_NEAR(distanceToEdge(g, QPointF(200, 62)), 12, 1e-6);
    EXPECT_NEAR(distanceToEdge(g, QPointF(103, 50)), 0, 1e-6); // the stub to the side
}

TEST(CanvasGeometry, HitTestingOrder)
{
    CanvasData d;
    d.nodes = {card("t1", {0, 0, 100, 100}), card("g", {-50, -50, 400, 400}, NodeType::Group), card("t2", {50, 50, 100, 100})};
    // Groups are painted first, then cards in list order.
    EXPECT_EQ(paintOrder(d), QList<int>({1, 0, 2}));
    EXPECT_EQ(nodeAt(d, QPointF(75, 75)), 2); // t2 is above t1
    EXPECT_EQ(nodeAt(d, QPointF(10, 10)), 0);
    EXPECT_EQ(nodeAt(d, QPointF(300, 300)), 1);
    // Inside an unselected group only its border hits.
    EXPECT_EQ(nodeAt(d, QPointF(300, 300), 0, false), -1);
    EXPECT_EQ(nodeAt(d, QPointF(349, 300), 4, false), 1);
    // Tolerance.
    EXPECT_EQ(nodeAt(d, QPointF(155, 155), 8), 2);
    EXPECT_EQ(nodeAt(d, QPointF(900, 900), 8), -1);
}

TEST(CanvasGeometry, BoxSelection)
{
    CanvasData d;
    d.nodes = {card("a", {0, 0, 100, 100}), card("g", {0, 0, 300, 300}, NodeType::Group), card("b", {500, 0, 100, 100})};
    EXPECT_EQ(nodesInBox(d, QRectF(90, 90, 20, 20)), QList<int>({0})); // groups only when fully inside
    EXPECT_EQ(nodesInBox(d, QRectF(-10, -10, 320, 320)), QList<int>({0, 1}));
    EXPECT_EQ(nodesInBox(d, QRectF(100, 100, 5, 5)), QList<int>({0})); // touching counts
    EXPECT_EQ(nodesInside(d, d.nodes[1].rect(), 1), QList<int>({0}));
}

TEST(CanvasGeometry, NearestSide)
{
    CanvasData d;
    d.nodes = {card("a", {0, 0, 100, 100}), card("b", {300, 0, 100, 100})};
    auto hit = nearestSide(d, {0, 1}, QPointF(305, 45), 15);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->node, 1);
    EXPECT_EQ(hit->side, Side::Left);
    hit = nearestSide(d, {0}, QPointF(60, 80), 15); // none within 15: the closest side
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->side, Side::Bottom);
    EXPECT_FALSE(nearestSide(d, {}, QPointF(0, 0), 15));
}

TEST(CanvasGeometry, SnapAxis)
{
    const QList<QPointF> src = snapPoints({QRectF(3, 0, 100, 50)});
    const QList<QPointF> dest = snapPoints({QRectF(0, 100, 50, 50), QRectF(200, 0, 10, 10)});
    auto x = snapAxis(src, dest, 15, 0);
    ASSERT_TRUE(x);
    EXPECT_EQ(x->delta, -3); // left edges line up
    ASSERT_FALSE(x->matches.isEmpty());
    EXPECT_EQ(x->matches.first().src.first().x(), 3);
    EXPECT_EQ(x->matches.first().dest.first().x(), 0);
    // Out of reach.
    EXPECT_FALSE(snapAxis(src, snapPoints({QRectF(500, 500, 10, 10)}), 15, 0));
    // Distance 0: exact matches only.
    EXPECT_FALSE(snapAxis(src, dest, 0, 0));
    auto exact = snapAxis(snapPoints({QRectF(0, 0, 10, 10)}), dest, 0, 0);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->delta, 0);
    // The smaller move wins over a larger one.
    auto best = snapAxis(snapPoints({QRectF(0, 0, 100, 10)}), {QPointF(-10, 0), QPointF(104, 0)}, 15, 0);
    ASSERT_TRUE(best);
    EXPECT_EQ(best->delta, 4);
}

TEST(CanvasGeometry, SnapMove)
{
    SnapOptions opt;
    opt.distance = 15;
    opt.gridSpacing = 20;
    // No other cards: the top-left snaps to the grid.
    EXPECT_EQ(snapMove({QRectF(13, 27, 100, 50)}, {}, true, true, opt), QPointF(7, -7));
    // Only axes that moved snap.
    EXPECT_EQ(snapMove({QRectF(13, 27, 100, 50)}, {}, true, false, opt), QPointF(7, 0));
    // A card within reach wins over the grid on that axis.
    SnapGuides guides;
    const QPointF d = snapMove({QRectF(13, 27, 100, 50)}, {QRectF(10, 300, 40, 40)}, true, true, opt, &guides);
    EXPECT_EQ(d, QPointF(-3, -7));
    ASSERT_TRUE(guides.x);
    EXPECT_FALSE(guides.y);
    opt.objects = false;
    EXPECT_EQ(snapMove({QRectF(13, 27, 100, 50)}, {QRectF(10, 300, 40, 40)}, true, true, opt), QPointF(7, -7));
    opt.grid = false;
    EXPECT_EQ(snapMove({QRectF(13, 27, 100, 50)}, {}, true, true, opt), QPointF(0, 0));
}

TEST(CanvasGeometry, Resize)
{
    SnapOptions opt;
    const QRectF start(0, 0, 200, 100);
    // Bottom-right to (263, 147) snaps to the grid (260, 140).
    EXPECT_EQ(resizeRect(start, Handle::BottomRight, QPointF(263, 147), 0, {}, opt, true), QRectF(0, 0, 260, 140));
    // Without snapping (Alt held).
    EXPECT_EQ(resizeRect(start, Handle::BottomRight, QPointF(263, 147), 0, {}, opt, false), QRectF(0, 0, 263, 147));
    // Minimum size 50.
    EXPECT_EQ(resizeRect(start, Handle::Left, QPointF(190, 50), 0, {}, opt, false), QRectF(150, 0, 50, 100));
    EXPECT_EQ(resizeRect(start, Handle::Top, QPointF(0, 300), 0, {}, opt, false), QRectF(0, 50, 200, 50));
    // Side handles only change one dimension.
    EXPECT_EQ(resizeRect(start, Handle::Right, QPointF(300, 999), 0, {}, opt, false), QRectF(0, 0, 300, 100));
    // Aspect ratio (images): the right side drives the height.
    EXPECT_EQ(resizeRect(start, Handle::Right, QPointF(300, 0), 2, {}, opt, false), QRectF(0, 0, 300, 150));
    // A corner keeps the ratio with the larger dimension.
    EXPECT_EQ(resizeRect(start, Handle::TopLeft, QPointF(-100, 0), 2, {}, opt, false), QRectF(-100, -50, 300, 150));
    // Snapping to a nearby card's edge.
    EXPECT_EQ(resizeRect(start, Handle::Right, QPointF(296, 50), 0, {QRectF(302, 400, 50, 50)}, opt, true), QRectF(0, 0, 302, 100));
}

TEST(CanvasGeometry, Handles)
{
    const QRectF r(0, 0, 200, 100);
    EXPECT_EQ(handleAt(r, QPointF(0, 0), 1), Handle::TopLeft);
    EXPECT_EQ(handleAt(r, QPointF(205, 105), 1), Handle::BottomRight);
    EXPECT_EQ(handleAt(r, QPointF(100, -9), 1), Handle::Top);
    EXPECT_EQ(handleAt(r, QPointF(100, -11), 1), Handle::None);
    EXPECT_EQ(handleAt(r, QPointF(195, 50), 1), Handle::Right);
    EXPECT_EQ(handleAt(r, QPointF(100, 50), 1), Handle::None);
    EXPECT_EQ(handleAt(r, QPointF(100, -18), 2), Handle::Top); // larger at low zoom
    EXPECT_EQ(connectionPoint(r, Side::Top, 1), QRectF(90, -9, 20, 20));
    EXPECT_EQ(connectionPoint(r, Side::Right, 1).center(), QPointF(199, 50));
}

TEST(CanvasGeometry, PlaceRect)
{
    const QSizeF s(250, 60);
    EXPECT_EQ(placeRect(QPointF(0, 0), s, Anchor::Center), QPointF(-125, -30));
    EXPECT_EQ(placeRect(QPointF(0, 0), s, Anchor::Left), QPointF(0, -30));
    EXPECT_EQ(placeRect(QPointF(0, 0), s, Anchor::Right), QPointF(-250, -30));
    EXPECT_EQ(placeRect(QPointF(0, 0), s, Anchor::Top), QPointF(-125, 0));
    EXPECT_EQ(placeRect(QPointF(0, 0), s, Anchor::Bottom), QPointF(-125, -60));
    EXPECT_EQ(placeRect(QPointF(3, 4), s, Anchor::TopLeft), QPointF(3, 4));
}
