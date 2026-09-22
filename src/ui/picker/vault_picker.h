#pragma once

#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QLineEdit;

namespace oc {

class VaultRegistry;

// Obsidian's "starter" window: recent vaults on the left, open/create options on the right.
class VaultPicker : public QWidget {
    Q_OBJECT
public:
    explicit VaultPicker(VaultRegistry* registry, QWidget* parent = nullptr);

signals:
    void openVault(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void rebuildList();
    void createVault();
    void openFolder();
    void showItemMenu(const QPoint& pos);

    VaultRegistry* m_registry;
    QListWidget* m_list;
    QLineEdit* m_newName;
    QLineEdit* m_newLocation;
};

} // namespace oc
