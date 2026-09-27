#include "ui/shell/ribbon.h"

#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"

#include <QEvent>
#include <QMouseEvent>

#include <algorithm>
#include <QPainter>
#include <QVBoxLayout>

namespace oc {

Ribbon::Ribbon(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocRibbon"));
    setFixedWidth(44);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* top = new QWidget(this);
    top->setFixedHeight(40);
    top->setProperty("ocDragRegion", true);
    auto* th = new QHBoxLayout(top);
    th->setContentsMargins(0, 0, 0, 0);
    m_toggle = new IconButton(QStringLiteral("sidebar-left"), tr("Collapse"), top);
    m_toggle->setButtonSize({28, 28});
    th->addWidget(m_toggle, 0, Qt::AlignCenter);
    v->addWidget(top);

    auto* actions = new QWidget(this);
    m_actionsWidget = actions;
    m_actionsLayout = new QVBoxLayout(actions);
    m_actionsLayout->setContentsMargins(0, 4, 0, 4);
    m_actionsLayout->setSpacing(6);
    m_actionsLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    v->addWidget(actions);
    v->addStretch(1);

    auto* bottom = new QWidget(this);
    m_bottomLayout = new QVBoxLayout(bottom);
    m_bottomLayout->setContentsMargins(0, 4, 0, 12);
    m_bottomLayout->setSpacing(6);
    m_bottomLayout->setAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    v->addWidget(bottom);

    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

IconButton* Ribbon::makeButton(const QString& id, const QString& icon, const QString& title)
{
    auto* b = new IconButton(icon, title, this);
    b->setButtonSize({28, 28});
    connect(b, &IconButton::clicked, this, [this, id] { emit actionTriggered(id); });
    m_actions.push_back({id, b});
    return b;
}

IconButton* Ribbon::addAction(const QString& id, const QString& icon, const QString& title)
{
    IconButton* b = makeButton(id, icon, title);
    b->setParent(m_actionsWidget);
    m_actionsLayout->addWidget(b, 0, Qt::AlignHCenter);
    b->installEventFilter(this);
    syncActionOrder();
    return b;
}

QStringList Ribbon::order() const
{
    QStringList ids;
    for (int i = 0; i < m_actionsLayout->count(); ++i)
        if (QWidget* w = m_actionsLayout->itemAt(i)->widget())
            for (const auto& [id, b] : m_actions)
                if (b == w)
                    ids << id;
    return ids;
}

void Ribbon::setOrder(const QStringList& ids)
{
    QList<IconButton*> buttons;
    for (int i = 0; i < m_actionsLayout->count(); ++i)
        if (auto* b = qobject_cast<IconButton*>(m_actionsLayout->itemAt(i)->widget()))
            buttons << b;
    auto idOf = [this](IconButton* b) {
        for (const auto& [id, button] : m_actions)
            if (button == b)
                return id;
        return QString();
    };
    // Obsidian: items.sort((a, b) => order.indexOf(a.id) - order.indexOf(b.id)); unknown ones (-1) come first
    std::stable_sort(buttons.begin(), buttons.end(), [&](IconButton* a, IconButton* b) { return ids.indexOf(idOf(a)) < ids.indexOf(idOf(b)); });
    for (IconButton* b : std::as_const(buttons))
        m_actionsLayout->removeWidget(b);
    for (IconButton* b : std::as_const(buttons))
        m_actionsLayout->addWidget(b, 0, Qt::AlignHCenter);
    syncActionOrder();
}

void Ribbon::syncActionOrder()
{
    QList<QPair<QString, IconButton*>> top, rest;
    for (const auto& a : std::as_const(m_actions))
        (a.second->parentWidget() == m_actionsWidget ? top : rest) << a;
    std::stable_sort(top.begin(), top.end(), [this](const auto& a, const auto& b) {
        return m_actionsLayout->indexOf(a.second) < m_actionsLayout->indexOf(b.second);
    });
    m_actions = top + rest;
}

bool Ribbon::eventFilter(QObject* obj, QEvent* ev)
{
    auto* b = qobject_cast<IconButton*>(obj);
    if (!b || b->parentWidget() != m_actionsWidget)
        return QWidget::eventFilter(obj, ev);
    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            m_dragButton = b;
            m_pressPos = me->globalPosition().toPoint();
            m_dragging = false;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (m_dragButton != b || !(me->buttons() & Qt::LeftButton))
            break;
        const QPoint gp = me->globalPosition().toPoint();
        if (!m_dragging && (gp - m_pressPos).manhattanLength() >= 5)
            m_dragging = true;
        if (!m_dragging)
            break;
        // the dragged item goes after every other visible item whose middle is above the pointer
        const int y = m_actionsWidget->mapFromGlobal(gp).y();
        QList<QWidget*> others;
        for (int i = 0; i < m_actionsLayout->count(); ++i)
            if (QWidget* w = m_actionsLayout->itemAt(i)->widget(); w && w != b)
                others << w;
        int target = 0;
        for (int i = 0; i < others.size(); ++i)
            if (!others[i]->isHidden() && others[i]->geometry().center().y() < y)
                target = i + 1;
        if (m_actionsLayout->indexOf(b) != target) {
            m_actionsLayout->removeWidget(b);
            m_actionsLayout->insertWidget(target, b, 0, Qt::AlignHCenter);
            m_actionsLayout->activate();
        }
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (m_dragButton != b)
            break;
        const bool dragged = m_dragging;
        m_dragButton = nullptr;
        m_dragging = false;
        if (dragged) {
            b->setDown(false); // no click after a drag
            syncActionOrder();
            emit orderChanged();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(obj, ev);
}

IconButton* Ribbon::addBottomAction(const QString& id, const QString& icon, const QString& title)
{
    IconButton* b = makeButton(id, icon, title);
    m_bottomLayout->addWidget(b, 0, Qt::AlignHCenter);
    return b;
}

void Ribbon::setActionHidden(const QString& id, bool hidden)
{
    if (hidden)
        m_hidden.insert(id);
    else
        m_hidden.remove(id);
    for (auto& [aid, b] : m_actions)
        if (aid == id)
            b->setHidden(hidden || m_unavailable.contains(id));
}

void Ribbon::setActionAvailable(const QString& id, bool available)
{
    if (available)
        m_unavailable.remove(id);
    else
        m_unavailable.insert(id);
    for (auto& [aid, b] : m_actions)
        if (aid == id)
            b->setHidden(!available || m_hidden.contains(id));
}

void Ribbon::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    update();
}

void Ribbon::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    const QColor bg = t->color(QStringLiteral("--ribbon-background"), t->color(QStringLiteral("--background-secondary")));
    // .workspace-ribbon.mod-left.is-collapsed uses --ribbon-background-collapsed.
    p.fillRect(rect(), m_collapsed ? t->color(QStringLiteral("--ribbon-background-collapsed"), bg) : bg);
    // The header part (.workspace-ribbon.mod-left:before) takes the title bar colors, brighter while the
    // window is focused, with a bottom border like the tab headers next to it.
    const bool focused = window() && window()->isActiveWindow();
    p.fillRect(QRect(0, 0, width(), 40),
               t->color(focused ? QStringLiteral("--titlebar-background-focused") : QStringLiteral("--titlebar-background"), bg));
    p.fillRect(QRect(0, 39, width(), 1), t->color(QStringLiteral("--tab-outline-color")));
    // right divider below the header row
    p.fillRect(QRect(width() - 1, 40, 1, height() - 40), t->color(QStringLiteral("--divider-color")));
}

void Ribbon::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange)
        update();
    QWidget::changeEvent(e);
}

} // namespace oc
