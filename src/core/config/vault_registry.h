#pragma once

#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>

namespace oc {

struct VaultEntry {
    QString id;       // 16 hex chars, like Obsidian's vault ids
    QString path;     // absolute, no trailing slash
    qint64 ts = 0;    // last opened, ms since epoch
    bool open = false;

    QString name() const; // folder name
};

// Registry of vaults known to the clone: ~/.config/obsidian-cpp/obsidian-cpp.json
//   {"vaults": {"<id>": {"path": "...", "ts": 123, "open": true}}, ...global options...}
// Unknown top-level keys are preserved on save.
class VaultRegistry : public QObject {
    Q_OBJECT
public:
    explicit VaultRegistry(QObject* parent = nullptr);

    void load();
    bool save();

    // Most recently opened first.
    QList<VaultEntry> vaults() const;
    const VaultEntry* byId(const QString& id) const;
    const VaultEntry* byPath(const QString& path) const;

    // Registers (or finds) a vault for this folder, marks it opened now; returns its id.
    QString touch(const QString& path, bool open = true);
    void setOpen(const QString& id, bool open);
    void remove(const QString& id);

    // Vaults registered in the real Obsidian (read-only suggestions, never written).
    static QList<VaultEntry> obsidianVaults();

    // Global (non-vault) options stored beside the registry, e.g. "frame": "hidden"|"native".
    QJsonValue option(const QString& key) const;
    void setOption(const QString& key, const QJsonValue& value);

    static QString normalizePath(const QString& path);
    static QString newId();

signals:
    void changed();

private:
    QList<VaultEntry> m_vaults;
    QByteArray m_extraJson;         // unknown top-level keys, preserved verbatim
};

} // namespace oc
