#pragma once

#include <QPointer>
#include <QRect>
#include <QWidget>

namespace oc {

class MainWindow;
class NoticeManager;
class OverlayLayer;
class WindowControls;
class WindowFrameController;

// A pop-out window (Obsidian's "floating" workspace windows, body.is-popout-window): a separate
// top-level window showing one root split of tab groups — no ribbon, no sidedocks, no status bar.
// With the frameless ("hidden") frame the top tab header rows double as the title bar and the window
// buttons sit in the top-right corner, like the main window. It has its own overlay layer (modals,
// suggest popups, page previews, notices open in the focused window).
//
// The workspace creates, fills (setContent) and destroys pop-outs; a pop-out never deletes itself.
// It is parentless (its own taskbar entry, not kept above the main window) and carries the dynamic
// property "ocOwnerWindow" (the MainWindow, as QObject*) so app-wide helpers (HotkeyDispatcher,
// MainWindow::of, OverlayLayer::focused) treat it as part of that window.
class PopoutWindow : public QWidget {
    Q_OBJECT
public:
    // `main` may be null (workspace tests without a main window).
    PopoutWindow(MainWindow* main, bool frameless);
    ~PopoutWindow() override;

    MainWindow* mainWindow() const { return m_main; }
    OverlayLayer* overlay() const { return m_overlay; }
    NoticeManager* notices() const { return m_notices; }
    bool isFrameless() const { return m_frameless; }
    // Width kept free at the right end of the top-right tab header row: the window buttons plus
    // Obsidian's 8px container padding (--frame-right-space 126px); 0 with a native frame.
    int topRightReserve() const;

    // The window's root split widget; it fills the window. The pop-out doesn't own it.
    void setContent(QWidget* content);
    QWidget* content() const { return m_content; }

    // Normal (not maximized / minimized) geometry: frame position as reported by Qt (unreliable or
    // (0, 0) on Wayland, where windows can't place themselves) and client size.
    QRect normalGeometryForSave() const { return m_normal.isNull() ? QRect(pos(), size()) : m_normal; }

    // The pop-out showing `w` (null when `w` is in another kind of window).
    static PopoutWindow* of(const QWidget* w);

signals:
    // The user closed the window (window button, Alt+F4, workspace:close-window). The close is
    // accepted (the window hides); the workspace decides what happens to its leaves.
    void closeRequested();
    void activated();
    void geometryChanged();

protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void relayout();
    void rememberNormal();

    QPointer<MainWindow> m_main;
    bool m_frameless;
    QPointer<QWidget> m_content;
    WindowControls* m_controls = nullptr;
    WindowFrameController* m_frame = nullptr;
    OverlayLayer* m_overlay;
    NoticeManager* m_notices;
    QRect m_normal;
};

} // namespace oc
