#pragma once

// Evaluating a base over the vault (Obsidian's query controller, minus the UI):
//  - QueryContext: the combined filter (base filters AND view filters), the formulas and the "this"
//    file; Entry: one file of the vault as a row — "note.x" / bare names read its properties,
//    "file.x" its file values, "formula.x" its formulas (cached per entry, with loop detection),
//    "this" the context's file;
//  - evaluateFile(): does a file pass the filter (errors are reported like Obsidian's "Failed to
//    evaluate a filter: ..." notices, and the file is left out);
//  - ViewData: a view's rows (search, sort by the view's sort — default file name A→Z —, limit),
//    its columns (the view's order, restricted to known properties), its groups (groupBy, keys
//    compared loosely, the "None" group last) and summaries.
//
// Everything runs on the GUI thread (like Obsidian); QueryRunner (base_runner.h) slices the work.

#include "core/bases/base_file.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

namespace oc::bases {

class Entry;

class QueryContext : public std::enable_shared_from_this<QueryContext> {
public:
    static std::shared_ptr<QueryContext> create(const DataSource* ds, const BaseFile& base, const BaseView* view, const QString& thisPath);

    const DataSource* ds = nullptr;
    std::optional<Filter> filter;
    QList<QPair<QString, FormulaPtr>> formulas;
    QString thisPath;

    // The entry for "this" (null without a this file); marks the context as depending on it.
    std::shared_ptr<Entry> local();
    bool localUsed() const { return m_localUsed; }
    FormulaPtr formula(const QString& name) const;
    QStringList formulaNames() const;

private:
    std::shared_ptr<Entry> m_local;
    bool m_localUsed = false;
};

class Entry : public Scope, public std::enable_shared_from_this<Entry> {
public:
    Entry(std::shared_ptr<QueryContext> ctx, const QString& path);
    // The "this" entry of a context (holds it weakly).
    Entry(std::weak_ptr<QueryContext> ctx, const QString& path);

    const QString& path() const { return m_path; }
    const md::CachedMetadata* metadata() const;

    // Scope
    Value getByIdentifier(const QString& id) override;
    QStringList keys() override;
    const DataSource* dataSource() const override;
    std::optional<Value> formulaValue(const QString& name) override;
    QStringList formulaNames() override;

    // Obsidian's getValue(propertyId): note / file / formula values (a formula's error comes back as
    // an Error value; nullopt-like missing formulas as null). Throws EvalError. Cached.
    Value value(const QString& propertyId);
    // The same, with thrown errors turned into Error values.
    Value valueOrError(const QString& propertyId);
    Value note();
    Value file();
    QStringList propertyKeys() const;                 // frontmatter keys as written
    QVariant rawProperty(const QString& key) const;   // getRawProperty(): case-insensitive, invalid when missing
    bool hasRawProperty(const QString& key) const;

    void clearCache();

private:
    std::shared_ptr<QueryContext> context() const;

    std::shared_ptr<QueryContext> m_ctx;
    std::weak_ptr<QueryContext> m_weakCtx;
    QString m_path;
    std::optional<Value> m_note, m_file;
    QHash<QString, Value> m_formulaResults;
    QSet<QString> m_evaluating;
    QHash<QString, Value> m_values;
};
using EntryPtr = std::shared_ptr<Entry>;

// Does `path` pass the context's filter? Filter evaluation errors return false and set *error to
// Obsidian's notice text.
bool evaluateFile(const std::shared_ptr<QueryContext>& ctx, const EntryPtr& entry, QString* error = nullptr);

// Natural string order (Obsidian's Intl.Collator: numeric, case-insensitive).
int naturalCompare(const QString& a, const QString& b);

// Obsidian's sort comparator for two values of a sort key (-1 / 0 / 1 before the direction; null
// last in both directions is handled by the caller through `nullRank`).
int compareValues(const Value& a, const Value& b);

struct ViewGroup {
    Value key;         // null = no group / "None"
    bool hasKey = false;
    QList<EntryPtr> entries;
};

struct ViewData {
    QList<EntryPtr> rows;       // searched, sorted, limited
    int matched = 0;            // results before the limit (the toolbar's count uses min(matched, limit))
    QStringList allProperties;  // file.* + note.<relevant> + formula.*
    QStringList columns;        // the view's order, known properties only
    QList<ViewGroup> groups;    // one ungrouped group when the view has no groupBy
};

// The properties a base knows for its results: the file properties, note.<name> for every property
// used by a result (named as the vault writes it, via `propertyName`), formula.<name>.
QStringList allPropertiesOf(const BaseFile& base, const QList<EntryPtr>& results,
                            const std::function<QString(const QString& lowerKey)>& propertyName = {});

// Build a view's data from the matching entries (in any order).
ViewData buildViewData(const BaseFile& base, const BaseView& view, QList<EntryPtr> results, const QString& search = {},
                       const std::function<QString(const QString& lowerKey)>& propertyName = {});

// A summary of `values` (Obsidian's getSummaryValue): the custom or built-in function `name`
// applied to the list; null when there is none.
Value summaryValue(const BaseFile& base, const DataSource* ds, const QList<Value>& values, const QString& name);

// The type of the first non-null value (what built-in summaries are offered for).
std::optional<ValueType> firstValueType(const QList<Value>& values);

// Obsidian's wQ(): frontmatter (and folder) a new item should get so that the view's filters keep
// it: "status == \"done\"" -> status: done, file.inFolder("Books") -> folder "Books",
// file.hasTag("x") -> tags: [x], list columns in the order -> empty properties.
struct NewItemDefaults {
    QList<QPair<QString, QVariant>> frontmatter;
    QString folder;
};
NewItemDefaults newItemDefaults(const BaseFile& base, const BaseView* view, const DataSource* ds, const EntryPtr& entry = {},
                                const std::function<QString(const QString& key)>& inferredType = {});

// Obsidian's CL table export: tab-separated / CSV / Markdown of the view's columns.
struct TableExport {
    QStringList headers;
    QList<QStringList> rows;
    QString toTsv() const;
    QString toCsv() const;
    QString toMarkdown() const;
};
TableExport exportTable(const BaseFile& base, const ViewData& data);

} // namespace oc::bases
