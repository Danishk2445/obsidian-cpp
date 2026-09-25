#include "core/metadata/property_index.h"

#include "markdown/metadata.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc {

QString inferPropertyType(const QVariant& v)
{
    if (!v.isValid() || v.isNull())
        return QStringLiteral("text");
    switch (v.typeId()) {
    case QMetaType::QString: {
        static const QRegularExpression datetime(QStringLiteral("^\\d{4}-[01]\\d-[0-3]\\dT[0-2]\\d:[0-5]\\d$"));
        static const QRegularExpression date(QStringLiteral("^\\d{4}-[01]\\d-[0-3]\\d$"));
        const QString s = v.toString();
        if (datetime.match(s).hasMatch())
            return QStringLiteral("datetime");
        if (date.match(s).hasMatch())
            return QStringLiteral("date");
        return QStringLiteral("text");
    }
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return QStringLiteral("number");
    case QMetaType::Bool:
        return QStringLiteral("checkbox");
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        for (const QVariant& e : v.toList())
            if (e.isValid() && !e.isNull() && e.typeId() != QMetaType::QString)
                return QStringLiteral("unknown");
        return QStringLiteral("multitext");
    }
    default:
        return QStringLiteral("unknown");
    }
}

QString propertyTypeIcon(const QString& type)
{
    static const QHash<QString, QString> icons{
        {QStringLiteral("aliases"), QStringLiteral("forward")},    {QStringLiteral("checkbox"), QStringLiteral("check-square")},
        {QStringLiteral("date"), QStringLiteral("calendar")},      {QStringLiteral("datetime"), QStringLiteral("clock")},
        {QStringLiteral("file"), QStringLiteral("file")},          {QStringLiteral("folder"), QStringLiteral("folder")},
        {QStringLiteral("multitext"), QStringLiteral("list")},     {QStringLiteral("property"), QStringLiteral("info")},
        {QStringLiteral("number"), QStringLiteral("binary")},      {QStringLiteral("tags"), QStringLiteral("tags")},
        {QStringLiteral("text"), QStringLiteral("text")},
    };
    return icons.value(type.toLower(), QStringLiteral("file-question"));
}

const QHash<QString, QPair<QString, QString>>& reservedPropertyTypes()
{
    static const QHash<QString, QPair<QString, QString>> reserved{
        {QStringLiteral("aliases"), {QStringLiteral("aliases"), QStringLiteral("aliases")}},
        {QStringLiteral("cssclasses"), {QStringLiteral("cssclasses"), QStringLiteral("multitext")}},
        {QStringLiteral("tags"), {QStringLiteral("tags"), QStringLiteral("tags")}},
    };
    return reserved;
}

QHash<QString, PropertyInfo> collectProperties(const QList<const md::CachedMetadata*>& caches, const QHash<QString, QPair<QString, QString>>& assigned)
{
    QHash<QString, PropertyInfo> out;
    for (auto it = assigned.cbegin(); it != assigned.cend(); ++it)
        out.insert(it.key(), PropertyInfo{it->first, it->second, 0});
    for (const md::CachedMetadata* meta : caches) {
        if (!meta || !meta->frontmatter)
            continue;
        for (const md::FrontmatterValue& v : *meta->frontmatter) {
            const QString key = v.key.toLower();
            const bool hasValue = v.value.isValid() && !v.value.isNull();
            auto found = out.find(key);
            if (found != out.end()) {
                ++found->occurrences;
                if (found->type.isEmpty() && hasValue)
                    found->type = inferPropertyType(v.value);
            } else {
                out.insert(key, PropertyInfo{v.key, hasValue ? inferPropertyType(v.value) : QString(), 1});
            }
        }
    }
    for (PropertyInfo& p : out)
        if (p.type.isEmpty())
            p.type = QStringLiteral("text");
    return out;
}

void sortProperties(QList<PropertyInfo>& list, CountSortOrder order)
{
    std::stable_sort(list.begin(), list.end(), [order](const PropertyInfo& a, const PropertyInfo& b) {
        switch (order) {
        case CountSortOrder::Alphabetical: return naturalTagCompare(a.name, b.name) < 0;
        case CountSortOrder::AlphabeticalReverse: return naturalTagCompare(a.name, b.name) > 0;
        case CountSortOrder::Frequency:
        case CountSortOrder::FrequencyReverse:
            if (a.occurrences != b.occurrences)
                return order == CountSortOrder::Frequency ? a.occurrences > b.occurrences : a.occurrences < b.occurrences;
            return naturalTagCompare(a.name.toLower(), b.name.toLower()) < 0;
        }
        return false;
    });
}

} // namespace oc
