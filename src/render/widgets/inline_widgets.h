#pragma once

#include "render/layout/decoration.h"

#include <QColor>
#include <QImage>
#include <QString>

#include <memory>

namespace oc {

// List bullet (.list-bullet:after): a disc of --list-bullet-size in --list-marker-color, `centerX` px from the
// widget's left. The .list-bullet box is an inline-flex of the line's line-height that centres the disc, so its
// centre sits (ascent - descent) / 2 above the baseline.
class BulletWidget : public InlineWidget {
public:
    BulletWidget(QColor color, qreal widthPx, qreal centerX, qreal dotPx) : m_color(color), m_width(widthPx), m_center(centerX), m_dot(dotPx) {}
    QSizeF size(const LineMetrics& lm) const override { return {m_width, lm.ascent + lm.descent}; }
    qreal ascent(const LineMetrics& lm) const override { return lm.ascent; }
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    // The disc's centre for a widget laid out at `rect` on a line with these metrics.
    QPointF dotCenter(const QRectF& rect, const LineMetrics& lm) const;
    qreal dotSize() const { return m_dot; }

private:
    QColor m_color;
    qreal m_width;
    qreal m_center;
    qreal m_dot;
};

// Task checkbox (Obsidian .task-list-item-checkbox): rounded square, accent fill + check when done. `lead` px of
// margin before the box and `marginEnd` after it (the widget is lead + size + marginEnd wide); the box's centre
// sits `raise` px above the baseline.
class CheckboxWidget : public InlineWidget {
public:
    CheckboxWidget(QChar status, qreal sizePx, qreal leadPx, qreal marginEndPx, qreal raisePx)
        : m_status(status), m_size(sizePx), m_lead(leadPx), m_margin(marginEndPx), m_raise(raisePx) {}
    QSizeF size(const LineMetrics& lm) const override;
    qreal ascent(const LineMetrics& lm) const override;
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    bool checked() const { return m_status != u' '; }
    QChar status() const { return m_status; }
    QRectF boxRect(const QRectF& widgetRect, const LineMetrics& lm) const; // the painted box (hit area)

private:
    QChar m_status;
    qreal m_size;
    qreal m_lead;
    qreal m_margin;
    qreal m_raise;
};

// Empty room of a fixed width in the text flow (CSS padding in front of a span: Live Preview's list marker
// padding, a continuation line's --list-marker-space).
class SpacerWidget : public InlineWidget {
public:
    explicit SpacerWidget(qreal widthPx) : m_width(widthPx) {}
    QSizeF size(const LineMetrics& lm) const override { return {m_width, lm.ascent + lm.descent}; }
    qreal ascent(const LineMetrics& lm) const override { return lm.ascent; }
    void paint(QPainter*, const QRectF&, const LineMetrics&) const override {}
    quint64 signature() const override;

private:
    qreal m_width;
};

// A short text drawn in the line's font (Reading view's ordered list numbers, `ol > li::marker`): `format`'s
// foreground and font features (tabular-nums) apply.
class MarkerTextWidget : public InlineWidget {
public:
    MarkerTextWidget(QString text, QColor color, bool tabularNums) : m_text(std::move(text)), m_color(color), m_tabular(tabularNums) {}
    QSizeF size(const LineMetrics& lm) const override;
    qreal ascent(const LineMetrics& lm) const override { return lm.ascent; }
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    QString accessibleText() const override { return m_text; }
    const QString& text() const { return m_text; }
    static QFont fontFor(const LineMetrics& lm, bool tabularNums);

private:
    QString m_text;
    QColor m_color;
    bool m_tabular;
};

// Rendered inline math ($...$).
class MathWidget : public InlineWidget {
public:
    MathWidget(QString tex, bool display, qreal fontPx, QColor color);
    QSizeF size(const LineMetrics& lm) const override;
    qreal ascent(const LineMetrics& lm) const override;
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    QString accessibleText() const override { return m_tex; }

private:
    QString m_tex;
    bool m_display;
    qreal m_fontPx;
    QColor m_color;
};

// Inline image (![[x.png|200]] / ![](x.png)), width limited to maxWidth; loads asynchronously.
class ImageWidget : public InlineWidget {
public:
    ImageWidget(QString absPath, int requestedWidth, int requestedHeight, qreal maxWidth);
    QSizeF size(const LineMetrics& lm) const override;
    qreal ascent(const LineMetrics& lm) const override { return size(lm).height(); } // sits on the baseline
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    const QString& path() const { return m_path; }
    bool valid() const { return m_natural.isValid(); }

private:
    QString m_path;
    int m_reqW, m_reqH;
    qreal m_maxWidth;
    QSize m_natural;
};

} // namespace oc
