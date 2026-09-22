#pragma once

#include <QObject>
#include <QString>

namespace oc {

class ConfigStore;
class Vault;
class DocumentManager;
class CommandRegistry;
class Workspace;
class ViewRegistry;

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

    // Services (like Obsidian's `app.*`), wired by the Application when the window is created.
    Vault* vault() const { return m_vault; }
    DocumentManager* documents() const { return m_documents; }
    CommandRegistry* commands() const { return m_commands; }
    Workspace* workspace() const { return m_workspace; }
    ViewRegistry* viewRegistry() const { return m_viewRegistry; }
    void setVault(Vault* v) { m_vault = v; }
    void setDocuments(DocumentManager* d) { m_documents = d; }
    void setCommands(CommandRegistry* c) { m_commands = c; }
    void setWorkspace(Workspace* w) { m_workspace = w; }
    void setViewRegistry(ViewRegistry* r) { m_viewRegistry = r; }

private:
    Vault* m_vault = nullptr;
    DocumentManager* m_documents = nullptr;
    CommandRegistry* m_commands = nullptr;
    Workspace* m_workspace = nullptr;
    ViewRegistry* m_viewRegistry = nullptr;

    QString m_id;
    QString m_root;
    ConfigStore* m_config;
};

} // namespace oc
