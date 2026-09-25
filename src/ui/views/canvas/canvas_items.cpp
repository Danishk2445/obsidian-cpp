#include "ui/views/canvas/canvas_items.h"

#include "ui/views/canvas/canvas_widget.h"

namespace oc::canvasui {

CanvasItem::CanvasItem(CanvasWidget* canvas, Kind kind, QString id, QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , m_canvas(canvas)
    , m_kind(kind)
    , m_id(std::move(id))
{
    setAcceptedMouseButtons(Qt::NoButton); // the canvas widget handles input
    setAcceptHoverEvents(false);
}

void CanvasItem::setBounds(const QRectF& r)
{
    if (r == m_bounds)
        return;
    prepareGeometryChange();
    m_bounds = r;
}

void CanvasItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option);
    Q_UNUSED(widget);
    m_canvas->paintItem(painter, *this);
}

} // namespace oc::canvasui
