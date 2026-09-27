#include "render/widgets/inline_widgets.h"

#include "render/services/image_cache.h"
#include "render/services/math_renderer.h"
#include "render/theme/theme.h"

#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace oc {

QPointF BulletWidget::dotCenter(const QRectF& r, const LineMetrics& lm) const
{
    // r.top() + ascent(lm) is the baseline; the line box's centre is (ascent - descent) / 2 above it
    return QPointF(r.left() + m_center, r.top() + lm.ascent - (lm.ascent - lm.descent) / 2);
}

void BulletWidget::paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    p->setBrush(m_color);
    p->drawEllipse(dotCenter(r, lm), m_dot / 2, m_dot / 2);
    p->restore();
}

quint64 BulletWidget::signature() const { return qHashMulti(1, m_color.rgba(), m_width, m_center, m_dot); }

QSizeF CheckboxWidget::size(const LineMetrics& lm) const
{
    // tall enough for the box wherever it sits (it never grows a text line: raise keeps it inside the line box)
    return {m_lead + m_size + m_margin, std::max(ascent(lm) + lm.descent, ascent(lm) - m_raise + m_size / 2)};
}

qreal CheckboxWidget::ascent(const LineMetrics& lm) const { return std::max(lm.ascent, m_raise + m_size / 2); }

QRectF CheckboxWidget::boxRect(const QRectF& r, const LineMetrics& lm) const
{
    const qreal centerY = r.top() + ascent(lm) - m_raise;
    return QRectF(r.left() + m_lead, centerY - m_size / 2, m_size, m_size);
}

void CheckboxWidget::paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const
{
    const ThemePtr t = currentTheme();
    const qreal s = m_size;
    const QRectF box = boxRect(r, lm).adjusted(0.5, 0.5, -0.5, -0.5); // a 1px border inside the box
    const qreal radius = t->px(QStringLiteral("--checkbox-radius"), -1, 4);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(box, radius, radius);
    if (checked()) {
        p->fillPath(path, t->color(QStringLiteral("--checkbox-color"), t->color(QStringLiteral("--interactive-accent"))));
        QPen pen(t->color(QStringLiteral("--checkbox-marker-color"), t->color(QStringLiteral("--background-primary"))), s * 0.13);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p->setPen(pen);
        QPainterPath check;
        check.moveTo(box.left() + s * 0.26, box.top() + s * 0.52);
        check.lineTo(box.left() + s * 0.43, box.top() + s * 0.68);
        check.lineTo(box.left() + s * 0.74, box.top() + s * 0.34);
        p->drawPath(check);
    } else {
        p->setPen(QPen(t->color(QStringLiteral("--checkbox-border-color"), t->color(QStringLiteral("--text-faint"))), 1));
        p->drawPath(path);
    }
    p->restore();
}

quint64 CheckboxWidget::signature() const { return qHashMulti(2, m_status, m_size, m_lead, m_margin, m_raise); }

quint64 SpacerWidget::signature() const { return qHashMulti(4, m_width); }

QFont MarkerTextWidget::fontFor(const LineMetrics& lm, bool tabularNums)
{
    QFont f = lm.font;
    if (tabularNums)
        f.setFeature(QFont::Tag("tnum"), 1); // font-variant-numeric: tabular-nums (the UA ::marker style)
    return f;
}

QSizeF MarkerTextWidget::size(const LineMetrics& lm) const
{
    return {QFontMetricsF(fontFor(lm, m_tabular)).horizontalAdvance(m_text), lm.ascent + lm.descent};
}

void MarkerTextWidget::paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const
{
    p->save();
    p->setFont(fontFor(lm, m_tabular));
    p->setPen(m_color);
    p->drawText(QPointF(r.left(), r.top() + lm.ascent), m_text);
    p->restore();
}

quint64 MarkerTextWidget::signature() const { return qHashMulti(5, m_text, m_color.rgba(), m_tabular); }

MathWidget::MathWidget(QString tex, bool display, qreal fontPx, QColor color)
    : m_tex(std::move(tex))
    , m_display(display)
    , m_fontPx(fontPx)
    , m_color(color)
{
}

QSizeF MathWidget::size(const LineMetrics&) const
{
    return MathRenderer::instance().render(m_tex, m_display, m_fontPx, m_color)->size;
}

qreal MathWidget::ascent(const LineMetrics&) const { return MathRenderer::instance().render(m_tex, m_display, m_fontPx, m_color)->ascent; }

void MathWidget::paint(QPainter* p, const QRectF& r, const LineMetrics&) const
{
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const auto res = MathRenderer::instance().render(m_tex, m_display, m_fontPx, m_color, dpr);
    p->drawImage(QRectF(r.topLeft(), res->size), res->image);
}

quint64 MathWidget::signature() const { return qHashMulti(3, m_tex, m_display, m_fontPx, m_color.rgba()); }

ImageWidget::ImageWidget(QString absPath, int requestedWidth, int requestedHeight, qreal maxWidth)
    : m_path(std::move(absPath))
    , m_reqW(requestedWidth)
    , m_reqH(requestedHeight)
    , m_maxWidth(maxWidth)
{
    m_natural = ImageCache::instance().naturalSize(m_path);
}

QSizeF ImageWidget::size(const LineMetrics& lm) const
{
    if (!m_natural.isValid())
        return {lm.font.pixelSize() * 1.0, lm.ascent + lm.descent};
    qreal w = m_natural.width();
    qreal h = m_natural.height();
    if (m_reqW > 0 && m_reqH > 0) {
        w = m_reqW;
        h = m_reqH;
    } else if (m_reqW > 0) {
        h = h * m_reqW / std::max(1.0, w);
        w = m_reqW;
    }
    if (w > m_maxWidth) {
        h = h * m_maxWidth / w;
        w = m_maxWidth;
    }
    return {std::round(w), std::round(h)};
}

void ImageWidget::paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const
{
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const QSizeF s = size(lm);
    const auto img = ImageCache::instance().image(m_path, int(std::ceil(s.width())), dpr);
    const ThemePtr t = currentTheme();
    if (!img) {
        p->fillRect(QRectF(r.topLeft(), s), t->color(QStringLiteral("--background-secondary")));
        return;
    }
    p->save();
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal radius = t->px(QStringLiteral("--image-radius"), -1, 4);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(r.topLeft(), s), radius, radius);
    p->setClipPath(clip, Qt::IntersectClip);
    p->drawImage(QRectF(r.topLeft(), s), *img);
    p->restore();
}

quint64 ImageWidget::signature() const { return qHashMulti(4, m_path, m_reqW, m_reqH, m_maxWidth, m_natural.width()); }

} // namespace oc
