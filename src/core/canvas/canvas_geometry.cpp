#include "core/canvas/canvas_geometry.h"

#include <QLineF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace oc::canvas {

// ---------------------------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------------------------

double Viewport::scale() const { return std::pow(2.0, zoom); }
double Viewport::zoomMultiplier() const { return std::sqrt(1.0 / scale()); }

QPointF Viewport::toView(const QPointF& p) const
{
    const double s = scale();
    return QPointF((p.x() - x) * s + size.width() / 2, (p.y() - y) * s + size.height() / 2);
}

QPointF Viewport::toCanvas(const QPointF& v) const
{
    const double s = scale();
    return QPointF(x + (v.x() - size.width() / 2) / s, y + (v.y() - size.height() / 2) / s);
}

QRectF Viewport::toView(const QRectF& r) const { return QRectF(toView(r.topLeft()), toView(r.bottomRight())); }

QRectF Viewport::visibleRect() const { return QRectF(toCanvas(QPointF(0, 0)), toCanvas(QPointF(size.width(), size.height()))); }

void Viewport::zoomAround(double delta, std::optional<QPointF> viewAnchor)
{
    const double before = scale();
    zoom = clampZoom(zoom + delta);
    const double after = scale();
    if (viewAnchor) {
        const QPointF f = *viewAnchor - QPointF(size.width() / 2, size.height() / 2);
        x -= f.x() / after - f.x() / before;
        y -= f.y() / after - f.y() / before;
    }
}

double clampZoom(double zoom) { return std::clamp(zoom, kMinZoom, kMaxZoom); }

double gridSpacing(double zoom)
{
    if (zoom < -3.3)
        return 160;
    if (zoom < -2.16)
        return 80;
    if (zoom < -0.91)
        return 40;
    return 20;
}

double snapDistance(double scale) { return std::ceil(kObjectSnapDistance / scale); }

std::optional<ZoomTarget> zoomToRect(const QRectF& bbox, const QSizeF& viewSize)
{
    if (viewSize.width() <= 0 || viewSize.height() <= 0)
        return std::nullopt;
    const double r = viewSize.width() / (1.1 * bbox.width());
    const double o = viewSize.height() / (1.1 * bbox.height());
    const double a = std::min(1.0, std::min(r, o)); // Math.clamp(min(r, o), -4, 1)
    return ZoomTarget{bbox.center(), clampZoom(std::log2(a))};
}

// ---------------------------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------------------------

QPointF sideAnchor(const QRectF& r, Side side)
{
    switch (side) {
    case Side::Top: return QPointF((r.left() + r.right()) / 2, r.top());
    case Side::Right: return QPointF(r.right(), (r.top() + r.bottom()) / 2);
    case Side::Bottom: return QPointF((r.left() + r.right()) / 2, r.bottom());
    case Side::Left: return QPointF(r.left(), (r.top() + r.bottom()) / 2);
    }
    return r.center();
}

QPointF sideDirection(Side side)
{
    switch (side) {
    case Side::Top: return QPointF(0, -1);
    case Side::Right: return QPointF(1, 0);
    case Side::Bottom: return QPointF(0, 1);
    case Side::Left: return QPointF(-1, 0);
    }
    return QPointF();
}

QPointF EdgeGeometry::pointAt(double t) const
{
    const double a = 1 - t;
    const double s = a * a * a, l = 3 * t * a * a, c = 3 * t * t * a, u = t * t * t;
    return s * from + l * cp1 + c * cp2 + u * to;
}

QRectF EdgeGeometry::bounds() const
{
    double minX = fromAnchor.x(), maxX = minX, minY = fromAnchor.y(), maxY = minY;
    for (const QPointF& p : {toAnchor, from, to, cp1, cp2}) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

EdgeGeometry edgeGeometry(const QRectF& fromRect, Side fromSide, const QRectF& toRect, Side toSide, EdgeEnd fromEnd, EdgeEnd toEnd)
{
    EdgeGeometry g;
    g.fromAnchor = sideAnchor(fromRect, fromSide);
    g.toAnchor = sideAnchor(toRect, toSide);
    g.from = g.fromAnchor + sideDirection(fromSide) * kEdgeEndOffset;
    g.to = g.toAnchor + sideDirection(toSide) * kEdgeEndOffset;
    const double dist = QLineF(g.from, g.to).length();
    const double o = std::clamp(dist / 2, 70.0, 150.0);
    g.cp1 = g.from + sideDirection(fromSide) * o;
    g.cp2 = g.to + sideDirection(toSide) * o;
    g.fromArrow = fromEnd == EdgeEnd::Arrow;
    g.toArrow = toEnd == EdgeEnd::Arrow;
    if (!g.fromArrow) {
        g.path.moveTo(g.fromAnchor);
        g.path.lineTo(g.from);
    }
    g.path.moveTo(g.from);
    g.path.cubicTo(g.cp1, g.cp2, g.to);
    if (!g.toArrow) {
        g.path.moveTo(g.to);
        g.path.lineTo(g.toAnchor);
    }
    return g;
}

QPolygonF arrowHead(const QPointF& anchor, Side side, double scale)
{
    // Obsidian's "0,0 6.5,10.4 -6.5,10.4", rotated so that the base points away from the node.
    double deg = 0;
    switch (side) {
    case Side::Top: deg = 180; break;
    case Side::Bottom: deg = 0; break;
    case Side::Left: deg = 90; break;
    case Side::Right: deg = 270; break;
    }
    QTransform t;
    t.translate(anchor.x(), anchor.y());
    t.rotate(deg);
    t.scale(scale, scale);
    return t.map(QPolygonF({QPointF(0, 0), QPointF(6.5, 10.4), QPointF(-6.5, 10.4)}));
}

namespace {
double distToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    double t = len2 > 0 ? QPointF::dotProduct(p - a, ab) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + ab * t;
    return QLineF(p, q).length();
}
} // namespace

double distanceToEdge(const EdgeGeometry& g, const QPointF& pt)
{
    double best = std::min(distToSegment(pt, g.fromAnchor, g.from), distToSegment(pt, g.to, g.toAnchor));
    constexpr int kSteps = 48;
    QPointF prev = g.from;
    for (int i = 1; i <= kSteps; ++i) {
        const QPointF p = g.pointAt(double(i) / kSteps);
        best = std::min(best, distToSegment(pt, prev, p));
        prev = p;
    }
    return best;
}

QRectF edgeBox(const EdgeGeometry& g)
{
    return QRectF(QPointF(std::min(g.fromAnchor.x(), g.toAnchor.x()), std::min(g.fromAnchor.y(), g.toAnchor.y())),
                  QPointF(std::max(g.fromAnchor.x(), g.toAnchor.x()), std::max(g.fromAnchor.y(), g.toAnchor.y())));
}

// ---------------------------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------------------------

namespace {
// Obsidian's bbox tests are inclusive (touching counts).
bool intersects(const QRectF& a, const QRectF& b)
{
    return b.left() <= a.right() && b.top() <= a.bottom() && b.right() >= a.left() && b.bottom() >= a.top();
}
bool containsRect(const QRectF& outer, const QRectF& inner)
{
    return outer.left() <= inner.left() && outer.top() <= inner.top() && outer.right() >= inner.right() && outer.bottom() >= inner.bottom();
}
bool containsPoint(const QRectF& r, const QPointF& p)
{
    return p.x() >= r.left() && p.x() <= r.right() && p.y() >= r.top() && p.y() <= r.bottom();
}
} // namespace

QList<int> paintOrder(const CanvasData& data)
{
    QList<int> groups, others;
    for (int i = 0; i < data.nodes.size(); ++i) {
        const Node& n = data.nodes[i];
        if (!n.isShown())
            continue;
        (n.isGroup() ? groups : others) << i;
    }
    std::stable_sort(groups.begin(), groups.end(), [&](int a, int b) {
        return data.nodes[a].width * data.nodes[a].height > data.nodes[b].width * data.nodes[b].height;
    });
    return groups + others;
}

int nodeAt(const CanvasData& data, const QPointF& pt, double tolerance, bool groupBody)
{
    const QList<int> order = paintOrder(data);
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        const Node& n = data.nodes[*it];
        const QRectF r = n.rect().adjusted(-tolerance, -tolerance, tolerance, tolerance);
        if (!containsPoint(r, pt))
            continue;
        if (n.isGroup() && !groupBody) {
            const QRectF inner = n.rect().adjusted(tolerance, tolerance, -tolerance, -tolerance);
            if (inner.isValid() && containsPoint(inner, pt))
                continue; // click-through inside an unselected group
        }
        return *it;
    }
    return -1;
}

QList<int> nodesInBox(const CanvasData& data, const QRectF& box)
{
    QList<int> out;
    for (int i = 0; i < data.nodes.size(); ++i) {
        const Node& n = data.nodes[i];
        if (!n.isShown() || !intersects(box, n.rect()))
            continue;
        if (n.isGroup() && !containsRect(box, n.rect()))
            continue;
        out << i;
    }
    return out;
}

QList<int> nodesInside(const CanvasData& data, const QRectF& rect, int except)
{
    QList<int> out;
    for (int i = 0; i < data.nodes.size(); ++i) {
        const Node& n = data.nodes[i];
        if (i != except && n.isShown() && containsRect(rect, n.rect()))
            out << i;
    }
    return out;
}

QRectF itemsBounds(const CanvasData& data, const QSet<QString>& ids)
{
    QRectF box;
    bool any = false;
    auto add = [&](const QRectF& r) {
        box = any ? QRectF(QPointF(std::min(box.left(), r.left()), std::min(box.top(), r.top())),
                           QPointF(std::max(box.right(), r.right()), std::max(box.bottom(), r.bottom())))
                  : r;
        any = true;
    };
    for (const Node& n : data.nodes)
        if (n.isShown() && ids.contains(n.id))
            add(n.rect());
    for (const Edge& e : data.edges) {
        if (!ids.contains(e.id) || !data.isEdgeShown(e))
            continue;
        const Node* a = data.node(e.fromNode);
        const Node* b = data.node(e.toNode);
        add(edgeBox(edgeGeometry(a->rect(), e.fromSide, b->rect(), e.toSide)));
    }
    return box;
}

QRectF allNodesBounds(const CanvasData& data)
{
    QSet<QString> ids;
    for (const Node& n : data.nodes)
        if (n.isShown())
            ids.insert(n.id);
    return itemsBounds(data, ids);
}

std::optional<SideHit> nearestSide(const CanvasData& data, const QList<int>& candidates, const QPointF& pt, double distance)
{
    std::optional<SideHit> best;
    double bestDist = std::numeric_limits<double>::infinity();
    for (int idx : candidates) {
        const QRectF r = data.nodes[idx].rect();
        for (Side s : {Side::Top, Side::Right, Side::Bottom, Side::Left}) {
            const double d = QLineF(pt, sideAnchor(r, s)).length();
            if (d <= distance)
                return SideHit{idx, s};
            if (d < bestDist) {
                bestDist = d;
                best = SideHit{idx, s};
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// Snapping
// ---------------------------------------------------------------------------------------------

QList<QPointF> snapPoints(const QList<QRectF>& rects)
{
    QList<QPointF> pts;
    for (const QRectF& r : rects) {
        pts << QPointF(r.left(), r.top()) << QPointF(r.left(), r.bottom()) << QPointF(r.right(), r.top())
            << QPointF(r.right(), r.bottom()) << QPointF((r.left() + r.right()) / 2, (r.top() + r.bottom()) / 2);
    }
    std::sort(pts.begin(), pts.end(), [](const QPointF& a, const QPointF& b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
    QList<QPointF> out;
    for (const QPointF& p : pts)
        if (out.isEmpty() || out.last().x() != p.x() || out.last().y() != p.y())
            out << p;
    return out;
}

std::optional<AxisSnap> snapAxis(QList<QPointF> e, QList<QPointF> t, double n, int axis)
{
    if (e.isEmpty() || t.isEmpty())
        return std::nullopt;
    auto r = [axis](const QPointF& p) { return axis == 0 ? p.x() : p.y(); };
    auto byAxis = [&](const QPointF& a, const QPointF& b) { return r(a) < r(b); };
    std::stable_sort(e.begin(), e.end(), byAxis);
    std::stable_sort(t.begin(), t.end(), byAxis);
    double o = n;
    QList<SnapMatch> a;
    QList<QPointF> s, l;
    double c = std::numeric_limits<double>::infinity();
    const int tn = int(t.size());
    for (const QPointF& p : std::as_const(e)) {
        const double d = r(p);
        if (d != c) {
            c = d;
            if (!s.isEmpty() || !l.isEmpty()) {
                a << SnapMatch{s, l};
                s.clear();
                l.clear();
            }
            int f = 0, v = tn - 1;
            while (f <= v) {
                const int m = (f + v) >> 1;
                const double g = d - r(t[m]);
                if (g > 0) {
                    f = m + 1;
                } else if (g < 0) {
                    v = m - 1;
                } else {
                    f = v = m;
                    break;
                }
            }
            if (f != v) {
                if (v < 0) {
                    v = 0;
                } else if (f >= tn - 1) {
                    f = tn - 1;
                } else {
                    const double y = r(t[f]) - d;
                    const double b = d - r(t[v]);
                    if (y == b) {
                        if (y == o)
                            v = f;
                        else
                            f = v;
                    } else {
                        f = v = y < b ? f : v;
                    }
                }
            }
            const double w = r(t[f]);
            const double k = w - d;
            if (k == o || std::abs(k) < std::abs(o)) {
                if (k != o) {
                    a.clear();
                    s.clear();
                    l.clear();
                    o = k;
                }
                while (f > 0 && r(t[f - 1]) == w)
                    --f;
                while (v < tn - 1 && r(t[v + 1]) == w)
                    ++v;
                s << p;
                for (int i = f; i <= v; ++i)
                    l << t[i];
            }
        } else if (!s.isEmpty() && r(s.last()) == d) {
            s << p;
        }
    }
    if (!s.isEmpty() || !l.isEmpty())
        a << SnapMatch{s, l};
    if (a.isEmpty())
        return std::nullopt;
    return AxisSnap{o, a};
}

namespace {
QRectF boundsOf(const QList<QRectF>& rects)
{
    QRectF b = rects.first();
    for (const QRectF& r : rects)
        b = QRectF(QPointF(std::min(b.left(), r.left()), std::min(b.top(), r.top())),
                   QPointF(std::max(b.right(), r.right()), std::max(b.bottom(), r.bottom())));
    return b;
}
} // namespace

QPointF snapMove(const QList<QRectF>& moving, const QList<QRectF>& others, bool moveX, bool moveY, const SnapOptions& opt,
                 SnapGuides* guides)
{
    if (moving.isEmpty())
        return QPointF();
    double c = 0, u = 0;
    std::optional<AxisSnap> rx, ry;
    if (opt.objects) {
        const QList<QPointF> h = snapPoints(moving);
        const QList<QPointF> p = snapPoints(others);
        rx = snapAxis(h, p, moveX ? opt.distance : 0, 0);
        if (rx)
            c += rx->delta;
        ry = snapAxis(h, p, moveY ? opt.distance : 0, 1);
        if (ry)
            u += ry->delta;
    }
    if (opt.grid && (!rx || !ry)) {
        const QRectF f = boundsOf(moving);
        const double v = opt.gridSpacing;
        if (!rx && moveX)
            c += jsRound(f.left() / v) * v - f.left();
        if (!ry && moveY)
            u += jsRound(f.top() / v) * v - f.top();
    }
    if (guides) {
        guides->x = rx;
        guides->y = ry;
        guides->offset = QPointF(c, u);
    }
    return QPointF(c, u);
}

// ---------------------------------------------------------------------------------------------
// Resizing
// ---------------------------------------------------------------------------------------------

bool handleHas(Handle h, Side side)
{
    switch (side) {
    case Side::Top: return h == Handle::Top || h == Handle::TopLeft || h == Handle::TopRight;
    case Side::Bottom: return h == Handle::Bottom || h == Handle::BottomLeft || h == Handle::BottomRight;
    case Side::Left: return h == Handle::Left || h == Handle::TopLeft || h == Handle::BottomLeft;
    case Side::Right: return h == Handle::Right || h == Handle::TopRight || h == Handle::BottomRight;
    }
    return false;
}

QRectF resizeRect(const QRectF& start, Handle handle, const QPointF& d, double aspect, const QList<QRectF>& others,
                  const SnapOptions& opt, bool snap, SnapGuides* guides)
{
    const double p = kMinNodeDimension;
    // Obsidian's N8: the moving edge (or corner) as a rect to snap.
    QRectF source;
    bool resizeX = false, resizeY = false;
    switch (handle) {
    case Handle::Top:
    case Handle::Bottom:
        source = QRectF(start.x(), d.y(), start.width(), 0);
        resizeY = true;
        break;
    case Handle::Left:
    case Handle::Right:
        source = QRectF(d.x(), start.y(), 0, start.height());
        resizeX = true;
        break;
    case Handle::None:
        return start;
    default:
        source = QRectF(d.x(), d.y(), 0, 0);
        resizeX = resizeY = true;
        break;
    }
    double f = 0, v = 0;
    std::optional<AxisSnap> i, c;
    if (snap) {
        if (opt.objects) {
            const QList<QPointF> w = snapPoints({source});
            const QList<QPointF> k = snapPoints(others);
            if (resizeX && (i = snapAxis(w, k, opt.distance, 0)))
                f = i->delta;
            if (resizeY && (c = snapAxis(w, k, opt.distance, 1)))
                v = c->delta;
        }
        if (opt.grid) {
            const double e = opt.gridSpacing;
            if (resizeX && !i)
                f = jsRound(d.x() / e) * e - d.x();
            if (resizeY && !c)
                v = jsRound(d.y() / e) * e - d.y();
        }
    }
    const double M = d.x() + f, S = d.y() + v;
    double minX = start.left(), minY = start.top(), maxX = start.right(), maxY = start.bottom();
    if (handleHas(handle, Side::Top)) {
        minY = std::min(maxY - p, S);
        if (minY != S) {
            c.reset();
            v = minY - d.y();
        }
    }
    if (handleHas(handle, Side::Left)) {
        minX = std::min(maxX - p, M);
        if (minX != M) {
            i.reset();
            f = minX - d.x();
        }
    }
    if (handleHas(handle, Side::Bottom)) {
        maxY = std::max(minY + p, S);
        if (maxY != S) {
            c.reset();
            v = maxY - d.y();
        }
    }
    if (handleHas(handle, Side::Right)) {
        maxX = std::max(minX + p, M);
        if (maxX != M) {
            i.reset();
            f = maxX - d.x();
        }
    }
    QRectF T(minX, minY, maxX - minX, maxY - minY);
    if (aspect != 0) {
        double A = T.width(), P = T.height();
        if (resizeX && resizeY) {
            const double I = std::max(A, P * aspect);
            P = std::max(P, A / aspect);
            A = I;
            if (A != T.width())
                i.reset();
            else if (P != T.height())
                c.reset();
        } else if (resizeX) {
            P = A / aspect;
        } else if (resizeY) {
            A = P * aspect;
        }
        double tx = T.x(), ty = T.y();
        if (handleHas(handle, Side::Left)) {
            const double L = start.x() + start.width() - A;
            f += L - tx;
            tx = L;
        }
        if (handleHas(handle, Side::Top)) {
            const double O = start.y() + start.height() - P;
            v += O - ty;
            ty = O;
        }
        if (handleHas(handle, Side::Right))
            f += A - T.width();
        if (handleHas(handle, Side::Bottom))
            v += P - T.height();
        T = QRectF(tx, ty, A, P);
    }
    if (guides) {
        guides->x = i;
        guides->y = c;
        guides->offset = QPointF(f, v);
    }
    return T;
}

Handle handleAt(const QRectF& rect, const QPointF& pt, double zm)
{
    const double h = kResizerSize * zm / 2;
    auto near = [&](double a, double b) { return std::abs(a - b) <= h; };
    const bool inX = pt.x() >= rect.left() - h && pt.x() <= rect.right() + h;
    const bool inY = pt.y() >= rect.top() - h && pt.y() <= rect.bottom() + h;
    if (!inX || !inY)
        return Handle::None;
    const bool top = near(pt.y(), rect.top()), bottom = near(pt.y(), rect.bottom());
    const bool left = near(pt.x(), rect.left()), right = near(pt.x(), rect.right());
    if (top && left)
        return Handle::TopLeft;
    if (top && right)
        return Handle::TopRight;
    if (bottom && left)
        return Handle::BottomLeft;
    if (bottom && right)
        return Handle::BottomRight;
    const bool withinX = pt.x() >= rect.left() && pt.x() <= rect.right();
    const bool withinY = pt.y() >= rect.top() && pt.y() <= rect.bottom();
    if (top && withinX)
        return Handle::Top;
    if (bottom && withinX)
        return Handle::Bottom;
    if (left && withinY)
        return Handle::Left;
    if (right && withinY)
        return Handle::Right;
    return Handle::None;
}

QRectF connectionPoint(const QRectF& rect, Side side, double zm)
{
    const double d = kResizerSize * zm;
    QPointF c;
    switch (side) {
    case Side::Top: c = QPointF(rect.center().x(), rect.top() + 1); break;
    case Side::Bottom: c = QPointF(rect.center().x(), rect.bottom() - 1); break;
    case Side::Left: c = QPointF(rect.left() + 1, rect.center().y()); break;
    case Side::Right: c = QPointF(rect.right() - 1, rect.center().y()); break;
    }
    return QRectF(c.x() - d / 2, c.y() - d / 2, d, d);
}

QPointF placeRect(const QPointF& pos, const QSizeF& size, Anchor anchor)
{
    const double w = size.width(), h = size.height();
    switch (anchor) {
    case Anchor::TopLeft: return pos;
    case Anchor::Center: return pos - QPointF(w / 2, h / 2);
    case Anchor::Left: return QPointF(pos.x(), pos.y() - h / 2);
    case Anchor::Top: return QPointF(pos.x() - w / 2, pos.y());
    case Anchor::Right: return QPointF(pos.x() - w, pos.y() - h / 2);
    case Anchor::Bottom: return QPointF(pos.x() - w / 2, pos.y() - h);
    }
    return pos;
}

} // namespace oc::canvas
