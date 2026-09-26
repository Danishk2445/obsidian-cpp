#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace oc {

// In-window layer for modals, suggest popups, hover popovers and notices. Real top-level popups are
// avoided (Wayland can't position them; Qt::Popup steals focus from the editor). The layer only
// intercepts mouse input where its children are, unless a blocking modal is shown.
class OverlayLayer : public QWidget {
    Q_OBJECT
public:
    explicit OverlayLayer(QWidget* window);
    ~OverlayLayer() override; // deletes the overlays first, while they can still reach the layer

    // Blocking children dim the window and capture input (modals).
    void setBlocking(QWidget* child, bool blocking);
    void refresh(); // recompute mask/visibility after children changed

    // The layer of `w`'s window (the main window's or a pop-out's); null when that window has none.
    static OverlayLayer* of(const QWidget* w);
    // Obsidian opens modals and notices in the focused window: the layer of the active window when it
    // is `layer`'s window or one of its pop-outs (windows whose dynamic property "ocOwnerWindow" is
    // `layer`'s window), else `layer` itself.
    static OverlayLayer* focused(OverlayLayer* layer);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void childEvent(QChildEvent* ev) override;
    void paintEvent(QPaintEvent*) override;

private:
    QList<QPointer<QWidget>> m_blocking;
    QTimer m_refreshTimer;
    bool m_destroying = false; // ~OverlayLayer: overlays being deleted must not re-show / raise the layer
};

class Notice;

// Obsidian-style notices stacked in the top-right corner.
class NoticeManager : public QObject {
    Q_OBJECT
public:
    explicit NoticeManager(OverlayLayer* layer);
    Notice* show(const QString& message, int timeoutMs = 4500);
    void relayout();
    static NoticeManager* of(const OverlayLayer* layer); // the layer's notices (null if none)

private:
    OverlayLayer* m_layer;
    QList<QPointer<Notice>> m_notices;
};

class Notice : public QWidget {
    Q_OBJECT
public:
    Notice(const QString& message, QWidget* parent);
    void setMessage(const QString& message);
    QSize sizeHint() const override;
    void dismiss();

signals:
    void dismissed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QString m_text;
};

} // namespace oc
