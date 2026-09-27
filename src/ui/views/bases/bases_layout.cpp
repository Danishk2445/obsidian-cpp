#include "ui/views/bases/bases_layout.h"

#include "ui/views/bases/bases_cards.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_list.h"
#include "ui/views/bases/bases_table.h"

#include <QScrollBar>

namespace oc::basesui {

BaseLayout::BaseLayout(BasesController* controller, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_controller(controller)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    if (isEmbedded())
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    horizontalScrollBar()->setSingleStep(20);
    verticalScrollBar()->setSingleStep(30);
}

bool BaseLayout::isEmbedded() const { return m_controller && m_controller->isEmbedded(); }

int BaseLayout::scrollX() const { return horizontalScrollBar()->value(); }

int BaseLayout::scrollY() const { return isEmbedded() ? 0 : verticalScrollBar()->value(); }

void BaseLayout::updateScrollBars(int contentWidth, int contentHeight)
{
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, contentWidth - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    if (isEmbedded()) {
        verticalScrollBar()->setRange(0, 0);
    } else {
        verticalScrollBar()->setRange(0, std::max(0, contentHeight - vp.height()));
        verticalScrollBar()->setPageStep(vp.height());
    }
}

QJsonObject BaseLayout::ephemeralState() const
{
    const int h = std::max(1, contentHeight());
    return {{QStringLiteral("scroll"), QJsonObject{{QStringLiteral("top"), double(scrollY()) / h}, {QStringLiteral("left"), double(scrollX())}}}};
}

void BaseLayout::setEphemeralState(const QJsonObject& state)
{
    const QJsonObject s = state.value(QStringLiteral("scroll")).toObject();
    if (s.isEmpty())
        return;
    verticalScrollBar()->setValue(int(s.value(QStringLiteral("top")).toDouble() * contentHeight()));
    horizontalScrollBar()->setValue(s.value(QStringLiteral("left")).toInt());
}

BaseLayout* createLayout(const QString& type, BasesController* controller, QWidget* parent)
{
    if (type == u"table")
        return new TableLayout(controller, parent);
    if (type == u"cards")
        return new CardsLayout(controller, parent);
    if (type == u"list")
        return new ListLayout(controller, parent);
    return nullptr;
}

} // namespace oc::basesui
