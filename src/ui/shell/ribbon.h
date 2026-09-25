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
    QList<QPair<QString, IconButton*>> actions() const { return m_actions; }
    // The left sidedock is collapsed: the ribbon takes --ribbon-background-collapsed.
    void setCollapsed(bool collapsed);

signals:
    void actionTriggered(const QString& id);

protected:
    void paintEvent(QPaintEvent*) override;
    void changeEvent(QEvent*) override;

private:
    IconButton* makeButton(const QString& id, const QString& icon, const QString& title);

    IconButton* m_toggle;
    QVBoxLayout* m_actionsLayout;
    QVBoxLayout* m_bottomLayout;
    QList<QPair<QString, IconButton*>> m_actions;
    bool m_collapsed = false;
    QSet<QString> m_hidden;
    QSet<QString> m_unavailable;
};

} // namespace oc
