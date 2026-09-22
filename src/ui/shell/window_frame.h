#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

namespace oc {

class IconButton;

// Min / max / close buttons for the frameless (Obsidian "hidden" frame) window.
class WindowControls : public QWidget {
    Q_OBJECT
public:
    explicit WindowControls(QWidget* window);
    void updateMaximizeIcon();

private:
    QWidget* m_window;
    IconButton* m_min;
    IconButton* m_max;
    IconButton* m_close;
};

// Implements move/resize for a frameless window: 6px edge bands start a system resize; presses on
// widgets flagged with the dynamic property "ocDragRegion" (empty header space) start a system move;
// double-click there toggles maximize.
class WindowFrameController : public QObject {
    Q_OBJECT
public:
    explicit WindowFrameController(QWidget* window);
    void setEnabled(bool on) { m_enabled = on; }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    Qt::Edges edgesAt(const QPoint& windowPos) const;
    QWidget* m_window;
    bool m_enabled = true;
    bool m_cursorOverridden = false;
};

} // namespace oc
