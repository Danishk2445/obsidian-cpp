#include "ui/shell/popout_window.h"

#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/shell/window_frame.h"
#include "ui/workspace/workspace.h"

#include <QCloseEvent>
#include <QEvent>
#include <QPainter>
#include <QWindow>

namespace oc {

namespace {
constexpr int kControlsWidth = 126; // --frame-right-space (Windows / Linux)
constexpr int kHeaderPadding = 8;   // .workspace-tab-header-container padding (--size-4-2)
} // namespace

PopoutWindow::PopoutWindow(MainWindow* main, bool frameless)
    : QWidget(nullptr)
    , m_main(main)
    , m_frameless(frameless)
{
    setObjectName(QStringLiteral("ocPopoutWindow"));
    setProperty("ocOwnerWindow", QVariant::fromValue<QObject*>(main));
    if (frameless)
        setWindowFlag(Qt::FramelessWindowHint, true);
    setMinimumSize(400, 300);
    if (main)
        setWindowIcon(main->windowIcon());
    if (frameless) {
        m_controls = new WindowControls(this);
        m_frame = new WindowFrameController(this);
    }
    m_overlay = new OverlayLayer(this);
    m_notices = new NoticeManager(m_overlay);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    resize(800, 600); // Electron's default BrowserWindow size (Obsidian's openPopoutLeaf())
}

PopoutWindow::~PopoutWindow()
{
    // The overlays go first, while they can still reach this window (like MainWindow).
    delete m_overlay;
    m_overlay = nullptr;
}

int PopoutWindow::topRightReserve() const { return m_frameless ? kControlsWidth + kHeaderPadding : 0; }

void PopoutWindow::setContent(QWidget* content)
{
    if (m_content == content)
        return;
    m_content = content;
    if (content) {
        content->setParent(this);
        content->show();
    }
    relayout();
}

PopoutWindow* PopoutWindow::of(const QWidget* w) { return w ? qobject_cast<PopoutWindow*>(w->window()) : nullptr; }

bool PopoutWindow::isAlwaysOnTop() const { return windowFlags().testFlag(Qt::WindowStaysOnTopHint); }

void PopoutWindow::setAlwaysOnTop(bool on)
{
    if (isAlwaysOnTop() == on)
        return;
    Qt::WindowFlags flags = windowFlags();
    flags.setFlag(Qt::WindowStaysOnTopHint, on);
    // QWidget::setWindowFlags() would hide the window (to be shown again): tell the widget without touching
    // the window system, then change the live window's flags.
    overrideWindowFlags(flags);
    if (QWindow* handle = windowHandle())
        handle->setFlags(flags);
    emit alwaysOnTopChanged(on);
}

void PopoutWindow::relayout()
{
    if (m_content) {
        m_content->setGeometry(rect());
        m_content->lower();
    }
    if (m_controls) {
        m_controls->move(width() - m_controls->width(), 0);
        m_controls->raise();
    }
    if (m_overlay) {
        m_overlay->setGeometry(rect());
        m_overlay->refresh();
    }
}

void PopoutWindow::rememberNormal()
{
    if (windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen))
        return;
    m_normal = QRect(pos(), size());
}

void PopoutWindow::closeEvent(QCloseEvent* e)
{
    emit closeRequested();
    QWidget::closeEvent(e);
}

void PopoutWindow::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange && isActiveWindow())
        emit activated();
    if (e->type() == QEvent::WindowStateChange) {
        if (m_controls)
            m_controls->updateMaximizeIcon();
        emit geometryChanged();
    }
    QWidget::changeEvent(e);
}

void PopoutWindow::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    relayout();
    rememberNormal();
    emit geometryChanged();
}

void PopoutWindow::moveEvent(QMoveEvent* e)
{
    QWidget::moveEvent(e);
    rememberNormal();
    emit geometryChanged();
}

void PopoutWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-primary")));
}

} // namespace oc
