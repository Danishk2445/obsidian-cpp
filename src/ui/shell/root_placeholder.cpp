#include "ui/shell/root_placeholder.h"

#include "render/theme/theme.h"
#include "ui/shell/empty_view.h"
#include "ui/widgets/icon_button.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace oc {

RootPlaceholder::RootPlaceholder(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_header = new QWidget(this);
    m_header->setFixedHeight(40);
    m_header->setProperty("ocDragRegion", true);
    auto* h = new QHBoxLayout(m_header);
    h->setContentsMargins(8, 0, 8, 0);
    auto* plus = new IconButton(QStringLiteral("plus"), tr("New tab"), m_header);
    plus->setButtonSize({28, 28});
    h->addWidget(plus);
    h->addStretch(1);
    auto* list = new IconButton(QStringLiteral("chevron-down"), tr("Show tab list"), m_header);
    list->setButtonSize({28, 28});
    h->addWidget(list);
    m_reserve = new QWidget(m_header);
    m_reserve->setFixedWidth(0);
    h->addWidget(m_reserve);
    v->addWidget(m_header);
    m_empty = new EmptyView(this);
    v->addWidget(m_empty, 1);
}

void RootPlaceholder::setRightReserve(int px) { m_reserve->setFixedWidth(px); }

void RootPlaceholder::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.fillRect(QRect(0, 0, width(), 40), t->color(QStringLiteral("--tab-container-background")));
    p.fillRect(QRect(0, 39, width(), 1), t->color(QStringLiteral("--background-modifier-border")));
}

} // namespace oc
