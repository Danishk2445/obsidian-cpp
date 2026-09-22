#pragma once

#include <QAbstractButton>

namespace oc {

// Obsidian's "clickable-icon": a Lucide icon with a rounded hover background. Colors follow
// --icon-color / --icon-color-hover / --icon-color-active; checked state uses the active color.
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(const QString& icon, const QString& tooltip = {}, QWidget* parent = nullptr);

    void setIconName(const QString& icon);
    QString iconName() const { return m_icon; }

    void setIconSize(int px);        // default 18 (--icon-m)
    void setButtonSize(QSize size);  // default 26x26 (icon + 4px padding each side)
    void setHoverColor(const QColor& bg) { m_hoverBg = bg; update(); } // e.g. red for window close
    void setHoverIconColor(const QColor& c) { m_hoverFg = c; update(); }
    void setRadius(double r) { m_radius = r; update(); }
    void setShowBackgroundWhenChecked(bool on) { m_checkedBg = on; update(); }

    QSize sizeHint() const override { return m_size; }
    QSize minimumSizeHint() const override { return m_size; }

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QString m_icon;
    int m_iconPx = 18;
    QSize m_size{26, 26};
    QColor m_hoverBg;
    QColor m_hoverFg;
    double m_radius = 4;
    bool m_hover = false;
    bool m_checkedBg = true;
};

} // namespace oc
