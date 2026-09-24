#include "ui/widgets/value_slider.h"

#include "render/theme/theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace oc {

namespace {
constexpr double kGap = 8; // .setting-item-control gap (--size-4-2)

QFont valueFont()
{
    const ThemePtr t = currentTheme();
    QFont f = t->interfaceFont(t->px(QStringLiteral("--slider-value-font-size"), -1, 13));
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

double thumbSize()
{
    return currentTheme()->px(QStringLiteral("--slider-thumb-width"), -1, 18);
}
} // namespace

ValueSlider::ValueSlider(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_Hover);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void ValueSlider::setRange(double minimum, double maximum)
{
    m_min = minimum;
    m_max = std::max(minimum, maximum);
    m_value = snap(m_value);
    updateGeometry();
    update();
}

void ValueSlider::setStep(double step)
{
    m_step = std::max(0.0, step);
    m_value = snap(m_value);
    update();
}

void ValueSlider::setValue(double value)
{
    m_value = snap(value);
    update();
}

void ValueSlider::setShowValue(bool show)
{
    m_showValue = show;
    updateGeometry();
    update();
}

double ValueSlider::snap(double v) const
{
    if (std::isnan(v))
        v = m_min;
    v = std::clamp(v, m_min, m_max);
    if (m_step > 0) {
        v = m_min + std::round((v - m_min) / m_step) * m_step;
        v = std::round(v * 1e10) / 1e10; // decimal steps: 0.1 * 3 -> 0.3
        v = std::clamp(v, m_min, m_max);
    }
    return v;
}

QString ValueSlider::valueText() const
{
    // SliderComponent.getValuePretty(): "any" or fractional steps -> two decimals.
    if (m_step == 0 || m_step < 1)
        return QString::number(m_value, 'f', 2);
    return QString::number(m_value, 'g', 12);
}

double ValueSlider::valueLabelWidth() const
{
    if (!m_showValue)
        return 0;
    const QFontMetricsF fm(valueFont());
    auto text = [&](double v) {
        return (m_step == 0 || m_step < 1) ? QString::number(v, 'f', 2) : QString::number(v, 'g', 12);
    };
    return std::ceil(std::max({fm.horizontalAdvance(text(m_min)), fm.horizontalAdvance(text(m_max)), fm.horizontalAdvance(valueText())}));
}

QRectF ValueSlider::trackRect() const
{
    const double h = currentTheme()->px(QStringLiteral("--slider-track-height"), -1, 4);
    const double left = m_showValue ? valueLabelWidth() + kGap : 0;
    return {left, (height() - h) / 2, std::max(0.0, width() - left), h};
}

QSize ValueSlider::sizeHint() const
{
    return {int(valueLabelWidth() + kGap + 120), int(std::ceil(std::max(thumbSize(), 20.0)))};
}

QSize ValueSlider::minimumSizeHint() const
{
    return {int(valueLabelWidth() + kGap + 40), sizeHint().height()};
}

void ValueSlider::setFromX(double x)
{
    const QRectF tr = trackRect();
    const double tw = thumbSize();
    const double span = tr.width() - tw;
    const double ratio = span > 0 ? std::clamp((x - tr.left() - tw / 2) / span, 0.0, 1.0) : 0;
    userSet(m_min + ratio * (m_max - m_min));
}

void ValueSlider::userSet(double v)
{
    v = snap(v);
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit valueChanged(m_value);
}

void ValueSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !isEnabled())
        return QWidget::mousePressEvent(e);
    if (e->position().x() < trackRect().left() - 2) // on the value label
        return QWidget::mousePressEvent(e);
    m_dragging = true;
    setFromX(e->position().x());
    update();
}

void ValueSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging)
        setFromX(e->position().x());
}

void ValueSlider::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        update();
    }
}

void ValueSlider::keyPressEvent(QKeyEvent* e)
{
    const double step = m_step > 0 ? m_step : (m_max - m_min) / 100;
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        userSet(m_value - step);
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        userSet(m_value + step);
        break;
    case Qt::Key_Home:
        userSet(m_min);
        break;
    case Qt::Key_End:
        userSet(m_max);
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

void ValueSlider::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void ValueSlider::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

void ValueSlider::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.5);

    if (m_showValue) {
        p.setFont(valueFont());
        p.setPen(t->color(QStringLiteral("--text-normal")));
        p.drawText(QRectF(0, 0, valueLabelWidth(), height()), Qt::AlignLeft | Qt::AlignVCenter, valueText());
    }

    const QRectF tr = trackRect();
    const double tw = thumbSize();
    const double ratio = m_max > m_min ? (m_value - m_min) / (m_max - m_min) : 0;
    const double cx = tr.left() + tw / 2 + (tr.width() - tw) * ratio;
    const double radius = tr.height() / 2;
    p.setPen(Qt::NoPen);
    p.setBrush(t->color(QStringLiteral("--slider-track-background")));
    p.drawRoundedRect(tr, radius, radius);
    p.setBrush(t->color(QStringLiteral("--slider-fill-background")));
    p.drawRoundedRect(QRectF(tr.left(), tr.top(), cx - tr.left(), tr.height()), radius, radius);

    // Thumb with --slider-thumb-shadow (0 0 2px + 0 2px 6px, both 10% black).
    const QPointF c(cx, height() / 2.0);
    const double r = tw / 2;
    for (int i = 3; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 7));
        p.drawEllipse(c + QPointF(0, 2), r + i, r + i);
    }
    p.setBrush(QColor(0, 0, 0, 20));
    p.drawEllipse(c, r + 1, r + 1);
    const bool active = m_hover || m_dragging || hasFocus();
    const QColor border = active ? t->color(QStringLiteral("--background-modifier-border-focus"))
                                 : t->color(QStringLiteral("--slider-thumb-border-color"));
    p.setPen(QPen(border, std::max(0.5, t->px(QStringLiteral("--slider-thumb-border-width"), -1, 0.5))));
    p.setBrush(t->color(active ? QStringLiteral("--slider-thumb-background-hover") : QStringLiteral("--slider-thumb-background"), Qt::white));
    p.drawEllipse(c, r - 0.25, r - 0.25);
}

} // namespace oc
