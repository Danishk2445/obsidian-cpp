#pragma once

#include <QObject>
#include <QString>

namespace oc {

class ConfigStore;

// Everything that belongs to one open vault (one main window per vault, like Obsidian).
// Services are added as milestones land: Vault, DocumentManager, MetadataCache, CommandRegistry, ...
class VaultSession : public QObject {
    Q_OBJECT
public:
    VaultSession(QString id, QString root, QObject* parent = nullptr);
    ~VaultSession() override;

    const QString& id() const { return m_id; }
    const QString& root() const { return m_root; }
    QString name() const;

    ConfigStore* config() const { return m_config; }

private:
    QString m_id;
    QString m_root;
    ConfigStore* m_config;
};

} // namespace oc
