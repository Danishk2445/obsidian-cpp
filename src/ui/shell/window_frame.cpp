#include "ui/shell/window_frame.h"

#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWindow>

namespace oc {

WindowControls::WindowControls(QWidget* window)
    : QWidget(window)
    , m_window(window)
{
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    auto make = [&](const QString& icon, const QString& tip) {
        auto* b = new IconButton(icon, tip, this);
        b->setButtonSize({42, 40});
        b->setIconSize(16);
        b->setRadius(0);
        h->addWidget(b);
        return b;
    };
    m_min = make(QStringLiteral("minus"), tr("Minimize"));
    m_max = make(QStringLiteral("square"), tr("Maximize"));
    m_close = make(QStringLiteral("x"), tr("Close"));
    m_close->setHoverColor(QColor(0xe8, 0x11, 0x23));
    m_close->setHoverIconColor(Qt::white);
    connect(m_min, &IconButton::clicked, m_window, &QWidget::showMinimized);
    connect(m_max, &IconButton::clicked, this, [this] {
        m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
    });
    connect(m_close, &IconButton::clicked, m_window, &QWidget::close);
    setFixedSize(126, 40);
}

void WindowControls::updateMaximizeIcon()
{
    m_max->setIconName(m_window->isMaximized() ? QStringLiteral("copy") : QStringLiteral("square"));
    m_max->setToolTip(m_window->isMaximized() ? tr("Restore down") : tr("Maximize"));
}

// ---------------------------------------------------------------------------------------------

WindowFrameController::WindowFrameController(QWidget* window)
    : QObject(window)
    , m_window(window)
{
    qApp->installEventFilter(this);
}

Qt::Edges WindowFrameController::edgesAt(const QPoint& p) const
{
    if (m_window->isMaximized() || m_window->isFullScreen())
        return {};
    const int m = 6;
    Qt::Edges e;
    if (p.x() < m)
        e |= Qt::LeftEdge;
    if (p.x() >= m_window->width() - m)
        e |= Qt::RightEdge;
    if (p.y() < m)
        e |= Qt::TopEdge;
    if (p.y() >= m_window->height() - m)
        e |= Qt::BottomEdge;
    return e;
}

static Qt::CursorShape cursorFor(Qt::Edges e)
{
    const bool l = e & Qt::LeftEdge, r = e & Qt::RightEdge, t = e & Qt::TopEdge, b = e & Qt::BottomEdge;
    if ((l && t) || (r && b))
        return Qt::SizeFDiagCursor;
    if ((r && t) || (l && b))
        return Qt::SizeBDiagCursor;
    if (l || r)
        return Qt::SizeHorCursor;
    return Qt::SizeVerCursor;
}

bool WindowFrameController::eventFilter(QObject* obj, QEvent* ev)
{
    if (!m_enabled || !(m_window->windowFlags() & Qt::FramelessWindowHint))
        return false;
    const auto type = ev->type();
    if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick
        && type != QEvent::HoverMove)
        return false;
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != m_window)
        return false;
    QPoint globalPos;
    if (type == QEvent::HoverMove)
        globalPos = w->mapToGlobal(static_cast<QHoverEvent*>(ev)->position().toPoint());
    else
        globalPos = static_cast<QMouseEvent*>(ev)->globalPosition().toPoint();
    const QPoint wp = m_window->mapFromGlobal(globalPos);
    const Qt::Edges edges = edgesAt(wp);

    if (type == QEvent::MouseMove || type == QEvent::HoverMove) {
        auto* me = type == QEvent::MouseMove ? static_cast<QMouseEvent*>(ev) : nullptr;
        if (me && me->buttons() != Qt::NoButton)
            return false;
        if (edges) {
            const Qt::CursorShape shape = cursorFor(edges);
            if (!m_cursorOverridden) {
                QApplication::setOverrideCursor(shape);
                m_cursorOverridden = true;
            } else {
                QApplication::changeOverrideCursor(shape);
            }
        } else if (m_cursorOverridden) {
            QApplication::restoreOverrideCursor();
            m_cursorOverridden = false;
        }
        return false;
    }
    auto* me = static_cast<QMouseEvent*>(ev);
    if (me->button() != Qt::LeftButton)
        return false;
    if (type == QEvent::MouseButtonPress && edges) {
        if (auto* h = m_window->windowHandle())
            h->startSystemResize(edges);
        return true;
    }
    if (w->property("ocDragRegion").toBool()) {
        if (type == QEvent::MouseButtonDblClick) {
            m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
            return true;
        }
        if (type == QEvent::MouseButtonPress) {
            if (auto* h = m_window->windowHandle())
                h->startSystemMove();
            return true;
        }
    }
    return false;
}

} // namespace oc
