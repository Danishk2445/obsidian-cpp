#pragma once

// Small widgets of the Bases UI:
//  - TextIconButton: Obsidian's .text-icon-button (toolbar items, "Add sort", "Add filter"): an icon,
//    an optional label and aux icon, a badge; hover / active / "has-active-menu" states;
//  - MenuPanel: the .menu.bases-toolbar-menu popup, shown in the window's overlay layer under its
//    button; closes on Escape, on a click outside it and when the button is clicked again; holds
//    a stack of pages (views list -> "Configure view");
//  - MenuRow: a .bases-toolbar-menu-item (icon, name, trailing widgets, hover), optional checkbox;
//  - SectionHeader: a .bases-toolbar-section-header (collapse triangle, title, flair count).

#include <QAbstractButton>
#include <QPointer>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QScrollArea;
class QVBoxLayout;

namespace oc::basesui {

class TextIconButton : public QAbstractButton {
    Q_OBJECT
public:
    TextIconButton(const QString& icon, const QString& label, QWidget* parent = nullptr);

    void setIconName(const QString& icon);
    QString iconName() const { return m_icon; }
    void setLabel(const QString& label);
    QString label() const { return m_label; }
    void setAuxIcon(const QString& icon); // e.g. chevrons-up-down after the label
    void setBadge(const QString& badge);  // the flair count ("" = none)
    void setLabelVisible(bool on);
    void setActive(bool on);              // .is-active: accent color
    bool isActive() const { return m_active; }
    void setMenuOpen(bool on);            // .has-active-menu: hover background
    void setMuted(bool on);               // the result count: muted small text
    void setError(bool on);
    void setFullWidth(bool on) { m_fullWidth = on; updateGeometry(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QFont labelFont() const;
    QString m_icon, m_label, m_aux, m_badge;
    bool m_labelVisible = true;
    bool m_active = false;
    bool m_menuOpen = false;
    bool m_muted = false;
    bool m_error = false;
    bool m_hover = false;
    bool m_fullWidth = false;
};

class MenuPanel : public QWidget {
    Q_OBJECT
public:
    // `anchor`: the toolbar button (the panel appears under it); `width`: fixed width (-1: content).
    explicit MenuPanel(QWidget* anchor, int width = -1);
    ~MenuPanel() override;

    // Page stack: push shows a new page; back pops (closing when the first page is popped).
    void push(QWidget* page);
    void back();
    QWidget* currentPage() const;
    int depth() const { return int(m_pages.size()); }

    void popup();
    void reposition();
    void close();
    bool isOpen() const { return isVisible(); }
    QWidget* anchor() const { return m_anchor; }

    // The open panel of an anchor (at most one panel per window is open).
    static MenuPanel* openPanel(QWidget* window);

signals:
    void closed();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QPointer<QWidget> m_anchor;
    QScrollArea* m_scroll;
    QWidget* m_holder;
    QVBoxLayout* m_holderLayout;
    QList<QPointer<QWidget>> m_pages;
    int m_width;
    bool m_closing = false;
};

class MenuRow : public QWidget {
    Q_OBJECT
public:
    MenuRow(const QString& icon, const QString& text, QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const { return m_text; }
    void setIconName(const QString& icon);
    void setIconColor(const QColor& c) { m_iconColor = c; update(); }
    void setActive(bool on);      // .mod-active: bold
    void setFaint(bool on);       // .mod-hidden
    void setDestructive(bool on); // .mod-destructive
    void setCheckable(bool on);   // a checkbox before the icon
    void setChecked(bool on);
    bool isChecked() const { return m_checked; }
    void setValueText(const QString& text); // muted text at the end (summary values)
    void setChevron(bool on);               // a chevron-right button at the end
    void setCheckMark(bool on);             // a check icon at the end (the selected option)
    void setSelected(bool on);              // keyboard selection

    QSize sizeHint() const override;

signals:
    void clicked();
    void chevronClicked();
    void checkToggled(bool checked);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRect chevronRect() const;
    QRect checkboxRect() const;
    QString m_icon, m_text, m_value;
    QColor m_iconColor;
    bool m_active = false, m_faint = false, m_destructive = false;
    bool m_checkable = false, m_checked = false, m_chevron = false, m_checkMark = false;
    bool m_hover = false, m_selected = false;
    bool m_pressed = false;
};

class SectionHeader : public QWidget {
    Q_OBJECT
public:
    SectionHeader(const QString& title, bool collapsible, QWidget* parent = nullptr);
    void setCollapsed(bool on);
    bool isCollapsed() const { return m_collapsed; }
    void setFlair(const QString& flair);
    QSize sizeHint() const override;

signals:
    void toggled(bool collapsed);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QString m_title, m_flair;
    bool m_collapsible;
    bool m_collapsed = false;
};

// A header row of a menu page: "‹ Configure view" back button, optional "⋮" and "×".
class PageHeader : public QWidget {
    Q_OBJECT
public:
    PageHeader(const QString& title, bool back, bool more, bool closeButton, QWidget* parent = nullptr);
    QWidget* moreButton() const { return m_more; }

signals:
    void backClicked();
    void moreClicked();
    void closeClicked();

private:
    QWidget* m_more = nullptr;
};

} // namespace oc::basesui
