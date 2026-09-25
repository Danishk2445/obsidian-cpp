#include "core/properties/property_types.h"

#include "core/config/config_store.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/property_yaml.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"

#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>

namespace oc {

namespace props {

namespace {

bool isNumeric(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

bool isList(const QVariant& v) { return v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList; }
bool isMap(const QVariant& v) { return v.typeId() == QMetaType::QVariantMap || v.typeId() == QMetaType::QVariantHash; }

// A string, or a list of strings (nulls ignored): Obsidian's validate for list widgets.
bool isStringOrStringList(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString)
        return true;
    if (!isList(v))
        return false;
    for (const QVariant& e : v.toList())
        if (!isNullProperty(e) && e.typeId() != QMetaType::QString)
            return false;
    return true;
}

// JS falsiness for the date validators (`!value`).
bool isFalsyValue(const QVariant& v)
{
    if (isNullProperty(v))
        return true;
    if (v.typeId() == QMetaType::Bool)
        return !v.toBool();
    if (v.typeId() == QMetaType::QString)
        return v.toString().isEmpty();
    if (isNumeric(v))
        return v.toDouble() == 0 || std::isnan(v.toDouble());
    return false;
}

// The digit runs moment's non-strict parser would pick for a format of numeric tokens: each token
// takes the first run of up to `maxDigits[i]` digits after the previous one.
QList<int> digitRuns(const QString& text, const QList<int>& maxDigits)
{
    QList<int> out;
    int i = 0;
    for (int max : maxDigits) {
        while (i < text.size() && !text[i].isDigit())
            ++i;
        if (i >= text.size())
            break;
        int j = i;
        while (j < text.size() && j - i < max && text[j].isDigit() && text[j].unicode() < 128)
            ++j;
        out << text.mid(i, j - i).toInt();
        i = j;
    }
    return out;
}

} // namespace

const QList<PropertyType>& propertyTypes()
{
    static const QList<PropertyType> types{
        {QStringLiteral("aliases"), QObject::tr("Aliases"), QStringLiteral("forward"), {QStringLiteral("aliases")}, true},
        {QStringLiteral("checkbox"), QObject::tr("Checkbox"), QStringLiteral("check-square"), {}, true},
        {QStringLiteral("date"), QObject::tr("Date"), QStringLiteral("calendar"), {}, true},
        {QStringLiteral("datetime"), QObject::tr("Date & time"), QStringLiteral("clock"), {}, true},
        {QStringLiteral("file"), QObject::tr("File"), QStringLiteral("file"), {}, false},
        {QStringLiteral("folder"), QObject::tr("Folder"), QStringLiteral("folder"), {}, false},
        {QStringLiteral("multitext"), QObject::tr("List"), QStringLiteral("list"), {}, true},
        {QStringLiteral("property"), QObject::tr("Property"), QStringLiteral("info"), {}, false},
        {QStringLiteral("number"), QObject::tr("Number"), QStringLiteral("binary"), {}, true},
        {QStringLiteral("tags"), QObject::tr("Tags"), QStringLiteral("tags"), {QStringLiteral("tags")}, true},
        {QStringLiteral("text"), QObject::tr("Text"), QStringLiteral("text"), {}, true},
        {QStringLiteral("unknown"), QObject::tr("Unknown"), QStringLiteral("file-question"), {}, false},
    };
    return types;
}

const PropertyType& propertyType(const QString& id)
{
    const QList<PropertyType>& all = propertyTypes();
    const QString lower = id.toLower();
    for (const PropertyType& t : all)
        if (t.id == lower)
            return t;
    return all.last();
}

QList<PropertyType> menuPropertyTypes(const QString& key)
{
    QList<PropertyType> out;
    for (const PropertyType& t : propertyTypes()) {
        if (!t.offered)
            continue;
        if (!t.reservedKeys.isEmpty() && !t.reservedKeys.contains(key))
            continue;
        out << t;
    }
    return out;
}

bool validatePropertyValue(const QString& type, const QVariant& v)
{
    const QString t = type.toLower();
    if (t == u"text" || t == u"file" || t == u"folder" || t == u"property")
        return v.typeId() == QMetaType::QString;
    if (t == u"multitext" || t == u"tags" || t == u"aliases")
        return isStringOrStringList(v);
    if (t == u"number")
        return isNumeric(v) && !std::isnan(v.toDouble());
    if (t == u"checkbox")
        return v.typeId() == QMetaType::Bool;
    if (t == u"date")
        return isFalsyValue(v) || (v.typeId() == QMetaType::QString && parseLenientDate(v.toString()).has_value());
    if (t == u"datetime")
        return isFalsyValue(v) || (v.typeId() == QMetaType::QString && parseLenientDateTime(v.toString()).has_value());
    return true; // unknown
}

bool isNullProperty(const QVariant& v) { return !v.isValid() || v.typeId() == QMetaType::Nullptr; }

QString propertyValueText(const QVariant& v)
{
    if (isNullProperty(v))
        return QString();
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (isNumeric(v)) {
        const double d = v.toDouble();
        if (std::isnan(d))
            return QStringLiteral("NaN");
        if (std::isinf(d))
            return d < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
        return jsNumber(d);
    }
    if (isList(v)) {
        QStringList parts;
        for (const QVariant& e : v.toList())
            parts << propertyValueText(e);
        return parts.join(u',');
    }
    if (isMap(v))
        return jsonText(v);
    return v.toString();
}

QStringList propertyValueList(const QVariant& v)
{
    if (isNullProperty(v) || (v.typeId() == QMetaType::QString && v.toString().isEmpty()))
        return {};
    if (isList(v)) {
        QStringList out;
        for (const QVariant& e : v.toList())
            if (!isNullProperty(e))
                out << propertyValueText(e);
        return out;
    }
    return {propertyValueText(v)};
}

QVariant normalizePropertyValue(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString && v.toString().isEmpty())
        return QVariant();
    if (isList(v) && v.toList().isEmpty())
        return QVariant();
    return v;
}

std::optional<QDate> parseLenientDate(const QString& text)
{
    const QList<int> runs = digitRuns(text, {4, 2, 2});
    if (runs.isEmpty())
        return std::nullopt;
    const QDate d(runs[0], runs.value(1, 1), runs.value(2, 1));
    if (!d.isValid())
        return std::nullopt;
    return d;
}

std::optional<QDateTime> parseLenientDateTime(const QString& text)
{
    const QList<int> runs = digitRuns(text, {4, 2, 2, 2, 2, 2});
    if (runs.isEmpty())
        return std::nullopt;
    const QDate d(runs[0], runs.value(1, 1), runs.value(2, 1));
    const QTime t(runs.value(3, 0), runs.value(4, 0), runs.value(5, 0));
    if (!d.isValid() || !t.isValid())
        return std::nullopt;
    return QDateTime(d, t);
}

} // namespace props

// -------------------------------------------------------------------------------------------------
// PropertyTypeManager
// -------------------------------------------------------------------------------------------------
PropertyTypeManager::PropertyTypeManager(ConfigStore* config, MetadataCache* cache, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_cache(cache)
{
    reload();
    if (cache) {
        connect(cache, &MetadataCache::changed, this, [this] { invalidate(); });
        connect(cache, &MetadataCache::deleted, this, [this] { invalidate(); });
        connect(cache, &MetadataCache::resolved, this, [this] { invalidate(); });
    }
}

PropertyTypeManager* PropertyTypeManager::of(VaultSession* session)
{
    if (!session)
        return nullptr;
    if (auto* m = session->findChild<PropertyTypeManager*>(QString(), Qt::FindDirectChildrenOnly))
        return m;
    return new PropertyTypeManager(session->config(), session->metadata(), session);
}

bool PropertyTypeManager::isReserved(const QString& key) { return reservedPropertyTypes().contains(key.toLower()); }

QString PropertyTypeManager::assignedType(const QString& key) const
{
    const auto it = m_assigned.constFind(key.toLower());
    return it == m_assigned.cend() ? QString() : it->second;
}

void PropertyTypeManager::reload()
{
    const QHash<QString, QPair<QString, QString>> old = m_assigned;
    m_assigned.clear();
    if (m_config) {
        const QJsonObject types = m_config->object(QStringLiteral("types")).value(QStringLiteral("types")).toObject();
        for (auto it = types.begin(); it != types.end(); ++it) {
            const QString lower = it.key().toLower();
            if (!m_assigned.contains(lower) && it.value().isString())
                m_assigned.insert(lower, {it.key(), it.value().toString()});
        }
    }
    const auto& reserved = reservedPropertyTypes();
    for (auto it = reserved.cbegin(); it != reserved.cend(); ++it)
        m_assigned.insert(it.key(), it.value());
    invalidate();
    QSet<QString> keys;
    for (auto it = old.cbegin(); it != old.cend(); ++it)
        keys.insert(it.key());
    for (auto it = m_assigned.cbegin(); it != m_assigned.cend(); ++it)
        keys.insert(it.key());
    for (const QString& k : std::as_const(keys))
        if (old.value(k).second != m_assigned.value(k).second)
            emit changed(k);
}

void PropertyTypeManager::save()
{
    if (!m_config)
        return;
    QJsonObject types;
    for (auto it = m_assigned.cbegin(); it != m_assigned.cend(); ++it)
        types.insert(it->first, it->second);
    m_config->setObject(QStringLiteral("types"), QJsonObject{{QStringLiteral("types"), types}});
}

void PropertyTypeManager::setType(const QString& key, const QString& type)
{
    const QString lower = key.toLower();
    if (key.isEmpty() || isReserved(key))
        return;
    auto it = m_assigned.find(lower);
    if (it != m_assigned.end()) {
        if (it->second == type)
            return;
        it->second = type;
    } else {
        m_assigned.insert(lower, {key, type});
    }
    save();
    invalidate();
    emit changed(lower);
}

void PropertyTypeManager::unsetType(const QString& key)
{
    const QString lower = key.toLower();
    if (isReserved(key) || !m_assigned.remove(lower))
        return;
    save();
    invalidate();
    emit changed(lower);
}

PropertyTypeManager::TypeInfo PropertyTypeManager::typeInfo(const QString& key, const QVariant& value) const
{
    const QString assigned = assignedType(key);
    const bool known = !assigned.isEmpty() && props::propertyType(assigned).id != u"unknown";
    if (known && (props::isNullProperty(value) || props::validatePropertyValue(assigned, value)))
        return {assigned, assigned};
    if (props::isNullProperty(value)) {
        const QString vaultType = propertyInfo(key).type;
        if (props::propertyType(vaultType).id != u"unknown")
            return {known ? assigned : vaultType, vaultType};
    }
    const QString inferred = inferPropertyType(value);
    return {known ? assigned : inferred, inferred};
}

const QHash<QString, PropertyInfo>& PropertyTypeManager::allProperties() const
{
    if (!m_propertiesValid) {
        QList<const md::CachedMetadata*> caches;
        if (m_cache)
            for (const QString& path : m_cache->markdownFiles())
                caches << m_cache->fileCache(path);
        m_properties = collectProperties(caches, m_assigned);
        m_propertiesValid = true;
    }
    return m_properties;
}

PropertyInfo PropertyTypeManager::propertyInfo(const QString& key) const
{
    const auto& all = allProperties();
    const auto it = all.constFind(key.toLower());
    if (it != all.cend())
        return *it;
    return PropertyInfo{key, QStringLiteral("text"), 0};
}

QStringList PropertyTypeManager::valuesForKey(const QString& key) const
{
    if (key.isEmpty() || !m_cache)
        return {};
    auto usable = [](const QString& s) { return !s.trimmed().isEmpty() && !s.contains(u'\n') && !(s.size() > 100 && s.contains(u' ')); };
    QSet<QString> seen;
    QStringList out;
    auto add = [&](const QString& s) {
        if (usable(s) && !seen.contains(s)) {
            seen.insert(s);
            out << s;
        }
    };
    for (const QString& path : m_cache->markdownFiles()) {
        const md::CachedMetadata* meta = m_cache->fileCache(path);
        if (!meta || !meta->frontmatter)
            continue;
        for (const md::FrontmatterValue& fv : *meta->frontmatter) {
            if (fv.key != key)
                continue;
            if (fv.value.typeId() == QMetaType::QVariantList) {
                const QVariantList items = fv.value.toList();
                const bool allStrings = std::all_of(items.cbegin(), items.cend(), [](const QVariant& e) { return e.typeId() == QMetaType::QString; });
                if (allStrings)
                    for (const QVariant& e : items)
                        add(e.toString());
            } else if (fv.value.typeId() == QMetaType::QString) {
                add(fv.value.toString());
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; });
    return out;
}

} // namespace oc
