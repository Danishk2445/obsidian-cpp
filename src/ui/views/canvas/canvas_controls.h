#pragma once

// The canvas' floating widgets, painted after Obsidian's CSS:
//  - CanvasControls (".canvas-controls", top right): settings / read-only lock; zoom in, reset zoom,
//    zoom to fit, zoom out; undo, redo; help.
//  - CardMenu (".canvas-card-menu", bottom center): drag (or click) to add a card, a note, media.
//  - SelectionMenu (".canvas-menu" above the selection): icon buttons built per selection, with the
//    color picker submenu (".canvas-submenu").

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

namespace oc::canvasui {

// A square icon button of the canvas widgets (hover/active colors per variant).
class CanvasButton : public QWidget {
    Q_OBJECT
public:
    enum class Variant { Control, CardMenu, Menu };
    CanvasButton(Variant variant, const QString& icon, const QString& tooltip, QWidget* parent);
    void setIcon(const QString& icon);
    QString iconName() const { return m_icon; }
    void setDisabledLook(bool disabled);
    bool disabledLook() const { return m_disabled; }
    void setActive(bool active); // ".is-active" / ".has-active-menu"
    bool isActive() const { return m_active; }
    QSize sizeHint() const override;
    void click() { emit clicked(); }

signals:
    void clicked();
    void pressed(const QPoint& globalPos);
    void dragMoved(const QPoint& globalPos);
    void dragFinished(const QPoint& globalPos, bool dragged);

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    Variant m_variant;
    QString m_icon;
    bool m_hover = false;
    bool m_disabled = false;
    bool m_active = false;
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_pressPos;
};

class CanvasControls : public QWidget {
    Q_OBJECT
public:
    explicit CanvasControls(QWidget* parent);
    void setReadOnly(bool readOnly);            // settings icon becomes a lock ("Disable read-only")
    void setHistory(bool canUndo, bool canRedo); // disabled looks
    CanvasButton* button(const QString& name) const; // "settings", "zoom-in", "reset-zoom", "zoom-to-fit", "zoom-out", "undo", "redo", "help"
    QSize sizeHint() const override;

signals:
    void triggered(const QString& name, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void relayout();
    QList<QList<CanvasButton*>> m_groups;
    QHash<QString, CanvasButton*> m_buttons;
};

class CardMenu : public QWidget {
    Q_OBJECT
public:
    explicit CardMenu(QWidget* parent);
    CanvasButton* button(const QString& kind) const; // "card", "note", "media"
    QSize sizeHint() const override;

signals:
    void clicked(const QString& kind);
    void dragMoved(const QString& kind, const QPoint& globalPos);
    void dragFinished(const QString& kind, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QList<CanvasButton*> m_buttons;
    QStringList m_kinds;
};

class SelectionMenu : public QWidget {
    Q_OBJECT
public:
    struct Item {
        QString name; // "remove", "color", "zoom-to-selection", "create-group", "align", "line-direction", "remove-label", "edit", "background"
        QString icon;
        QString tooltip;
        std::function<void(CanvasButton* button)> run;
    };
    explicit SelectionMenu(QWidget* parent);
    void setItems(const QList<Item>& items);
    CanvasButton* button(const QString& name) const;
    // The color picker (".canvas-submenu"): "" + "1".."6" + custom; `current` is highlighted.
    void toggleColorPicker(const QString& current, std::function<void(const QString&)> pick);
    bool isColorPickerOpen() const { return m_colorPicker != nullptr; }
    QWidget* colorPicker() const { return m_colorPicker; }
    QRect toolbarRect() const; // the toolbar box (without the submenu)
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void relayout();
    QList<Item> m_items;
    QList<CanvasButton*> m_buttons;
    QWidget* m_colorPicker = nullptr;
};

} // namespace oc::canvasui
