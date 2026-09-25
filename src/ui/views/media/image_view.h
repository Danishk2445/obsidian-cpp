#pragma once

// Obsidian's image view ("image", lucide-image): the image centred horizontally at the top of the
// view content (padding 12px 12px 32px), at its natural size but never wider than the view
// ("max-width: 100%"), scrolling vertically when taller. Zoom follows Obsidian 1.13's image
// lightbox: Ctrl+wheel zooms (×1…×10 of the fitted size) around the pointer, a double-click toggles
// ×2.5 at the pointer and back, dragging pans a zoomed image. Animated GIF / WebP play; SVG renders
// at the displayed size. The view reloads when the file changes on disk.

#include "ui/workspace/view.h"

#include <QAbstractScrollArea>
#include <QImage>
#include <QPointer>

#include <memory>
#include <optional>

class QMovie;
class QSvgRenderer;

namespace oc::media {

class ImageCanvas : public QAbstractScrollArea {
    Q_OBJECT
public:
    static constexpr double kMaxZoom = 10.0;
    static constexpr double kDoubleClickZoom = 2.5;

    explicit ImageCanvas(QWidget* parent = nullptr);
    ~ImageCanvas() override;

    void setPath(const QString& absPath); // (re)loads the image
    void reload();
    const QString& path() const { return m_path; }
    bool isValid() const { return m_natural.isValid(); }
    bool isAnimated() const { return m_movie != nullptr; }
    QSize naturalSize() const { return m_natural; }

    double zoom() const { return m_zoom; } // 1 = fitted
    void setZoom(double zoom, std::optional<QPointF> around = std::nullopt);
    QRectF imageRect() const; // viewport coordinates of the displayed image

signals:
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QSizeF fittedSize() const;
    QSizeF displaySize() const { return fittedSize() * m_zoom; }
    QRectF contentImageRect() const; // content coordinates
    void updateScrollBars();
    void updateCursor();
    QImage frame() const;

    QString m_path;
    QSize m_natural;
    QImage m_image;
    std::unique_ptr<QSvgRenderer> m_svg;
    QMovie* m_movie = nullptr;
    mutable QImage m_scaled; // the frame at the displayed size (downscaled smoothly)
    mutable QSize m_scaledFor;
    mutable qint64 m_scaledFrame = -1;
    double m_zoom = 1;
    double m_wheelAccum = 0;
    bool m_panning = false;
    QPoint m_panStart;
    QPoint m_scrollStart;
};

class ImageView : public FileView {
    Q_OBJECT
public:
    explicit ImageView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~ImageView() override;

    QString viewType() const override { return QStringLiteral("image"); }
    QString icon() const override { return QStringLiteral("image"); }
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;
    void focusContent() override;

    ImageCanvas* canvas() const { return m_canvas; }

private:
    ImageCanvas* m_canvas;
    QMetaObject::Connection m_modifiedConn;
};

} // namespace oc::media
