#pragma once

// Obsidian's property types (the metadataTypeManager): the widget registry, types.json (the types the
// user assigned, stored in the clone's per-vault config dir via ConfigStore — never in the vault), the
// vault-wide property index (names, occurrences, inferred types) and the value suggestions.

#include "core/metadata/property_index.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>

namespace oc {

class ConfigStore;
class MetadataCache;
class VaultSession;

namespace props {

// One of Obsidian's registered property widgets.
struct PropertyType {
    QString id;               // "text", "multitext", "number", "checkbox", "date", "datetime", "tags", "aliases", ...
    QString name;             // menu label: "Text", "List", "Number", "Checkbox", "Date", "Date & time", ...
    QString icon;             // Lucide icon name
    QStringList reservedKeys; // non-empty: only offered for these keys ("tags", "aliases")
    bool offered = true;      // shown in "Property type" menus (Obsidian's file/folder/property are not)
};

// Every widget, in Obsidian's registration order (aliases, checkbox, date, datetime, file, folder,
// multitext, property, number, tags, text), plus "unknown" last.
const QList<PropertyType>& propertyTypes();
const PropertyType& propertyType(const QString& id); // unknown ids -> the "unknown" type
// The types a "Property type" menu offers for `key` (Obsidian: reserved ones only for their key).
QList<PropertyType> menuPropertyTypes(const QString& key);
// Obsidian's widget.validate(value): can the widget show this value as is?
bool validatePropertyValue(const QString& type, const QVariant& value);

// Values as the widgets see them.
bool isNullProperty(const QVariant& value);
QStringList propertyValueList(const QVariant& value);  // Obsidian's nN(): null/"" -> [], "a" -> ["a"]
QVariant normalizePropertyValue(const QVariant& value); // Obsidian's sO(): "" and [] are stored as null
QString propertyValueText(const QVariant& value);       // the text widget's String(value ?? "")
// moment(text, "YYYY-MM-DD") / the datetime formats, non-strict (Obsidian's date widgets).
std::optional<QDate> parseLenientDate(const QString& text);
std::optional<QDateTime> parseLenientDateTime(const QString& text);

} // namespace props

// Obsidian's app.metadataTypeManager for one vault. types.json lives in the clone's config dir
// ("types": {name: type}); the reserved aliases / cssclasses / tags always have their types.
class PropertyTypeManager : public QObject {
    Q_OBJECT
public:
    PropertyTypeManager(ConfigStore* config, MetadataCache* cache, QObject* parent = nullptr);
    // The session's manager (created on first use as a child of the session).
    static PropertyTypeManager* of(VaultSession* session);

    // Assigned types (types.json + the reserved ones), keyed by lowercase name: {name, type}.
    const QHash<QString, QPair<QString, QString>>& assignedTypes() const { return m_assigned; }
    QString assignedType(const QString& key) const; // "" when the type is automatic
    static bool isReserved(const QString& key);     // aliases, cssclasses, tags (case-sensitive like Obsidian)
    void setType(const QString& key, const QString& type);
    void unsetType(const QString& key);

    // Obsidian's getTypeInfo(key, value): the widget the property should use (expected) and the one its
    // value fits (inferred). They differ when the value doesn't validate for the assigned type.
    struct TypeInfo {
        QString expected;
        QString inferred;
    };
    TypeInfo typeInfo(const QString& key, const QVariant& value = QVariant()) const;

    // getAllPropertyInfos(): every property used in the vault (and the assigned ones), by lowercase name.
    const QHash<QString, PropertyInfo>& allProperties() const;
    PropertyInfo propertyInfo(const QString& key) const;
    // getFrontmatterPropertyValuesForKey(): the distinct string values (and list items) of `key` in
    // the vault, sorted; values with line breaks, blank ones and long sentences are left out.
    QStringList valuesForKey(const QString& key) const;

    void reload(); // re-read types.json

signals:
    void changed(const QString& lowerKey); // an assigned type changed (setType/unsetType/reload)

private:
    void save();
    void invalidate() { m_propertiesValid = false; }

    QPointer<ConfigStore> m_config;
    QPointer<MetadataCache> m_cache;
    QHash<QString, QPair<QString, QString>> m_assigned;
    mutable QHash<QString, PropertyInfo> m_properties;
    mutable bool m_propertiesValid = false;
};

} // namespace oc
