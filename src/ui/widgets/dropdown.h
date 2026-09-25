#pragma once

#include <QList>
#include <QPair>
#include <QWidget>

namespace oc {

// Obsidian's DropdownComponent (<select class="dropdown">): 30 px high, --dropdown-background
// (--interactive-normal) with --input-shadow and --input-radius, 13 px text with 0.8em / 2.4em
// padding and the chevrons-up-down icon 0.65em from the end. As wide as the selected option (Obsidian
// measures --dropdown-fitted-width), at most 400 px. Clicking, Space, Enter or Alt+↓ open the options
// as a menu below it; ↑/↓ step through them without opening.
class Dropdown : public QWidget {
    Q_OBJECT
public:
    explicit Dropdown(QWidget* parent = nullptr);

    void addOption(const QString& value, const QString& display);
    void setOptions(const QList<QPair<QString, QString>>& options); // (value, display)
    void clearOptions();
    int count() const { return int(m_options.size()); }
    QStringList values() const;
    QString displayFor(const QString& value) const;

    QString value() const;                // "" when empty
    QString displayText() const;
    void setValue(const QString& value);  // programmatic: never emits valueChanged
    int currentIndex() const { return m_index; }

    // A user choice (menu, keys, tests): emits valueChanged when the value changes.
    void choose(const QString& value);
    void showPopup();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valueChanged(const QString& value);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void changeEvent(QEvent*) override;

private:
    QFont textFont() const;
    QList<QPair<QString, QString>> m_options;
    int m_index = -1;
    bool m_hover = false;
    bool m_focusVisible = false;
};

} // namespace oc
