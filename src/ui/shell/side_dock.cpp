#include "ui/shell/side_dock.h"

#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace oc {

SideDock::SideDock(Side side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
{
    setObjectName(side == Side::Left ? QStringLiteral("ocLeftDock") : QStringLiteral("ocRightDock"));
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setFixedHeight(40);
    m_header->setProperty("ocDragRegion", true);
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(2);
    auto* tabs = new QWidget(m_header);
    m_tabsLayout = new QHBoxLayout(tabs);
    m_tabsLayout->setContentsMargins(0, 0, 0, 0);
    m_tabsLayout->setSpacing(2);
    hl->addWidget(tabs);
    hl->addStretch(1);
    m_reserve = new QWidget(m_header);
    m_reserve->setFixedWidth(0);
    m_reserve->setAttribute(Qt::WA_TransparentForMouseEvents);
    hl->addWidget(m_reserve);
    m_root->addWidget(m_header);

    m_stack = new QStackedWidget(this);
    m_root->addWidget(m_stack, 1);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

int SideDock::addView(const QString& type, const QString& icon, const QString& title, QWidget* view)
{
    auto* b = new IconButton(icon, title, m_header);
    b->setCheckable(true);
    b->setButtonSize({30, 28});
    m_tabsLayout->addWidget(b);
    const int idx = m_stack->addWidget(view);
    m_tabs.push_back({type, b});
    connect(b, &IconButton::clicked, this, [this, idx] { setCurrent(idx); });
    if (m_stack->count() == 1)
        setCurrent(0);
    return idx;
}

int SideDock::indexOf(const QString& type) const
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].first == type)
            return i;
    return -1;
}

QWidget* SideDock::view(const QString& type) const
{
    const int i = indexOf(type);
    return i < 0 ? nullptr : m_stack->widget(i);
}

void SideDock::setCurrent(int index)
{
    if (index < 0 || index >= m_stack->count())
        return;
    m_stack->setCurrentIndex(index);
    for (int i = 0; i < m_tabs.size(); ++i)
        m_tabs[i].second->setChecked(i == index);
    emit currentChanged(m_tabs[index].first);
}

void SideDock::setCurrent(const QString& type) { setCurrent(indexOf(type)); }
int SideDock::current() const { return m_stack->currentIndex(); }
QString SideDock::currentType() const
{
    const int i = current();
    return i < 0 ? QString() : m_tabs[i].first;
}

void SideDock::setFooter(QWidget* footer)
{
    if (m_footer)
        m_footer->deleteLater();
    m_footer = footer;
    if (footer)
        m_root->addWidget(footer);
}

void SideDock::setHeaderRightReserve(int px) { m_reserve->setFixedWidth(px); }

void SideDock::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.fillRect(rect(), t->color(QStringLiteral("--background-secondary")));
    // header bottom divider
    p.fillRect(QRect(0, 39, width(), 1), t->color(QStringLiteral("--background-modifier-border")));
}

} // namespace oc
