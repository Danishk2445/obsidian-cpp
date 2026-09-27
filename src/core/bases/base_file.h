#pragma once

// The model of a .base file (Obsidian's query object), read from and written to YAML exactly like
// Obsidian 1.13 does:
//
//   filters: <filter>                  a formula string, or {and|or|not: [<filter>...]}
//   formulas: {name: "<formula>"}
//   properties: {<property id>: {displayName: "..."}}
//   summaries: {name: "<formula over values>"}  custom summary functions
//   views:
//     - type: table | cards | list
//       name: "..."
//       filters / groupBy {property, direction} / order [ids] / sort [{property, direction}] /
//       limit / summaries {id: name} / any view option (columnSize, rowHeight, image, cardSize...)
//
// Unknown keys are kept (top level: before the known keys, like Obsidian's getSerializable();
// views: after the known ones). Property ids are "note.x" / "file.x" / "formula.x"; they are
// written the short way ("x", "file.name", "formula.x"). The legacy top-level "display" map is
// migrated into properties.*.displayName, "column" sort keys into "property".
//
// toString() is Obsidian's serialization of the model; a view saves the file only when that text
// differs from the file's (so opening a base never rewrites it).

#include "core/bases/base_expr.h"
#include "core/bases/base_yaml.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace oc::bases {

// A property id split into its namespace and name ("note.status" -> note / status).
struct PropertyRef {
    QString type; // "note", "file", "formula"
    QString name;
};
PropertyRef parsePropertyId(const QString& id);           // Obsidian's qX()
QString propertyIdFromWritten(const QString& written);    // pQ(): "status" -> "note.status", "file" -> "file.file"
QString writtenPropertyId(const QString& id);             // hQ(): "note.status" -> "status", "file.file" -> "file"
QString makePropertyId(const QString& type, const QString& name); // WX()
// The file properties a base offers (file.file, file.name, ... file.tags) and their labels.
const QStringList& fileProperties();
QString filePropertyLabel(const QString& id); // "file name", "" for others

struct Filter {
    enum class Kind { Expr, And, Or, Not };
    Kind kind = Kind::Expr;
    QString text;        // Expr: the formula
    FormulaPtr formula;  // Expr: parsed
    QList<Filter> children;

    static Filter expr(const QString& text);
    static Filter group(Kind kind, const QList<Filter>& children = {});
    // Obsidian's uQ(); throws BaseError-style messages as std::runtime_error text via *error.
    static std::optional<Filter> parse(const YamlNode& node, QString* error);
    YamlNode serialize() const;
    // Does the entry pass? Throws EvalError.
    bool test(Scope& scope) const;
    bool hasError() const; // an invalid formula somewhere
    std::optional<Filter> optimized() const; // Obsidian's optimize(): empty groups vanish, and(and) flattens
    static std::optional<Filter> both(const std::optional<Filter>& a, const std::optional<Filter>& b); // QX.and()
    bool operator==(const Filter& o) const;

    struct Rule {
        const Filter* rule;
        bool negated;
    };
    QList<Rule> rules() const; // Obsidian's getFilterRules(): every Expr leaf with its negation
};

struct SortSpec {
    QString property; // property id
    QString direction; // "ASC" / "DESC"
    bool operator==(const SortSpec&) const = default;
};

struct BaseView {
    QString type = QStringLiteral("table");
    QString name;
    std::optional<Filter> filters;
    std::optional<SortSpec> groupBy;
    std::optional<QStringList> order;         // property ids
    std::optional<QList<SortSpec>> sort;
    double limit = 0;                         // 0 = none
    std::optional<QList<QPair<QString, QString>>> summaries; // property id -> summary name
    QList<YamlNode::Entry> data;              // view options and unknown keys

    QStringList columns() const; // getOrder(): the order or ["file.name"]
    QList<SortSpec> sortSpecs() const { return sort.value_or(QList<SortSpec>()); }
    YamlNode option(const QString& key) const;
    void setOption(const QString& key, const YamlNode& value); // null removes
    QString summaryOf(const QString& propertyId) const;
    void setSummary(const QString& propertyId, const QString& name); // "" removes
    // Obsidian's setSortProperty(): "ASC" / "DESC" puts it first, "NONE" removes, "TOGGLE" cycles.
    void setSortProperty(const QString& propertyId, const QString& direction);
    YamlNode serialize() const;
};

struct PropertyConfig {
    QString displayName;
    QList<YamlNode::Entry> unrecognized;
    YamlNode serialize() const; // null when empty
};

class BaseFile {
public:
    // Parse a .base file (an empty text gives one table view "Table"). nullopt and *error (the
    // YAML error or Obsidian's message) when it can't be read.
    static std::optional<BaseFile> fromString(const QString& text, QString* error = nullptr);
    static std::optional<BaseFile> fromYaml(const YamlNode& root, QString* error = nullptr);

    YamlNode serialize() const;
    QString toString() const;

    // Views ("" = the first).
    const BaseView* view(const QString& name) const;
    BaseView* view(const QString& name);
    int viewIndex(const QString& name) const;
    QStringList viewNames() const;
    // A name for a new view: `base`, "base 2", "base 3", ... (Obsidian's mQ()).
    QString uniqueViewName(const QString& base) const;

    // Formulas (names looked up exactly, then case-insensitively like Obsidian's IA()).
    FormulaPtr formula(const QString& name) const;
    QStringList formulaNames() const;
    void setFormula(const QString& name, const QString& text); // adds or replaces
    void removeFormula(const QString& name); // also from the views' order / sort / summaries / groupBy

    // Display names.
    QString displayName(const QString& propertyId) const;
    void setDisplayName(const QString& propertyId, const QString& name); // "" clears
    PropertyConfig* propertyConfig(const QString& propertyId);

    // Summaries: the custom summary formula `name`, else the built-in one for values of `type`
    // (the first non-null value's type); nullptr when neither exists.
    FormulaPtr summaryFormula(const QString& name, std::optional<ValueType> type) const;
    // Built-in summary names for a value type, in Obsidian's order (Empty, Filled, Unique, then the
    // type's own: Average, Min, ... / Earliest, Latest, Range / Checked, Unchecked).
    static QStringList builtinSummaries(std::optional<ValueType> type);
    QStringList customSummaryNames() const;

    QList<YamlNode::Entry> unrecognized;
    std::optional<Filter> filters;
    QList<QPair<QString, FormulaPtr>> formulas;
    std::optional<QList<QPair<QString, FormulaPtr>>> summaryFormulas;
    QList<QPair<QString, PropertyConfig>> properties; // key as written -> config
    QList<BaseView> views;
    QString newItemFolder;
    QString newItemTemplate;
};

} // namespace oc::bases
