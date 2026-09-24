#pragma once

#include "ui/widgets/button.h"

#include <QList>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace oc {

class ColorSwatch;
class Dropdown;
class IconButton;
class SearchInput;
class ToggleSwitch;
class ValueSlider;

// Obsidian's Setting (.setting-item): an info column (name, description) and a right-aligned control
// row (.setting-item-control, 8 px gaps) holding toggles, dropdowns, inputs, buttons and extra icon
// buttons in the order they were added — the builder methods mirror Obsidian's Setting API
// (addToggle, addDropdown, addText, addSlider, addButton, addExtraButton, addColorPicker). Rows sit in
// a SettingGroup card, which paints the background and the dividers; the row itself is transparent.
//
// Kinds: Normal (controls top-aligned with the name, like 1.13's align-items: flex-start), Heading
// (.setting-item-heading: 15 px semibold name outside the card), Navigable (.mod-navigable: a display
// value and a chevron, the whole row opens a page), Action (.mod-action: accent-coloured name, the row
// is a button) and EmptyState (.mod-empty-state: muted 13 px text).
class SettingItem : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Normal, Heading, Navigable, Action, EmptyState };

    explicit SettingItem(QWidget* parent = nullptr);

    void setKind(Kind kind);
    Kind kind() const { return m_kind; }

    void setName(const QString& name);
    QString name() const;
    void setNameFamily(const QString& family); // font chooser rows show the name in the font
    void setDesc(const QString& text);         // plain text
    void setRichDesc(const QString& html);     // Qt rich text: <b>, <br>, <a href>, <code>, <span class=warning>
    QString desc() const;                      // plain text of the description
    void setDescWarning(bool warning);         // descEl.mod-warning (--text-warning)
    void setIcon(const QString& icon);         // .setting-item-icon (list rows); "" hides it
    QString icon() const { return m_iconName; }
    void setPadding(int x, int y);             // --setting-items-padding-x / -y (set by the group)

    // .is-disabled: controls are disabled; action/navigable rows ignore clicks and fade.
    void setDisabled(bool disabled);
    bool isDisabledItem() const { return m_disabled; }
    // .setting-item-error: an orange message under the controls ("" clears it).
    void setErrorMessage(const QString& message);
    QString errorMessage() const;

    // Navigable rows: the value shown before the chevron and an optional warning status icon.
    void setDisplayValue(const QString& value, bool warning = false);
    QString displayValue() const;
    // Navigable / action rows: what a click (or Enter) does.
    void setClickHandler(std::function<void()> onClick) { m_onClick = std::move(onClick); }
    void activate(); // runs the click handler (tests, keyboard)

    // Controls, appended to the control row.
    ToggleSwitch* addToggle(bool value, std::function<void(bool)> onChange = {});
    Dropdown* addDropdown(const QList<QPair<QString, QString>>& options, const QString& value,
                          std::function<void(const QString&)> onChange = {});
    QLineEdit* addText(const QString& value, const QString& placeholder = {}, std::function<void(const QString&)> onChange = {});
    ValueSlider* addSlider(double min, double max, double step, double value, std::function<void(double)> onChange = {});
    Button* addButton(const QString& text, Button::Style style = Button::Style::Normal, std::function<void()> onClick = {});
    IconButton* addExtraButton(const QString& icon, const QString& tooltip, std::function<void()> onClick = {});
    ColorSwatch* addColorPicker(const QColor& value, std::function<void(const QColor&)> onChange = {});
    void addControl(QWidget* widget); // any widget (hotkey chips, file pickers, ...)
    void clearControls();

    QWidget* controlEl() const { return m_control; }
    QList<QWidget*> controls() const;
    template <typename T> T* control() const
    {
        for (QWidget* w : controls())
            if (auto* t = qobject_cast<T*>(w))
                return t;
        return nullptr;
    }

    // Briefly highlights the row (search result navigation).
    void flash();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void applyStyle();
    void updateControlState();

    Kind m_kind = Kind::Normal;
    QHBoxLayout* m_layout;
    QLabel* m_iconLabel;
    QWidget* m_info;
    QLabel* m_name;
    QLabel* m_desc;
    QWidget* m_controlColumn;
    QWidget* m_control;
    QHBoxLayout* m_controlLayout;
    QLabel* m_error;
    QLabel* m_value = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_chevron = nullptr;
    QString m_iconName;
    QString m_nameText;
    QString m_descPlain;
    QString m_descHtml;
    QString m_nameFamily;
    bool m_descWarning = false;
    bool m_disabled = false;
    bool m_hover = false;
    bool m_pressed = false;
    double m_flash = 0;
    int m_padX = 20;
    int m_padY = 20;
    std::function<void()> m_onClick;
};

// Obsidian 1.13's SettingGroup (.setting-group): an optional heading row (.setting-item-heading, with
// extra buttons at its end), an optional search row (.setting-group-search: a search input and a
// control row such as the hotkey filters) and the rows in a rounded card (.setting-items:
// --setting-items-background, --setting-items-radius) separated by inset 1 px dividers. List groups
// (.mod-list) use 16 / 8 px row padding and centre their rows. At most --setting-group-max-width
// (700 px) wide. A group whose rows are all hidden hides itself.
class SettingGroup : public QWidget {
    Q_OBJECT
public:
    explicit SettingGroup(QWidget* parent = nullptr);

    void setHeading(const QString& heading);
    QString heading() const;
    SettingItem* headingItem() const { return m_heading; }
    IconButton* addHeaderButton(const QString& icon, const QString& tooltip, std::function<void()> onClick);

    void setList(bool list);
    bool isList() const { return m_list; }

    SearchInput* addSearch(const QString& placeholder);
    SearchInput* searchInput() const { return m_search; }
    QWidget* addSearchControl(); // .setting-group-search-control, under the search input

    SettingItem* addItem();
    void addItem(SettingItem* item);
    QList<SettingItem*> items() const { return m_items; }
    void clearItems();
    // .mod-empty-state row, shown while the group has no visible rows (list groups).
    void setEmptyState(const QString& text);
    SettingItem* emptyStateItem() const { return m_empty; }
    // When set, the group stays visible even if every row is hidden (lists, searches).
    void setAlwaysVisible(bool on) { m_alwaysVisible = on; }

    // Recomputes the dividers, the empty state and the group's own visibility after rows were shown
    // or hidden.
    void refresh();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QRect cardRect() const;

    QVBoxLayout* m_layout;
    SettingItem* m_heading;
    QWidget* m_searchRow = nullptr;
    QVBoxLayout* m_searchLayout = nullptr;
    SearchInput* m_search = nullptr;
    QWidget* m_card;
    QVBoxLayout* m_cardLayout;
    QList<SettingItem*> m_items;
    QSet<QObject*> m_itemObjects; // identities for eventFilter()
    SettingItem* m_empty = nullptr;
    bool m_list = false;
    bool m_alwaysVisible = false;
};

} // namespace oc
