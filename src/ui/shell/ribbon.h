#pragma once

#include <QSet>
#include <QWidget>

class QVBoxLayout;

namespace oc {

class IconButton;

// Left ribbon (44px): sidebar toggle at the top (aligned with the 40px header row), plugin actions,
// and help/settings at the bottom.
class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget* parent = nullptr);

    IconButton* sidebarToggle() const { return m_toggle; }
    IconButton* addAction(const QString& id, const QString& icon, const QString& title);
    IconButton* addBottomAction(const QString& id, const QString& icon, const QString& title);
    void setActionHidden(const QString& id, bool hidden); // the user's choice (left-ribbon.hiddenItems)
    void setActionAvailable(const QString& id, bool available); // false while its core plugin is off
    QList<QPair<QString, IconButton*>> actions() const { return m_actions; } // top items in their order, then the bottom ones
    // Order of the top (plugin) items. Obsidian's ribbon sorts by the saved order and puts items it doesn't know
    // first; dragging an item (5px threshold, like Obsidian's sortable) moves it and emits orderChanged().
    QStringList order() const;
    void setOrder(const QStringList& ids);
    // The left sidedock is collapsed: the ribbon takes --ribbon-background-collapsed.
    void setCollapsed(bool collapsed);

signals:
    void actionTriggered(const QString& id);
    void orderChanged(); // the user dragged an item

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void paintEvent(QPaintEvent*) override;
    void changeEvent(QEvent*) override;

private:
    IconButton* makeButton(const QString& id, const QString& icon, const QString& title);
    void syncActionOrder(); // m_actions follows the layout

    IconButton* m_toggle;
    QWidget* m_actionsWidget;
    QVBoxLayout* m_actionsLayout;
    IconButton* m_dragButton = nullptr;
    QPoint m_pressPos;
    bool m_dragging = false;
    QVBoxLayout* m_bottomLayout;
    QList<QPair<QString, IconButton*>> m_actions;
    bool m_collapsed = false;
    QSet<QString> m_hidden;
    QSet<QString> m_unavailable;
};

} // namespace oc
