#pragma once

#include <QWidget>

class QHBoxLayout;
class QStackedWidget;
class QVBoxLayout;

namespace oc {

class IconButton;

// A sidebar: header row of icon tabs (Obsidian hides sidebar tab titles) + stacked views, optional
// footer (the left dock shows the vault profile there). Collapsing is handled by MainWindow.
class SideDock : public QWidget {
    Q_OBJECT
public:
    enum class Side { Left, Right };
    SideDock(Side side, QWidget* parent = nullptr);

    Side side() const { return m_side; }
    int addView(const QString& type, const QString& icon, const QString& title, QWidget* view);
    int indexOf(const QString& type) const;
    QWidget* view(const QString& type) const;
    void setCurrent(int index);
    void setCurrent(const QString& type);
    int current() const;
    QString currentType() const;

    void setFooter(QWidget* footer);
    // Space reserved at the right end of the header (window controls live there when this is the
    // rightmost header).
    void setHeaderRightReserve(int px);
    QWidget* header() const { return m_header; }

    int preferredWidth() const { return m_preferredWidth; }
    void setPreferredWidth(int w) { m_preferredWidth = w; }

signals:
    void currentChanged(const QString& type);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Side m_side;
    QWidget* m_header;
    QHBoxLayout* m_tabsLayout;
    QWidget* m_reserve;
    QStackedWidget* m_stack;
    QVBoxLayout* m_root;
    QWidget* m_footer = nullptr;
    QList<QPair<QString, IconButton*>> m_tabs;
    int m_preferredWidth = 300;
};

} // namespace oc
