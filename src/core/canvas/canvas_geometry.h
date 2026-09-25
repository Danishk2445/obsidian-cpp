#pragma once

// Canvas geometry, mirroring Obsidian's canvas: the viewport (a center point and a log2 zoom), edge
// routing (bezier curves between side anchors), hit testing, snapping to the grid and to other cards,
// resizing, and zoom-to-fit. Pure functions over canvas coordinates (1 unit = 1 CSS px at 100%).

#include "core/canvas/canvas_model.h"

#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSet>
#include <QSizeF>

#include <optional>

namespace oc::canvas {

// Obsidian's zoom limits (log2 scale): 1/16 .. 2.
inline constexpr double kMinZoom = -4;
inline constexpr double kMaxZoom = 1;
// At or below this zoom card contents are replaced by placeholders (".mod-zoomed-out"), shifted down
// by the "zoomBreakpoint" option.
inline constexpr double kZoomedOutZoom = -1.7;
inline constexpr double kButtonZoomStep = 0.5;    // "zoomMultiplier" of the zoom buttons
inline constexpr double kObjectSnapDistance = 15; // screen px (divided by the scale for snapping)
inline constexpr double kEdgeEndOffset = 7;       // bezier ends this far outside the node sides
inline constexpr double kResizerSize = 20;        // --resizer-size (times the zoom multiplier)

// The view onto the canvas: (x, y) is the canvas point at the center of the view.
struct Viewport {
    double x = 0;
    double y = 0;
    double zoom = 0; // log2 of the scale
    QSizeF size;     // view size in px

    double scale() const;
    // --zoom-multiplier: sizes that should shrink slower than the canvas (handles, edge widths, labels).
    double zoomMultiplier() const;
    QPointF toView(const QPointF& canvasPt) const;
    QPointF toCanvas(const QPointF& viewPt) const;
    QRectF toView(const QRectF& canvasRect) const;
    QRectF visibleRect() const; // canvas coordinates
    // Zoom by `delta` (log2) keeping the canvas point under `viewAnchor` (view coordinates) in place.
    void zoomAround(double delta, std::optional<QPointF> viewAnchor);
};

double clampZoom(double zoom);
// Background grid spacing for a zoom level (the dot pattern and grid snapping).
double gridSpacing(double zoom);
// Object snapping distance in canvas units at a scale.
double snapDistance(double scale);

// Obsidian's zoomToBbox(): center on `bbox` with 10% margin, never above 100%.
struct ZoomTarget {
    QPointF center;
    double zoom = 0;
};
std::optional<ZoomTarget> zoomToRect(const QRectF& bbox, const QSizeF& viewSize);

// ---- edges --------------------------------------------------------------------------------------

QPointF sideAnchor(const QRectF& rect, Side side); // middle of a side
QPointF sideDirection(Side side);                  // unit vector pointing out of the side

struct EdgeGeometry {
    QPointF fromAnchor, toAnchor; // on the node sides
    QPointF from, to;             // bezier ends (kEdgeEndOffset outside the sides)
    QPointF cp1, cp2;
    bool fromArrow = false, toArrow = false;
    QPainterPath path; // with the straight stubs to the sides where there is no arrow
    QPointF pointAt(double t) const; // on the bezier
    QPointF center() const { return pointAt(0.5); }
    QRectF bounds() const;           // of the path's control polygon
};
EdgeGeometry edgeGeometry(const QRectF& fromRect, Side fromSide, const QRectF& toRect, Side toSide, EdgeEnd fromEnd = EdgeEnd::None,
                          EdgeEnd toEnd = EdgeEnd::Arrow);
// The arrow head at `anchor` pointing into a node through `side`, `scale`d (the zoom multiplier).
QPolygonF arrowHead(const QPointF& anchor, Side side, double scale = 1);
// Distance from `pt` to the drawn edge (bezier + stubs).
double distanceToEdge(const EdgeGeometry& g, const QPointF& pt);
// Obsidian's edge "bbox": the two anchors.
QRectF edgeBox(const EdgeGeometry& g);

// ---- hit testing ------------------------------------------------------------------------------

// Display order: groups first (largest first), then the other cards in list order.
QList<int> paintOrder(const CanvasData& data);
// The topmost shown node whose rect, grown by `tolerance`, contains `pt` (index or -1). Groups count
// only in their label area and within `tolerance` of their border unless `groupBody`.
int nodeAt(const CanvasData& data, const QPointF& pt, double tolerance = 0, bool groupBody = true);
// Box selection: nodes intersecting `box`; groups only when entirely inside.
QList<int> nodesInBox(const CanvasData& data, const QRectF& box);
// Nodes entirely inside `rect` (the contents of a group).
QList<int> nodesInside(const CanvasData& data, const QRectF& rect, int except = -1);
// Bounding box of nodes (and edges between shown nodes) by id.
QRectF itemsBounds(const CanvasData& data, const QSet<QString>& ids);
QRectF allNodesBounds(const CanvasData& data);

// Obsidian's W8: the side of one of `candidates` to connect to at `pt`: the first side whose anchor is
// within `distance`, else the closest side.
struct SideHit {
    int node = -1;
    Side side = Side::Top;
};
std::optional<SideHit> nearestSide(const CanvasData& data, const QList<int>& candidates, const QPointF& pt, double distance);

// ---- snapping ---------------------------------------------------------------------------------

// A matched group of snap points: moving points `src` line up with `dest` points.
struct SnapMatch {
    QList<QPointF> src, dest;
};
struct AxisSnap {
    double delta = 0;
    QList<SnapMatch> matches;
};
// The 4 corners and the center of each rect, sorted by x then y, without duplicates.
QList<QPointF> snapPoints(const QList<QRectF>& rects);
// Obsidian's K8: the smallest move along `axis` (0 = x, 1 = y) that lines up a `src` point with a
// `dest` point, if one is within `distance` (0 = exact matches only).
std::optional<AxisSnap> snapAxis(QList<QPointF> src, QList<QPointF> dest, double distance, int axis);

struct SnapGuides {
    std::optional<AxisSnap> x, y;
    QPointF offset; // applied to the src points of the guides
    bool isEmpty() const { return !x && !y; }
};
struct SnapOptions {
    bool grid = true;
    bool objects = true;
    double gridSpacing = 20;
    double distance = 15; // canvas units
};
// Obsidian's getSnapping(): the offset to add to a move of `moving` rects (already moved), snapping to
// `others` (objects) and then to the grid on the axes without an object snap. Only axes that moved
// (`moveX`, `moveY`) snap with a distance.
QPointF snapMove(const QList<QRectF>& moving, const QList<QRectF>& others, bool moveX, bool moveY, const SnapOptions& opt,
                 SnapGuides* guides = nullptr);

// ---- resizing ---------------------------------------------------------------------------------

enum class Handle { None, Top, Right, Bottom, Left, TopLeft, TopRight, BottomLeft, BottomRight };
bool handleHas(Handle h, Side side);
// The rect of a node being resized from `start` by dragging `handle` to `pointer` (Obsidian's
// onResizePointerdown): min size kMinNodeDimension, `aspect` (w/h, 0 = free) kept, the moving sides
// snapped to `others` / the grid.
QRectF resizeRect(const QRectF& start, Handle handle, const QPointF& pointer, double aspect, const QList<QRectF>& others,
                  const SnapOptions& opt, bool snap, SnapGuides* guides = nullptr);
// The handle under `pt` for a node `rect` (hit areas kResizerSize * zm wide centered on the sides).
Handle handleAt(const QRectF& rect, const QPointF& pt, double zoomMultiplier);
// Connection point (circle at the middle of a side, diameter kResizerSize * zm, inset 1px).
QRectF connectionPoint(const QRectF& rect, Side side, double zoomMultiplier);

// ---- creation -----------------------------------------------------------------------------------

// Obsidian's R8: the top-left of a new card of `size` placed at `pos`: `pos` is its top-left
// (nullopt), its center, or the middle of one of its sides.
enum class Anchor { TopLeft, Center, Top, Right, Bottom, Left };
QPointF placeRect(const QPointF& pos, const QSizeF& size, Anchor anchor);

} // namespace oc::canvas
