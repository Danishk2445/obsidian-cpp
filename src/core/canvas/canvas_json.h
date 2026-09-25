#pragma once

// An ordered JSON value for canvas files (JSON Canvas 1.0). Objects keep their member order and
// scalars read from text keep their source lexeme, so a value that was not changed is written back
// byte for byte. Parsing is strict RFC 8259 JSON, like JSON.parse (duplicate keys: the last value
// wins, at the first key's position). The writers reproduce JSON.stringify and Obsidian's canvas
// file layout (see stringifyCanvas()).

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>
#include <vector>

namespace oc::canvas {

struct JsonMember;

class Json {
public:
    enum class Type : quint8 { Null, Bool, Number, String, Array, Object };

    Json();
    Json(std::nullptr_t);
    Json(bool b);
    Json(double d);
    Json(int i);
    Json(qint64 i);
    Json(const QString& s);
    Json(const char* utf8);
    Json(const Json& other);
    Json(Json&& other) noexcept;
    Json& operator=(const Json& other);
    Json& operator=(Json&& other) noexcept;
    ~Json();

    static Json array();
    static Json object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    double toDouble(double fallback = 0) const { return m_type == Type::Number ? m_number : fallback; }
    QString toString(const QString& fallback = {}) const { return m_type == Type::String ? m_string : fallback; }

    // Array items / object members (empty for other types).
    qsizetype size() const;
    const std::vector<Json>& items() const;
    std::vector<Json>& items(); // converts a null value into an empty array
    void append(Json value);

    const std::vector<JsonMember>& members() const;
    std::vector<JsonMember>& members(); // converts a null value into an empty object
    const Json* find(QStringView key) const;
    Json* find(QStringView key);
    bool contains(QStringView key) const { return find(key) != nullptr; }
    Json value(QStringView key) const; // null when missing
    qsizetype indexOf(QStringView key) const;
    // Replaces the value of `key` in place, or appends the member (converts null into an object).
    void insert(const QString& key, Json value);
    // Inserts before member `index` (or replaces the existing member in place).
    void insertAt(qsizetype index, const QString& key, Json value);
    bool remove(QStringView key);

    // The source text of a scalar read by parseJson() (for strings including the quotes and escapes);
    // empty for values built in code. The writers reuse it.
    const QByteArray& lexeme() const { return m_lexeme; }

    // By value: numbers compare as doubles, strings by content, objects member by member in order.
    // Lexemes are ignored.
    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    friend class JsonParser;
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    QString m_string;
    QByteArray m_lexeme;
    std::vector<Json> m_items;
    std::vector<JsonMember> m_members; // JsonMember is incomplete here: special members are out of line
};

struct JsonMember {
    QString key;
    Json value;
    bool operator==(const JsonMember& o) const { return key == o.key && value == o.value; }
};

struct JsonError {
    qsizetype offset = -1; // byte offset of the problem
    QString message;
};

// Parses UTF-8 JSON text (one value, surrounding whitespace allowed; a leading UTF-8 BOM is skipped,
// as TextDecoder does before JSON.parse). nullopt + *error on invalid input.
std::optional<Json> parseJson(QByteArrayView utf8, JsonError* error = nullptr);

// JavaScript's Number.prototype.toString(): shortest round-trip digits, "1e+21", "1e-7", "-0" -> "0".
QByteArray jsNumber(double d);
// JSON.stringify(string): escapes '"', '\\', control characters (\b \f \n \r \t, else \u00xx) and lone
// surrogates (\udxxx); everything else is written as UTF-8.
QByteArray jsonQuote(const QString& s);

// JSON.stringify(value) without indentation. Scalars with a lexeme are written as read.
QByteArray stringifyCompact(const Json& value);

// Obsidian's canvas file writer: an object or array whose values are all strings, numbers or booleans
// is written on one line (JSON.stringify); otherwise it is broken over lines, one member / item per
// line, indented with tabs, "key":value without spaces. null counts as a nested value (JavaScript's
// typeof null is "object"). No final newline. Scalars with a lexeme are written as read.
QByteArray stringifyCanvas(const Json& value);

} // namespace oc::canvas
