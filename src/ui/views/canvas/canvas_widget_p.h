#pragma once

// Internals shared by canvas_widget.cpp (model, viewport, painting) and canvas_input.cpp (input,
// drags, menus).

#include "ui/views/canvas/canvas_widget.h"

#include <QHash>
#include <QRectF>

namespace oc {

struct CanvasWidget::Drag {
    enum class Kind { Pan, BoxSelect, Move, Resize, ResizeSelection, Connect, EdgeEnd, NewCard };
    Kind kind = Kind::Pan;
    bool started = false; // moved past the drag threshold (Obsidian starts drags after a few px)
    Qt::MouseButton button = Qt::LeftButton;
    QPoint pressView;
    QPointF pressCanvas;
    QPointF pointer; // current canvas position
    Qt::KeyboardModifiers mods;

    // Pan: the canvas point that stays under the pointer.
    QPointF panAnchor;

    // BoxSelect
    bool additive = false;
    bool modCreate = false;
    QSet<QString> initialSelection;
    QRectF box;

    // Move: the selection (with the contents of selected groups).
    QString pressedNode;
    QSet<QString> moving;
    QHash<QString, QRectF> startRects;
    bool clone = false;
    QPointF offset; // applied move

    // Resize / ResizeSelection
    QString nodeId;
    canvas::Handle handle = canvas::Handle::None;
    QRectF startRect; // node, or the selection box
    double aspect = 0;
    QHash<QString, QRectF> relative; // selection resize: node boxes relative to the selection box

    // Connect / EdgeEnd: an edge from a fixed end to the pointer (or a snapped side).
    QString edgeId;       // EdgeEnd: the edge being reconnected
    bool movingFrom = false;
    QString fixedNode;
    canvas::Side fixedSide = canvas::Side::Right;
    canvas::EdgeEnd fixedEnd = canvas::EdgeEnd::None;
    canvas::EdgeEnd freeEnd = canvas::EdgeEnd::Arrow;
    canvas::Side freeSide = canvas::Side::Left; // side of the dummy end while not snapped
    std::optional<canvas::SideHit> target;

    // NewCard (dragged from the card menu)
    QString cardKind; // "card", "note", "media"
    QSizeF cardSize;
    QRectF cardRect;

    canvas::SnapGuides guides;
};

namespace canvasui {
inline constexpr int kDragThreshold = 3;
inline constexpr double kZoomAnimation = 0.984; // Obsidian eases by 1 - 0.984^ms per frame
} // namespace canvasui

} // namespace oc
