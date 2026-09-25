#pragma once

// Scene items of the canvas view. They only hold a bounding rect and an id; CanvasWidget paints them
// from its document (see CanvasWidget::paintItem). Coordinates are canvas coordinates (the items are
// children of the root item carrying the canvas transform).

#include <QGraphicsItem>
#include <QString>

namespace oc {

class CanvasWidget;

namespace canvasui {

class CanvasItem : public QGraphicsItem {
public:
    enum class Kind { Node, Edge, EdgeLabel, Overlay };

    CanvasItem(CanvasWidget* canvas, Kind kind, QString id, QGraphicsItem* parent);

    Kind kind() const { return m_kind; }
    const QString& id() const { return m_id; }
    void setBounds(const QRectF& r);

    QRectF boundingRect() const override { return m_bounds; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    CanvasWidget* m_canvas;
    Kind m_kind;
    QString m_id;
    QRectF m_bounds;
};

} // namespace canvasui
} // namespace oc
