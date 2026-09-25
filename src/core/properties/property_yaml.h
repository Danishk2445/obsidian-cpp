#pragma once

// Obsidian's frontmatter serializer. Obsidian 1.13 writes properties with the `yaml` library's
// stringify(object, {nullStr: "", lineWidth: 0}): block mappings, sequences indented by two spaces
// ("key:\n  - a"), plain scalars unless they would read back as something else (then double quotes,
// or single quotes when the text contains '"' but no '\''), block literals ("|-", "|", "|+") for
// multi-line strings, JavaScript number formatting, "key:" for null. This reproduces that output
// (behaviour only; see tests/core/properties_yaml_test.cpp for the reference vectors).
//
// Values are QVariants as produced by md::parseFrontmatter(): QString, double (any numeric type),
// bool, QVariantList / QStringList, QVariantMap, and an invalid/null QVariant for null.

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace oc::props {

// A JavaScript Number's toString(): shortest round-trip digits, exponent form below 1e-6 and from
// 1e21 ("1e+21"), "-0" for negative zero, ".nan" / ".inf" / "-.inf" for the non-finite values.
QString jsNumber(double v);

// A top-level mapping key as Obsidian writes it: plain ("title", "my key"), or quoted when it would
// not read back as the same string ("\"#tag\"", "\"1\"", "\"a: b\"", "\"\"").
QString yamlKey(const QString& key);

// What follows "key:" in Obsidian's output for `value` at the top level: "" (null), " text",
// " []", " |-\n  line1\n  line2", "\n  - a\n  - b", "\n  sub: 1". Lines are '\n'-separated.
QString yamlPairValue(const QVariant& value);

// The whole "key: value" text of one top-level entry ('\n'-separated lines, no final newline).
QString yamlEntry(const QString& key, const QVariant& value);

// A whole frontmatter body (the text between the --- lines, no final newline), Obsidian's
// bL(object).trim(); empty for no entries.
QString yamlDocument(const QList<QPair<QString, QVariant>>& entries);

// A scalar value as a single-line YAML sequence item text ("a", "\"#t\"", "1"); empty optional when
// the value needs more than one line (block scalars, collections).
QString yamlInlineItem(const QVariant& value, bool* singleLine);

// JSON text of a value (Obsidian's JSON.stringify for the "unknown" property widget).
QString jsonText(const QVariant& value);

} // namespace oc::props
