#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QTimer>

namespace oc {

// Per-vault configuration: a directory of Obsidian-shaped JSON files (app.json, appearance.json,
// hotkeys.json, graph.json, ...) living in ~/.config/obsidian-cpp/vaults/<id>/ — never in the vault.
// Reads are cached; writes are debounced (500 ms) and only touch files whose content changed.
class ConfigStore : public QObject {
    Q_OBJECT
public:
    explicit ConfigStore(QString dir, QObject* parent = nullptr);
    ~ConfigStore() override;

    const QString& dir() const { return m_dir; }

    // file is the base name without ".json", e.g. "app", "appearance", "plugins/greentext-guard".
    QJsonObject object(const QString& file) const;
    QJsonValue value(const QString& file, const QString& key, const QJsonValue& fallback = {}) const;

    void setObject(const QString& file, const QJsonObject& obj);
    // Setting an undefined/null value removes the key (Obsidian stores only non-default keys).
    void setValue(const QString& file, const QString& key, const QJsonValue& v);

    // Write pending changes now (called on quit).
    void flush();

signals:
    void changed(const QString& file, const QString& key);

private:
    QString path(const QString& file) const;

    QString m_dir;
    mutable QHash<QString, QJsonObject> m_cache;
    QSet<QString> m_dirty;
    QTimer m_saveTimer;
};

} // namespace oc
