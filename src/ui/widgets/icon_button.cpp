#include "ui/widgets/icon_button.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QPainter>
#include <QPainterPath>

namespace oc {

IconButton::IconButton(const QString& icon, const QString& tooltip, QWidget* parent)
    : QAbstractButton(parent)
    , m_icon(icon)
{
    setToolTip(tooltip);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void IconButton::setIconName(const QString& icon)
{
    m_icon = icon;
    update();
}

void IconButton::setIconSize(int px)
{
    m_iconPx = px;
    update();
}

void IconButton::setButtonSize(QSize size)
{
    m_size = size;
    updateGeometry();
    update();
}

void IconButton::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void IconButton::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void IconButton::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool active = isChecked() || isDown();
    if ((m_hover && isEnabled()) || (active && m_checkedBg)) {
        QColor bg = m_hoverBg.isValid() && m_hover ? m_hoverBg : t->color(QStringLiteral("--background-modifier-hover"));
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), m_radius, m_radius);
        p.fillPath(path, bg);
    }
    QColor fg;
    if (!isEnabled())
        fg = t->color(QStringLiteral("--text-faint"));
    else if (m_hover && m_hoverFg.isValid())
        fg = m_hoverFg;
    else if (isChecked())
        fg = t->color(QStringLiteral("--icon-color-active"));
    else if (m_hover)
        fg = t->color(QStringLiteral("--icon-color-hover"));
    else
        fg = t->color(QStringLiteral("--icon-color"));
    const double opacity = (m_hover || isChecked()) ? 1.0 : t->number(QStringLiteral("--icon-opacity"), 0.85);
    p.setOpacity(opacity);
    const QPixmap pm = IconProvider::instance().pixmap(m_icon, m_iconPx, fg, 1.75, devicePixelRatioF());
    const QPointF tl((width() - m_iconPx) / 2.0, (height() - m_iconPx) / 2.0);
    p.drawPixmap(tl, pm);
}

} // namespace oc
