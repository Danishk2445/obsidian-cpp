#include "ui/shell/main_window.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/side_dock.h"
#include "ui/shell/status_bar.h"
#include "ui/shell/window_frame.h"
#include "ui/widgets/icon_button.h"

#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QPainter>
#include <QSplitter>

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

    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    m_ribbon = new Ribbon(this);
    h->addWidget(m_ribbon);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_left = new SideDock(SideDock::Side::Left, m_splitter);
    m_right = new SideDock(SideDock::Side::Right, m_splitter);
    m_left->setMinimumWidth(180);
    m_right->setMinimumWidth(180);
    auto* placeholder = new QWidget(m_splitter);
    m_splitter->insertWidget(0, m_left);
    m_splitter->insertWidget(1, placeholder);
    m_splitter->insertWidget(2, m_right);
    m_center = placeholder;
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    h->addWidget(m_splitter, 1);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        const QList<int> sizes = m_splitter->sizes();
        if (!isLeftCollapsed())
            m_leftWidth = sizes.value(0);
        if (!isRightCollapsed())
            m_rightWidth = sizes.value(2);
    });

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
    m_splitter->setSizes({m_leftWidth, 600, m_rightWidth});
    updateReserve();
}

MainWindow::~MainWindow() = default;

void MainWindow::setCenter(QWidget* center)
{
    QWidget* old = m_center;
    m_splitter->replaceWidget(1, center);
    m_center = center;
    if (old)
        old->deleteLater();
    m_splitter->setStretchFactor(1, 1);
    updateReserve();
    layoutFloating();
}

bool MainWindow::isLeftCollapsed() const { return m_left->isHidden(); }
bool MainWindow::isRightCollapsed() const { return m_right->isHidden(); }

void MainWindow::setLeftCollapsed(bool collapsed)
{
    if (collapsed == isLeftCollapsed())
        return;
    if (collapsed)
        m_leftWidth = m_left->width();
    m_left->setVisible(!collapsed);
    if (!collapsed) {
        QList<int> s = m_splitter->sizes();
        const int total = s.value(0) + s.value(1);
        s[0] = m_leftWidth;
        s[1] = std::max(200, total - m_leftWidth);
        m_splitter->setSizes(s);
    }
    m_ribbon->sidebarToggle()->setToolTip(collapsed ? tr("Expand") : tr("Collapse"));
    updateReserve();
}

void MainWindow::setRightCollapsed(bool collapsed)
{
    if (collapsed == isRightCollapsed())
        return;
    if (collapsed)
        m_rightWidth = m_right->width();
    m_right->setVisible(!collapsed);
    if (!collapsed) {
        QList<int> s = m_splitter->sizes();
        const int total = s.value(1) + s.value(2);
        s[2] = m_rightWidth;
        s[1] = std::max(200, total - m_rightWidth);
        m_splitter->setSizes(s);
    }
    m_rightToggle->setToolTip(collapsed ? tr("Expand") : tr("Collapse"));
    updateReserve();
}

int MainWindow::topRightReserve() const { return m_topRight->sizeHint().width() + 4; }

void MainWindow::updateReserve()
{
    const int reserve = topRightReserve();
    const bool centerRightmost = isRightCollapsed();
    m_right->setHeaderRightReserve(centerRightmost ? 0 : reserve);
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
                  {QStringLiteral("isMaximized"), isMaximized()},
                  {QStringLiteral("leftWidth"), m_leftWidth},
                  {QStringLiteral("rightWidth"), m_rightWidth},
                  {QStringLiteral("leftCollapsed"), isLeftCollapsed()},
                  {QStringLiteral("rightCollapsed"), isRightCollapsed()}};
    JsonFile::write(ClonePaths::vaultConfigDir(m_session->id()) + QStringLiteral("/window.json"), o);
}

void MainWindow::restoreWindowState()
{
    const QJsonObject o = JsonFile::read(ClonePaths::vaultConfigDir(m_session->id()) + QStringLiteral("/window.json"));
    if (o.isEmpty())
        return;
    const int w = o.value(QStringLiteral("width")).toInt(1200), hgt = o.value(QStringLiteral("height")).toInt(800);
    resize(std::max(400, w), std::max(300, hgt));
    m_leftWidth = o.value(QStringLiteral("leftWidth")).toInt(300);
    m_rightWidth = o.value(QStringLiteral("rightWidth")).toInt(300);
    m_splitter->setSizes({m_leftWidth, std::max(200, width() - 44 - m_leftWidth - m_rightWidth), m_rightWidth});
    setLeftCollapsed(o.value(QStringLiteral("leftCollapsed")).toBool());
    setRightCollapsed(o.value(QStringLiteral("rightCollapsed")).toBool());
    if (o.value(QStringLiteral("isMaximized")).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);
}

} // namespace oc
