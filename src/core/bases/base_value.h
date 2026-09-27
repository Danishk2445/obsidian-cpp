#pragma once

// The values of Obsidian's Bases formula language (Obsidian 1.13 behaviour, reimplemented):
// null, errors, booleans, numbers, strings and their kinds (tags, links, URLs, images, icons, HTML),
// dates (with or without a time; "relative" dates print like moment's fromNow()), durations, lists
// (and tag lists), objects (frontmatter maps, lazily converted), regular expressions, files, the
// "this" file and the formula namespace.
//
// Semantics follow Obsidian's classes: isTruthy(), toString(), strict equals() (same class) and
// looseEquals() (the == operator: JavaScript loose equality between primitives, dates by day when
// either has no time, links by the file they resolve to...), objectAccess() for "value.name"
// (string/list length, date parts, file.name / file.tags / ..., object keys), and the class
// hierarchy used by function parameters and isType() (a Link is a String, a Tag is a String, a
// TagList is a List, a RelativeDate is a Date, ThisFile is a File).
//
// Frontmatter values convert like Obsidian's note object: "[[x|y]]" strings become links, URLs
// become URL values, "2024-01-31" / "2024-01-31T10:00" strings become dates, the "tags" property a
// tag list; nested lists and maps convert the same way.

#include "core/bases/base_data.h"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <optional>

namespace oc::bases {

enum class ValueType : quint8 {
    Null,
    Error,
    Boolean,
    Number,
    String,
    Tag,
    Link,
    Url,
    Image,
    Icon,
    Html,
    Date,
    RelativeDate,
    Duration,
    List,
    TagList,
    Object,
    RegExp,
    File,
    ThisFile,
    Formulas,
};

// The classes of Obsidian's `instanceof` checks (function parameter types, isType()).
enum class ValueClass : quint8 {
    Any,       // every value
    NotNull,   // every value but null and errors
    Primitive, // booleans, numbers, strings (and their kinds)
    Null,
    Error,
    Boolean,
    Number,
    String, // strings, tags, links, URLs, images, icons, HTML
    Tag,
    Link,
    Url,
    Image,
    Icon,
    Html,
    Date, // dates and relative dates
    Duration,
    List, // lists and tag lists
    TagList,
    Object,
    RegExp,
    File, // files and "this"
    Formulas,
};

// An evaluation error (Obsidian throws Error objects; formula cells show them).
struct EvalError {
    QString message;
};

struct DurationParts {
    double years = 0, months = 0, days = 0, hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
    bool operator==(const DurationParts&) const = default;
};

class Value;
class Scope;

// A lazily converted object (frontmatter maps; object literals).
struct ObjectData {
    QStringList keys;
    mutable QList<std::shared_ptr<Value>> values; // converted values (nullptr = not yet)
    QList<QVariant> raw;                          // frontmatter values (when `frontmatter`)
    bool frontmatter = false;
    const DataSource* ds = nullptr;
    QString sourcePath; // frontmatter: the note (links resolve from it)

    int indexOf(const QString& key) const;            // exact
    int indexOfInsensitive(const QString& key) const; // Obsidian's IA(): exact first, then any case
    Value at(int index) const;
};

class Value {
public:
    Value() = default; // null

    static Value null() { return Value(); }
    static Value error(const QString& message);
    static Value boolean(bool b);
    static Value number(double n);
    static Value string(const QString& s);
    static Value tag(const QString& tag); // "#" is added when missing
    static Value link(const DataSource* ds, const QString& path, const QString& sourcePath, const Value* display = nullptr);
    static Value url(const QString& url, const Value* display = nullptr);
    static Value image(const QString& src);
    static Value icon(const QString& name);
    static Value html(const QString& html);
    static Value date(double ms, bool time);         // ms since the epoch (NaN = invalid date)
    static Value relativeDate(double ms, bool time); // prints like moment's fromNow()
    static Value duration(const DurationParts& d);
    static Value list(const QList<Value>& items);
    static Value tagList(const QStringList& tags);
    static Value object(std::shared_ptr<ObjectData> data);
    static Value regexp(const QString& pattern, const QString& flags);
    static Value file(const DataSource* ds, const QString& path);
    // `this`: a file whose members are looked up in `scope` (its entry: properties, file, formula).
    static Value thisFile(const DataSource* ds, const QString& path, std::shared_ptr<Scope> scope);
    static Value formulas(std::shared_ptr<Scope> scope, const QStringList& names);

    // Obsidian's xK(): a plain JavaScript-like value (QString, double, bool, lists, maps, null).
    static Value fromVariant(const QVariant& v);
    // vK.fromFrontMatter()'s conversion of one property value of the note at `sourcePath`.
    static Value fromFrontmatter(const DataSource* ds, const QString& sourcePath, const QString& key, const QVariant& v);
    // A note's properties as an object (lazily converted).
    static Value frontmatterObject(const DataSource* ds, const QString& sourcePath, const QList<md::FrontmatterValue>& fm);

    ValueType type() const { return m_type; }
    bool isNull() const { return m_type == ValueType::Null; }
    bool isError() const { return m_type == ValueType::Error; }
    bool is(ValueClass c) const;
    // The class name (Obsidian's constructor.toString()): "Null", "String", "Number", "Link", ...
    QString typeName() const;
    // isType(name): true for the value's class and its ancestors ("any" matches everything).
    bool isOfType(const QString& name) const;

    bool isTruthy() const;
    QString toString() const;

    // Data.
    bool boolValue() const { return m_flag; }
    double number() const { return m_num; }                 // numbers
    const QString& text() const { return m_str; }           // string kinds (link path, URL, ...), error message, regexp source
    double dateMs() const { return m_num; }                 // dates
    bool hasTime() const { return m_flag; }                 // dates
    const DurationParts& durationParts() const;            // durations
    const QList<Value>& items() const;                      // lists
    int length() const { return int(items().size()); }
    const ObjectData* objectData() const;                   // objects
    const Value* display() const;                           // links / URLs (nullptr = none)
    QString linkSourcePath() const;                         // links
    const DataSource* dataSource() const;                   // links / files
    const QString& filePath() const { return m_str; }       // files
    const QRegularExpression& regex() const;                // regexps
    QString regexFlags() const;
    std::shared_ptr<Scope> scope() const;                   // this / formulas

    // Links: the file they resolve to ("" when unresolved).
    QString resolve() const;
    // Files: cached lazily computed members.
    Value fileLinks() const;
    Value fileEmbeds() const;
    Value fileBacklinks() const;
    Value fileTags() const;
    Value fileProperties() const;

    // Obsidian's objectAccess(name): the member, or nullopt when the value has no such member
    // (callers raise "Cannot find ..."). May throw EvalError (a formula that failed).
    std::optional<Value> objectAccess(const QString& name) const;
    QStringList keys() const;

    // Obsidian's iK.equals / iK.looseEquals.
    static bool equals(const Value& a, const Value& b);
    static bool looseEquals(const Value& a, const Value& b);

    // Tags: does this tag match `query` (a string; "#a" matches "#a" and "#a/b")?
    bool tagMatches(const Value& query) const;
    // Lists: includes() (looseEquals against every item; tag lists match tags hierarchically).
    bool listIncludes(const Value& v) const;

    // Dates.
    static std::optional<Value> parseDate(const QString& s); // Obsidian's gK.parseFromString
    Value dateOnly() const;
    QString printDate() const; // YYYY-MM-DD (local)
    QString printTime() const; // HH:mm:ss (local)
    QString relative() const;  // moment(date).fromNow()
    // Durations.
    static std::optional<Value> parseDuration(const QString& s); // "1 week", "3d", "P1DT2H"
    double durationMs() const; // from now (months and years are calendar-dependent)
    Value addToDate(const Value& date, bool subtract) const;
    QString humanize() const;
    // Links.
    static std::optional<Value> parseLink(const DataSource* ds, const QString& s, const QString& sourcePath); // "[[a|b]]"

    // JavaScript number formatting (String(n)): "1", "0.5", "1e+21", "NaN", "Infinity".
    static QString jsNumber(double n);
    // JavaScript ToNumber(string) / parseFloat(string).
    static double jsToNumber(const QString& s);
    static double jsParseFloat(const QString& s);
    // Is `s` a URL (new URL(s) succeeds)? Obsidian's $v().
    static bool isUrl(const QString& s);

    struct Payload;

private:
    ValueType m_type = ValueType::Null;
    bool m_flag = false;
    double m_num = 0;
    QString m_str;
    std::shared_ptr<const Payload> m_p;
};

// What identifiers resolve against: an entry (a row of the base), a list callback (index / value /
// acc on top of the entry), the summary context ("values").
class Scope {
public:
    virtual ~Scope() = default;
    virtual Value getByIdentifier(const QString& id) = 0;
    virtual QStringList keys() = 0;
    virtual const DataSource* dataSource() const = 0;
    // The formula namespace ("formula.x"): nullopt when there is no such formula. An Error value is
    // thrown by "formula.x" (EvalError). May throw EvalError (a formula referencing itself).
    virtual std::optional<Value> formulaValue(const QString& name) { Q_UNUSED(name); return std::nullopt; }
    virtual QStringList formulaNames() { return {}; }
};

} // namespace oc::bases
