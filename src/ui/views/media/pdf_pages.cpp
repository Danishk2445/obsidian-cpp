#include "ui/views/media/pdf_pages.h"

#include "render/theme/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfSelection>
#include <QScrollBar>

#include <cmath>

namespace oc::media {

namespace {
constexpr int kScrollbar = 12;         // the app's scrollbar width (app.qss)
constexpr double kScrollStep = 40;     // arrow keys / wheel line
constexpr double kEmbedGap = 14;       // removePageBorders: margin-bottom 10 + next page's border-top 4
constexpr double kEmbedTailMargin = 10;
constexpr double kMaxAutoScale = 1.25; // PDF.js MAX_AUTO_SCALE

bool isPreset(const QString& v)
{
    return v == u"page-width" || v == u"page-height" || v == u"page-fit" || v == u"auto";
}

QRectF grow(const QRectF& r, double d) { return r.adjusted(-d, -d, d, d); }
} // namespace

PdfPagesView::PdfPagesView(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("ocPdfPages"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    m_selectionTimer.setSingleShot(true);
    m_selectionTimer.setInterval(0);
    connect(&m_selectionTimer, &QTimer::timeout, this, &PdfPagesView::updateSelection);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, viewport(), qOverload<>(&QWidget::update));
}

PdfPagesView::~PdfPagesView() = default;

// ---------------------------------------------------------------------------------------------
// file / options
// ---------------------------------------------------------------------------------------------
void PdfPagesView::setFile(std::shared_ptr<PdfFile> file)
{
    const bool same = file && m_file && file.get() == m_file.get();
    disconnect(m_renderedConn);
    disconnect(m_reloadedConn);
    const Location keep = location();
    m_file = std::move(file);
    m_selection.clear();
    m_selectAll = false;
    m_highlights.clear();
    m_currentHighlight = -1;
    m_hoverLink = m_pressLink = nullptr;
    m_pages.clear();
    if (m_file) {
        for (int i = 0; i < m_file->pageCount(); ++i)
            m_pages.append(PageSlot{m_file->pagePointSize(i), {}});
        m_renderedConn = connect(m_file.get(), &PdfFile::rendered, this, [this](int page) {
            if (page >= 0 && page < m_pages.size()) {
                const QRectF r = m_pages[page].rect.translated(-scrollOffset());
                if (r.intersects(QRectF(viewport()->rect())))
                    viewport()->update();
            }
        });
        m_reloadedConn = connect(m_file.get(), &PdfFile::reloaded, this, [this] {
            // same file, new content (modified on disk / password): keep the location
            std::shared_ptr<PdfFile> f = m_file;
            setFile(std::move(f));
        });
    }
    m_current = same ? std::clamp(keep.page, 0, std::max(0, pageCount() - 1)) : 0;
    const double s = isPreset(m_scaleValue) ? fitScale(m_scaleValue) : m_scaleValue.toDouble();
    m_scale = std::clamp(s > 0 ? s : 1.0, kMinScale, kMaxScale);
    relayout();
    if (same)
        goToLocation(keep.page, keep.point);
    else
        goToPage(0);
    updateCurrentPage();
    emit scaleChanged();
    viewport()->update();
}

void PdfPagesView::setEmbedded(bool embedded)
{
    if (m_embedded == embedded)
        return;
    m_embedded = embedded;
    applyScale(isPreset(m_scaleValue) ? fitScale(m_scaleValue) : m_scale, m_scaleValue, std::nullopt);
}

void PdfPagesView::setSpreadMode(SpreadMode mode)
{
    if (m_spread == mode)
        return;
    const Location keep = location();
    m_spread = mode;
    const double s = isPreset(m_scaleValue) ? fitScale(m_scaleValue) : m_scale;
    m_scale = std::clamp(s, kMinScale, kMaxScale);
    relayout();
    goToPage(keep.page);
    emit scaleChanged();
}

void PdfPagesView::setThemed(bool themed)
{
    if (m_themed == themed)
        return;
    m_themed = themed;
    viewport()->update();
}

bool PdfPagesView::themedNow() const { return m_themed && currentTheme()->isDark(); }

QColor PdfPagesView::pageBackground() const
{
    const ThemePtr t = currentTheme();
    return t->color(QStringLiteral("--pdf-page-background"), t->color(QStringLiteral("--background-primary")));
}

// ---------------------------------------------------------------------------------------------
// scale
// ---------------------------------------------------------------------------------------------
double PdfPagesView::increasedScale(double scale, int steps)
{
    double s = scale;
    do {
        s = std::ceil(std::round(s * 1.1 * 100) / 100 * 10) / 10; // Math.ceil((s * 1.1).toFixed(2) * 10) / 10
    } while (--steps > 0 && s < kMaxScale);
    return std::min(kMaxScale, s);
}

double PdfPagesView::decreasedScale(double scale, int steps)
{
    double s = scale;
    do {
        s = std::floor(std::round(s / 1.1 * 100) / 100 * 10) / 10;
    } while (--steps > 0 && s > kMinScale);
    return std::max(kMinScale, s);
}

double PdfPagesView::fitScale(const QString& preset) const
{
    if (m_pages.isEmpty())
        return 1;
    const QSize avail = maximumViewportSize();
    const int ref = std::clamp(m_current, 0, int(m_pages.size()) - 1);
    const QSizeF pts = m_pages[ref].points;
    const double widthFactor = m_spread == SpreadMode::None ? 1 : 2;
    const double hPad = m_embedded ? 0 : 40; // PDF.js SCROLLBAR_PADDING
    const double vPad = m_embedded ? 0 : 5;  // PDF.js VERTICAL_PADDING
    auto widthScale = [&](double clientW) { return std::max(0.01, (clientW - hPad) / (pts.width() * kCssPerPoint * widthFactor)); };
    auto totalHeight = [&](double s) {
        double h = 0;
        for (const PageSlot& p : m_pages)
            h += p.points.height() * s * kCssPerPoint + (m_embedded ? kEmbedGap : 8 * s * kCssPerPoint);
        return m_spread == SpreadMode::None ? h : h / 2;
    };
    double pageWidthScale = widthScale(avail.width());
    if (totalHeight(pageWidthScale) > avail.height()) // a vertical scrollbar will take its width
        pageWidthScale = widthScale(avail.width() - kScrollbar);
    const double pageHeightScale = std::max(0.01, (avail.height() - vPad) / (pts.height() * kCssPerPoint));
    if (preset == u"page-height")
        return pageHeightScale;
    if (preset == u"page-fit")
        return std::min(pageWidthScale, pageHeightScale);
    if (preset == u"auto") {
        const bool portrait = pts.width() <= pts.height();
        return std::min(kMaxAutoScale, portrait ? pageWidthScale : std::min(pageHeightScale, pageWidthScale));
    }
    return pageWidthScale;
}

void PdfPagesView::setScaleValue(const QString& value)
{
    if (isPreset(value)) {
        applyScale(fitScale(value), value, std::nullopt);
        return;
    }
    bool ok = false;
    const double s = value.toDouble(&ok);
    if (ok && s > 0)
        applyScale(s, QString::number(std::clamp(s, kMinScale, kMaxScale)), std::nullopt);
}

void PdfPagesView::zoomIn(int steps, std::optional<QPoint> around)
{
    const double s = increasedScale(m_scale, steps);
    applyScale(s, QString::number(s), around);
}

void PdfPagesView::zoomOut(int steps, std::optional<QPoint> around)
{
    const double s = decreasedScale(m_scale, steps);
    applyScale(s, QString::number(s), around);
}

void PdfPagesView::applyScale(double scale, const QString& value, std::optional<QPoint> anchor)
{
    scale = std::clamp(scale, kMinScale, kMaxScale);
    // Keep the page point under the anchor (default: the viewport's top-left, like PDF.js).
    const QPoint a = anchor.value_or(QPoint(0, 0));
    const QPointF content = viewportToContent(a);
    const int page = m_pages.isEmpty() ? -1 : pageAt(content, true);
    const QPointF point = page >= 0 ? toPage(page, content) : QPointF();
    const bool changed = !qFuzzyCompare(scale, m_scale) || value != m_scaleValue;
    m_scale = scale;
    m_scaleValue = value;
    relayout();
    if (page >= 0) {
        const QPointF c = toContent(page, point);
        m_settingScroll = true;
        horizontalScrollBar()->setValue(int(std::lround(c.x() - a.x())));
        verticalScrollBar()->setValue(int(std::lround(c.y() - a.y())));
        m_settingScroll = false;
    }
    updateCurrentPage();
    viewport()->update();
    if (changed)
        emit scaleChanged();
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
void PdfPagesView::relayout()
{
    const double sf = scaleFactor();
    const double border = m_embedded ? 0 : 4 * sf; // --page-border: calc(var(--size-4-1) * var(--scale-factor))
    const double spreadGap = m_embedded ? 10 : std::max(2.0, 2 * border - 7);
    QList<QList<int>> rows;
    for (int i = 0; i < m_pages.size();) {
        int n = 1;
        if (m_spread == SpreadMode::Odd)
            n = 2;
        else if (m_spread == SpreadMode::Even)
            n = i == 0 ? 1 : 2;
        QList<int> row;
        for (int k = 0; k < n && i < m_pages.size(); ++k)
            row.append(i++);
        rows.append(row);
    }
    // A row: its pages side by side `spreadGap` apart, inside the outer page borders.
    auto rowWidth = [&](const QList<int>& row) {
        double w = 2 * border + (row.size() - 1) * spreadGap;
        for (int p : row)
            w += m_pages[p].points.width() * sf;
        return w;
    };
    double widest = 0;
    for (const QList<int>& row : rows)
        widest = std::max(widest, rowWidth(row));
    const double viewW = viewport()->width();
    m_contentWidth = std::max(viewW, widest);
    double y = 0;
    for (qsizetype r = 0; r < rows.size(); ++r) {
        const QList<int>& row = rows[r];
        double rowH = 0;
        for (int p : row)
            rowH = std::max(rowH, m_pages[p].points.height() * sf);
        double x = (m_contentWidth - rowWidth(row)) / 2 + border;
        for (int p : row) {
            const QSizeF size = m_pages[p].points * sf;
            // pages of a spread are vertically centred in their row
            m_pages[p].rect = QRectF(QPointF(x, y + border + (rowH - size.height()) / 2), size);
            x += size.width() + spreadGap;
        }
        y += rowH + 2 * border;
        if (m_embedded)
            y += r + 1 < rows.size() ? kEmbedGap : kEmbedTailMargin;
    }
    m_contentHeight = y;
    updateScrollBars();
    emit layoutChanged();
}

void PdfPagesView::updateScrollBars()
{
    const QSize vp = viewport()->size();
    m_settingScroll = true;
    horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(m_contentWidth - vp.width()))));
    horizontalScrollBar()->setPageStep(vp.width());
    horizontalScrollBar()->setSingleStep(int(kScrollStep));
    verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(m_contentHeight - vp.height()))));
    verticalScrollBar()->setPageStep(vp.height());
    verticalScrollBar()->setSingleStep(int(kScrollStep));
    m_settingScroll = false;
}

QPointF PdfPagesView::scrollOffset() const { return QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()); }

QPointF PdfPagesView::viewportToContent(const QPoint& vp) const { return QPointF(vp) + scrollOffset(); }

QRectF PdfPagesView::pageRect(int page) const { return page >= 0 && page < m_pages.size() ? m_pages[page].rect : QRectF(); }

int PdfPagesView::pageAt(const QPointF& c, bool nearest) const
{
    int best = -1;
    double bestDist = 1e18;
    for (int i = 0; i < m_pages.size(); ++i) {
        const QRectF& r = m_pages[i].rect;
        if (r.contains(c))
            return i;
        if (!nearest)
            continue;
        const double dx = c.x() < r.left() ? r.left() - c.x() : c.x() > r.right() ? c.x() - r.right() : 0;
        const double dy = c.y() < r.top() ? r.top() - c.y() : c.y() > r.bottom() ? c.y() - r.bottom() : 0;
        const double d = dy * 4 + dx; // prefer the page of the same row
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
        if (r.top() > c.y() + r.height() * 2 && best >= 0)
            break;
    }
    return best;
}

QPointF PdfPagesView::toPage(int page, const QPointF& c) const
{
    const QRectF& r = m_pages[page].rect;
    return (c - r.topLeft()) / scaleFactor();
}

QPointF PdfPagesView::toContent(int page, const QPointF& point) const
{
    const QRectF& r = m_pages[page].rect;
    return r.topLeft() + point * scaleFactor();
}

PdfPagesView::Location PdfPagesView::location() const
{
    if (m_pages.isEmpty())
        return {};
    const QPointF c = viewportToContent(QPoint(0, 0));
    const int page = std::clamp(m_current, 0, int(m_pages.size()) - 1);
    return Location{page, toPage(page, c)};
}

void PdfPagesView::goToPage(int page)
{
    if (m_pages.isEmpty())
        return;
    page = std::clamp(page, 0, int(m_pages.size()) - 1);
    const double border = m_embedded ? 0 : 4 * scaleFactor();
    m_settingScroll = true;
    verticalScrollBar()->setValue(int(std::lround(m_pages[page].rect.top() - border)));
    m_settingScroll = false;
    m_current = page; // the requested page is current even if the last pages can't reach the top
    emit currentPageChanged(m_current);
    viewport()->update();
}

void PdfPagesView::goToLocation(int page, QPointF point)
{
    if (m_pages.isEmpty())
        return;
    page = std::clamp(page, 0, int(m_pages.size()) - 1);
    const QPointF c = toContent(page, QPointF(std::max(0.0, point.x()), std::max(0.0, point.y())));
    m_settingScroll = true;
    if (point.x() > 0)
        horizontalScrollBar()->setValue(int(std::lround(c.x())));
    verticalScrollBar()->setValue(int(std::lround(point.y() >= 0 ? c.y() : m_pages[page].rect.top())));
    m_settingScroll = false;
    m_current = page;
    emit currentPageChanged(m_current);
    viewport()->update();
}

void PdfPagesView::updateCurrentPage()
{
    if (m_pages.isEmpty())
        return;
    // PDF.js: keep the current page while it is fully visible, else the most visible page.
    const QRectF view(scrollOffset(), QSizeF(viewport()->size()));
    int best = -1;
    double bestFrac = 0;
    bool currentFull = false;
    for (int i = 0; i < m_pages.size(); ++i) {
        const QRectF& r = m_pages[i].rect;
        if (r.bottom() < view.top())
            continue;
        if (r.top() > view.bottom())
            break;
        const QRectF vis = r.intersected(view);
        const double frac = r.height() > 0 ? vis.height() / r.height() : 0;
        if (i == m_current && frac >= 0.999)
            currentFull = true;
        if (frac > bestFrac + 1e-6) {
            bestFrac = frac;
            best = i;
        }
    }
    const int next = currentFull ? m_current : (best >= 0 ? best : m_current);
    if (next != m_current) {
        m_current = next;
        emit currentPageChanged(m_current);
    }
}

void PdfPagesView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    if (isPreset(m_scaleValue))
        applyScale(fitScale(m_scaleValue), m_scaleValue, std::nullopt);
    else
        applyScale(m_scale, m_scaleValue, std::nullopt);
}

void PdfPagesView::scrollContentsBy(int, int)
{
    if (m_settingScroll)
        return;
    updateCurrentPage();
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------------------------
void PdfPagesView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const ThemePtr t = currentTheme();
    p.fillRect(viewport()->rect(), t->color(QStringLiteral("--pdf-background"), t->color(QStringLiteral("--background-primary"))));
    if (m_pages.isEmpty() || !m_file)
        return;
    const QPointF off = scrollOffset();
    const QRectF view(QPointF(0, 0), QSizeF(viewport()->size()));
    const double dpr = devicePixelRatioF();
    const bool themed = themedNow();
    const QRgb bg = pageBackground().rgb();
    for (int i = 0; i < m_pages.size(); ++i) {
        const QRectF r = m_pages[i].rect.translated(-off);
        if (r.bottom() < -view.height())
            continue;
        if (r.top() > view.bottom() + view.height())
            break;
        const int px = int(std::lround(r.width() * dpr));
        if (r.intersects(view)) {
            paintPage(p, i, r);
            if (m_file->cachedImage(i, px, themed, bg).isNull())
                m_file->requestImage(i, px, themed, bg);
        } else if (m_file->cachedImage(i, px, themed, bg).isNull()) {
            m_file->requestImage(i, px, themed, bg); // the neighbouring pages, ahead of scrolling
        }
    }
    // Visible pages first: they were requested last, so they render first (newest first).
    for (int i = 0; i < m_pages.size(); ++i) {
        const QRectF r = m_pages[i].rect.translated(-off);
        if (r.intersects(view)) {
            const int px = int(std::lround(r.width() * dpr));
            if (m_file->cachedImage(i, px, themed, bg).isNull())
                m_file->requestImage(i, px, themed, bg);
        }
    }
}

void PdfPagesView::paintPage(QPainter& p, int page, const QRectF& r)
{
    const ThemePtr t = currentTheme();
    const bool themed = themedNow();
    const QColor bg = pageBackground();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // --pdf-shadow: dark = a 1px border-coloured ring; light = a faint ring + a soft drop shadow
    if (t->isDark()) {
        p.fillRect(grow(r, 1), t->color(QStringLiteral("--background-modifier-border")));
    } else {
        for (int k = 1; k <= 4; ++k) {
            QColor c(0, 0, 0, 7 - k);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(grow(r.translated(0, 2), 1 + k), 2 + k, 2 + k);
        }
        p.fillRect(grow(r, 1), QColor(0, 0, 0, 13));
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(r, themed ? bg : QColor(Qt::white));
    const double dpr = p.device()->devicePixelRatioF();
    const int px = int(std::lround(r.width() * dpr));
    QImage img = m_file->cachedImage(page, px, themed, bg.rgb());
    if (img.isNull())
        img = m_file->anyImage(page, themed);
    if (!img.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, img);
    }
    p.setRenderHint(QPainter::Antialiasing);
    const double sf = scaleFactor();
    auto toView = [&](const QRectF& pts) { return QRectF(r.topLeft() + pts.topLeft() * sf, pts.size() * sf); };
    const double radius = t->px(QStringLiteral("--radius-s"), -1, 4);
    // hovered link: color-mix(accent 10%)
    if (m_hoverLink && m_hoverLinkPage == page) {
        QColor c = t->color(QStringLiteral("--color-accent"));
        c.setAlphaF(0.1);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(toView(m_hoverLink->rect));
    }
    // search highlights (inside the text layer at opacity .2): 2px spread, radius-s
    for (int i = 0; i < m_highlights.size(); ++i) {
        const Highlight& h = m_highlights[i];
        const bool current = i == m_currentHighlight;
        if (h.page != page || (!current && !m_highlightAll))
            continue;
        QColor c = current ? t->color(QStringLiteral("--color-accent")) : t->color(QStringLiteral("--color-yellow"));
        c.setAlphaF(0.2 * c.alphaF());
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (const QRectF& rr : h.rects)
            p.drawRoundedRect(grow(toView(rr), 2), radius, radius);
    }
    // text selection: --color-accent in the .2-opacity text layer
    QColor sel = t->color(QStringLiteral("--color-accent"));
    sel.setAlphaF(0.2);
    p.setPen(Qt::NoPen);
    p.setBrush(sel);
    if (m_selectAll) {
        for (const QRectF& box : m_file->pageText(page).boxes)
            p.drawRect(toView(box));
    } else {
        for (const PageSelection& s : m_selection)
            if (s.page == page)
                for (const QRectF& rr : s.rects)
                    p.drawRect(toView(rr));
    }
    p.restore();
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------
void PdfPagesView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        m_wheelAccum += e->angleDelta().y();
        const int steps = int(m_wheelAccum / 120);
        if (steps != 0) {
            m_wheelAccum -= steps * 120;
            const QPoint at = e->position().toPoint();
            if (steps > 0)
                zoomIn(steps, at);
            else
                zoomOut(-steps, at);
        }
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void PdfPagesView::keyPressEvent(QKeyEvent* e)
{
    if (e->matches(QKeySequence::Copy)) {
        copySelection();
        return;
    }
    if (e->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    QScrollBar* v = verticalScrollBar();
    QScrollBar* h = horizontalScrollBar();
    const bool fit = m_scaleValue == u"page-width" || m_scaleValue == u"page-height";
    switch (e->key()) {
    case Qt::Key_Down:
        v->setValue(v->value() + int(kScrollStep));
        return;
    case Qt::Key_Up:
        v->setValue(v->value() - int(kScrollStep));
        return;
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        if (e->key() == Qt::Key_Space && (e->modifiers() & Qt::ShiftModifier))
            v->setValue(v->value() - v->pageStep() + int(kScrollStep));
        else
            v->setValue(v->value() + v->pageStep() - int(kScrollStep));
        return;
    case Qt::Key_PageUp:
        v->setValue(v->value() - v->pageStep() + int(kScrollStep));
        return;
    case Qt::Key_Home:
        v->setValue(0);
        return;
    case Qt::Key_End:
        v->setValue(v->maximum());
        return;
    case Qt::Key_Left:
        if (m_arrowPaging && fit)
            goToPage(m_current - 1);
        else
            h->setValue(h->value() - int(kScrollStep));
        return;
    case Qt::Key_Right:
        if (m_arrowPaging && fit)
            goToPage(m_current + 1);
        else
            h->setValue(h->value() + int(kScrollStep));
        return;
    case Qt::Key_Escape:
        if (hasSelection()) {
            clearSelection();
            return;
        }
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

const PdfFile::Link* PdfPagesView::linkAt(const QPointF& c, int* pageOut) const
{
    if (!m_file)
        return nullptr;
    const int page = pageAt(c, false);
    if (page < 0)
        return nullptr;
    const QPointF pt = toPage(page, c);
    for (const PdfFile::Link& l : m_file->links(page)) {
        if (l.rect.contains(pt)) {
            if (pageOut)
                *pageOut = page;
            return &l;
        }
    }
    return nullptr;
}

void PdfPagesView::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (e->button() != Qt::LeftButton || m_pages.isEmpty()) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    const QPointF c = viewportToContent(e->position().toPoint());
    m_pressPos = e->position().toPoint();
    m_pressLink = linkAt(c, &m_pressLinkPage);
    if (m_pressLink)
        return;
    const bool hadSelection = hasSelection();
    m_selection.clear();
    m_selectAll = false;
    m_selecting = true;
    m_anchorPage = pageAt(c, true);
    m_anchorPoint = toPage(m_anchorPage, c);
    m_headPage = -1;
    if (hadSelection)
        emit selectionChanged();
    viewport()->update();
}

void PdfPagesView::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint vp = e->position().toPoint();
    const QPointF c = viewportToContent(vp);
    if (m_selecting && (e->buttons() & Qt::LeftButton)) {
        if ((vp - m_pressPos).manhattanLength() < 3 && m_headPage < 0)
            return;
        // drag beyond the edges: scroll
        if (vp.y() < 0)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + vp.y() / 2 - 4);
        else if (vp.y() > viewport()->height())
            verticalScrollBar()->setValue(verticalScrollBar()->value() + (vp.y() - viewport()->height()) / 2 + 4);
        const QPointF cc = viewportToContent(vp);
        m_headPage = pageAt(cc, true);
        m_headPoint = toPage(m_headPage, cc);
        m_selectionDirty = true;
        if (!m_selectionTimer.isActive())
            m_selectionTimer.start(); // coalesce: each update queries pdfium
        return;
    }
    int page = -1;
    const PdfFile::Link* l = e->buttons() == Qt::NoButton ? linkAt(c, &page) : nullptr;
    if (l != m_hoverLink) {
        m_hoverLink = l;
        m_hoverLinkPage = page;
        viewport()->update();
    }
    if (l)
        viewport()->setCursor(Qt::PointingHandCursor);
    else if (pageAt(c, false) >= 0)
        viewport()->setCursor(Qt::IBeamCursor);
    else
        viewport()->setCursor(Qt::ArrowCursor);
}

void PdfPagesView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    if (m_pressLink) {
        const PdfFile::Link link = *m_pressLink;
        m_pressLink = nullptr;
        if ((e->position().toPoint() - m_pressPos).manhattanLength() < 4) {
            if (link.url.isValid()) {
                if (openExternal)
                    openExternal(link.url);
                else
                    QDesktopServices::openUrl(link.url);
            } else if (link.page >= 0) {
                goToLocation(link.page, link.location);
            }
        }
        return;
    }
    if (m_selecting) {
        m_selecting = false;
        if (m_selectionDirty)
            updateSelection();
        if (hasSelection()) {
            if (QClipboard* cb = QGuiApplication::clipboard(); cb && cb->supportsSelection())
                cb->setText(selectedText(), QClipboard::Selection);
        }
    }
}

void PdfPagesView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_pages.isEmpty())
        return;
    const QPointF c = viewportToContent(e->position().toPoint());
    const int page = pageAt(c, false);
    if (page < 0)
        return;
    m_selecting = false;
    selectWordAt(page, toPage(page, c));
}

void PdfPagesView::contextMenuEvent(QContextMenuEvent* e) { emit contextMenuWanted(e->globalPos()); }

bool PdfPagesView::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && m_hoverLink) {
        m_hoverLink = nullptr;
        viewport()->update();
    }
    return QAbstractScrollArea::viewportEvent(e);
}

// ---------------------------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------------------------
std::optional<QPointF> PdfPagesView::snapToText(int page, const QPointF& pt) const
{
    // A point on (or nearest to) a glyph: QtPdf only selects from/to points that hit text.
    const QList<QRectF>& boxes = m_file->pageText(page).boxes;
    if (boxes.isEmpty())
        return std::nullopt;
    const QRectF* best = nullptr;
    double bestD = 1e18;
    for (const QRectF& b : boxes) {
        if (b.contains(pt))
            return pt;
        const bool sameLine = pt.y() >= b.top() && pt.y() <= b.bottom();
        const double dx = pt.x() < b.left() ? b.left() - pt.x() : pt.x() > b.right() ? pt.x() - b.right() : 0;
        const double dy = pt.y() < b.top() ? b.top() - pt.y() : pt.y() > b.bottom() ? pt.y() - b.bottom() : 0;
        const double d = sameLine ? dx : 1e6 + dy * 1000 + dx;
        if (d < bestD) {
            bestD = d;
            best = &b;
        }
    }
    const double inset = std::min(0.5, best->width() / 4);
    return QPointF(std::clamp(pt.x(), best->left() + inset, best->right() - inset), best->center().y());
}

int PdfPagesView::glyphIndexAt(int page, const QPointF& pt) const
{
    const PdfFile::PageText& text = m_file->pageText(page);
    const auto snapped = snapToText(page, pt);
    if (!snapped || text.boxes.isEmpty())
        return -1;
    QPdfDocument* doc = m_file->document();
    const QPdfSelection toEnd = doc->getSelection(page, *snapped, text.boxes.last().center());
    if (!toEnd.text().isEmpty())
        return toEnd.startIndex();
    const QPdfSelection fromStart = doc->getSelection(page, text.boxes.first().center(), *snapped);
    if (!fromStart.text().isEmpty())
        return fromStart.endIndex();
    return 0;
}

PdfPagesView::PageSelection PdfPagesView::selectionFromIndex(int page, int from, int to) const
{
    PageSelection out;
    out.page = page;
    const PdfFile::PageText& text = m_file->pageText(page);
    from = std::clamp(from, 0, int(text.text.size()));
    to = std::clamp(to, from, int(text.text.size()));
    if (from >= to)
        return out;
    if (from == 0 && to == text.text.size()) {
        out.text = text.text;
        out.rects = text.boxes;
        return out;
    }
    const QPdfSelection s = m_file->document()->getSelectionAtIndex(page, from, to - from);
    out.text = s.text();
    for (const QPolygonF& poly : s.bounds())
        out.rects.append(poly.boundingRect());
    return out;
}

PdfPagesView::PageSelection PdfPagesView::selectionBetween(int page, const QPointF& a, const QPointF& b) const
{
    PageSelection out;
    out.page = page;
    const auto sa = snapToText(page, a), sb = snapToText(page, b);
    if (!sa || !sb)
        return out;
    const QPdfSelection s = m_file->document()->getSelection(page, *sa, *sb);
    if (s.text().isEmpty() && *sa == *sb)
        return out;
    // QtPdf leaves out the glyph under the later point: take it when the pointer went past it.
    const bool bLater = b.y() > a.y() + 2 || (std::abs(b.y() - a.y()) <= 2 && b.x() >= a.x());
    const QPointF later = bLater ? b : a, laterSnap = bLater ? *sb : *sa;
    int from = s.text().isEmpty() ? -1 : s.startIndex();
    int to = s.text().isEmpty() ? -1 : s.endIndex();
    if (later.x() > laterSnap.x() + 0.5 || s.text().isEmpty()) {
        if (from < 0) // both points on the same glyph
            from = to = glyphIndexAt(page, laterSnap);
        if (from >= 0)
            return selectionFromIndex(page, from, to + 1);
        return out;
    }
    out.text = s.text();
    for (const QPolygonF& poly : s.bounds())
        out.rects.append(poly.boundingRect());
    return out;
}

void PdfPagesView::updateSelection()
{
    m_selectionDirty = false;
    if (!m_file || m_anchorPage < 0 || m_headPage < 0)
        return;
    QList<PageSelection> sel;
    if (m_anchorPage == m_headPage) {
        sel.append(selectionBetween(m_anchorPage, m_anchorPoint, m_headPoint));
    } else {
        const bool forward = m_headPage > m_anchorPage;
        const int first = forward ? m_anchorPage : m_headPage;
        const int last = forward ? m_headPage : m_anchorPage;
        const QPointF firstPt = forward ? m_anchorPoint : m_headPoint;
        const QPointF lastPt = forward ? m_headPoint : m_anchorPoint;
        // first page: from the point to the end of its text
        const int from = glyphIndexAt(first, firstPt);
        if (from >= 0)
            sel.append(selectionFromIndex(first, from, int(m_file->pageText(first).text.size())));
        for (int p = first + 1; p < last; ++p)
            sel.append(selectionFromIndex(p, 0, int(m_file->pageText(p).text.size())));
        // last page: from the start of its text to the point
        const PdfFile::PageText& lt = m_file->pageText(last);
        if (!lt.boxes.isEmpty())
            sel.append(selectionBetween(last, lt.boxes.first().topLeft() + QPointF(0.1, lt.boxes.first().height() / 2), lastPt));
    }
    sel.removeIf([](const PageSelection& s) { return s.text.isEmpty(); });
    m_selection = sel;
    m_selectAll = false;
    viewport()->update();
    emit selectionChanged();
}

void PdfPagesView::selectWordAt(int page, const QPointF& pt)
{
    const int i = glyphIndexAt(page, pt);
    const QString& text = m_file->pageText(page).text;
    if (i < 0 || i >= text.size())
        return;
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == u'_' || c.category() == QChar::Mark_NonSpacing; };
    int a = i, b = i + 1;
    if (isWord(text[i])) {
        while (a > 0 && isWord(text[a - 1]))
            --a;
        while (b < text.size() && isWord(text[b]))
            ++b;
    }
    m_selection = {selectionFromIndex(page, a, b)};
    m_selection.removeIf([](const PageSelection& s) { return s.text.isEmpty(); });
    m_selectAll = false;
    viewport()->update();
    emit selectionChanged();
}

void PdfPagesView::selectText(int page, int from, int to)
{
    if (!m_file || page < 0 || page >= pageCount())
        return;
    m_selection = {selectionFromIndex(page, from, to)};
    m_selection.removeIf([](const PageSelection& s) { return s.text.isEmpty(); });
    m_selectAll = false;
    viewport()->update();
    emit selectionChanged();
}

QString PdfPagesView::selectedText() const
{
    QStringList parts;
    if (m_selectAll && m_file) {
        for (int p = 0; p < pageCount(); ++p)
            parts << m_file->pageText(p).text;
    } else {
        for (const PageSelection& s : m_selection)
            parts << s.text;
    }
    QString out = parts.join(QLatin1Char('\n'));
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}

void PdfPagesView::copySelection() const
{
    if (!hasSelection())
        return;
    if (QClipboard* cb = QGuiApplication::clipboard())
        cb->setText(selectedText());
}

void PdfPagesView::selectAll()
{
    if (!m_file)
        return;
    m_selection.clear();
    m_selectAll = true;
    viewport()->update();
    emit selectionChanged();
}

void PdfPagesView::clearSelection()
{
    if (!hasSelection())
        return;
    m_selection.clear();
    m_selectAll = false;
    viewport()->update();
    emit selectionChanged();
}

// ---------------------------------------------------------------------------------------------
// search highlights
// ---------------------------------------------------------------------------------------------
void PdfPagesView::setHighlights(QList<Highlight> matches, int current, bool all)
{
    m_highlights = std::move(matches);
    m_currentHighlight = current;
    m_highlightAll = all;
    viewport()->update();
}

void PdfPagesView::revealHighlight(int index)
{
    if (index < 0 || index >= m_highlights.size())
        return;
    const Highlight& h = m_highlights[index];
    if (h.page < 0 || h.page >= m_pages.size() || h.rects.isEmpty())
        return;
    QRectF bounds;
    for (const QRectF& r : h.rects)
        bounds |= r;
    const QRectF c(toContent(h.page, bounds.topLeft()), bounds.size() * scaleFactor());
    const QRectF view(scrollOffset(), QSizeF(viewport()->size()));
    if (!view.contains(c)) {
        // PDF.js scrolls the match to about a third from the top (and into view horizontally)
        m_settingScroll = true;
        verticalScrollBar()->setValue(int(c.top() - viewport()->height() / 3.0));
        if (c.left() < view.left() || c.right() > view.right())
            horizontalScrollBar()->setValue(int(c.center().x() - viewport()->width() / 2.0));
        m_settingScroll = false;
        updateCurrentPage();
    }
    viewport()->update();
}

} // namespace oc::media
