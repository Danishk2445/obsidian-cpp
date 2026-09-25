#include "ui/views/media/image_view.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/services/image_cache.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/views/media/media_views.h"

#include <QImageReader>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QVBoxLayout>

#include <cmath>

namespace oc::media {

namespace {
// .workspace-leaf-content .view-content { padding: var(--size-4-3) var(--size-4-3) var(--size-4-8) }
constexpr int kPadX = 12;
constexpr int kPadTop = 12;
constexpr int kPadBottom = 32;
} // namespace

// ---------------------------------------------------------------------------------------------
// ImageCanvas
// ---------------------------------------------------------------------------------------------
ImageCanvas::ImageCanvas(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("ocImageCanvas"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, viewport(), qOverload<>(&QWidget::update));
}

ImageCanvas::~ImageCanvas() = default;

void ImageCanvas::setPath(const QString& absPath)
{
    m_path = absPath;
    reload();
}

void ImageCanvas::reload()
{
    delete m_movie;
    m_movie = nullptr;
    m_svg.reset();
    m_image = QImage();
    m_natural = QSize();
    m_scaled = QImage();
    m_scaledFor = QSize();
    if (!m_path.isEmpty()) {
        if (m_path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
            m_svg = std::make_unique<QSvgRenderer>(m_path);
            if (m_svg->isValid())
                m_natural = m_svg->defaultSize();
        } else {
            QImageReader reader(m_path);
            reader.setAutoTransform(true);
            if (reader.supportsAnimation() && reader.imageCount() > 1) {
                m_movie = new QMovie(m_path, QByteArray(), this);
                m_movie->setCacheMode(QMovie::CacheAll);
                connect(m_movie, &QMovie::frameChanged, viewport(), qOverload<>(&QWidget::update));
                m_movie->start();
                m_natural = m_movie->currentImage().size();
                if (!m_natural.isValid())
                    m_natural = reader.size();
            } else {
                m_image = reader.read();
                m_natural = m_image.size();
            }
        }
    }
    ImageCache::instance().invalidate(m_path); // embeds of the file pick up the new content
    updateScrollBars();
    viewport()->update();
}

QSizeF ImageCanvas::fittedSize() const
{
    if (!m_natural.isValid())
        return {};
    // img:not([width]) { max-width: 100% } inside the padded view content
    const double avail = std::max(1, maximumViewportSize().width() - 2 * kPadX);
    const double w = std::min<double>(m_natural.width(), avail);
    return {w, w * m_natural.height() / std::max(1, m_natural.width())};
}

QRectF ImageCanvas::contentImageRect() const
{
    const QSizeF s = displaySize();
    const double avail = viewport()->width() - 2 * kPadX;
    const double x = kPadX + std::max(0.0, (avail - s.width()) / 2); // text-align: center
    return {QPointF(x, kPadTop), s};
}

QRectF ImageCanvas::imageRect() const
{
    return contentImageRect().translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

void ImageCanvas::updateScrollBars()
{
    const QSizeF s = displaySize();
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(s.width() + 2 * kPadX - vp.width()))));
    horizontalScrollBar()->setPageStep(vp.width());
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(s.height() + kPadTop + kPadBottom - vp.height()))));
    verticalScrollBar()->setPageStep(vp.height());
    verticalScrollBar()->setSingleStep(40);
    updateCursor();
}

void ImageCanvas::setZoom(double zoom, std::optional<QPointF> around)
{
    zoom = std::clamp(zoom, 1.0, kMaxZoom);
    if (qFuzzyCompare(zoom, m_zoom) || !isValid())
        return;
    const QPointF at = around.value_or(QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
    const QRectF before = imageRect();
    const QPointF rel((at.x() - before.left()) / std::max(1.0, before.width()), (at.y() - before.top()) / std::max(1.0, before.height()));
    m_zoom = zoom;
    updateScrollBars();
    // keep the image point under `at`
    const QRectF after = contentImageRect();
    const QPointF target(after.left() + rel.x() * after.width(), after.top() + rel.y() * after.height());
    horizontalScrollBar()->setValue(int(std::lround(target.x() - at.x())));
    verticalScrollBar()->setValue(int(std::lround(target.y() - at.y())));
    viewport()->update();
    emit zoomChanged(m_zoom);
}

void ImageCanvas::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
}

void ImageCanvas::scrollContentsBy(int, int) { viewport()->update(); }

QImage ImageCanvas::frame() const { return m_movie ? m_movie->currentImage() : m_image; }

void ImageCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const ThemePtr t = currentTheme();
    p.fillRect(viewport()->rect(), t->color(QStringLiteral("--background-primary")));
    if (!isValid()) {
        if (m_path.isEmpty())
            return;
        // a broken image: Obsidian shows the browser's empty image; draw a muted icon instead
        const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("image-off"), 32, t->color(QStringLiteral("--text-faint")), 1.75,
                                                           devicePixelRatioF());
        p.drawPixmap(QPointF((viewport()->width() - 32) / 2.0, kPadTop + 16), pm);
        return;
    }
    const QRectF r = imageRect();
    const double dpr = devicePixelRatioF();
    const QSize px(int(std::lround(r.width() * dpr)), int(std::lround(r.height() * dpr)));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_svg) {
        if (m_scaledFor != px) {
            m_scaled = QImage(px, QImage::Format_ARGB32_Premultiplied);
            m_scaled.fill(Qt::transparent);
            QPainter sp(&m_scaled);
            m_svg->render(&sp);
            m_scaledFor = px;
        }
        p.drawImage(r, m_scaled);
        return;
    }
    const QImage img = frame();
    if (img.isNull())
        return;
    if (px.width() < img.width()) {
        // downscale once with a proper filter (the painter's bilinear would alias)
        const qint64 frameNo = m_movie ? m_movie->currentFrameNumber() : 0;
        if (m_scaledFor != px || m_scaledFrame != frameNo) {
            m_scaled = img.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_scaledFor = px;
            m_scaledFrame = frameNo;
        }
        p.drawImage(r, m_scaled);
    } else {
        p.drawImage(r, img);
    }
}

void ImageCanvas::updateCursor()
{
    const bool pannable = m_zoom > 1.0 && (horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0);
    viewport()->setCursor(m_panning ? Qt::ClosedHandCursor : pannable ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void ImageCanvas::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        // lightbox: zoom += -deltaY / 150 (a wheel notch is 100 px of deltaY)
        m_wheelAccum += e->angleDelta().y() / 120.0 * 100.0 / 150.0;
        if (std::abs(m_wheelAccum) >= 0.05) {
            setZoom(m_zoom + m_wheelAccum, e->position());
            m_wheelAccum = 0;
        }
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void ImageCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::LeftButton && m_zoom > 1.0) {
        m_panning = true;
        m_panStart = e->position().toPoint();
        m_scrollStart = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        updateCursor();
        return;
    }
    QAbstractScrollArea::mousePressEvent(e);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (m_panning) {
        const QPoint d = e->position().toPoint() - m_panStart;
        horizontalScrollBar()->setValue(m_scrollStart.x() - d.x());
        verticalScrollBar()->setValue(m_scrollStart.y() - d.y());
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(e);
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_panning) {
        m_panning = false;
        updateCursor();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(e);
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !isValid())
        return;
    m_panning = false;
    setZoom(m_zoom > 1.0 ? 1.0 : kDoubleClickZoom, e->position());
    updateCursor();
}

// ---------------------------------------------------------------------------------------------
// ImageView
// ---------------------------------------------------------------------------------------------
ImageView::ImageView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_canvas(new ImageCanvas(this))
{
    setObjectName(QStringLiteral("ocImageView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_canvas);
    setFocusProxy(m_canvas);
}

ImageView::~ImageView() = default;

bool ImageView::canAcceptExtension(const QString& extension) const { return imageExtensions().contains(extension.toLower()); }

void ImageView::loadFile(TFile* file)
{
    m_file = file;
    VaultSession* s = session();
    if (s && s->vault() && !m_modifiedConn) {
        m_modifiedConn = connect(s->vault(), &Vault::modified, this, [this](TFile* f, bool) {
            if (f && f == m_file)
                m_canvas->reload();
        });
    }
    m_canvas->setPath(file && s && s->vault() ? s->vault()->absolutePath(file->path()) : QString());
    emit titleChanged();
}

void ImageView::onRename(TFile* file, const QString& oldPath)
{
    if (VaultSession* s = session(); s && s->vault() && file == m_file) {
        const double zoom = m_canvas->zoom();
        m_canvas->setPath(s->vault()->absolutePath(file->path()));
        m_canvas->setZoom(zoom);
    }
    FileView::onRename(file, oldPath);
}

void ImageView::focusContent() { m_canvas->setFocus(); }

} // namespace oc::media
