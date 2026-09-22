#include "core/config/config_store.h"

#include "core/config/json_file.h"

namespace oc {

ConfigStore::ConfigStore(QString dir, QObject* parent)
    : QObject(parent)
    , m_dir(std::move(dir))
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &ConfigStore::flush);
}

ConfigStore::~ConfigStore() { flush(); }

QString ConfigStore::path(const QString& file) const { return m_dir + QLatin1Char('/') + file + QStringLiteral(".json"); }

QJsonObject ConfigStore::object(const QString& file) const
{
    auto it = m_cache.constFind(file);
    if (it != m_cache.cend())
        return *it;
    QJsonObject o = JsonFile::read(path(file));
    m_cache.insert(file, o);
    return o;
}

QJsonValue ConfigStore::value(const QString& file, const QString& key, const QJsonValue& fallback) const
{
    const QJsonObject o = object(file);
    auto it = o.constFind(key);
    return it == o.constEnd() ? fallback : *it;
}

void ConfigStore::setObject(const QString& file, const QJsonObject& obj)
{
    if (object(file) == obj)
        return;
    m_cache.insert(file, obj);
    m_dirty.insert(file);
    m_saveTimer.start();
    emit changed(file, QString());
}

void ConfigStore::setValue(const QString& file, const QString& key, const QJsonValue& v)
{
    QJsonObject o = object(file);
    if (v.isUndefined() || v.isNull()) {
        if (!o.contains(key))
            return;
        o.remove(key);
    } else {
        if (o.value(key) == v)
            return;
        o.insert(key, v);
    }
    m_cache.insert(file, o);
    m_dirty.insert(file);
    m_saveTimer.start();
    emit changed(file, key);
}

void ConfigStore::flush()
{
    m_saveTimer.stop();
    const QSet<QString> dirty = std::exchange(m_dirty, {});
    for (const QString& file : dirty)
        JsonFile::write(path(file), m_cache.value(file));
}

} // namespace oc
