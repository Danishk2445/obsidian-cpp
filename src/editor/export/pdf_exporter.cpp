#include "editor/export/pdf_exporter.h"

#include "editor/live_preview_decorator.h"
#include "markdown/doc_source.h"
#include "render/layout/doc_layout.h"
#include "render/theme/theme.h"

#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

#include <cmath>

namespace oc {

namespace {
QPageSize::PageSizeId pageSizeId(const QString& s)
{
    const QString l = s.toLower();
    if (l == u"a3")
        return QPageSize::A3;
    if (l == u"a4")
        return QPageSize::A4;
    if (l == u"a5")
        return QPageSize::A5;
    if (l == u"legal")
        return QPageSize::Legal;
    if (l == u"tabloid")
        return QPageSize::Tabloid;
    return QPageSize::Letter;
}

qreal marginMm(const QString& m)
{
    if (m == u"0")
        return 0;
    if (m == u"2")
        return 5; // small
    return 10;    // default (~1 cm)
}
} // namespace

bool exportPdf(const Text& body, const QString& title, const QString& outPath, const PdfExportSettings& settings, const PdfExportContext& ctx,
               QString* error)
{
    // Content: optional title as an H1, then the note.
    Text text = body;
    if (settings.includeName && !title.isEmpty())
        text = Text::fromString(QString(QStringLiteral("# ") + title + QStringLiteral("\n\n") + body.toString()));

    ThemeManager* tm = ThemeManager::instance();
    const ThemePtr theme = tm ? tm->themeFor(Theme::Scheme::Light) : currentTheme();
    const StyleSheetPtr styles = StyleSheet::build(*theme);

    QPdfWriter writer(outPath);
    writer.setCreator(QStringLiteral("Obsidian C++"));
    writer.setTitle(title);
    writer.setResolution(96); // 1 unit = 1 CSS px
    const qreal mm = marginMm(settings.margin);
    writer.setPageLayout(QPageLayout(QPageSize(pageSizeId(settings.pageSize)), settings.landscape ? QPageLayout::Landscape : QPageLayout::Portrait,
                                     QMarginsF(mm, mm, mm, mm), QPageLayout::Millimeter));
    const QRect page = writer.pageLayout().paintRectPixels(96);
    const qreal scale = std::clamp(settings.downscalePercent, 10, 100) / 100.0;
    // Obsidian's print view adds its own inner padding when margins are "none".
    const qreal innerPad = mm == 0 ? 24 : 0;
    const qreal contentW = (page.width() - 2 * innerPad) / scale;
    const qreal pageH = (page.height() - 2 * innerPad) / scale;

    md::StaticDocSource source(text);
    LivePreviewDecorator deco(&source);
    deco.setStyleSheet(styles.get());
    deco.setReading(true);
    deco.setContentWidth(contentW);
    deco.isResolved = ctx.isResolved;
    deco.resolveAttachment = ctx.resolveAttachment;
    DocLayout layout;
    layout.setDecorator(&deco);
    layout.setStyleSheet(styles);
    layout.setContentWidth(contentW);
    layout.setText(source.text());
    layout.ensureLaidOut(0, 1e12);

    // Page breaks at visual-line / unit boundaries.
    QList<qreal> breaks{0};
    const double total = layout.totalHeight();
    qreal pageStart = 0;
    for (int line = 0; line < layout.lineCount();) {
        const double top = layout.lineTop(line), h = layout.lineHeight(line);
        // next unit's first line
        int next = line + 1;
        while (next < layout.lineCount() && layout.lineTop(next) == top)
            ++next;
        if (top + h - pageStart > pageH && top > pageStart) {
            // Split inside a multi-line text unit when possible.
            const DocLayout::UnitLayout* ul = layout.layoutFor(line);
            qreal cut = top;
            if (ul) {
                for (const DocLayout::VisualLine& vl : ul->lines) {
                    if (top + vl.y + vl.height - pageStart <= pageH)
                        cut = top + vl.y + vl.height;
                }
            }
            if (cut <= pageStart)
                cut = top;
            pageStart = cut;
            breaks.push_back(pageStart);
            // very tall blocks: slice by pages
            while (top + h - pageStart > pageH) {
                pageStart += pageH;
                breaks.push_back(pageStart);
            }
        }
        line = next;
    }

    QPainter p;
    if (!p.begin(&writer)) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(outPath);
        return false;
    }
    for (qsizetype pi = 0; pi < breaks.size(); ++pi) {
        const qreal y0 = breaks[pi];
        const qreal y1 = pi + 1 < breaks.size() ? breaks[pi + 1] : total;
        if (pi > 0)
            writer.newPage();
        p.save();
        p.translate(innerPad, innerPad);
        p.scale(scale, scale);
        p.setClipRect(QRectF(-2, 0, contentW + 4, y1 - y0));
        p.translate(0, -y0);
        DocLayout::PaintContext pc;
        pc.drawSelection = false;
        pc.clip = QRectF(0, y0, contentW, y1 - y0);
        pc.rightEdge = contentW;
        layout.paint(&p, pc);
        p.restore();
    }
    p.end();
    return true;
}

} // namespace oc
