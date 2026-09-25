#include "ui/widgets/color_swatch.h"

#include "render/theme/theme.h"

#include <QColorDialog>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>

#include <cmath>

namespace oc {

namespace {
double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
} // namespace

ColorSwatch::ColorSwatch(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void ColorSwatch::setColor(const QColor& color)
{
    m_color = color.isValid() ? color : QColor(Qt::black);
    update();
}

QString ColorSwatch::hexValue() const { return m_color.name(QColor::HexRgb); }

void ColorSwatch::choose(const QColor& color)
{
    if (!color.isValid() || color.rgb() == m_color.rgb())
        return;
    setColor(color);
    emit colorChanged(m_color);
}

void ColorSwatch::openDialog()
{
    auto* dlg = new QColorDialog(m_color, window());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setOption(QColorDialog::DontUseNativeDialog);
    QPointer<ColorSwatch> self = this;
    // Like the <input type="color"> "input" event: the value follows the dialog while picking.
    connect(dlg, &QColorDialog::currentColorChanged, this, [self](const QColor& c) {
        if (self)
            self->choose(c);
    });
    connect(dlg, &QColorDialog::colorSelected, this, [self](const QColor& c) {
        if (self)
            self->choose(c);
    });
    dlg->open();
}

QSize ColorSwatch::sizeHint() const
{
    return {int(std::lround(tokPx("--swatch-width", 22))) + 4, int(std::lround(tokPx("--swatch-height", 22)))};
}

void ColorSwatch::mousePressEvent(QMouseEvent*)
{
    if (isEnabled())
        openDialog();
}

void ColorSwatch::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        openDialog();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ColorSwatch::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.5);
    const double w = tokPx("--swatch-width", 22);
    const double h = tokPx("--swatch-height", 22);
    const QRectF r((width() - w) / 2, (height() - h) / 2, w, h);
    const double radius = std::min(tokPx("--swatch-radius", 14), std::min(w, h) / 2);
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    p.drawRoundedRect(r, radius, radius);
    // --swatch-shadow: inset 0 0 0 1px color-mix(in oklch, var(--mono-100) 15%, transparent)
    QColor ring = t->color(QStringLiteral("--mono-100"), t->isDark() ? Qt::white : Qt::black);
    ring.setAlphaF(0.15f);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ring, 1));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
    if (hasFocus()) {
        p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border-focus")), tokPx("--input-border-width-focus", 2)));
        p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
    }
}

} // namespace oc
