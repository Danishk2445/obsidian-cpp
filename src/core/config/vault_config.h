#pragma once

#include "core/metadata/link_resolver.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>

#include <algorithm>

namespace oc {

class ConfigStore;

// Obsidian's vault.getConfig()/setConfig(): the per-vault options stored in app.json, and the
// appearance ones (theme, accent, fonts, ...) stored in appearance.json — both in the clone's config
// dir, never in the vault. Falls back to Obsidian 1.13's defaults; a value equal to its default is
// removed from the file, like Obsidian does.
class VaultConfig : public QObject {
    Q_OBJECT
public:
    explicit VaultConfig(ConfigStore* store, QObject* parent = nullptr);

    static const QJsonObject& defaults();
    static bool isAppearanceKey(const QString& key);

    QJsonValue get(const QString& key) const;
    void set(const QString& key, const QJsonValue& value);

    bool flag(const QString& key) const { return get(key).toBool(); }
    int number(const QString& key) const { return get(key).toInt(); }
    QString string(const QString& key) const { return get(key).toString(); }

    // Typed helpers for frequently used options.
    LinkResolver::LinkFormat newLinkFormat() const; // "shortest" | "relative" | "absolute"
    bool alwaysUpdateLinks() const { return flag(QStringLiteral("alwaysUpdateLinks")); }
    bool useMarkdownLinks() const { return flag(QStringLiteral("useMarkdownLinks")); }
    int tabSize() const { return std::clamp(number(QStringLiteral("tabSize")), 1, 16); }
    bool useTab() const { return flag(QStringLiteral("useTab")); }

    ConfigStore* store() const { return m_store; }

signals:
    void configChanged(const QString& key); // Obsidian's "config-changed" event

private:
    ConfigStore* m_store;
};

} // namespace oc
