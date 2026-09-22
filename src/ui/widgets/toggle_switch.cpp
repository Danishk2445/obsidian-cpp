#include "ui/widgets/toggle_switch.h"

#include "render/theme/theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

namespace oc {

namespace {
struct Metrics {
    double width, thumb, border;
};
Metrics metrics(const Theme& t, ToggleSwitch::Size size)
{
    if (size == ToggleSwitch::Size::Normal)
        return {t.px(QStringLiteral("--toggle-width"), -1, 40), t.px(QStringLiteral("--toggle-thumb-width"), -1, 18),
                t.px(QStringLiteral("--toggle-border-width"), -1, 2)};
    return {t.px(QStringLiteral("--toggle-s-width"), -1, 34), t.px(QStringLiteral("--toggle-s-thumb-width"), -1, 15),
            t.px(QStringLiteral("--toggle-s-border-width"), -1, 2)};
}
} // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(150);
    m_anim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_pos = v.toDouble();
        update();
    });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void ToggleSwitch::setSize(Size size)
{
    m_size = size;
    updateGeometry();
    update();
}

QSize ToggleSwitch::sizeHint() const
{
    const Metrics m = metrics(*currentTheme(), m_size);
    return {int(std::lround(m.width)), int(std::lround(m.thumb + 2 * m.border))};
}

void ToggleSwitch::checkStateSet()
{
    // Programmatic changes jump; user toggles animate (nextCheckState).
    if (m_anim->state() != QAbstractAnimation::Running)
        m_pos = isChecked() ? 1 : 0;
    update();
}

void ToggleSwitch::nextCheckState()
{
    QAbstractButton::nextCheckState();
    animateTo(isChecked());
}

void ToggleSwitch::animateTo(bool on)
{
    m_anim->stop();
    m_anim->setStartValue(m_pos);
    m_anim->setEndValue(on ? 1.0 : 0.0);
    m_anim->start();
}

bool ToggleSwitch::hitButton(const QPoint& pos) const { return rect().contains(pos); }

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    const Metrics m = metrics(*t, m_size);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.5);

    const double h = m.thumb + 2 * m.border;
    const QRectF track((width() - m.width) / 2, (height() - h) / 2, m.width, h);
    const double radius = std::min(t->px(QStringLiteral("--toggle-radius"), -1, 18), h / 2);
    QPainterPath pill;
    pill.addRoundedRect(track, radius, radius);

    const QColor off = t->color(QStringLiteral("--background-modifier-border-hover"));
    const QColor on = t->color(QStringLiteral("--interactive-accent"));
    // The background switches with the class (no colour transition in Obsidian's CSS).
    p.fillPath(pill, isChecked() ? on : off);
    // box-shadow: inset 0 4px 10px rgba(0, 0, 0, 0.07)
    QLinearGradient inset(track.topLeft(), QPointF(track.left(), track.top() + h * 0.7));
    inset.setColorAt(0, QColor(0, 0, 0, 18));
    inset.setColorAt(1, QColor(0, 0, 0, 0));
    p.fillPath(pill, inset);

    if (hasFocus()) {
        QPen focus(t->color(QStringLiteral("--background-modifier-border-focus")), m.border);
        p.setPen(focus);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track.adjusted(-m.border / 2, -m.border / 2, m.border / 2, m.border / 2), radius + m.border / 2, radius + m.border / 2);
    }

    // Thumb: slides from border to width - thumb - border; grows while pressed.
    const double travel = m.width - m.thumb - 2 * m.border;
    double tw = m.thumb, th = m.thumb;
    double tx = track.left() + m.border + travel * m_pos;
    double ty = track.top() + m.border;
    if (isDown() && isEnabled()) {
        const double grow = m_size == Size::Normal ? 4 * m.border : 2 * m.border;
        tw += grow;
        th += grow;
        ty = track.top() + (h - th) / 2;
        if (m_size == Size::Normal && isChecked())
            tx -= 2 * m.border;
    }
    const QRectF thumb(tx, ty, tw, th);
    const double tr = std::min(t->px(QStringLiteral("--toggle-thumb-radius"), -1, 18), th / 2);
    // box-shadow: 0 1px 2px rgba(0, 0, 0, 0.15)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 20));
    p.drawRoundedRect(thumb.translated(0, 1).adjusted(-0.5, -0.5, 0.5, 0.5), tr + 0.5, tr + 0.5);
    p.setBrush(QColor(0, 0, 0, 26));
    p.drawRoundedRect(thumb.translated(0, 1), tr, tr);
    p.setBrush(t->color(QStringLiteral("--toggle-thumb-color"), Qt::white));
    p.drawRoundedRect(thumb, tr, tr);
}

} // namespace oc
