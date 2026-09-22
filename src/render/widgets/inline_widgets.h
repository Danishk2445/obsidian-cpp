#pragma once

#include "render/layout/decoration.h"

#include <QColor>
#include <QImage>
#include <QString>

#include <memory>

namespace oc {

// Live preview list bullet ("•" drawn as a disc of --list-bullet-size, --list-marker-color).
class BulletWidget : public InlineWidget {
public:
    BulletWidget(QColor color, qreal widthPx) : m_color(color), m_width(widthPx) {}
    QSizeF size(const LineMetrics& lm) const override { return {m_width, lm.ascent + lm.descent}; }
    qreal ascent(const LineMetrics& lm) const override { return lm.ascent; }
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;

private:
    QColor m_color;
    qreal m_width;
};

// Task checkbox (Obsidian .task-list-item-checkbox): rounded square, accent fill + check when done.
class CheckboxWidget : public InlineWidget {
public:
    CheckboxWidget(QChar status, qreal sizePx, qreal marginRightPx) : m_status(status), m_size(sizePx), m_margin(marginRightPx) {}
    QSizeF size(const LineMetrics& lm) const override { return {m_size + m_margin, std::max(m_size, lm.ascent + lm.descent)}; }
    qreal ascent(const LineMetrics& lm) const override;
    void paint(QPainter* p, const QRectF& r, const LineMetrics& lm) const override;
    quint64 signature() const override;
    bool checked() const { return m_status != u' '; }
    QChar status() const { return m_status; }
    QRectF boxRect(const QRectF& widgetRect) const; // hit area

private:
    QChar m_status;
    qreal m_size;
    qreal m_margin;
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
