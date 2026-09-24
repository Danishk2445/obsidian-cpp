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

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void childEvent(QChildEvent* ev) override;
    void paintEvent(QPaintEvent*) override;

private:
    QList<QPointer<QWidget>> m_blocking;
    QTimer m_refreshTimer;
};

class Notice;

// Obsidian-style notices stacked in the top-right corner.
class NoticeManager : public QObject {
    Q_OBJECT
public:
    explicit NoticeManager(OverlayLayer* layer);
    Notice* show(const QString& message, int timeoutMs = 4500);
    void relayout();

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
