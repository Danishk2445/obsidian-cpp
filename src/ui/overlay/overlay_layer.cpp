#include "ui/overlay/overlay_layer.h"

#include "render/theme/theme.h"

#include <QApplication>
#include <QChildEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QTextLayout>

namespace oc {

OverlayLayer::OverlayLayer(QWidget* window)
    : QWidget(window)
{
    setObjectName(QStringLiteral("ocOverlayLayer"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setGeometry(window->rect());
    window->installEventFilter(this);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(0);
    connect(&m_refreshTimer, &QTimer::timeout, this, &OverlayLayer::refresh);
    hide();
}

OverlayLayer::~OverlayLayer()
{
    // Modals and popups hold QPointer<OverlayLayer>; deleting them from ~QWidget would let them see a
    // half-destroyed layer.
    // (Deleting one may delete others, e.g. a modal its popups: re-query each time.)
    // An open modal calls setBlocking(false) from its destructor; the refresh it triggers must not
    // raise() the layer: the window may be deleting its children, and raise() walks the siblings.
    m_destroying = true;
    m_refreshTimer.stop();
    while (QWidget* w = findChild<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        delete w;
}

OverlayLayer* OverlayLayer::of(const QWidget* w)
{
    const QWidget* top = w ? w->window() : nullptr;
    return top ? top->findChild<OverlayLayer*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

OverlayLayer* OverlayLayer::focused(OverlayLayer* layer)
{
    QWidget* home = layer ? layer->window() : nullptr;
    QWidget* active = QApplication::activeWindow();
    if (!home || !active || active == home || active->property("ocOwnerWindow").value<QObject*>() != home)
        return layer;
    OverlayLayer* l = of(active);
    return l ? l : layer;
}

void OverlayLayer::setBlocking(QWidget* child, bool blocking)
{
    m_blocking.removeIf([&](const QPointer<QWidget>& w) { return !w || w == child; });
    if (blocking)
        m_blocking.push_back(child);
    refresh();
}

void OverlayLayer::refresh()
{
    if (m_destroying)
        return;
    // isHidden(), not isVisible(): a modal shown while the layer itself is still hidden must keep blocking.
    m_blocking.removeIf([](const QPointer<QWidget>& w) { return !w || w->isHidden(); });
    QRegion region;
    bool any = false;
    for (QObject* o : children()) {
        auto* w = qobject_cast<QWidget*>(o);
        if (!w || w->isHidden())
            continue;
        any = true;
        region += w->geometry();
    }
    if (!any) {
        hide();
        return;
    }
    if (m_blocking.isEmpty())
        setMask(region);
    else
        clearMask();
    show();
    raise();
    update();
}

bool OverlayLayer::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == parentWidget() && ev->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        m_refreshTimer.start();
    }
    if (ev->type() == QEvent::Show || ev->type() == QEvent::Hide || ev->type() == QEvent::Move || ev->type() == QEvent::Resize) {
        if (obj != parentWidget())
            m_refreshTimer.start();
    }
    return QWidget::eventFilter(obj, ev);
}

void OverlayLayer::childEvent(QChildEvent* ev)
{
    if (ev->added() && ev->child()->isWidgetType())
        ev->child()->installEventFilter(this);
    m_refreshTimer.start();
    QWidget::childEvent(ev);
}

void OverlayLayer::paintEvent(QPaintEvent*)
{
    if (m_blocking.isEmpty())
        return;
    QPainter p(this);
    const ThemePtr t = currentTheme();
    p.fillRect(rect(), t->color(QStringLiteral("--background-modifier-cover"), QColor(10, 10, 10, 100)));
}

// ---------------------------------------------------------------------------------------------

NoticeManager::NoticeManager(OverlayLayer* layer)
    : QObject(layer)
    , m_layer(layer)
{
    layer->installEventFilter(this);
}

NoticeManager* NoticeManager::of(const OverlayLayer* layer)
{
    return layer ? layer->findChild<NoticeManager*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

Notice* NoticeManager::show(const QString& message, int timeoutMs)
{
    auto* n = new Notice(message, m_layer);
    m_notices.push_back(n);
    connect(n, &Notice::dismissed, this, [this, n] {
        m_notices.removeAll(n);
        n->deleteLater();
        relayout();
    });
    if (timeoutMs > 0)
        QTimer::singleShot(timeoutMs, n, &Notice::dismiss);
    n->show();
    relayout();
    return n;
}

void NoticeManager::relayout()
{
    m_notices.removeIf([](const QPointer<Notice>& n) { return !n; });
    int y = 48; // below the header row
    const int right = 16;
    for (const auto& n : m_notices) {
        const QSize s = n->sizeHint();
        n->setGeometry(m_layer->width() - right - s.width(), y, s.width(), s.height());
        y += s.height() + 8;
    }
    m_layer->refresh();
}

Notice::Notice(const QString& message, QWidget* parent)
    : QWidget(parent)
    , m_text(message)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::PointingHandCursor);
}

void Notice::setMessage(const QString& message)
{
    m_text = message;
    updateGeometry();
    update();
}

QSize Notice::sizeHint() const
{
    const QFontMetrics fm(font());
    const int maxW = 380;
    const QRect r = fm.boundingRect(QRect(0, 0, maxW - 28, 10000), Qt::TextWordWrap, m_text);
    return {std::min(maxW, r.width() + 28), r.height() + 20};
}

void Notice::dismiss()
{
    hide();
    emit dismissed();
}

void Notice::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    p.fillPath(path, t->color(QStringLiteral("--background-modifier-message"), QColor(0, 0, 0, 230)));
    p.setPen(QColor(0xdd, 0xdd, 0xdd));
    p.drawText(rect().adjusted(14, 10, -14, -10), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter, m_text);
}

void Notice::mousePressEvent(QMouseEvent*) { dismiss(); }

} // namespace oc
