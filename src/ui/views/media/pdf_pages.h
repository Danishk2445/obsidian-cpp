#pragma once

// The scrolling page area of the PDF viewer (Obsidian's PDF.js .pdf-viewer-container): pages stacked
// vertically (or in two-page spreads), centred, rendered asynchronously at the current zoom, with
// PDF.js's scale semantics (1 = 100% = 96/72 px per point; "page-width" / "page-height" / "page-fit"
// / "auto" presets; zoom steps of ×1.1 rounded to tenths, 0.1…10), text selection and copy, clickable
// links and search highlights. Coordinates: "content" = CSS px at the current scale, origin at the
// top-left of the scrollable area; page points = PDF points from the page's top-left.

#include "ui/views/media/pdf_file.h"

#include <QAbstractScrollArea>
#include <QPolygonF>
#include <QTimer>

#include <memory>
#include <optional>

namespace oc::media {

class PdfPagesView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class SpreadMode { None = 0, Odd = 1, Even = 2 };
    static constexpr double kMinScale = 0.1;
    static constexpr double kMaxScale = 10.0;
    static constexpr double kCssPerPoint = 96.0 / 72.0;

    explicit PdfPagesView(QWidget* parent = nullptr);
    ~PdfPagesView() override;

    void setFile(std::shared_ptr<PdfFile> file); // keeps page and zoom when the same file reloads
    PdfFile* file() const { return m_file.get(); }
    // PDF.js removePageBorders (embeds): pages span the width, 14 px apart, no side borders.
    void setEmbedded(bool embedded);
    bool isEmbedded() const { return m_embedded; }

    // "page-width" | "page-height" | "page-fit" | "auto" | a scale ("1.5"). Keeps the location.
    void setScaleValue(const QString& value);
    QString scaleValue() const { return m_scaleValue; }
    double scale() const { return m_scale; }
    double scaleFactor() const { return m_scale * kCssPerPoint; } // CSS px per PDF point
    void zoomIn(int steps = 1, std::optional<QPoint> around = std::nullopt);
    void zoomOut(int steps = 1, std::optional<QPoint> around = std::nullopt);
    bool canZoomIn() const { return m_scale < kMaxScale; }
    bool canZoomOut() const { return m_scale > kMinScale; }
    static double increasedScale(double scale, int steps = 1); // PDF.js increaseScale()
    static double decreasedScale(double scale, int steps = 1); // PDF.js decreaseScale()

    void setSpreadMode(SpreadMode mode);
    SpreadMode spreadMode() const { return m_spread; }
    void setThemed(bool themed); // "Adapt to theme" (applies in dark mode)
    bool isThemed() const { return m_themed; }
    // Arrow left / right turn pages while a fit-to preset is active (Obsidian's PDF view only).
    void setArrowPaging(bool on) { m_arrowPaging = on; }

    int pageCount() const { return int(m_pages.size()); }
    int currentPage() const { return m_current; } // 0-based
    void goToPage(int page);                       // page top at the viewport top
    // PDF.js destinations: point in page points shown at the viewport's top-left (x or y < 0: keep).
    void goToLocation(int page, QPointF point);
    QRectF pageRect(int page) const; // content coordinates (excluding the page border)
    QPointF scrollOffset() const;    // content coordinates of the viewport's top-left

    // Text selection (Ctrl+C copies, Ctrl+A selects everything).
    bool hasSelection() const { return m_selectAll || !m_selection.isEmpty(); }
    QString selectedText() const;
    void copySelection() const;
    void selectAll();
    void clearSelection();
    // Selects [from, to) of the page's text (tests, "#selection" subpaths).
    void selectText(int page, int from, int to);

    // Search highlights (page points). `current` is shown in the accent colour; the others only
    // when `all` is set (Obsidian's "Highlight all").
    struct Highlight {
        int page = 0;
        QList<QRectF> rects;
    };
    void setHighlights(QList<Highlight> matches, int current, bool all);
    void revealHighlight(int index); // scroll it into view (centred when off-screen)

    // Page links are handled here; external URLs go through this hook (default: the desktop).
    std::function<void(const QUrl&)> openExternal;

    // Current location like PDF.js's (page, top-left point) for tests.
    struct Location {
        int page = 0;
        QPointF point;
    };
    Location location() const;

signals:
    void currentPageChanged(int page);
    void scaleChanged();
    void selectionChanged();
    void layoutChanged();
    void contextMenuWanted(const QPoint& globalPos); // the viewer builds the menu

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool viewportEvent(QEvent* e) override;

private:
    struct PageSlot {
        QSizeF points;
        QRectF rect; // content coordinates
    };
    struct PageSelection {
        int page = 0;
        QList<QRectF> rects; // page points
        QString text;
    };
    double fitScale(const QString& preset) const;
    void applyScale(double scale, const QString& value, std::optional<QPoint> anchor);
    void relayout();
    void updateScrollBars();
    void updateCurrentPage();
    int pageAt(const QPointF& content, bool nearest) const;
    QPointF toPage(int page, const QPointF& content) const; // content -> page points
    QPointF toContent(int page, const QPointF& point) const;
    QPointF viewportToContent(const QPoint& vp) const;
    std::optional<QPointF> snapToText(int page, const QPointF& pt) const;
    const PdfFile::Link* linkAt(const QPointF& content, int* pageOut = nullptr) const;
    void updateSelection();
    int glyphIndexAt(int page, const QPointF& pt) const; // start of the glyph at pt (snapped), -1
    PageSelection selectionBetween(int page, const QPointF& a, const QPointF& b) const;
    PageSelection selectionFromIndex(int page, int from, int to) const;
    void selectWordAt(int page, const QPointF& pt);
    void paintPage(QPainter& p, int page, const QRectF& r);
    QColor pageBackground() const;
    bool themedNow() const;

    std::shared_ptr<PdfFile> m_file;
    QMetaObject::Connection m_renderedConn, m_reloadedConn;
    QList<PageSlot> m_pages;
    double m_contentWidth = 0;
    double m_contentHeight = 0;
    double m_scale = 1;
    QString m_scaleValue = QStringLiteral("page-width");
    SpreadMode m_spread = SpreadMode::None;
    bool m_embedded = false;
    bool m_themed = false;
    bool m_arrowPaging = false;
    int m_current = 0;
    bool m_settingScroll = false;

    // selection
    QList<PageSelection> m_selection;
    bool m_selectAll = false;
    bool m_selecting = false;
    int m_anchorPage = -1;
    QPointF m_anchorPoint; // page points
    int m_headPage = -1;
    QPointF m_headPoint;
    bool m_selectionDirty = false;
    QTimer m_selectionTimer;
    QPoint m_pressPos;
    int m_pressLinkPage = -1;
    const PdfFile::Link* m_pressLink = nullptr;
    const PdfFile::Link* m_hoverLink = nullptr;
    int m_hoverLinkPage = -1;
    double m_wheelAccum = 0;

    // search
    QList<Highlight> m_highlights;
    int m_currentHighlight = -1;
    bool m_highlightAll = false;
};

} // namespace oc::media
