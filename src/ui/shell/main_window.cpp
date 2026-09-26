#include "ui/shell/main_window.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/shell/window_frame.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/workspace.h"

#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QPainter>

namespace oc {

MainWindow::MainWindow(VaultSession* session, bool frameless, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_frameless(frameless)
{
    setObjectName(QStringLiteral("ocMainWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(session->name() + QStringLiteral(" - Obsidian C++"));
    if (frameless)
        setWindowFlag(Qt::FramelessWindowHint, true);
    setMinimumSize(400, 300);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_ribbon = new Ribbon(this);
    m_layout->addWidget(m_ribbon);
    m_placeholder = new QWidget(this); // until setWorkspace()
    m_layout->addWidget(m_placeholder, 1);

    // Top-right cluster: right sidebar toggle + window buttons.
    m_topRight = new QWidget(this);
    auto* th = new QHBoxLayout(m_topRight);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(4);
    m_rightToggle = new IconButton(QStringLiteral("sidebar-right"), tr("Expand"), m_topRight);
    m_rightToggle->setButtonSize({28, 28});
    th->addWidget(m_rightToggle, 0, Qt::AlignVCenter);
    if (frameless) {
        m_controls = new WindowControls(this);
        m_controls->setParent(m_topRight);
        th->addWidget(m_controls);
        m_frame = new WindowFrameController(this);
    } else {
        th->addSpacing(8);
    }
    m_topRight->adjustSize();

    connect(m_ribbon->sidebarToggle(), &IconButton::clicked, this, [this] { setLeftCollapsed(!isLeftCollapsed()); });
    connect(m_rightToggle, &IconButton::clicked, this, [this] { setRightCollapsed(!isRightCollapsed()); });

    m_statusBar = new StatusBar(this);
    m_overlay = new OverlayLayer(this);
    m_notices = new NoticeManager(m_overlay);

    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));

    resize(1200, 800);
    updateToggles();
    updateReserve();
}

MainWindow::~MainWindow()
{
    // Tear the workspace down first: its views may still use the session (a child of this window).
    delete m_workspace.data();
}

OverlayLayer* MainWindow::overlay() const { return OverlayLayer::focused(m_overlay); }

NoticeManager* MainWindow::notices() const
{
    OverlayLayer* layer = overlay();
    NoticeManager* n = layer != m_overlay ? NoticeManager::of(layer) : nullptr;
    return n ? n : m_notices;
}

MainWindow* MainWindow::of(const QWidget* w)
{
    QWidget* top = w ? w->window() : nullptr;
    if (!top)
        return nullptr;
    if (auto* mw = qobject_cast<MainWindow*>(top))
        return mw;
    return qobject_cast<MainWindow*>(top->property("ocOwnerWindow").value<QObject*>());
}

void MainWindow::setWorkspace(Workspace* workspace)
{
    if (m_workspace == workspace)
        return;
    delete m_workspace.data();
    m_workspace = workspace;
    if (!workspace)
        return;
    workspace->setParent(this);
    QWidget* w = workspace->widget();
    if (m_placeholder) {
        delete m_layout->replaceWidget(m_placeholder, w); // the caller owns the replaced layout item
        m_placeholder->deleteLater();
        m_placeholder = nullptr;
    } else {
        m_layout->addWidget(w, 1);
    }
    m_layout->setStretchFactor(w, 1);
    w->show();
    connect(workspace, &Workspace::layoutChanged, this, [this] {
        updateToggles();
        layoutFloating();
    });
    updateToggles();
    updateReserve();
}

bool MainWindow::isLeftCollapsed() const { return m_workspace && m_workspace->isSideCollapsed(Workspace::Side::Left); }
bool MainWindow::isRightCollapsed() const { return !m_workspace || m_workspace->isSideCollapsed(Workspace::Side::Right); }

void MainWindow::setLeftCollapsed(bool collapsed)
{
    if (m_workspace)
        m_workspace->setSideCollapsed(Workspace::Side::Left, collapsed);
    updateToggles();
}

void MainWindow::setRightCollapsed(bool collapsed)
{
    if (m_workspace)
        m_workspace->setSideCollapsed(Workspace::Side::Right, collapsed);
    updateToggles();
    updateReserve();
}

void MainWindow::updateToggles()
{
    const bool left = isLeftCollapsed(), right = isRightCollapsed();
    m_ribbon->sidebarToggle()->setToolTip(left ? tr("Expand") : tr("Collapse"));
    m_ribbon->setCollapsed(left);
    m_rightToggle->setToolTip(right ? tr("Expand") : tr("Collapse"));
}

int MainWindow::topRightReserve() const { return m_topRight->sizeHint().width() + 4; }

void MainWindow::updateReserve()
{
    const int reserve = topRightReserve();
    const bool centerRightmost = isRightCollapsed();
    emit topRightReserveChanged(centerRightmost ? reserve : 0, centerRightmost);
    layoutFloating();
}

void MainWindow::layoutFloating()
{
    const QSize tr = m_topRight->sizeHint();
    m_topRight->setGeometry(width() - tr.width(), (40 - tr.height()) / 2, tr.width(), tr.height());
    m_topRight->raise();
    m_statusBar->adjustSize();
    const QSize sb = m_statusBar->sizeHint();
    m_statusBar->setGeometry(width() - sb.width(), height() - sb.height(), sb.width(), sb.height());
    m_statusBar->raise();
    m_overlay->setGeometry(rect());
    m_overlay->refresh();
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutFloating();
}

void MainWindow::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::WindowStateChange && m_controls)
        m_controls->updateMaximizeIcon();
    QWidget::changeEvent(e);
}

void MainWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-secondary")));
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    emit closing();
    saveWindowState();
    QWidget::closeEvent(e);
}

void MainWindow::saveWindowState() const
{
    const QRect g = normalGeometry().isValid() ? normalGeometry() : geometry();
    QJsonObject o{{QStringLiteral("x"), g.x()},
                  {QStringLiteral("y"), g.y()},
                  {QStringLiteral("width"), g.width()},
                  {QStringLiteral("height"), g.height()},
                  {QStringLiteral("isMaximized"), isMaximized()}};
    JsonFile::write(ClonePaths::vaultConfigDir(m_session->id()) + QStringLiteral("/window.json"), o);
}

void MainWindow::restoreWindowState()
{
    const QJsonObject o = JsonFile::read(ClonePaths::vaultConfigDir(m_session->id()) + QStringLiteral("/window.json"));
    if (o.isEmpty())
        return;
    const int w = o.value(QStringLiteral("width")).toInt(1200), hgt = o.value(QStringLiteral("height")).toInt(800);
    resize(std::max(400, w), std::max(300, hgt));
    if (o.value(QStringLiteral("isMaximized")).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);
}

} // namespace oc
