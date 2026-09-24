#include "ui/widgets/dropdown.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QActionGroup>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace oc {

namespace {
constexpr int kMaxWidth = 400; // .setting-item-control select { max-width: 400px }

double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
} // namespace

Dropdown::Dropdown(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void Dropdown::addOption(const QString& value, const QString& display)
{
    m_options.append({value, display});
    if (m_index < 0)
        m_index = 0;
    updateGeometry();
    update();
}

void Dropdown::setOptions(const QList<QPair<QString, QString>>& options)
{
    const QString current = value();
    m_options = options;
    m_index = -1;
    for (qsizetype i = 0; i < m_options.size(); ++i)
        if (m_options[i].first == current)
            m_index = int(i);
    if (m_index < 0 && !m_options.isEmpty())
        m_index = 0;
    updateGeometry();
    update();
}

void Dropdown::clearOptions()
{
    m_options.clear();
    m_index = -1;
    updateGeometry();
    update();
}

QStringList Dropdown::values() const
{
    QStringList out;
    for (const auto& o : m_options)
        out << o.first;
    return out;
}

QString Dropdown::displayFor(const QString& value) const
{
    for (const auto& o : m_options)
        if (o.first == value)
            return o.second;
    return {};
}

QString Dropdown::value() const { return m_index >= 0 && m_index < m_options.size() ? m_options[m_index].first : QString(); }

QString Dropdown::displayText() const { return m_index >= 0 && m_index < m_options.size() ? m_options[m_index].second : QString(); }

void Dropdown::setValue(const QString& value)
{
    for (qsizetype i = 0; i < m_options.size(); ++i) {
        if (m_options[i].first == value) {
            m_index = int(i);
            update();
            return;
        }
    }
    // Like a <select> given an unknown value: nothing selected.
    m_index = -1;
    update();
}

void Dropdown::choose(const QString& value)
{
    const int before = m_index;
    setValue(value);
    if (m_index != before)
        emit valueChanged(this->value());
}

QFont Dropdown::textFont() const { return currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)); }

QSize Dropdown::sizeHint() const
{
    const QFontMetricsF fm(textFont());
    const double em = textFont().pixelSize();
    double widest = 0;
    for (const auto& o : m_options)
        widest = std::max(widest, fm.horizontalAdvance(o.second));
    const int w = int(std::ceil(widest + 0.8 * em + 2.4 * em));
    return {std::min(kMaxWidth, std::max(w, int(3.2 * em))), int(tokPx("--input-height", 30))};
}

QSize Dropdown::minimumSizeHint() const { return {int(3.2 * textFont().pixelSize()), int(tokPx("--input-height", 30))}; }

void Dropdown::showPopup()
{
    if (m_options.isEmpty() || !isEnabled())
        return;
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFont(textFont());
    auto* group = new QActionGroup(menu);
    for (qsizetype i = 0; i < m_options.size(); ++i) {
        QAction* a = menu->addAction(m_options[i].second);
        a->setCheckable(true);
        a->setChecked(i == m_index);
        a->setData(m_options[i].first);
        group->addAction(a);
        if (i == m_index)
            menu->setActiveAction(a);
    }
    connect(menu, &QMenu::triggered, this, [this](QAction* a) { choose(a->data().toString()); });
    menu->setMinimumWidth(width());
    menu->popup(mapToGlobal(QPoint(0, height() + 2)));
}

void Dropdown::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        showPopup();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void Dropdown::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        showPopup();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (e->modifiers() & Qt::AltModifier) {
            showPopup();
            return;
        }
        if (!m_options.isEmpty()) {
            const int next = std::clamp(m_index + (e->key() == Qt::Key_Down ? 1 : -1), 0, int(m_options.size()) - 1);
            if (next != m_index)
                choose(m_options[next].first);
        }
        return;
    default:
        QWidget::keyPressEvent(e);
    }
}

void Dropdown::focusInEvent(QFocusEvent* e)
{
    m_focusVisible = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;
    QWidget::focusInEvent(e);
}

void Dropdown::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void Dropdown::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

void Dropdown::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::EnabledChange)
        update();
    QWidget::changeEvent(e);
}

void Dropdown::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.5); // .dropdown.mod-disabled
    const double radius = tokPx("--input-radius", 5);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    // --input-shadow / --input-shadow-hover: a soft drop shadow plus an inset ring (a faint top highlight
    // in the dark theme).
    const bool hover = m_hover && isEnabled();
    QPainterPath shadow;
    shadow.addRoundedRect(r.translated(0, 1), radius, radius);
    p.fillPath(shadow, QColor(0, 0, 0, t->isDark() ? (hover ? 110 : 70) : (hover ? 30 : 22)));
    QColor bg = t->color(hover ? QStringLiteral("--dropdown-background-hover") : QStringLiteral("--dropdown-background"));
    if (!bg.isValid())
        bg = t->color(hover ? QStringLiteral("--interactive-hover") : QStringLiteral("--interactive-normal"));
    p.fillPath(path, bg);
    p.setPen(QPen(t->isDark() ? QColor(255, 255, 255, hover ? 40 : 23) : QColor(0, 0, 0, hover ? 43 : 31), 1));
    p.drawPath(path);
    if (hasFocus() && m_focusVisible) {
        p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border-focus")), 3));
        p.drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), radius + 1, radius + 1);
    }

    const QFont f = textFont();
    const double em = f.pixelSize();
    // chevrons-up-down, 1em wide, 0.65em from the end; white 60% (dark) / black 90% blended (light)
    const int iconPx = int(std::lround(em));
    const QColor iconColor = t->isDark() ? QColor(255, 255, 255, 153) : QColor(0, 0, 0, 190);
    const QPixmap icon = IconProvider::instance().pixmap(QStringLiteral("chevrons-up-down"), iconPx, iconColor, 2.0, devicePixelRatioF());
    const double ix = width() - 0.65 * em - iconPx;
    p.drawPixmap(QPointF(ix, (height() - iconPx) / 2.0), icon);

    p.setFont(f);
    p.setPen(t->color(QStringLiteral("--text-normal")));
    const QRectF textRect(0.8 * em, 0, width() - 0.8 * em - 2.4 * em, height());
    const QString text = QFontMetricsF(f).elidedText(displayText(), Qt::ElideRight, textRect.width());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
}

} // namespace oc
