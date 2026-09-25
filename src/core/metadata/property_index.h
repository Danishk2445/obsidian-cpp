#pragma once

// Obsidian's "All properties" model (core plugin "properties", view "all-properties"): every property
// name used in the vault's frontmatter with the number of notes using it and its type (the assigned
// one from types.json, else inferred from the first non-null value), merged case-insensitively.

#include "core/metadata/tag_tree.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>

namespace oc::md {
struct CachedMetadata;
}

namespace oc {

struct PropertyInfo {
    QString name;        // first spelling seen (or the assigned one)
    QString type;        // "text", "multitext", "number", "checkbox", "date", "datetime", "aliases", "tags", ...
    int occurrences = 0; // notes whose frontmatter has the key
};

// Obsidian's type inference for a value: null -> text, "YYYY-MM-DD" -> date, "YYYY-MM-DDTHH:mm" ->
// datetime, other strings -> text, numbers -> number, booleans -> checkbox, lists of strings ->
// multitext, anything else -> unknown.
QString inferPropertyType(const QVariant& value);

// The widget icon of a property type (Lucide name).
QString propertyTypeIcon(const QString& type);

// Types every vault has (aliases, cssclasses, tags), lowercase name -> {name, type}.
const QHash<QString, QPair<QString, QString>>& reservedPropertyTypes();

// Counts over `caches`; `assigned` (lowercase name -> {name, type}, e.g. types.json plus the reserved
// ones) are listed even when no note uses them. Keyed by lowercase name.
QHash<QString, PropertyInfo> collectProperties(const QList<const md::CachedMetadata*>& caches, const QHash<QString, QPair<QString, QString>>& assigned);

// Obsidian's orders: by name (natural), or by occurrences (ties by lowercase name).
void sortProperties(QList<PropertyInfo>& list, CountSortOrder order);

} // namespace oc
