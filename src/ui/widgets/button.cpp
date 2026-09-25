#include "ui/widgets/button.h"

#include "render/theme/theme.h"

#include <QFocusEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace oc {

namespace {
QColor tok(const char* name, const QColor& fallback = {}) { return currentTheme()->color(QString::fromLatin1(name), fallback); }
double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
} // namespace

Button::Button(const QString& text, Style style, QWidget* parent)
    : QPushButton(text, parent)
    , m_style(style)
{
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFont(currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)));
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            setFont(currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)));
            updateGeometry();
            update();
        });
}

void Button::setButtonStyle(Style style)
{
    m_style = style;
    update();
}

void Button::setLoading(bool loading)
{
    m_loading = loading;
    update();
}

QSize Button::sizeHint() const
{
    const QFontMetricsF fm(font());
    return {int(std::ceil(fm.horizontalAdvance(text()))) + 2 * int(tokPx("--size-4-3", 12)), int(tokPx("--input-height", 30))};
}

void Button::focusInEvent(QFocusEvent* e)
{
    m_focusVisible = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;
    QPushButton::focusInEvent(e);
}

void Button::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.7); // button[disabled]
    const bool hover = isEnabled() && (underMouse() || isDown());
    const double radius = tokPx("--button-radius", 5);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    QColor bg, fg = tok("--text-normal");
    bool shadow = true;
    switch (m_style) {
    case Style::Normal:
        bg = hover ? tok("--interactive-hover") : tok("--interactive-normal");
        break;
    case Style::Cta:
        bg = hover ? tok("--interactive-accent-hover") : tok("--interactive-accent");
        fg = tok("--text-on-accent", Qt::white);
        break;
    case Style::Warning:
        bg = hover ? tok("--background-modifier-error-hover") : tok("--background-modifier-error");
        fg = tok("--text-on-accent", Qt::white);
        break;
    case Style::Destructive:
        bg = tok("--background-modifier-error");
        bg.setAlphaF(hover ? 0.15f : 0.1f);
        fg = tok("--text-error");
        shadow = false;
        break;
    }
    if (shadow) {
        QPainterPath s;
        s.addRoundedRect(r.translated(0, 1), radius, radius);
        p.fillPath(s, QColor(0, 0, 0, t->isDark() ? 70 : 22));
    }
    p.fillPath(path, bg);
    if (m_style == Style::Normal) {
        p.setPen(QPen(t->isDark() ? QColor(255, 255, 255, hover ? 40 : 23) : QColor(0, 0, 0, hover ? 43 : 31), 1));
        p.drawPath(path);
    }
    if (hasFocus() && m_focusVisible) {
        p.setPen(QPen(tok("--background-modifier-border-focus"), 3));
        p.drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), radius + 1, radius + 1);
    }
    if (m_loading)
        return; // .mod-loading: color transparent
    p.setPen(fg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

} // namespace oc
