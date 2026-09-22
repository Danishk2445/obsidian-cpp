#pragma once

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
    void setActionHidden(const QString& id, bool hidden);
    QList<QPair<QString, IconButton*>> actions() const { return m_actions; }

signals:
    void actionTriggered(const QString& id);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    IconButton* makeButton(const QString& id, const QString& icon, const QString& title);

    IconButton* m_toggle;
    QVBoxLayout* m_actionsLayout;
    QVBoxLayout* m_bottomLayout;
    QList<QPair<QString, IconButton*>> m_actions;
};

} // namespace oc
