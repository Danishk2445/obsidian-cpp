#include "core/config/vault_registry.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>

namespace oc {

QString VaultEntry::name() const { return QFileInfo(path).fileName(); }

VaultRegistry::VaultRegistry(QObject* parent)
    : QObject(parent)
{
}

QString VaultRegistry::normalizePath(const QString& path)
{
    QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

QString VaultRegistry::newId()
{
    auto* rng = QRandomGenerator::global();
    return QStringLiteral("%1%2")
        .arg(rng->generate(), 8, 16, QLatin1Char('0'))
        .arg(rng->generate(), 8, 16, QLatin1Char('0'));
}

void VaultRegistry::load()
{
    m_vaults.clear();
    QJsonObject root = JsonFile::read(ClonePaths::registryFile());
    const QJsonObject vaults = root.value(QStringLiteral("vaults")).toObject();
    for (auto it = vaults.begin(); it != vaults.end(); ++it) {
        const QJsonObject v = it.value().toObject();
        VaultEntry e;
        e.id = it.key();
        e.path = v.value(QStringLiteral("path")).toString();
        e.ts = qint64(v.value(QStringLiteral("ts")).toDouble());
        e.open = v.value(QStringLiteral("open")).toBool();
        if (!e.path.isEmpty())
            m_vaults.push_back(e);
    }
    root.remove(QStringLiteral("vaults"));
    m_extraJson = QJsonDocument(root).toJson(QJsonDocument::Compact);
    emit changed();
}

bool VaultRegistry::save()
{
    QJsonObject root = QJsonDocument::fromJson(m_extraJson).object();
    QJsonObject vaults;
    for (const VaultEntry& e : m_vaults) {
        QJsonObject v{{QStringLiteral("path"), e.path}, {QStringLiteral("ts"), double(e.ts)}};
        if (e.open)
            v.insert(QStringLiteral("open"), true);
        vaults.insert(e.id, v);
    }
    root.insert(QStringLiteral("vaults"), vaults);
    return JsonFile::write(ClonePaths::registryFile(), root);
}

QList<VaultEntry> VaultRegistry::vaults() const
{
    QList<VaultEntry> out = m_vaults;
    std::sort(out.begin(), out.end(), [](const VaultEntry& a, const VaultEntry& b) { return a.ts > b.ts; });
    return out;
}

const VaultEntry* VaultRegistry::byId(const QString& id) const
{
    for (const VaultEntry& e : m_vaults)
        if (e.id == id)
            return &e;
    return nullptr;
}

const VaultEntry* VaultRegistry::byPath(const QString& path) const
{
    const QString p = normalizePath(path);
    for (const VaultEntry& e : m_vaults)
        if (e.path == p)
            return &e;
    return nullptr;
}

QString VaultRegistry::touch(const QString& path, bool open)
{
    const QString p = normalizePath(path);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (VaultEntry& e : m_vaults) {
        if (e.path == p) {
            e.ts = now;
            e.open = open;
            emit changed();
            return e.id;
        }
    }
    VaultEntry e;
    e.id = newId();
    e.path = p;
    e.ts = now;
    e.open = open;
    m_vaults.push_back(e);
    emit changed();
    return e.id;
}

void VaultRegistry::setOpen(const QString& id, bool open)
{
    for (VaultEntry& e : m_vaults)
        if (e.id == id)
            e.open = open;
    emit changed();
}

void VaultRegistry::remove(const QString& id)
{
    m_vaults.removeIf([&](const VaultEntry& e) { return e.id == id; });
    emit changed();
}

QJsonValue VaultRegistry::option(const QString& key) const
{
    return QJsonDocument::fromJson(m_extraJson).object().value(key);
}

void VaultRegistry::setOption(const QString& key, const QJsonValue& value)
{
    QJsonObject o = QJsonDocument::fromJson(m_extraJson).object();
    if (value.isUndefined() || value.isNull())
        o.remove(key);
    else
        o.insert(key, value);
    m_extraJson = QJsonDocument(o).toJson(QJsonDocument::Compact);
    emit changed();
}

QList<VaultEntry> VaultRegistry::obsidianVaults()
{
    QList<VaultEntry> out;
    const QJsonObject vaults = JsonFile::read(ClonePaths::obsidianRegistryFile()).value(QStringLiteral("vaults")).toObject();
    for (auto it = vaults.begin(); it != vaults.end(); ++it) {
        const QJsonObject v = it.value().toObject();
        VaultEntry e;
        e.id = it.key();
        e.path = v.value(QStringLiteral("path")).toString();
        e.ts = qint64(v.value(QStringLiteral("ts")).toDouble());
        if (!e.path.isEmpty())
            out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const VaultEntry& a, const VaultEntry& b) { return a.ts > b.ts; });
    return out;
}

} // namespace oc
