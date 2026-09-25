#include "ui/shell/status_bar.h"

#include "render/theme/theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>

namespace oc {

StatusBar::StatusBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocStatusBar"));
    setAttribute(Qt::WA_TranslucentBackground);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(10, 4, 10, 4);
    m_layout->setSpacing(14);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            const ThemePtr t = currentTheme();
            for (auto& [id, l] : m_items) {
                QFont f = t->interfaceFont(t->px(QStringLiteral("--status-bar-font-size"), -1, 12));
                l->setFont(f);
                QPalette pal = l->palette();
                pal.setColor(QPalette::WindowText, t->color(QStringLiteral("--status-bar-text-color")));
                l->setPalette(pal);
            }
            update();
        });
}

QLabel* StatusBar::addItem(const QString& id, const QString& text)
{
    auto* l = new QLabel(text, this);
    const ThemePtr t = currentTheme();
    l->setFont(t->interfaceFont(t->px(QStringLiteral("--status-bar-font-size"), -1, 12)));
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, t->color(QStringLiteral("--status-bar-text-color")));
    l->setPalette(pal);
    l->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(t->color(QStringLiteral("--status-bar-text-color")).name()));
    l->installEventFilter(this);
    m_layout->addWidget(l);
    m_items.push_back({id, l});
    anchor();
    return l;
}

QLabel* StatusBar::item(const QString& id) const
{
    for (auto& [iid, l] : m_items)
        if (iid == id)
            return l;
    return nullptr;
}

void StatusBar::setItemText(const QString& id, const QString& text)
{
    if (QLabel* l = item(id)) {
        l->setText(text);
        anchor();
    }
}

void StatusBar::setItemVisible(const QString& id, bool visible)
{
    if (QLabel* l = item(id)) {
        if (visible)
            m_wanted.insert(id);
        else
            m_wanted.remove(id);
        l->setVisible(visible && !m_disabled.contains(id));
        anchor();
    }
}

void StatusBar::setItemEnabled(const QString& id, bool enabled)
{
    if (enabled)
        m_disabled.remove(id);
    else
        m_disabled.insert(id);
    if (QLabel* l = item(id)) {
        l->setVisible(enabled && m_wanted.contains(id));
        anchor();
    }
}

void StatusBar::anchor()
{
    adjustSize();
    if (QWidget* p = parentWidget())
        move(p->width() - width(), p->height() - height());
}

bool StatusBar::event(QEvent* e)
{
    const bool handled = QWidget::event(e);
    if (e->type() == QEvent::LayoutRequest) // an item's size changed: keep hugging the bottom-right corner
        anchor();
    return handled;
}

bool StatusBar::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::MouseButtonRelease) {
        for (auto& [id, l] : m_items)
            if (l == obj)
                emit itemClicked(id);
    }
    return QWidget::eventFilter(obj, ev);
}

void StatusBar::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = 8;
    QPainterPath path;
    const QRectF rc = QRectF(rect()).adjusted(0.5, 0.5, 0, 0);
    path.moveTo(rc.left(), rc.bottom() + 1);
    path.lineTo(rc.left(), rc.top() + r);
    path.quadTo(rc.left(), rc.top(), rc.left() + r, rc.top());
    path.lineTo(rc.right() + 1, rc.top());
    path.lineTo(rc.right() + 1, rc.bottom() + 1);
    path.closeSubpath();
    p.fillPath(path, t->color(QStringLiteral("--background-secondary")));
    QPainterPath border;
    border.moveTo(rc.left(), rc.bottom() + 1);
    border.lineTo(rc.left(), rc.top() + r);
    border.quadTo(rc.left(), rc.top(), rc.left() + r, rc.top());
    border.lineTo(rc.right() + 1, rc.top());
    p.setPen(QPen(t->color(QStringLiteral("--divider-color")), 1));
    p.drawPath(border);
}

} // namespace oc
