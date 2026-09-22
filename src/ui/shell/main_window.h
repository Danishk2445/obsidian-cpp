#pragma once

#include <QPointer>
#include <QWidget>

class QHBoxLayout;

namespace oc {

class IconButton;
class NoticeManager;
class OverlayLayer;
class Ribbon;
class StatusBar;
class VaultSession;
class WindowControls;
class WindowFrameController;
class Workspace;

// One window per vault. Layout mirrors Obsidian's .workspace:
//   [ribbon 44px][workspace: left sidedock | root split (tab groups) | right sidedock] + floating status bar
// With the default "hidden" frame the window is frameless: the 40px header rows double as the title
// bar and the window buttons sit in the top-right corner.
class MainWindow : public QWidget {
    Q_OBJECT
public:
    MainWindow(VaultSession* session, bool frameless, QWidget* parent = nullptr);
    ~MainWindow() override;

    VaultSession* session() const { return m_session; }
    Ribbon* ribbon() const { return m_ribbon; }
    StatusBar* statusBar() const { return m_statusBar; }
    OverlayLayer* overlay() const { return m_overlay; }
    NoticeManager* notices() const { return m_notices; }
    IconButton* rightSidebarToggle() const { return m_rightToggle; }

    // Places the workspace next to the ribbon. The window owns it and destroys it before anything else
    // (so views are closed while the session is still alive).
    void setWorkspace(Workspace* workspace);
    Workspace* workspace() const { return m_workspace; }

    // Sidebar collapse state lives in the workspace (workspace.json: left/right.collapsed).
    bool isLeftCollapsed() const;
    bool isRightCollapsed() const;
    void setLeftCollapsed(bool collapsed);
    void setRightCollapsed(bool collapsed);

    bool isFrameless() const { return m_frameless; }
    // Width to keep free at the right end of whichever header row is rightmost.
    int topRightReserve() const;

    void restoreWindowState();
    void saveWindowState() const;

signals:
    void topRightReserveChanged(int px, bool centerIsRightmost);
    void closing();

protected:
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void layoutFloating();
    void updateReserve();
    void updateToggles();

    VaultSession* m_session;
    bool m_frameless;
    Ribbon* m_ribbon;
    QHBoxLayout* m_layout;
    QWidget* m_placeholder;
    QPointer<Workspace> m_workspace;
    QWidget* m_topRight;
    IconButton* m_rightToggle;
    WindowControls* m_controls = nullptr;
    WindowFrameController* m_frame = nullptr;
    StatusBar* m_statusBar;
    OverlayLayer* m_overlay;
    NoticeManager* m_notices;
};

} // namespace oc
