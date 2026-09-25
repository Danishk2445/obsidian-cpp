#include "core/properties/property_yaml.h"

#include <QRegularExpression>

#include <charconv>
#include <cmath>

namespace oc::props {

namespace {

const QString kStep = QStringLiteral("  ");
constexpr int kMinMultiLineLength = 40; // doubleQuotedMinMultiLineLength

struct Ctx {
    QString indent;
    bool implicitKey = false;
    bool forceBlockIndent = false;
};

bool isNull(const QVariant& v) { return !v.isValid() || v.typeId() == QMetaType::Nullptr; }

bool isList(const QVariant& v) { return v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList; }
bool isMap(const QVariant& v) { return v.typeId() == QMetaType::QVariantMap || v.typeId() == QMetaType::QVariantHash; }
bool isNumber(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
        return true;
    default:
        return false;
    }
}

QChar at(const QString& s, qsizetype i) { return i >= 0 && i < s.size() ? s[i] : QChar(); }

// /^(%|---|\.\.\.)/m
bool containsDocumentMarker(const QString& s)
{
    for (qsizetype i = 0; i <= s.size(); i = s.indexOf(u'\n', i) + 1) {
        const QStringView line = QStringView(s).mid(i);
        if (line.startsWith(u'%') || line.startsWith(u"---") || line.startsWith(u"..."))
            return true;
        if (s.indexOf(u'\n', i) < 0)
            break;
    }
    return false;
}

// Control characters and unpaired surrogates force double quotes.
bool needsDoubleQuotes(const QString& s)
{
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t c = s[i].unicode();
        if (c <= 0x08 || (c >= 0x0b && c <= 0x1f) || (c >= 0x7f && c <= 0x9f))
            return true;
        if (QChar::isHighSurrogate(c)) {
            if (i + 1 < s.size() && QChar::isLowSurrogate(s[i + 1].unicode()))
                ++i;
            else
                return true;
        } else if (QChar::isLowSurrogate(c)) {
            return true;
        }
    }
    return false;
}

// Every run of '\n' followed by `suffix` (JS: s.replace(/\n+/g, "$&" + suffix)).
QString suffixNewlineRuns(const QString& s, const QString& suffix)
{
    QString out;
    out.reserve(s.size() + suffix.size() * 2);
    for (qsizetype i = 0; i < s.size();) {
        if (s[i] != u'\n') {
            out += s[i++];
            continue;
        }
        while (i < s.size() && s[i] == u'\n')
            out += s[i++];
        out += suffix;
    }
    return out;
}

// JSON.stringify for a string.
QString jsonQuote(const QString& s)
{
    QString out;
    out.reserve(s.size() + 2);
    out += u'"';
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t c = s[i].unicode();
        switch (c) {
        case u'"': out += QLatin1String("\\\""); continue;
        case u'\\': out += QLatin1String("\\\\"); continue;
        case u'\b': out += QLatin1String("\\b"); continue;
        case u'\f': out += QLatin1String("\\f"); continue;
        case u'\n': out += QLatin1String("\\n"); continue;
        case u'\r': out += QLatin1String("\\r"); continue;
        case u'\t': out += QLatin1String("\\t"); continue;
        default: break;
        }
        const bool lone = (QChar::isHighSurrogate(c) && !(i + 1 < s.size() && QChar::isLowSurrogate(s[i + 1].unicode())))
                          || (QChar::isLowSurrogate(c) && !(i > 0 && QChar::isHighSurrogate(s[i - 1].unicode())));
        if (c < 0x20 || lone) {
            out += QStringLiteral("\\u%1").arg(uint(c), 4, 16, QLatin1Char('0'));
            continue;
        }
        out += QChar(c);
    }
    out += u'"';
    return out;
}

QString doubleQuoted(const QString& value, const Ctx& ctx)
{
    const QString json = jsonQuote(value);
    const QString indent = !ctx.indent.isEmpty() ? ctx.indent : (containsDocumentMarker(value) ? kStep : QString());
    QString str;
    qsizetype start = 0;
    for (qsizetype i = 0; i < json.size(); ++i) {
        QChar ch = json[i];
        if (ch == u' ' && at(json, i + 1) == u'\\' && at(json, i + 2) == u'n') {
            // a space before a newline is escaped so that it isn't folded away
            str += json.mid(start, i - start) + QLatin1String("\\ ");
            i += 1;
            start = i;
            ch = u'\\';
        }
        if (ch != u'\\')
            continue;
        switch (at(json, i + 1).unicode()) {
        case u'u': {
            str += json.mid(start, i - start);
            const QString code = json.mid(i + 2, 4);
            if (code == u"0000")
                str += QLatin1String("\\0");
            else if (code == u"0007")
                str += QLatin1String("\\a");
            else if (code == u"000b")
                str += QLatin1String("\\v");
            else if (code == u"001b")
                str += QLatin1String("\\e");
            else if (code == u"0085")
                str += QLatin1String("\\N");
            else if (code == u"00a0")
                str += QLatin1String("\\_");
            else if (code == u"2028")
                str += QLatin1String("\\L");
            else if (code == u"2029")
                str += QLatin1String("\\P");
            else if (code.startsWith(QLatin1String("00")))
                str += QLatin1String("\\x") + code.mid(2);
            else
                str += json.mid(i, 6);
            i += 5;
            start = i + 1;
            break;
        }
        case u'n':
            if (ctx.implicitKey || at(json, i + 2) == u'"' || json.size() < kMinMultiLineLength) {
                i += 1;
            } else {
                // a multi-line double-quoted scalar: the first line break is folded, so write two
                str += json.mid(start, i - start) + QLatin1String("\n\n");
                while (at(json, i + 2) == u'\\' && at(json, i + 3) == u'n' && at(json, i + 4) != u'"') {
                    str += u'\n';
                    i += 2;
                }
                str += indent;
                if (at(json, i + 2) == u' ')
                    str += u'\\';
                i += 1;
                start = i + 1;
            }
            break;
        default:
            i += 1;
        }
    }
    return start ? str + json.mid(start) : json;
}

QString singleQuoted(const QString& value, const Ctx& ctx)
{
    static const QRegularExpression wsAroundNewline(QStringLiteral("[ \\t]\\n|\\n[ \\t]"));
    if ((ctx.implicitKey && value.contains(u'\n')) || wsAroundNewline.match(value).hasMatch())
        return doubleQuoted(value, ctx);
    const QString indent = !ctx.indent.isEmpty() ? ctx.indent : (containsDocumentMarker(value) ? kStep : QString());
    QString v = value;
    v.replace(QLatin1Char('\''), QLatin1String("''"));
    return u'\'' + suffixNewlineRuns(v, u'\n' + indent) + u'\'';
}

QString quoted(const QString& value, const Ctx& ctx)
{
    const bool hasDouble = value.contains(u'"');
    const bool hasSingle = value.contains(u'\'');
    if (hasDouble && !hasSingle)
        return singleQuoted(value, ctx);
    return doubleQuoted(value, ctx);
}

QString blockLiteral(QString value, const Ctx& ctx)
{
    static const QRegularExpression trailingIndentedBlank(QStringLiteral("\\n[\\t ]+\\z"));
    if (trailingIndentedBlank.match(value).hasMatch())
        return quoted(value, ctx);
    const QString indent = !ctx.indent.isEmpty() ? ctx.indent : ((ctx.forceBlockIndent || containsDocumentMarker(value)) ? kStep : QString());
    if (value.isEmpty())
        return QStringLiteral("|\n");
    // chomping from the whitespace at the end
    qsizetype endStart = value.size();
    while (endStart > 0) {
        const QChar ch = value[endStart - 1];
        if (ch != u'\n' && ch != u'\t' && ch != u' ')
            break;
        --endStart;
    }
    QString end = value.mid(endStart);
    const qsizetype endNl = end.indexOf(u'\n');
    QString chomp;
    if (endNl < 0)
        chomp = QStringLiteral("-");
    else if (value == end || endNl != end.size() - 1)
        chomp = QStringLiteral("+");
    if (!end.isEmpty()) {
        value.chop(end.size());
        if (end.endsWith(u'\n'))
            end.chop(1);
        // newline runs that are not at the very end get the indentation
        QString out;
        for (qsizetype i = 0; i < end.size();) {
            if (end[i] != u'\n') {
                out += end[i++];
                continue;
            }
            const qsizetype runStart = i;
            while (i < end.size() && end[i] == u'\n')
                ++i;
            out += end.mid(runStart, i - runStart);
            if (i < end.size())
                out += indent;
        }
        end = out;
    }
    // indentation indicator from the whitespace at the start
    bool startWithSpace = false;
    qsizetype startEnd = 0;
    qsizetype startNl = -1;
    for (; startEnd < value.size(); ++startEnd) {
        const QChar ch = value[startEnd];
        if (ch == u' ')
            startWithSpace = true;
        else if (ch == u'\n')
            startNl = startEnd;
        else
            break;
    }
    QString start = value.left(startNl < startEnd ? startNl + 1 : startEnd);
    if (!start.isEmpty()) {
        value = value.mid(start.size());
        start = suffixNewlineRuns(start, indent);
    }
    const QString header = (startWithSpace ? (indent.isEmpty() ? QStringLiteral("1") : QStringLiteral("2")) : QString()) + chomp;
    return u'|' + header + u'\n' + indent + start + suffixNewlineRuns(value, indent) + end;
}

// Would this plain text read back as something other than a string (core schema)?
bool resolvesToNonString(const QString& s)
{
    static const QRegularExpression tests[] = {
        QRegularExpression(QStringLiteral("\\A(?:~|[Nn]ull|NULL)?\\z")),
        QRegularExpression(QStringLiteral("\\A(?:[Tt]rue|TRUE|[Ff]alse|FALSE)\\z")),
        QRegularExpression(QStringLiteral("\\A0o[0-7]+\\z")),
        QRegularExpression(QStringLiteral("\\A[-+]?[0-9]+\\z")),
        QRegularExpression(QStringLiteral("\\A0x[0-9a-fA-F]+\\z")),
        QRegularExpression(QStringLiteral("\\A(?:[-+]?\\.(?:inf|Inf|INF)|\\.nan|\\.NaN|\\.NAN)\\z")),
        QRegularExpression(QStringLiteral("\\A[-+]?(?:\\.[0-9]+|[0-9]+(?:\\.[0-9]*)?)[eE][-+]?[0-9]+\\z")),
        QRegularExpression(QStringLiteral("\\A[-+]?(?:\\.[0-9]+|[0-9]+\\.[0-9]*)\\z")),
    };
    for (const QRegularExpression& re : tests)
        if (re.match(s).hasMatch())
            return true;
    return false;
}

QString plain(const QString& value, Ctx ctx)
{
    if (ctx.implicitKey && value.contains(u'\n'))
        return quoted(value, ctx);
    // not allowed plain: '-'/'?' alone, a leading indicator, "- "/"? " at the start, ": ", " #",
    // whitespace before a line break, or a trailing space / colon
    static const QRegularExpression disallowed(QStringLiteral(
        "^[\\n\\t ,\\[\\]{}#&*!|>'\"%@`]|^[?-]\\z|^[?-][ \\t]|[\\n:][ \\t]|[ \\t]\\n|[\\n\\t ]#|[\\n\\t :]\\z"));
    if (disallowed.match(value).hasMatch())
        return ctx.implicitKey || !value.contains(u'\n') ? quoted(value, ctx) : blockLiteral(value, ctx);
    if (!ctx.implicitKey && value.contains(u'\n'))
        return blockLiteral(value, ctx);
    if (containsDocumentMarker(value)) {
        if (ctx.indent.isEmpty()) {
            ctx.forceBlockIndent = true;
            return blockLiteral(value, ctx);
        }
        if (ctx.implicitKey && ctx.indent == kStep)
            return quoted(value, ctx);
    }
    const QString str = suffixNewlineRuns(value, u'\n' + ctx.indent);
    if (resolvesToNonString(str))
        return quoted(value, ctx);
    return str;
}

QString stringNode(const QString& value, const Ctx& ctx)
{
    if (needsDoubleQuotes(value))
        return doubleQuoted(value, ctx);
    return plain(value, ctx);
}

QString node(const QVariant& v, const Ctx& ctx);

QString pairValue(const QVariant& value, const Ctx& ctx)
{
    Ctx vc = ctx;
    vc.implicitKey = false;
    vc.indent = ctx.indent + kStep;
    const QString valueStr = node(value, vc);
    if (isList(value) || isMap(value)) {
        const bool empty = isList(value) ? value.toList().isEmpty() : (value.typeId() == QMetaType::QVariantHash ? value.toHash().isEmpty() : value.toMap().isEmpty());
        return (empty ? QStringLiteral(" ") : u'\n' + vc.indent) + valueStr;
    }
    if (valueStr.isEmpty() || valueStr.startsWith(u'\n'))
        return valueStr;
    return u' ' + valueStr;
}

QString pair(const QString& key, const QVariant& value, const Ctx& ctx)
{
    Ctx kc = ctx;
    kc.implicitKey = true;
    kc.indent = ctx.indent + kStep;
    return stringNode(key, kc) + u':' + pairValue(value, ctx);
}

QString joinItems(const QStringList& lines, const QString& indent)
{
    QString str = lines.value(0);
    for (qsizetype i = 1; i < lines.size(); ++i)
        str += lines[i].isEmpty() ? QStringLiteral("\n") : u'\n' + indent + lines[i];
    return str;
}

QString node(const QVariant& v, const Ctx& ctx)
{
    if (isNull(v))
        return QString(); // nullStr: ""
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (isNumber(v))
        return jsNumber(v.toDouble());
    if (isList(v)) {
        const QVariantList items = v.toList();
        if (items.isEmpty())
            return QStringLiteral("[]");
        Ctx ic = ctx;
        ic.implicitKey = false;
        ic.indent = ctx.indent + kStep;
        QStringList lines;
        for (const QVariant& item : items)
            lines << QStringLiteral("- ") + node(item, ic);
        return joinItems(lines, ctx.indent);
    }
    if (isMap(v)) {
        QList<QPair<QString, QVariant>> entries;
        if (v.typeId() == QMetaType::QVariantHash) {
            const QVariantHash h = v.toHash();
            for (auto it = h.cbegin(); it != h.cend(); ++it)
                entries.append({it.key(), it.value()});
        } else {
            const QVariantMap m = v.toMap();
            for (auto it = m.cbegin(); it != m.cend(); ++it)
                entries.append({it.key(), it.value()});
        }
        if (entries.isEmpty())
            return QStringLiteral("{}");
        Ctx ic = ctx;
        ic.implicitKey = false;
        QStringList lines;
        for (const auto& [k, val] : entries)
            lines << pair(k, val, ic);
        return joinItems(lines, ctx.indent);
    }
    return stringNode(v.toString(), ctx);
}

} // namespace

QString jsNumber(double v)
{
    if (std::isnan(v))
        return QStringLiteral(".nan");
    if (std::isinf(v))
        return v < 0 ? QStringLiteral("-.inf") : QStringLiteral(".inf");
    if (v == 0)
        return std::signbit(v) ? QStringLiteral("-0") : QStringLiteral("0");
    const bool negative = v < 0;
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), std::fabs(v), std::chars_format::scientific);
    const std::string sci(buf, res.ptr);
    const size_t e = sci.find('e');
    QString digits;
    for (size_t i = 0; i < e; ++i)
        if (sci[i] != '.')
            digits += QLatin1Char(sci[i]);
    const int exp = std::stoi(sci.substr(e + 1));
    const int k = int(digits.size());
    const int n = exp + 1;
    QString s;
    if (k <= n && n <= 21) {
        s = digits + QString(n - k, u'0');
    } else if (0 < n && n <= 21) {
        s = digits.left(n) + u'.' + digits.mid(n);
    } else if (-6 < n && n <= 0) {
        s = QStringLiteral("0.") + QString(-n, u'0') + digits;
    } else {
        const int e1 = n - 1;
        s = digits.left(1) + (k > 1 ? u'.' + digits.mid(1) : QString()) + u'e' + (e1 >= 0 ? u'+' : u'-') + QString::number(std::abs(e1));
    }
    return negative ? u'-' + s : s;
}

QString yamlKey(const QString& key)
{
    Ctx kc;
    kc.implicitKey = true;
    kc.indent = kStep;
    return stringNode(key, kc);
}

QString yamlPairValue(const QVariant& value) { return pairValue(value, Ctx{}); }

QString yamlEntry(const QString& key, const QVariant& value) { return pair(key, value, Ctx{}); }

QString yamlDocument(const QList<QPair<QString, QVariant>>& entries)
{
    QStringList lines;
    for (const auto& [k, v] : entries)
        lines << pair(k, v, Ctx{});
    return lines.join(u'\n').trimmed();
}

QString yamlInlineItem(const QVariant& value, bool* singleLine)
{
    Ctx ic;
    ic.indent = kStep + kStep;
    const QString s = node(value, ic);
    if (singleLine)
        *singleLine = !s.contains(u'\n') && !(isList(value) && !value.toList().isEmpty()) && !(isMap(value) && !value.toMap().isEmpty());
    return s;
}

QString jsonText(const QVariant& v)
{
    if (isNull(v))
        return QStringLiteral("null");
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (isNumber(v)) {
        const double d = v.toDouble();
        return std::isfinite(d) ? jsNumber(d) : QStringLiteral("null");
    }
    if (isList(v)) {
        QStringList parts;
        for (const QVariant& item : v.toList())
            parts << jsonText(item);
        return u'[' + parts.join(u',') + u']';
    }
    if (isMap(v)) {
        QStringList parts;
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            parts << jsonQuote(it.key()) + u':' + jsonText(it.value());
        return u'{' + parts.join(u',') + u'}';
    }
    return jsonQuote(v.toString());
}

} // namespace oc::props
