#include "core/canvas/canvas_json.h"

#include <QByteArrayList>

#include <charconv>
#include <cmath>
#include <cstring>

namespace oc::canvas {

// ---------------------------------------------------------------------------------------------
// Json
// ---------------------------------------------------------------------------------------------

Json::Json() = default;
Json::Json(std::nullptr_t) {}
Json::Json(bool b) : m_type(Type::Bool), m_bool(b) {}
Json::Json(double d) : m_type(Type::Number), m_number(d) {}
Json::Json(int i) : m_type(Type::Number), m_number(i) {}
Json::Json(qint64 i) : m_type(Type::Number), m_number(double(i)) {}
Json::Json(const QString& s) : m_type(Type::String), m_string(s) {}
Json::Json(const char* utf8) : m_type(Type::String), m_string(QString::fromUtf8(utf8)) {}
Json::Json(const Json& other) = default;
Json::Json(Json&& other) noexcept = default;
Json& Json::operator=(const Json& other) = default;
Json& Json::operator=(Json&& other) noexcept = default;
Json::~Json() = default;

Json Json::array()
{
    Json j;
    j.m_type = Type::Array;
    return j;
}

Json Json::object()
{
    Json j;
    j.m_type = Type::Object;
    return j;
}

qsizetype Json::size() const
{
    if (m_type == Type::Array)
        return qsizetype(m_items.size());
    if (m_type == Type::Object)
        return qsizetype(m_members.size());
    return 0;
}

const std::vector<Json>& Json::items() const
{
    static const std::vector<Json> none;
    return m_type == Type::Array ? m_items : none;
}

std::vector<Json>& Json::items()
{
    if (m_type == Type::Null)
        m_type = Type::Array;
    return m_items;
}

void Json::append(Json value) { items().push_back(std::move(value)); }

const std::vector<JsonMember>& Json::members() const
{
    static const std::vector<JsonMember> none;
    return m_type == Type::Object ? m_members : none;
}

std::vector<JsonMember>& Json::members()
{
    if (m_type == Type::Null)
        m_type = Type::Object;
    return m_members;
}

qsizetype Json::indexOf(QStringView key) const
{
    if (m_type != Type::Object)
        return -1;
    for (size_t i = 0; i < m_members.size(); ++i)
        if (m_members[i].key == key)
            return qsizetype(i);
    return -1;
}

const Json* Json::find(QStringView key) const
{
    const qsizetype i = indexOf(key);
    return i < 0 ? nullptr : &m_members[size_t(i)].value;
}

Json* Json::find(QStringView key)
{
    const qsizetype i = indexOf(key);
    return i < 0 ? nullptr : &m_members[size_t(i)].value;
}

Json Json::value(QStringView key) const
{
    const Json* v = find(key);
    return v ? *v : Json();
}

void Json::insert(const QString& key, Json value)
{
    if (Json* v = find(key)) {
        *v = std::move(value);
        return;
    }
    members().push_back(JsonMember{key, std::move(value)});
}

void Json::insertAt(qsizetype index, const QString& key, Json value)
{
    if (Json* v = find(key)) {
        *v = std::move(value);
        return;
    }
    std::vector<JsonMember>& m = members();
    index = std::clamp<qsizetype>(index, 0, qsizetype(m.size()));
    m.insert(m.begin() + index, JsonMember{key, std::move(value)});
}

bool Json::remove(QStringView key)
{
    const qsizetype i = indexOf(key);
    if (i < 0)
        return false;
    m_members.erase(m_members.begin() + i);
    return true;
}

bool Json::operator==(const Json& o) const
{
    if (m_type != o.m_type)
        return false;
    switch (m_type) {
    case Type::Null:
        return true;
    case Type::Bool:
        return m_bool == o.m_bool;
    case Type::Number:
        return m_number == o.m_number;
    case Type::String:
        return m_string == o.m_string;
    case Type::Array:
        return m_items == o.m_items;
    case Type::Object:
        return m_members == o.m_members;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

class JsonParser {
public:
    explicit JsonParser(QByteArrayView s) : m_s(s) {}

    std::optional<Json> parseDocument(JsonError* error)
    {
        if (m_s.size() >= 3 && quint8(m_s[0]) == 0xEF && quint8(m_s[1]) == 0xBB && quint8(m_s[2]) == 0xBF)
            m_pos = 3;
        skipWs();
        Json v;
        if (!parseValue(v, 0)) {
            if (error)
                *error = m_error;
            return std::nullopt;
        }
        skipWs();
        if (m_pos != m_s.size()) {
            fail(QStringLiteral("Unexpected content after the JSON value"));
            if (error)
                *error = m_error;
            return std::nullopt;
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 512;

    bool fail(const QString& msg)
    {
        if (m_error.offset < 0)
            m_error = JsonError{m_pos, msg};
        return false;
    }

    void skipWs()
    {
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++m_pos;
        }
    }

    bool literal(const char* word)
    {
        const qsizetype n = qsizetype(std::strlen(word));
        if (m_s.size() - m_pos < n || std::memcmp(m_s.data() + m_pos, word, size_t(n)) != 0)
            return fail(QStringLiteral("Invalid literal"));
        m_pos += n;
        return true;
    }

    bool parseValue(Json& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail(QStringLiteral("Nesting too deep"));
        if (m_pos >= m_s.size())
            return fail(QStringLiteral("Unexpected end of input"));
        const char c = m_s[m_pos];
        switch (c) {
        case '{':
            return parseObject(out, depth);
        case '[':
            return parseArray(out, depth);
        case '"': {
            const qsizetype start = m_pos;
            QString s;
            if (!parseString(s))
                return false;
            out = Json(s);
            out.m_lexeme = m_s.sliced(start, m_pos - start).toByteArray();
            return true;
        }
        case 't': {
            const qsizetype start = m_pos;
            if (!literal("true"))
                return false;
            out = Json(true);
            out.m_lexeme = m_s.sliced(start, 4).toByteArray();
            return true;
        }
        case 'f': {
            const qsizetype start = m_pos;
            if (!literal("false"))
                return false;
            out = Json(false);
            out.m_lexeme = m_s.sliced(start, 5).toByteArray();
            return true;
        }
        case 'n':
            if (!literal("null"))
                return false;
            out = Json();
            return true;
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return parseNumber(out);
            return fail(QStringLiteral("Unexpected character"));
        }
    }

    bool parseNumber(Json& out)
    {
        const qsizetype start = m_pos;
        auto digit = [&](qsizetype i) { return i < m_s.size() && m_s[i] >= '0' && m_s[i] <= '9'; };
        if (m_s[m_pos] == '-')
            ++m_pos;
        if (!digit(m_pos))
            return fail(QStringLiteral("Invalid number"));
        if (m_s[m_pos] == '0') {
            ++m_pos;
        } else {
            while (digit(m_pos))
                ++m_pos;
        }
        if (m_pos < m_s.size() && m_s[m_pos] == '.') {
            ++m_pos;
            if (!digit(m_pos))
                return fail(QStringLiteral("Invalid number"));
            while (digit(m_pos))
                ++m_pos;
        }
        if (m_pos < m_s.size() && (m_s[m_pos] == 'e' || m_s[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_s.size() && (m_s[m_pos] == '+' || m_s[m_pos] == '-'))
                ++m_pos;
            if (!digit(m_pos))
                return fail(QStringLiteral("Invalid number"));
            while (digit(m_pos))
                ++m_pos;
        }
        const char* b = m_s.data() + start;
        const char* e = m_s.data() + m_pos;
        double d = 0;
        const auto r = std::from_chars(b, e, d);
        if (r.ec == std::errc::result_out_of_range) {
            // JSON.parse: overflow -> +-Infinity, underflow -> +-0
            bool neg = *b == '-';
            bool bigExp = false;
            for (const char* p = b; p < e; ++p)
                if (*p == 'e' || *p == 'E') {
                    bigExp = p + 1 < e && p[1] != '-';
                    break;
                }
            d = bigExp ? (neg ? -HUGE_VAL : HUGE_VAL) : (neg ? -0.0 : 0.0);
        } else if (r.ec != std::errc() || r.ptr != e) {
            return fail(QStringLiteral("Invalid number"));
        }
        out = Json(d);
        out.m_lexeme = QByteArray(b, e - b);
        return true;
    }

    static int hexVal(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool parseString(QString& out)
    {
        ++m_pos; // opening quote
        qsizetype runStart = m_pos;
        auto flush = [&] {
            if (m_pos > runStart)
                out += QString::fromUtf8(m_s.sliced(runStart, m_pos - runStart));
        };
        while (true) {
            if (m_pos >= m_s.size())
                return fail(QStringLiteral("Unterminated string"));
            const quint8 c = quint8(m_s[m_pos]);
            if (c == '"') {
                flush();
                ++m_pos;
                return true;
            }
            if (c < 0x20)
                return fail(QStringLiteral("Control character in string"));
            if (c != '\\') {
                ++m_pos;
                continue;
            }
            flush();
            ++m_pos;
            if (m_pos >= m_s.size())
                return fail(QStringLiteral("Unterminated string"));
            const char esc = m_s[m_pos++];
            switch (esc) {
            case '"': out += u'"'; break;
            case '\\': out += u'\\'; break;
            case '/': out += u'/'; break;
            case 'b': out += u'\b'; break;
            case 'f': out += u'\f'; break;
            case 'n': out += u'\n'; break;
            case 'r': out += u'\r'; break;
            case 't': out += u'\t'; break;
            case 'u': {
                if (m_s.size() - m_pos < 4)
                    return fail(QStringLiteral("Invalid \\u escape"));
                int v = 0;
                for (int i = 0; i < 4; ++i) {
                    const int h = hexVal(m_s[m_pos + i]);
                    if (h < 0)
                        return fail(QStringLiteral("Invalid \\u escape"));
                    v = v * 16 + h;
                }
                m_pos += 4;
                out += QChar(char16_t(v)); // surrogates combine (or stay lone) in UTF-16, like JS
                break;
            }
            default:
                --m_pos;
                return fail(QStringLiteral("Invalid escape"));
            }
            runStart = m_pos;
        }
    }

    bool parseArray(Json& out, int depth)
    {
        ++m_pos;
        out = Json::array();
        skipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipWs();
            Json v;
            if (!parseValue(v, depth + 1))
                return false;
            out.m_items.push_back(std::move(v));
            skipWs();
            if (m_pos >= m_s.size())
                return fail(QStringLiteral("Unterminated array"));
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return fail(QStringLiteral("Expected ',' or ']'"));
        }
    }

    bool parseObject(Json& out, int depth)
    {
        ++m_pos;
        out = Json::object();
        skipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != '"')
                return fail(QStringLiteral("Expected a key"));
            QString key;
            if (!parseString(key))
                return false;
            skipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':')
                return fail(QStringLiteral("Expected ':'"));
            ++m_pos;
            skipWs();
            Json v;
            if (!parseValue(v, depth + 1))
                return false;
            out.insert(key, std::move(v)); // duplicate key: last value, first position (JS objects)
            skipWs();
            if (m_pos >= m_s.size())
                return fail(QStringLiteral("Unterminated object"));
            if (m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_s[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return fail(QStringLiteral("Expected ',' or '}'"));
        }
    }

    QByteArrayView m_s;
    qsizetype m_pos = 0;
    JsonError m_error;
};

std::optional<Json> parseJson(QByteArrayView utf8, JsonError* error)
{
    JsonParser p(utf8);
    return p.parseDocument(error);
}

// ---------------------------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------------------------

QByteArray jsNumber(double d)
{
    if (std::isnan(d) || std::isinf(d))
        return QByteArrayLiteral("null"); // JSON.stringify
    if (d == 0)
        return QByteArrayLiteral("0");
    // Shortest round-trip digits in scientific form: "-d.ddde+XX".
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    const std::string_view sci(buf, size_t(r.ptr - buf));
    const size_t ePos = sci.find('e');
    std::string_view mant = sci.substr(0, ePos);
    const int exp10 = std::stoi(std::string(sci.substr(ePos + 1)));
    const bool neg = !mant.empty() && mant[0] == '-';
    if (neg)
        mant.remove_prefix(1);
    std::string digits;
    for (char c : mant)
        if (c != '.')
            digits += c;
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    const int k = int(digits.size());
    const int n = exp10 + 1; // value = 0.d1..dk * 10^n
    std::string out = neg ? "-" : "";
    if (k <= n && n <= 21) {
        out += digits;
        out.append(size_t(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, size_t(n));
        out += '.';
        out += digits.substr(size_t(n));
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(size_t(-n), '0');
        out += digits;
    } else {
        out += digits[0];
        if (k > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        out += n - 1 >= 0 ? '+' : '-';
        out += std::to_string(std::abs(n - 1));
    }
    return QByteArray(out.data(), qsizetype(out.size()));
}

QByteArray jsonQuote(const QString& s)
{
    QByteArray out;
    out.reserve(s.size() + 2);
    out += '"';
    static const char hex[] = "0123456789abcdef";
    auto escapeUnit = [&](char16_t u) {
        out += "\\u";
        out += hex[(u >> 12) & 0xF];
        out += hex[(u >> 8) & 0xF];
        out += hex[(u >> 4) & 0xF];
        out += hex[u & 0xF];
    };
    const qsizetype n = s.size();
    qsizetype runStart = 0;
    auto flush = [&](qsizetype end) {
        if (end > runStart)
            out += QStringView(s).sliced(runStart, end - runStart).toUtf8();
    };
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t u = s[i].unicode();
        const char* rep = nullptr;
        switch (u) {
        case u'"': rep = "\\\""; break;
        case u'\\': rep = "\\\\"; break;
        case u'\b': rep = "\\b"; break;
        case u'\f': rep = "\\f"; break;
        case u'\n': rep = "\\n"; break;
        case u'\r': rep = "\\r"; break;
        case u'\t': rep = "\\t"; break;
        default: break;
        }
        if (rep) {
            flush(i);
            out += rep;
            runStart = i + 1;
            continue;
        }
        if (u < 0x20) {
            flush(i);
            escapeUnit(u);
            runStart = i + 1;
            continue;
        }
        if (QChar::isHighSurrogate(u)) {
            if (i + 1 < n && QChar::isLowSurrogate(s[i + 1].unicode())) {
                ++i; // a valid pair stays in the run
                continue;
            }
            flush(i);
            escapeUnit(u);
            runStart = i + 1;
            continue;
        }
        if (QChar::isLowSurrogate(u)) {
            flush(i);
            escapeUnit(u);
            runStart = i + 1;
        }
    }
    flush(n);
    out += '"';
    return out;
}

namespace {

// JavaScript's typeof x !== "object" (Obsidian's test for "plain" values).
bool isPlain(const Json& v) { return v.isBool() || v.isNumber() || v.isString(); }

void writeScalar(QByteArray& out, const Json& v)
{
    if (!v.lexeme().isEmpty()) {
        out += v.lexeme();
        return;
    }
    switch (v.type()) {
    case Json::Type::Null:
        out += "null";
        break;
    case Json::Type::Bool:
        out += v.toBool() ? "true" : "false";
        break;
    case Json::Type::Number:
        out += jsNumber(v.toDouble());
        break;
    case Json::Type::String:
        out += jsonQuote(v.toString());
        break;
    default:
        break;
    }
}

void writeCompact(QByteArray& out, const Json& v)
{
    if (v.isArray()) {
        out += '[';
        bool first = true;
        for (const Json& item : v.items()) {
            if (!first)
                out += ',';
            first = false;
            writeCompact(out, item);
        }
        out += ']';
    } else if (v.isObject()) {
        out += '{';
        bool first = true;
        for (const JsonMember& m : v.members()) {
            if (!first)
                out += ',';
            first = false;
            out += jsonQuote(m.key);
            out += ':';
            writeCompact(out, m.value);
        }
        out += '}';
    } else {
        writeScalar(out, v);
    }
}

// Obsidian's Zd(): the value as a list of lines (without indentation of the outer level).
QList<QByteArray> canvasLines(const Json& v)
{
    if (v.isArray()) {
        const auto& items = v.items();
        if (std::all_of(items.begin(), items.end(), isPlain)) {
            QByteArray line;
            writeCompact(line, v);
            return {line};
        }
        QList<QByteArray> lines{QByteArrayLiteral("[")};
        for (size_t i = 0; i < items.size(); ++i) {
            QList<QByteArray> sub = canvasLines(items[i]);
            for (qsizetype j = 0; j < sub.size(); ++j) {
                QByteArray l = '\t' + sub[j];
                if (j == sub.size() - 1 && i + 1 < items.size())
                    l += ',';
                lines << l;
            }
        }
        lines << QByteArrayLiteral("]");
        return lines;
    }
    if (v.isObject()) {
        const auto& members = v.members();
        if (std::all_of(members.begin(), members.end(), [](const JsonMember& m) { return isPlain(m.value); })) {
            QByteArray line;
            writeCompact(line, v);
            return {line};
        }
        QList<QByteArray> lines{QByteArrayLiteral("{")};
        for (size_t i = 0; i < members.size(); ++i) {
            QList<QByteArray> sub = canvasLines(members[i].value);
            sub[0] = jsonQuote(members[i].key) + ':' + sub[0];
            for (qsizetype j = 0; j < sub.size(); ++j) {
                QByteArray l = '\t' + sub[j];
                if (j == sub.size() - 1 && i + 1 < members.size())
                    l += ',';
                lines << l;
            }
        }
        lines << QByteArrayLiteral("}");
        return lines;
    }
    QByteArray line;
    writeScalar(line, v);
    return {line};
}

} // namespace

QByteArray stringifyCompact(const Json& value)
{
    QByteArray out;
    writeCompact(out, value);
    return out;
}

QByteArray stringifyCanvas(const Json& value) { return canvasLines(value).join('\n'); }

} // namespace oc::canvas
