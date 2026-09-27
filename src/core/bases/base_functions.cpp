// The functions of the Bases formula language (Obsidian 1.13's registry: 16 global functions and
// the methods of Any, Null, String, Number, List, Object, RegExp, Date, File, Link and Tag values).

#include "core/bases/base_functions_p.h"

#include "core/time/moment_format.h"

#include <QCollator>
#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>

namespace oc::bases::detail {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
using VC = ValueClass;

Param P(const char* name, std::initializer_list<ValueClass> types) { return Param{QString::fromLatin1(name), QList<ValueClass>(types), false, false}; }
Param Opt(const char* name, std::initializer_list<ValueClass> types) { return Param{QString::fromLatin1(name), QList<ValueClass>(types), true, false}; }
Param Var(const char* name, std::initializer_list<ValueClass> types) { return Param{QString::fromLatin1(name), QList<ValueClass>(types), false, true}; }

struct Registry {
    QHash<QString, Function> globals;                           // lowercase name
    QHash<int, QHash<QString, Function>> methods;               // class -> lowercase name
    QList<QPair<QString, QString>> order;                       // registration order (name, type)

    void global(const QString& name, QList<Param> params, std::function<Value(CallContext&, const Args&)> fn)
    {
        globals.insert(name.toLower(), Function{name, std::move(params), std::move(fn)});
        order.append({name, QString()});
    }
    void method(ValueClass c, const QString& name, QList<Param> params, std::function<Value(CallContext&, const Args&)> fn)
    {
        methods[int(c)].insert(name.toLower(), Function{name, std::move(params), std::move(fn)});
        order.append({name, className(c)});
    }
    // Obsidian's OK: a boolean result
    void predicate(ValueClass c, const QString& name, QList<Param> params, std::function<bool(CallContext&, const Args&)> fn)
    {
        method(c, name, std::move(params), [fn = std::move(fn)](CallContext& ctx, const Args& a) { return Value::boolean(fn(ctx, a)); });
    }
};

// The class chain of a value, most specific first (Obsidian's prototype chain).
QList<ValueClass> classChain(ValueType t)
{
    switch (t) {
    case ValueType::Null: return {VC::Null, VC::Any};
    case ValueType::Error: return {VC::Error, VC::Any};
    case ValueType::Boolean: return {VC::Boolean, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Number: return {VC::Number, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::String: return {VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Tag: return {VC::Tag, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Link: return {VC::Link, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Url: return {VC::Url, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Image: return {VC::Image, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Icon: return {VC::Icon, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Html: return {VC::Html, VC::String, VC::Primitive, VC::NotNull, VC::Any};
    case ValueType::Date:
    case ValueType::RelativeDate: return {VC::Date, VC::NotNull, VC::Any};
    case ValueType::Duration: return {VC::Duration, VC::NotNull, VC::Any};
    case ValueType::List: return {VC::List, VC::NotNull, VC::Any};
    case ValueType::TagList: return {VC::TagList, VC::List, VC::NotNull, VC::Any};
    case ValueType::Object: return {VC::Object, VC::NotNull, VC::Any};
    case ValueType::RegExp: return {VC::RegExp, VC::NotNull, VC::Any};
    case ValueType::File:
    case ValueType::ThisFile: return {VC::File, VC::NotNull, VC::Any};
    case ValueType::Formulas: return {VC::Formulas, VC::NotNull, VC::Any};
    }
    return {VC::Any};
}

// ---- JavaScript helpers ---------------------------------------------------------------------------
bool isJsSpace(QChar c) { return c.isSpace() || c == QChar(0xFEFF); }

QString jsTrim(const QString& s)
{
    qsizetype a = 0, b = s.size();
    while (a < b && isJsSpace(s[a]))
        ++a;
    while (b > a && isJsSpace(s[b - 1]))
        --b;
    return s.mid(a, b - a);
}

double toIntegerOrInfinity(double v)
{
    if (std::isnan(v))
        return 0;
    return std::trunc(v);
}

// Array/String.prototype.slice index resolution.
qsizetype sliceIndex(double v, qsizetype len, qsizetype fallback)
{
    if (std::isnan(v))
        return 0;
    if (std::isinf(v))
        return v < 0 ? 0 : len;
    const double i = toIntegerOrInfinity(v);
    if (i < 0)
        return qsizetype(std::max(0.0, double(len) + i));
    return qsizetype(std::min(double(len), i));
    Q_UNUSED(fallback);
}

double jsRound(double x)
{
    if (!std::isfinite(x))
        return x;
    double r = std::floor(x);
    if (x - r >= 0.5)
        r += 1;
    if (r == 0 && std::signbit(x))
        return -0.0;
    return r;
}

QString jsToFixed(double x, double digits)
{
    const double f = toIntegerOrInfinity(digits);
    if (f < 0 || f > 100 || std::isinf(f))
        throw EvalError{QStringLiteral("toFixed() digits argument must be between 0 and 100")};
    if (std::isnan(x))
        return QStringLiteral("NaN");
    if (std::abs(x) >= 1e21)
        return Value::jsNumber(x);
    const bool negative = x < 0;
    const QString s = QString::number(std::abs(x), 'f', int(f));
    return negative ? u'-' + s : s;
}

// String.prototype.replace with a RegExp and a replacement pattern ($&, $1, $<name>, $$, $`, $').
QString regexReplace(const QString& s, const QRegularExpression& re, bool global, const QString& replacement)
{
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        for (qsizetype i = 0; i < replacement.size(); ++i) {
            const QChar c = replacement[i];
            if (c != u'$' || i + 1 >= replacement.size()) {
                out += c;
                continue;
            }
            const QChar n = replacement[i + 1];
            if (n == u'$') {
                out += u'$';
                ++i;
            } else if (n == u'&') {
                out += m.captured(0);
                ++i;
            } else if (n == u'`') {
                out += s.left(m.capturedStart());
                ++i;
            } else if (n == u'\'') {
                out += s.mid(m.capturedEnd());
                ++i;
            } else if (n.isDigit()) {
                int idx = n.digitValue();
                qsizetype used = 1;
                if (i + 2 < replacement.size() && replacement[i + 2].isDigit()) {
                    const int two = idx * 10 + replacement[i + 2].digitValue();
                    if (two >= 1 && two <= re.captureCount()) {
                        idx = two;
                        used = 2;
                    }
                }
                if (idx >= 1 && idx <= re.captureCount()) {
                    out += m.captured(idx);
                    i += used;
                } else {
                    out += c;
                }
            } else if (n == u'<') {
                const qsizetype close = replacement.indexOf(u'>', i + 2);
                if (close < 0 || re.namedCaptureGroups().size() <= 1) {
                    out += c;
                    continue;
                }
                out += m.captured(replacement.mid(i + 2, close - i - 2));
                i = close;
            } else {
                out += c;
            }
        }
        last = m.capturedEnd();
        if (!global)
            break;
    }
    out += s.mid(last);
    return out;
}

QList<Value> splitString(const QString& s, const Value& sep, std::optional<double> limitArg)
{
    quint32 limit = std::numeric_limits<quint32>::max();
    if (limitArg) {
        const double l = *limitArg;
        limit = std::isfinite(l) ? quint32(qint64(std::trunc(l)) & 0xffffffff) : 0;
    }
    QList<Value> out;
    if (limit == 0)
        return out;
    if (sep.type() != ValueType::RegExp) {
        const QString& d = sep.text();
        if (d.isEmpty()) {
            for (qsizetype i = 0; i < s.size() && quint32(out.size()) < limit; ++i)
                out << Value::string(QString(s[i]));
            return out;
        }
        const QStringList parts = s.split(d);
        for (const QString& p : parts) {
            if (quint32(out.size()) >= limit)
                break;
            out << Value::string(p);
        }
        return out;
    }
    const QRegularExpression& re = sep.regex();
    if (s.isEmpty()) {
        const QRegularExpressionMatch m = re.match(s, 0, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch())
            out << Value::string(s);
        return out;
    }
    qsizetype p = 0, q = 0;
    while (q < s.size()) {
        const QRegularExpressionMatch m = re.match(s, q, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch()) {
            ++q;
            continue;
        }
        const qsizetype e = std::min(m.capturedEnd(), s.size());
        if (e == p) {
            ++q;
            continue;
        }
        out << Value::string(s.mid(p, q - p));
        if (quint32(out.size()) == limit)
            return out;
        for (int i = 1; i <= re.captureCount(); ++i) {
            out << (m.capturedStart(i) < 0 ? Value() : Value::string(m.captured(i)));
            if (quint32(out.size()) == limit)
                return out;
        }
        p = e;
        q = p;
    }
    out << Value::string(s.mid(p));
    return out;
}

// Obsidian's Nl(): normalize a folder path ("/a//b/" -> "a/b", "" -> "/").
QString normalizeFolder(QString p)
{
    p.replace(QRegularExpression(QStringLiteral("[\\\\/]+")), QStringLiteral("/"));
    while (p.startsWith(u'/'))
        p.remove(0, 1);
    while (p.endsWith(u'/'))
        p.chop(1);
    if (p.isEmpty())
        p = QStringLiteral("/");
    p.replace(QChar(0x00A0), u' ').replace(QChar(0x202F), u' ');
    return p.normalized(QString::NormalizationForm_C);
}

// ---- lists ----------------------------------------------------------------------------------------
QList<double> numbersOf(const Value& list)
{
    QList<double> out;
    for (const Value& v : list.items())
        if (v.type() == ValueType::Number)
            out << v.number();
    return out;
}

QList<double> datesOf(const Value& list)
{
    QList<double> out;
    for (const Value& v : list.items()) {
        if (v.is(VC::String)) {
            if (auto d = Value::parseDate(v.text()))
                out << d->dateMs();
        } else if (v.is(VC::Date)) {
            out << v.dateMs();
        }
    }
    return out;
}

Value listSum(const Value& list)
{
    const QList<double> n = numbersOf(list);
    if (n.isEmpty())
        return Value();
    double s = 0;
    for (double v : n)
        s += v;
    return Value::number(s);
}

void flattenInto(const QList<Value>& items, QList<Value>& out)
{
    for (const Value& v : items) {
        if (v.is(VC::List))
            flattenInto(v.items(), out);
        else
            out << v;
    }
}

// Is `k` an array index key ("0", "1", ... like JavaScript objects order them first)?
bool isArrayIndexKey(const QString& k)
{
    if (k.isEmpty() || k.size() > 10 || (k.size() > 1 && k[0] == u'0'))
        return false;
    for (QChar c : k)
        if (!c.isDigit() || c.unicode() > '9')
            return false;
    return k.toULongLong() < 4294967295ULL;
}

Value listUnique(const Value& list)
{
    // grouped by toString(); the groups come out in JavaScript object key order
    QStringList keys;
    QHash<QString, QList<Value>> groups;
    for (const Value& v : list.items()) {
        const QString k = v.toString();
        auto it = groups.find(k);
        if (it == groups.end()) {
            keys << k;
            groups.insert(k, {v});
            continue;
        }
        bool dup = false;
        for (const Value& e : *it)
            dup |= Value::equals(v, e);
        if (!dup)
            it->append(v);
    }
    QStringList indexKeys, otherKeys;
    for (const QString& k : std::as_const(keys))
        (isArrayIndexKey(k) ? indexKeys : otherKeys) << k;
    std::sort(indexKeys.begin(), indexKeys.end(), [](const QString& a, const QString& b) { return a.toULongLong() < b.toULongLong(); });
    QList<Value> out;
    for (const QString& k : std::as_const(indexKeys))
        out += groups.value(k);
    for (const QString& k : std::as_const(otherKeys))
        out += groups.value(k);
    return Value::list(out);
}

int naturalCompare(const QString& a, const QString& b);

Value listSort(const Value& list)
{
    QList<Value> items = list.items();
    struct Key {
        bool isNumber;
        double n;
        QString s;
    };
    auto keyOf = [](const Value& v) -> Key {
        if (v.type() == ValueType::Number)
            return {true, v.number(), {}};
        if (v.type() == ValueType::Boolean)
            return {false, 0, v.boolValue() ? QStringLiteral("true") : QStringLiteral("false")};
        if (v.is(VC::Primitive))
            return {false, 0, v.text()};
        return {false, 0, v.toString()};
    };
    std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
        const Key x = keyOf(a), y = keyOf(b);
        if (x.isNumber && y.isNumber)
            return x.n < y.n;
        return naturalCompare(x.isNumber ? Value::jsNumber(x.n) : x.s, y.isNumber ? Value::jsNumber(y.n) : y.s) < 0;
    });
    return Value::list(items);
}

Value listSlice(const Value& list, double start, std::optional<double> end)
{
    const QList<Value>& items = list.items();
    const qsizetype len = items.size();
    const qsizetype a = sliceIndex(start, len, 0);
    const qsizetype b = end ? sliceIndex(*end, len, len) : len;
    return Value::list(a < b ? items.mid(a, b - a) : QList<Value>());
}

bool listContainsAnyAll(const Value& list, const Args& a, bool all)
{
    QList<Value> values;
    for (qsizetype i = 1; i < a.size(); ++i)
        values << a[i].value_or(Value());
    if (values.size() == 1 && values[0].is(VC::List)) {
        const QList<Value> inner = values[0].items();
        values = inner;
    }
    for (const Value& v : values) {
        const bool has = list.listIncludes(v);
        if (all && !has)
            return false;
        if (!all && has)
            return true;
    }
    return all;
}

Registry buildRegistry()
{
    Registry r;
    const auto self = [](const Args& a) -> const Value& { return *a[0]; };

    // ---- globals ------------------------------------------------------------------------------
    r.global(QStringLiteral("now"), {}, [](CallContext&, const Args&) { return Value::date(double(QDateTime::currentMSecsSinceEpoch()), true); });
    r.global(QStringLiteral("today"), {}, [](CallContext&, const Args&) {
        return Value::date(double(QDateTime::currentMSecsSinceEpoch()), true).dateOnly();
    });
    r.global(QStringLiteral("date"), {P("input", {VC::String, VC::Date})}, [](CallContext&, const Args& a) {
        const Value& e = *a[0];
        if (e.is(VC::Date))
            return e;
        if (auto d = Value::parseDate(e.text()))
            return *d;
        static const QRegularExpression twelve(QStringLiteral("\\A\\d{12}\\z")), eight(QStringLiteral("\\A\\d{8}\\z"));
        const QString& s = e.text();
        if (twelve.match(s).hasMatch()) {
            const QDate d(s.left(4).toInt(), 1, 1);
            QDateTime dt(d.addMonths(s.mid(4, 2).toInt() - 1).addDays(s.mid(6, 2).toInt() - 1),
                         QTime(0, 0).addSecs(s.mid(8, 2).toInt() * 3600 + s.mid(10, 2).toInt() * 60));
            return Value::date(double(dt.toMSecsSinceEpoch()), true);
        }
        if (eight.match(s).hasMatch()) {
            const QDate d = QDate(s.left(4).toInt(), 1, 1).addMonths(s.mid(4, 2).toInt() - 1).addDays(s.mid(6, 2).toInt() - 1);
            return Value::date(double(QDateTime(d, QTime(0, 0)).toMSecsSinceEpoch()), false);
        }
        return Value();
    });
    r.global(QStringLiteral("if"), {P("condition", {VC::Boolean}), P("true_result", {VC::Any}), Opt("false_result", {VC::Any})},
             [](CallContext&, const Args& a) { return a[0]->boolValue() ? *a[1] : a.value(2).value_or(Value()); });
    r.global(QStringLiteral("random"), {}, [](CallContext&, const Args&) { return Value::number(QRandomGenerator::global()->generateDouble()); });
    auto minMax = [](bool isMax) {
        return [isMax](CallContext&, const Args& a) {
            QList<double> n;
            for (const auto& v : a)
                if (v)
                    n << v->number();
            if (n.isEmpty())
                return Value();
            double out = n[0];
            for (double v : n) {
                if (std::isnan(v))
                    return Value::number(kNaN);
                out = isMax ? std::max(out, v) : std::min(out, v);
            }
            return Value::number(out);
        };
    };
    r.global(QStringLiteral("min"), {Var("inputs", {VC::Number})}, minMax(false));
    r.global(QStringLiteral("max"), {Var("inputs", {VC::Number})}, minMax(true));
    r.global(QStringLiteral("list"), {P("self", {VC::NotNull})}, [self](CallContext&, const Args& a) {
        return self(a).is(VC::List) ? self(a) : Value::list({self(a)});
    });
    r.global(QStringLiteral("link"), {P("self", {VC::String, VC::File}), Opt("display", {VC::Any})}, [](CallContext& ctx, const Args& a) {
        const Value& e = *a[0];
        const Value display = a.value(1).value_or(Value());
        if (e.type() == ValueType::Url || (e.is(VC::String) && Value::isUrl(e.text())))
            return Value::url(e.text(), &display);
        if (e.is(VC::File))
            return Value::link(ctx.ds(), e.filePath(), QString(), &display);
        if (auto l = Value::parseLink(ctx.ds(), e.text(), QString()))
            return *l;
        return Value::link(ctx.ds(), e.text(), QString(), &display);
    });
    r.global(QStringLiteral("number"), {P("input", {VC::NotNull})}, [](CallContext&, const Args& a) {
        const Value& e = *a[0];
        if (e.type() == ValueType::Number)
            return e;
        if (e.is(VC::String)) {
            const double n = Value::jsParseFloat(e.text());
            if (std::isnan(n))
                throw EvalError{QStringLiteral("Unable to parse \"%1\" as a number.").arg(e.text())};
            return Value::number(n);
        }
        if (e.type() == ValueType::Boolean)
            return Value::number(e.boolValue() ? 1 : 0);
        if (e.is(VC::Date))
            return Value::number(e.dateMs());
        throw EvalError{QStringLiteral("Unable to convert \"%1\" to a number.").arg(e.typeName())};
    });
    r.global(QStringLiteral("duration"), {P("input", {VC::String})}, [](CallContext&, const Args& a) {
        return Value::parseDuration(a[0]->text()).value_or(Value());
    });
    r.global(QStringLiteral("image"), {P("input", {VC::String, VC::File, VC::Link, VC::Url})}, [](CallContext&, const Args& a) {
        const Value& e = *a[0];
        if (e.type() == ValueType::Link) {
            const QString t = e.resolve();
            return t.isEmpty() ? Value() : Value::image(t);
        }
        if (e.is(VC::String))
            return Value::image(e.text());
        if (e.is(VC::File))
            return Value::image(e.filePath());
        return Value();
    });
    r.global(QStringLiteral("icon"), {P("input", {VC::String})}, [](CallContext&, const Args& a) { return Value::icon(a[0]->text()); });
    r.global(QStringLiteral("file"), {P("input", {VC::String, VC::Link, VC::File})}, [](CallContext& ctx, const Args& a) {
        const Value& e = *a[0];
        if (e.is(VC::File))
            return e;
        QString t;
        if (e.type() == ValueType::Link) {
            t = e.resolve();
        } else if (ctx.ds()) {
            const qsizetype hash = e.text().indexOf(u'#');
            t = ctx.ds()->resolveLink(hash < 0 ? e.text() : e.text().left(hash), QString());
        }
        return t.isEmpty() ? Value() : Value::file(ctx.ds(), t);
    });
    r.global(QStringLiteral("html"), {P("input", {VC::String})}, [](CallContext&, const Args& a) { return Value::html(a[0]->text()); });
    r.global(QStringLiteral("escapeHTML"), {P("input", {VC::String})}, [](CallContext&, const Args& a) {
        QString s = a[0]->text();
        s.replace(u'&', QLatin1String("&amp;")).replace(u'<', QLatin1String("&lt;")).replace(u'>', QLatin1String("&gt;")).replace(u'"', QLatin1String("&quot;"));
        return Value::string(s);
    });

    // ---- any / null -----------------------------------------------------------------------------
    r.method(VC::Any, QStringLiteral("toString"), {P("self", {VC::Any})}, [self](CallContext&, const Args& a) { return Value::string(self(a).toString()); });
    r.method(VC::Any, QStringLiteral("isTruthy"), {P("self", {VC::Any})}, [self](CallContext&, const Args& a) { return Value::boolean(self(a).isTruthy()); });
    r.method(VC::Any, QStringLiteral("isType"), {P("self", {VC::Any}), P("type", {VC::String})},
             [self](CallContext&, const Args& a) { return Value::boolean(self(a).isOfType(a[1]->text())); });
    r.predicate(VC::Null, QStringLiteral("isEmpty"), {P("self", {VC::Null})}, [](CallContext&, const Args&) { return true; });

    // ---- string ---------------------------------------------------------------------------------
    r.predicate(VC::String, QStringLiteral("startsWith"), {P("self", {VC::String}), P("query", {VC::String})},
                [](CallContext&, const Args& a) { return a[0]->text().startsWith(a[1]->text()); });
    r.predicate(VC::String, QStringLiteral("endsWith"), {P("self", {VC::String}), P("query", {VC::String})},
                [](CallContext&, const Args& a) { return a[0]->text().endsWith(a[1]->text()); });
    r.method(VC::String, QStringLiteral("trim"), {P("self", {VC::String})}, [self](CallContext&, const Args& a) {
        const QString t = jsTrim(self(a).text());
        return t == self(a).text() ? self(a) : Value::string(t);
    });
    r.method(VC::String, QStringLiteral("title"), {P("self", {VC::String})}, [self](CallContext&, const Args& a) {
        QStringList words = self(a).text().split(u' ');
        for (QString& w : words)
            if (!w.isEmpty())
                w = QString(w[0]).toUpper() + w.mid(1).toLower();
        return Value::string(words.join(u' '));
    });
    r.predicate(VC::String, QStringLiteral("isEmpty"), {P("self", {VC::String})}, [](CallContext&, const Args& a) { return a[0]->text().isEmpty(); });
    r.method(VC::String, QStringLiteral("replace"), {P("value", {VC::String}), P("pattern", {VC::String, VC::RegExp}), P("replacement", {VC::String})},
             [self](CallContext&, const Args& a) {
                 const Value& p = *a[1];
                 if (p.type() == ValueType::RegExp)
                     return Value::string(regexReplace(self(a).text(), p.regex(), p.regexFlags().contains(u'g'), a[2]->text()));
                 if (p.text().isEmpty())
                     return self(a);
                 return Value::string(self(a).text().split(p.text()).join(a[2]->text()));
             });
    r.method(VC::String, QStringLiteral("reverse"), {P("self", {VC::String})}, [self](CallContext&, const Args& a) {
        QString s = self(a).text();
        std::reverse(s.begin(), s.end());
        return Value::string(s);
    });
    r.method(VC::String, QStringLiteral("lower"), {P("self", {VC::String})}, [self](CallContext&, const Args& a) { return Value::string(self(a).text().toLower()); });
    r.method(VC::String, QStringLiteral("split"), {P("self", {VC::String}), P("separator", {VC::String, VC::RegExp}), Opt("n", {VC::Number})},
             [self](CallContext&, const Args& a) {
                 std::optional<double> n;
                 if (a.value(2))
                     n = a[2]->number();
                 return Value::list(splitString(self(a).text(), *a[1], n));
             });
    r.predicate(VC::String, QStringLiteral("contains"), {P("self", {VC::String}), P("value", {VC::String})}, [](CallContext&, const Args& a) {
        return !a[1]->text().isEmpty() && a[0]->text().contains(a[1]->text(), Qt::CaseInsensitive);
    });
    r.predicate(VC::String, QStringLiteral("containsAny"), {P("self", {VC::String}), Var("values", {VC::String})}, [](CallContext&, const Args& a) {
        for (qsizetype i = 1; i < a.size(); ++i)
            if (a[i] && a[0]->text().contains(a[i]->text(), Qt::CaseInsensitive))
                return true;
        return false;
    });
    r.predicate(VC::String, QStringLiteral("containsAll"), {P("self", {VC::String}), Var("values", {VC::String})}, [](CallContext&, const Args& a) {
        for (qsizetype i = 1; i < a.size(); ++i)
            if (a[i] && !a[0]->text().contains(a[i]->text(), Qt::CaseInsensitive))
                return false;
        return true;
    });
    r.method(VC::String, QStringLiteral("slice"), {P("self", {VC::String}), P("start", {VC::Number}), Opt("end", {VC::Number})},
             [self](CallContext&, const Args& a) {
                 const QString& s = self(a).text();
                 const qsizetype from = sliceIndex(a[1]->number(), s.size(), 0);
                 const qsizetype to = a.value(2) ? sliceIndex(a[2]->number(), s.size(), s.size()) : s.size();
                 return Value::string(from < to ? s.mid(from, to - from) : QString());
             });
    r.method(VC::String, QStringLiteral("repeat"), {P("self", {VC::String}), P("count", {VC::Number})}, [self](CallContext&, const Args& a) {
        const double c = a[1]->number();
        const double n = toIntegerOrInfinity(c);
        if (n < 0 || std::isinf(n))
            throw EvalError{QStringLiteral("Invalid count value: ") + Value::jsNumber(c)};
        if (double(self(a).text().size()) * n > 1e8)
            throw EvalError{QStringLiteral("Invalid string length")};
        return Value::string(self(a).text().repeated(qsizetype(n)));
    });

    // ---- number ---------------------------------------------------------------------------------
    r.method(VC::Number, QStringLiteral("round"), {P("self", {VC::Number}), Opt("digits", {VC::Number})}, [](CallContext&, const Args& a) {
        const double x = a[0]->number();
        if (a.value(1) && a[1]->number() > 0) {
            const double f = std::pow(10.0, a[1]->number());
            return Value::number(jsRound(x * f) / f);
        }
        return Value::number(jsRound(x));
    });
    r.method(VC::Number, QStringLiteral("ceil"), {P("self", {VC::Number})}, [](CallContext&, const Args& a) { return Value::number(std::ceil(a[0]->number())); });
    r.method(VC::Number, QStringLiteral("floor"), {P("self", {VC::Number})}, [](CallContext&, const Args& a) { return Value::number(std::floor(a[0]->number())); });
    r.method(VC::Number, QStringLiteral("abs"), {P("self", {VC::Number})}, [](CallContext&, const Args& a) { return Value::number(std::abs(a[0]->number())); });
    r.method(VC::Number, QStringLiteral("toFixed"), {P("self", {VC::Number}), P("precision", {VC::Number})},
             [](CallContext&, const Args& a) { return Value::string(jsToFixed(a[0]->number(), a[1]->number())); });
    r.predicate(VC::Number, QStringLiteral("isEmpty"), {P("self", {VC::Number})}, [](CallContext&, const Args&) { return false; });

    // ---- list -----------------------------------------------------------------------------------
    r.predicate(VC::List, QStringLiteral("isEmpty"), {P("self", {VC::List})}, [](CallContext&, const Args& a) { return a[0]->items().isEmpty(); });
    auto extremeDate = [](bool latest) {
        return [latest](CallContext&, const Args& a) {
            const QList<double> d = datesOf(*a[0]);
            if (d.isEmpty())
                return Value();
            double t = d[0];
            for (double v : d)
                if (latest ? v > t : v < t)
                    t = v;
            return Value::date(t, true);
        };
    };
    r.method(VC::List, QStringLiteral("earliest"), {P("self", {VC::List})}, extremeDate(false));
    r.method(VC::List, QStringLiteral("latest"), {P("self", {VC::List})}, extremeDate(true));
    r.method(VC::List, QStringLiteral("median"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        QList<double> n = numbersOf(*a[0]);
        std::sort(n.begin(), n.end());
        const qsizetype t = n.size();
        if (t == 0)
            return Value();
        return Value::number(t % 2 == 0 ? (n[t / 2] + n[t / 2 - 1]) / 2 : n[(t - 1) / 2]);
    });
    r.method(VC::List, QStringLiteral("mean"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        const Value s = listSum(*a[0]);
        return s.isNull() ? Value() : Value::number(s.number() / double(a[0]->items().size()));
    });
    r.method(VC::List, QStringLiteral("max"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        const QList<double> n = numbersOf(*a[0]);
        if (n.isEmpty())
            return Value();
        double m = -std::numeric_limits<double>::infinity();
        for (double v : n) {
            if (std::isnan(v))
                return Value::number(kNaN);
            m = std::max(m, v);
        }
        return Value::number(m);
    });
    r.method(VC::List, QStringLiteral("min"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        const QList<double> n = numbersOf(*a[0]);
        if (n.isEmpty())
            return Value();
        double m = std::numeric_limits<double>::infinity();
        for (double v : n) {
            if (std::isnan(v))
                return Value::number(kNaN);
            m = std::min(m, v);
        }
        return Value::number(m);
    });
    r.method(VC::List, QStringLiteral("sum"), {P("self", {VC::List})}, [](CallContext&, const Args& a) { return listSum(*a[0]); });
    r.method(VC::List, QStringLiteral("stddev"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        const QList<double> n = numbersOf(*a[0]);
        if (n.isEmpty())
            return Value();
        auto mean = [](const QList<double>& v) {
            double s = 0;
            for (double x : v)
                s += x;
            return s / double(v.size());
        };
        const double m = mean(n);
        QList<double> sq;
        for (double x : n)
            sq << std::pow(x - m, 2);
        return Value::number(std::sqrt(mean(sq)));
    });
    r.method(VC::List, QStringLiteral("join"), {P("self", {VC::List}), P("separator", {VC::String})}, [](CallContext&, const Args& a) {
        QStringList parts;
        for (const Value& v : a[0]->items())
            parts << v.toString();
        return Value::string(parts.join(a[1]->text()));
    });
    r.method(VC::List, QStringLiteral("reverse"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        QList<Value> items = a[0]->items();
        std::reverse(items.begin(), items.end());
        return Value::list(items);
    });
    r.method(VC::List, QStringLiteral("flat"), {P("self", {VC::List})}, [](CallContext&, const Args& a) {
        QList<Value> out;
        flattenInto(a[0]->items(), out);
        return Value::list(out);
    });
    r.method(VC::List, QStringLiteral("unique"), {P("self", {VC::List})}, [](CallContext&, const Args& a) { return listUnique(*a[0]); });
    r.predicate(VC::List, QStringLiteral("contains"), {P("self", {VC::List}), P("value", {VC::Any})},
                [](CallContext&, const Args& a) { return a[0]->listIncludes(a[1].value_or(Value())); });
    r.predicate(VC::List, QStringLiteral("containsAny"), {P("self", {VC::List}), Var("values", {VC::Any})},
                [](CallContext&, const Args& a) { return listContainsAnyAll(*a[0], a, false); });
    r.predicate(VC::List, QStringLiteral("containsAll"), {P("self", {VC::List}), Var("values", {VC::Any})},
                [](CallContext&, const Args& a) { return listContainsAnyAll(*a[0], a, true); });
    r.method(VC::List, QStringLiteral("slice"), {P("self", {VC::List}), P("start", {VC::Number}), Opt("end", {VC::Number})},
             [](CallContext&, const Args& a) {
                 std::optional<double> end;
                 if (a.value(2))
                     end = a[2]->number();
                 return listSlice(*a[0], a[1]->number(), end);
             });
    r.method(VC::List, QStringLiteral("sort"), {P("self", {VC::List})}, [](CallContext&, const Args& a) { return listSort(*a[0]); });
    // map / filter / reduce are evaluated by the caller (they need the callback); these only run
    // when the name is spelled differently ("Map"), like Obsidian's placeholders.
    r.method(VC::List, QStringLiteral("map"), {P("self", {VC::List}), P("value", {VC::Any})}, [](CallContext&, const Args& a) { return *a[0]; });
    r.method(VC::List, QStringLiteral("filter"), {P("self", {VC::List}), P("value", {VC::Boolean})}, [](CallContext&, const Args& a) { return *a[0]; });
    r.method(VC::List, QStringLiteral("reduce"), {P("self", {VC::List}), P("value", {VC::Any}), P("acc", {VC::Any})},
             [](CallContext&, const Args& a) { return *a[0]; });

    // ---- object ---------------------------------------------------------------------------------
    r.predicate(VC::Object, QStringLiteral("isEmpty"), {P("self", {VC::Object})}, [](CallContext&, const Args& a) { return a[0]->objectData()->keys.isEmpty(); });
    r.method(VC::Object, QStringLiteral("keys"), {P("self", {VC::Object})}, [](CallContext&, const Args& a) {
        QList<Value> out;
        for (const QString& k : a[0]->objectData()->keys)
            out << Value::string(k);
        return Value::list(out);
    });
    auto objectValues = [](CallContext&, const Args& a) {
        QList<Value> out;
        const ObjectData* o = a[0]->objectData();
        for (int i = 0; i < o->keys.size(); ++i)
            out << o->at(i);
        return Value::list(out);
    };
    r.method(VC::Object, QStringLiteral("values"), {P("self", {VC::Object})}, objectValues);
    r.method(VC::Object, QStringLiteral("map"), {P("self", {VC::Object}), P("value", {VC::Any})}, objectValues);
    r.method(VC::Object, QStringLiteral("filter"), {P("self", {VC::Object}), P("value", {VC::Boolean})}, [](CallContext&, const Args& a) { return *a[0]; });

    // ---- regexp ---------------------------------------------------------------------------------
    r.predicate(VC::RegExp, QStringLiteral("matches"), {P("self", {VC::RegExp}), P("value", {VC::String})},
                [](CallContext&, const Args& a) { return a[0]->regex().match(a[1]->text()).hasMatch(); });

    // ---- date -----------------------------------------------------------------------------------
    r.method(VC::Date, QStringLiteral("format"), {P("self", {VC::Date}), P("format", {VC::String})}, [](CallContext&, const Args& a) {
        const double ms = a[0]->dateMs();
        if (std::isnan(ms))
            return Value::string(QStringLiteral("Invalid date"));
        return Value::string(formatMoment(QDateTime::fromMSecsSinceEpoch(qint64(ms)), a[1]->text()));
    });
    r.method(VC::Date, QStringLiteral("date"), {P("self", {VC::Date})}, [](CallContext&, const Args& a) {
        return a[0]->hasTime() ? a[0]->dateOnly() : *a[0];
    });
    r.method(VC::Date, QStringLiteral("time"), {P("self", {VC::Date})}, [](CallContext&, const Args& a) { return Value::string(a[0]->printTime()); });
    r.method(VC::Date, QStringLiteral("relative"), {P("self", {VC::Date})},
             [](CallContext&, const Args& a) { return Value::relativeDate(a[0]->dateMs(), a[0]->hasTime()); });
    r.predicate(VC::Date, QStringLiteral("isEmpty"), {P("self", {VC::Date})}, [](CallContext&, const Args&) { return false; });

    // ---- file -----------------------------------------------------------------------------------
    r.method(VC::File, QStringLiteral("asLink"), {P("self", {VC::File}), Opt("display", {VC::Any})}, [](CallContext& ctx, const Args& a) {
        const Value display = a.value(1).value_or(Value());
        return Value::link(ctx.ds(), a[0]->filePath(), QString(), &display);
    });
    r.predicate(VC::File, QStringLiteral("hasLink"), {P("self", {VC::File}), P("dest", {VC::File, VC::Link, VC::String})},
                [](CallContext& ctx, const Args& a) {
                    const Value& t = *a[1];
                    QString target, text;
                    if (t.is(VC::File)) {
                        target = t.filePath();
                        text = target;
                    } else if (t.type() == ValueType::Link) {
                        target = t.resolve();
                        text = target.isEmpty() ? t.text() : target;
                    } else {
                        const qsizetype hash = t.text().indexOf(u'#');
                        target = ctx.ds() ? ctx.ds()->resolveLink(hash < 0 ? t.text() : t.text().left(hash), QString()) : QString();
                        text = t.text();
                    }
                    const Value links = a[0]->fileLinks();
                    for (const Value& l : links.items()) {
                        if (l.type() != ValueType::Link)
                            continue;
                        if (!target.isEmpty() && l.resolve() == target)
                            return true;
                        if (target.isEmpty() && l.text() == text)
                            return true;
                    }
                    return false;
                });
    r.predicate(VC::File, QStringLiteral("inFolder"), {P("self", {VC::File}), P("folder", {VC::String})}, [](CallContext&, const Args& a) {
        const QString n = normalizeFolder(a[1]->text());
        return n == u"/" || a[0]->filePath().startsWith(n + u'/');
    });
    r.predicate(VC::File, QStringLiteral("hasTag"), {P("self", {VC::File}), Var("values", {VC::String})}, [](CallContext&, const Args& a) {
        QList<Value> queries;
        for (qsizetype i = 1; i < a.size(); ++i)
            if (a[i])
                queries << Value::tag(a[i]->text());
        const Value tags = a[0]->fileTags();
        for (const Value& t : tags.items())
            for (const Value& q : std::as_const(queries))
                if (t.tagMatches(q))
                    return true;
        return false;
    });
    r.predicate(VC::File, QStringLiteral("hasProperty"), {P("self", {VC::File}), P("property", {VC::String})}, [](CallContext&, const Args& a) {
        return a[0]->fileProperties().objectData()->keys.contains(a[1]->text());
    });

    // ---- link / tag -----------------------------------------------------------------------------
    r.predicate(VC::Link, QStringLiteral("linksTo"), {P("self", {VC::Link}), P("file", {VC::File})}, [](CallContext& ctx, const Args& a) {
        const QString& data = a[0]->text();
        const qsizetype hash = data.indexOf(u'#');
        const QString t = ctx.ds() ? ctx.ds()->resolveLink(hash < 0 ? data : data.left(hash), QString()) : QString();
        return !t.isEmpty() && t == a[1]->filePath();
    });
    r.method(VC::Link, QStringLiteral("asFile"), {P("self", {VC::Link})}, [](CallContext& ctx, const Args& a) {
        const QString t = a[0]->resolve();
        return t.isEmpty() ? Value() : Value::file(ctx.ds(), t);
    });
    r.predicate(VC::Tag, QStringLiteral("matches"), {P("self", {VC::Tag}), P("tag", {VC::String})},
                [](CallContext&, const Args& a) { return a[0]->tagMatches(*a[1]); });
    return r;
}

const Registry& registry()
{
    static const Registry r = buildRegistry();
    return r;
}

int naturalCompare(const QString& a, const QString& b)
{
    thread_local const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        c.setIgnorePunctuation(false);
        return c;
    }();
    return collator.compare(a, b);
}

} // namespace

const Function* findMethod(const Value& receiver, const QString& name)
{
    const QString key = name.toLower();
    const Registry& r = registry();
    for (ValueClass c : classChain(receiver.type())) {
        auto it = r.methods.constFind(int(c));
        if (it == r.methods.cend())
            continue;
        auto f = it->constFind(key);
        if (f != it->cend())
            return &*f;
    }
    return nullptr;
}

const Function* findGlobal(const QString& name)
{
    const Registry& r = registry();
    auto it = r.globals.constFind(name.toLower());
    return it == r.globals.cend() ? nullptr : &*it;
}

QString className(ValueClass c)
{
    switch (c) {
    case ValueClass::Any:
    case ValueClass::NotNull:
    case ValueClass::Primitive: return QStringLiteral("Any");
    case ValueClass::Null: return QStringLiteral("Null");
    case ValueClass::Error: return QStringLiteral("Error");
    case ValueClass::Boolean: return QStringLiteral("Boolean");
    case ValueClass::Number: return QStringLiteral("Number");
    case ValueClass::String:
    case ValueClass::Tag:
    case ValueClass::Icon: return QStringLiteral("String");
    case ValueClass::Link: return QStringLiteral("Link");
    case ValueClass::Url: return QStringLiteral("URL");
    case ValueClass::Image: return QStringLiteral("Image");
    case ValueClass::Html: return QStringLiteral("HTML");
    case ValueClass::Date: return QStringLiteral("Date");
    case ValueClass::Duration: return QStringLiteral("Duration");
    case ValueClass::List:
    case ValueClass::TagList: return QStringLiteral("List");
    case ValueClass::Object: return QStringLiteral("Object");
    case ValueClass::RegExp: return QStringLiteral("RegExp");
    case ValueClass::File: return QStringLiteral("File");
    case ValueClass::Formulas: return QStringLiteral("Formulas");
    }
    return QStringLiteral("Any");
}

QList<QPair<QString, QString>> registeredFunctions() { return registry().order; }

} // namespace oc::bases::detail
