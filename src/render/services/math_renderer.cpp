#include "render/services/math_renderer.h"

#include <QFontMetricsF>
#include <QPainter>

#ifdef OC_HAVE_JKQTMATHTEXT
#include <jkqtmathtext/jkqtmathtext.h>
#endif

#include <cmath>

namespace oc {

MathRenderer& MathRenderer::instance()
{
    static MathRenderer r;
    return r;
}

MathRenderer::MathRenderer() = default;

bool MathRenderer::available() const
{
#ifdef OC_HAVE_JKQTMATHTEXT
    return true;
#else
    return false;
#endif
}

void MathRenderer::clear() { m_cache.clear(); }

MathRenderer::ResultPtr MathRenderer::render(const QString& tex, bool display, qreal fontPx, const QColor& color, qreal dpr)
{
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(display).arg(fontPx).arg(color.rgba()).arg(dpr).arg(tex);
    if (auto it = m_cache.constFind(key); it != m_cache.cend())
        return *it;
    if (m_cache.size() > 2000)
        m_cache.clear();

    auto res = std::make_shared<Result>();
#ifdef OC_HAVE_JKQTMATHTEXT
    static JKQTMathText mt; // parsing/layout is single-threaded (GUI thread)
    static bool init = false;
    if (!init) {
        mt.useXITS();
        init = true;
    }
    mt.setFontSizePixels(fontPx);
    mt.setFontColor(color);
    const QString src = display ? QStringLiteral("$\\displaystyle ") + tex + QStringLiteral("$") : QStringLiteral("$") + tex + QStringLiteral("$");
    const bool ok = mt.parse(src) && mt.getErrorList().isEmpty();
    if (ok) {
        QImage probe(1, 1, QImage::Format_ARGB32_Premultiplied);
        QPainter pp(&probe);
        double w = 0, a = 0, d = 0, so = 0;
        mt.getSizeDetail(pp, w, a, d, so);
        pp.end();
        const QSizeF size(std::ceil(w) + 2, std::ceil(a + d) + 2);
        QImage img(QSize(std::max(1, int(std::ceil(size.width() * dpr))), std::max(1, int(std::ceil(size.height() * dpr)))),
                   QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        mt.draw(p, QPointF(1, 1 + a));
        p.end();
        res->image = img;
        res->size = size;
        res->ascent = 1 + a;
    } else {
        res->error = true;
        res->errorText = mt.getErrorList().join(QStringLiteral("; "));
    }
#else
    res->error = true;
    res->errorText = QStringLiteral("math rendering not available");
#endif
    if (res->error) {
        // Raw TeX in the error colour (monospace).
        QFont f(QStringLiteral("monospace"));
        f.setPixelSize(int(std::lround(fontPx * 0.875)));
        const QFontMetricsF fm(f);
        const QString text = display ? tex.trimmed() : tex;
        const QSizeF size(std::ceil(fm.horizontalAdvance(text)) + 2, std::ceil(fm.height()));
        QImage img(QSize(std::max(1, int(size.width() * dpr)), std::max(1, int(size.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setFont(f);
        p.setPen(QColor(0xfb, 0x46, 0x4c));
        p.drawText(QPointF(1, fm.ascent()), text);
        p.end();
        res->image = img;
        res->size = size;
        res->ascent = fm.ascent();
    }
    m_cache.insert(key, res);
    return res;
}

} // namespace oc
