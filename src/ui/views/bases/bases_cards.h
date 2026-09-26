#pragma once

// Obsidian's Bases cards view (.bases-cards-*): a grid of cards as wide as the "cardSize" option
// allows (default 200px, `floor(width / size)` per row, 12px apart), each with an optional cover
// (the "image" property: an image file, a link to one, or a "#rrggbb" color; fitted by "imageFit"
// cover / contain, "imageAspectRatio" of the card width tall), then the view's properties: the
// first as the bold title, the others as a muted label over the value ("—" when empty). Group-by
// sections have a heading and a bottom border. Only the cards in view are painted; a click opens
// the note, a right click shows its file menu, hovering with Mod previews it.

#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_render.h"

namespace oc::basesui {

class CardsLayout : public BaseLayout {
    Q_OBJECT
public:
    explicit CardsLayout(BasesController* controller, QWidget* parent = nullptr);

    QString type() const override { return QStringLiteral("cards"); }
    void setData(const bases::ViewData& data) override;
    int contentHeight() const override { return m_contentHeight; }

    // Geometry (tests).
    int cardsPerRow() const { return m_perRow; }
    QSizeF cardSize() const { return {m_cardWidth, m_cardHeight}; }
    QRectF cardRect(int group, int index) const; // viewport coordinates
    bases::EntryPtr entryAt(const QPoint& viewportPos) const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    struct GroupGeom {
        qreal headingTop = 0;
        qreal headingHeight = 0;
        qreal top = 0; // cards area
        qreal height = 0;
        int count = 0;
    };
    void relayout();
    void paintCard(QPainter& p, const QRectF& r, const bases::EntryPtr& e, bool hover);

    QList<GroupGeom> m_groups;
    QString m_imageProp;
    bool m_contain = false;
    double m_aspect = 1;
    int m_size = 200;
    int m_perRow = 1;
    qreal m_cardWidth = 200, m_cardHeight = 100, m_coverHeight = 0;
    qreal m_titleHeight = 24, m_propHeight = 40;
    qreal m_padX = 12, m_padTop = 12, m_padBottom = 12;
    int m_contentHeight = 0;
    QList<ValueHit> m_hits;
    QString m_hoverPath;
};

} // namespace oc::basesui
