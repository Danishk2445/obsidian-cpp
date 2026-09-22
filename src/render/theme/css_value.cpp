#include "render/theme/css_value.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oc {

double CssValue::toNumber(double fallback) const
{
    switch (kind) {
    case Number:
    case Angle:
    case Time:
        return num;
    case Percent:
        return num / 100.0;
    case Length:
        return len.px;
    case List:
        return items.isEmpty() ? fallback : items.first().toNumber(fallback);
    default:
        return fallback;
    }
}

double CssValue::toPx(double emPx, double remPx, double fallback) const
{
    switch (kind) {
    case Length:
        return len.resolve(emPx, remPx);
    case Number:
        return num; // unitless 0 or px-less numbers
    case Percent:
        return emPx * num / 100.0;
    case List:
        return items.isEmpty() ? fallback : items.first().toPx(emPx, remPx, fallback);
    default:
        return fallback;
    }
}

QColor CssValue::toColor(const QColor& fallback) const
{
    if (kind == Color)
        return color;
    if (kind == List && !items.isEmpty())
        return items.first().toColor(fallback);
    return fallback;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------------------------
struct Tok {
    enum T { End, Num, Ident, Hash, Str, Func, LParen, RParen, Comma, Slash, Op, Unknown };
    T t = End;
    double num = 0;
    QString unit; // for Num: "", "px", "em", "%", ...
    QString text; // Ident/Func name/Hash (without #)/Str/Op char
};

class Lexer {
public:
    explicit Lexer(QStringView s) : m_s(s) {}

    QList<Tok> all()
    {
        QList<Tok> out;
        for (;;) {
            Tok t = next();
            out.push_back(t);
            if (t.t == Tok::End)
                break;
        }
        return out;
    }

private:
    static bool isIdentStart(QChar c) { return c.isLetter() || c == u'_' || c.unicode() >= 0x80; }
    static bool isIdentChar(QChar c) { return isIdentStart(c) || c.isDigit() || c == u'-'; }

    Tok next()
    {
        while (m_i < m_s.size() && m_s[m_i].isSpace())
            ++m_i;
        Tok t;
        if (m_i >= m_s.size())
            return t;
        const QChar c = m_s[m_i];
        const QChar c1 = m_i + 1 < m_s.size() ? m_s[m_i + 1] : QChar();
        const QChar c2 = m_i + 2 < m_s.size() ? m_s[m_i + 2] : QChar();
        auto startsNumber = [&]() {
            if (c.isDigit())
                return true;
            if (c == u'.' && c1.isDigit())
                return true;
            if ((c == u'-' || c == u'+') && (c1.isDigit() || (c1 == u'.' && c2.isDigit())))
                return true;
            return false;
        };
        if (startsNumber()) {
            // A '-' directly after a number/ident/paren is a binary operator (calc needs spaces, but be lenient).
            if ((c == u'-' || c == u'+') && m_prevValue) {
                ++m_i;
                t.t = Tok::Op;
                t.text = c;
                m_prevValue = false;
                return t;
            }
            const qsizetype start = m_i;
            if (c == u'-' || c == u'+')
                ++m_i;
            while (m_i < m_s.size() && (m_s[m_i].isDigit() || m_s[m_i] == u'.'))
                ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == u'e' || m_s[m_i] == u'E') && m_i + 1 < m_s.size()
                && (m_s[m_i + 1].isDigit() || ((m_s[m_i + 1] == u'-' || m_s[m_i + 1] == u'+') && m_i + 2 < m_s.size() && m_s[m_i + 2].isDigit()))) {
                m_i += 2;
                while (m_i < m_s.size() && m_s[m_i].isDigit())
                    ++m_i;
            }
            t.t = Tok::Num;
            t.num = m_s.mid(start, m_i - start).toDouble();
            if (m_i < m_s.size() && m_s[m_i] == u'%') {
                t.unit = QStringLiteral("%");
                ++m_i;
            } else {
                const qsizetype us = m_i;
                while (m_i < m_s.size() && m_s[m_i].isLetter())
                    ++m_i;
                t.unit = m_s.mid(us, m_i - us).toString().toLower();
            }
            m_prevValue = true;
            return t;
        }
        if (c == u'#') {
            const qsizetype start = ++m_i;
            while (m_i < m_s.size() && (m_s[m_i].isLetterOrNumber()))
                ++m_i;
            t.t = Tok::Hash;
            t.text = m_s.mid(start, m_i - start).toString();
            m_prevValue = true;
            return t;
        }
        if (c == u'"' || c == u'\'') {
            const QChar q = c;
            ++m_i;
            QString s;
            while (m_i < m_s.size() && m_s[m_i] != q) {
                if (m_s[m_i] == u'\\' && m_i + 1 < m_s.size())
                    ++m_i;
                s += m_s[m_i++];
            }
            ++m_i;
            t.t = Tok::Str;
            t.text = s;
            m_prevValue = true;
            return t;
        }
        if (isIdentStart(c) || (c == u'-' && (isIdentStart(c1) || c1 == u'-'))) {
            const qsizetype start = m_i;
            ++m_i;
            while (m_i < m_s.size() && isIdentChar(m_s[m_i]))
                ++m_i;
            t.text = m_s.mid(start, m_i - start).toString();
            if (m_i < m_s.size() && m_s[m_i] == u'(') {
                ++m_i;
                t.t = Tok::Func;
                t.text = t.text.toLower();
                m_prevValue = false;
            } else {
                t.t = Tok::Ident;
                m_prevValue = true;
            }
            return t;
        }
        ++m_i;
        switch (c.unicode()) {
        case '(':
            t.t = Tok::LParen;
            m_prevValue = false;
            break;
        case ')':
            t.t = Tok::RParen;
            m_prevValue = true;
            break;
        case ',':
            t.t = Tok::Comma;
            m_prevValue = false;
            break;
        case '/':
            t.t = Tok::Slash;
            t.text = c;
            m_prevValue = false;
            break;
        case '*':
        case '+':
        case '-':
            t.t = Tok::Op;
            t.text = c;
            m_prevValue = false;
            break;
        default:
            t.t = Tok::Unknown;
            t.text = c;
            m_prevValue = false;
            break;
        }
        return t;
    }

    QStringView m_s;
    qsizetype m_i = 0;
    bool m_prevValue = false;
};

// ---------------------------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------------------------
double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double linearToSrgb(double c)
{
    const double v = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return std::clamp(v, 0.0, 1.0);
}

struct Lab {
    double L, a, b;
};

Lab toOklab(const QColor& c)
{
    const double r = srgbToLinear(c.redF()), g = srgbToLinear(c.greenF()), b = srgbToLinear(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

QColor fromOklab(Lab lab, double alpha)
{
    const double l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    const double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return QColor::fromRgbF(float(linearToSrgb(r)), float(linearToSrgb(g)), float(linearToSrgb(b)), float(std::clamp(alpha, 0.0, 1.0)));
}

QColor hslColor(double hDeg, double s, double l, double a)
{
    hDeg = std::fmod(hDeg, 360.0);
    if (hDeg < 0)
        hDeg += 360.0;
    s = std::clamp(s, 0.0, 1.0);
    l = std::clamp(l, 0.0, 1.0);
    // CSS hsl -> rgb
    auto f = [&](double n) {
        const double k = std::fmod(n + hDeg / 30.0, 12.0);
        const double aa = s * std::min(l, 1 - l);
        return l - aa * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
    };
    return QColor::fromRgbF(float(f(0)), float(f(8)), float(f(4)), float(std::clamp(a, 0.0, 1.0)));
}

// ---------------------------------------------------------------------------------------------
// Parser / evaluator
// ---------------------------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(QList<Tok> toks) : m_t(std::move(toks)) {}

    // value := component (',' component)*
    CssValue parseValue()
    {
        QList<CssValue> comps;
        comps.push_back(parseComponent());
        while (peek().t == Tok::Comma) {
            ++m_p;
            comps.push_back(parseComponent());
        }
        if (comps.size() == 1)
            return comps.first();
        CssValue v;
        v.kind = CssValue::List;
        v.commaList = true;
        v.items = comps;
        return v;
    }

    bool atEnd() const { return m_t[m_p].t == Tok::End; }

private:
    const Tok& peek(int o = 0) const { return m_t[std::min<qsizetype>(m_p + o, m_t.size() - 1)]; }

    // component := term+ (space separated)
    CssValue parseComponent()
    {
        QList<CssValue> terms;
        while (true) {
            const Tok& t = peek();
            if (t.t == Tok::End || t.t == Tok::Comma || t.t == Tok::RParen)
                break;
            if (t.t == Tok::Slash || (t.t == Tok::Op)) { // stray operator outside calc: skip
                ++m_p;
                continue;
            }
            CssValue v = parseTerm(); // unknown terms are dropped
            if (v.isValid())
                terms.push_back(v);
        }
        if (terms.size() == 1)
            return terms.first();
        if (terms.isEmpty())
            return {};
        CssValue v;
        v.kind = CssValue::List;
        v.items = terms;
        return v;
    }

    static CssValue numberValue(double n, const QString& unit)
    {
        CssValue v;
        if (unit.isEmpty()) {
            v.kind = CssValue::Number;
            v.num = n;
        } else if (unit == u"%") {
            v.kind = CssValue::Percent;
            v.num = n;
        } else if (unit == u"px") {
            v.kind = CssValue::Length;
            v.len.px = n;
        } else if (unit == u"em") {
            v.kind = CssValue::Length;
            v.len.em = n;
        } else if (unit == u"rem") {
            v.kind = CssValue::Length;
            v.len.rem = n;
        } else if (unit == u"pt") {
            v.kind = CssValue::Length;
            v.len.px = n * 96.0 / 72.0;
        } else if (unit == u"ch") {
            v.kind = CssValue::Length;
            v.len.em = n * 0.5;
        } else if (unit == u"deg") {
            v.kind = CssValue::Angle;
            v.num = n;
        } else if (unit == u"turn") {
            v.kind = CssValue::Angle;
            v.num = n * 360.0;
        } else if (unit == u"rad") {
            v.kind = CssValue::Angle;
            v.num = n * 180.0 / std::numbers::pi;
        } else if (unit == u"ms") {
            v.kind = CssValue::Time;
            v.num = n;
        } else if (unit == u"s") {
            v.kind = CssValue::Time;
            v.num = n * 1000.0;
        } else {
            v.kind = CssValue::Ident; // unsupported unit (vw, vh, ...): keep textual
            v.text = QString::number(n) + unit;
        }
        return v;
    }

    CssValue parseTerm()
    {
        const Tok t = peek();
        ++m_p;
        switch (t.t) {
        case Tok::Num:
            return numberValue(t.num, t.unit);
        case Tok::Hash: {
            CssValue v;
            const QColor c = CssEvaluator::parseHexColor(t.text);
            if (c.isValid()) {
                v.kind = CssValue::Color;
                v.color = c;
            }
            return v;
        }
        case Tok::Str: {
            CssValue v;
            v.kind = CssValue::String;
            v.text = t.text;
            return v;
        }
        case Tok::Ident: {
            CssValue v;
            const QColor c = CssEvaluator::namedColor(t.text.toLower());
            if (c.isValid()) {
                v.kind = CssValue::Color;
                v.color = c;
            } else {
                v.kind = CssValue::Ident;
                v.text = t.text;
            }
            return v;
        }
        case Tok::Func:
            return parseFunction(t.text);
        case Tok::LParen: {
            CssValue v = parseSum();
            if (peek().t == Tok::RParen)
                ++m_p;
            return v;
        }
        default:
            return {};
        }
    }

    // Collect function arguments as a list of comma-separated components; slash marks alpha separator.
    struct Args {
        QList<CssValue> values; // flattened in order
        int alphaIndex = -1;    // index in values where '/' alpha starts (space syntax)
    };

    Args parseArgs()
    {
        Args a;
        while (peek().t != Tok::End && peek().t != Tok::RParen) {
            const Tok& t = peek();
            if (t.t == Tok::Comma) {
                ++m_p;
                continue;
            }
            if (t.t == Tok::Slash) {
                ++m_p;
                a.alphaIndex = int(a.values.size());
                continue;
            }
            CssValue v = parseTerm();
            a.values.push_back(v);
        }
        if (peek().t == Tok::RParen)
            ++m_p;
        return a;
    }

    CssValue parseFunction(const QString& name)
    {
        if (name == u"calc")
            return finishCalc();
        if (name == u"min" || name == u"max" || name == u"clamp") {
            QList<CssValue> vals;
            while (peek().t != Tok::End && peek().t != Tok::RParen) {
                if (peek().t == Tok::Comma) {
                    ++m_p;
                    continue;
                }
                vals.push_back(parseSum());
            }
            if (peek().t == Tok::RParen)
                ++m_p;
            return minMax(name, vals);
        }
        if (name == u"hsl" || name == u"hsla" || name == u"rgb" || name == u"rgba") {
            // Arguments may be calc-able expressions: parse as sums.
            QList<CssValue> vals;
            int alphaIdx = -1;
            while (peek().t != Tok::End && peek().t != Tok::RParen) {
                if (peek().t == Tok::Comma) {
                    ++m_p;
                    continue;
                }
                if (peek().t == Tok::Slash) {
                    ++m_p;
                    alphaIdx = int(vals.size());
                    continue;
                }
                vals.push_back(parseSum());
            }
            if (peek().t == Tok::RParen)
                ++m_p;
            // Flatten nested lists (e.g. "rgba(var(--rgb), .5)" where var expanded to "0, 0, 0")
            QList<CssValue> flat;
            for (const CssValue& v : vals) {
                if (v.kind == CssValue::List)
                    flat += v.items;
                else
                    flat.push_back(v);
            }
            Q_UNUSED(alphaIdx);
            return colorFunction(name, flat);
        }
        if (name == u"color-mix")
            return colorMix();
        // Unknown function: skip balanced
        int depth = 1;
        while (peek().t != Tok::End && depth > 0) {
            if (peek().t == Tok::LParen || peek().t == Tok::Func)
                ++depth;
            else if (peek().t == Tok::RParen)
                --depth;
            ++m_p;
        }
        CssValue v;
        v.kind = CssValue::Ident;
        v.text = name + QStringLiteral("()");
        return v;
    }

    CssValue finishCalc()
    {
        CssValue v = parseSum();
        while (peek().t != Tok::End && peek().t != Tok::RParen)
            ++m_p;
        if (peek().t == Tok::RParen)
            ++m_p;
        return v;
    }

    // sum := product (('+'|'-') product)*
    CssValue parseSum()
    {
        CssValue lhs = parseProduct();
        while (peek().t == Tok::Op && (peek().text == u"+" || peek().text == u"-")) {
            const bool minus = peek().text == u"-";
            ++m_p;
            CssValue rhs = parseProduct();
            lhs = add(lhs, rhs, minus ? -1.0 : 1.0);
        }
        return lhs;
    }

    CssValue parseProduct()
    {
        CssValue lhs = parseUnary();
        while ((peek().t == Tok::Op && peek().text == u"*") || peek().t == Tok::Slash) {
            const bool div = peek().t == Tok::Slash;
            ++m_p;
            CssValue rhs = parseUnary();
            lhs = mul(lhs, rhs, div);
        }
        return lhs;
    }

    CssValue parseUnary()
    {
        if (peek().t == Tok::Op && peek().text == u"-") {
            ++m_p;
            CssValue v = parseUnary();
            return scale(v, -1.0);
        }
        return parseTerm();
    }

    static CssValue scale(CssValue v, double f)
    {
        switch (v.kind) {
        case CssValue::Number:
        case CssValue::Percent:
        case CssValue::Angle:
        case CssValue::Time:
            v.num *= f;
            break;
        case CssValue::Length:
            v.len.px *= f;
            v.len.em *= f;
            v.len.rem *= f;
            break;
        default:
            break;
        }
        return v;
    }

    static CssValue add(const CssValue& a, const CssValue& b, double sign)
    {
        if (a.kind == CssValue::Length || b.kind == CssValue::Length) {
            CssValue r;
            r.kind = CssValue::Length;
            auto asLen = [](const CssValue& v) {
                CssLength l;
                if (v.kind == CssValue::Length)
                    l = v.len;
                else if (v.kind == CssValue::Number)
                    l.px = v.num; // unitless 0
                return l;
            };
            const CssLength la = asLen(a), lb = asLen(b);
            r.len = {la.px + sign * lb.px, la.em + sign * lb.em, la.rem + sign * lb.rem};
            return r;
        }
        CssValue r = a.isValid() ? a : b;
        if (!a.isValid() || !b.isValid())
            return r;
        r.num = a.num + sign * b.num;
        return r;
    }

    static CssValue mul(const CssValue& a, const CssValue& b, bool div)
    {
        // One side must be a plain number.
        if (b.kind == CssValue::Number) {
            if (div)
                return b.num == 0 ? CssValue{} : scale(a, 1.0 / b.num);
            return scale(a, b.num);
        }
        if (a.kind == CssValue::Number && !div)
            return scale(b, a.num);
        return a.isValid() ? a : b;
    }

    static CssValue minMax(const QString& name, const QList<CssValue>& vals)
    {
        if (vals.isEmpty())
            return {};
        // Only same-kind comparisons; lengths compared by px part + em*16 (approximation).
        auto key = [](const CssValue& v) { return v.kind == CssValue::Length ? v.len.resolve(16) : v.num; };
        if (name == u"clamp" && vals.size() == 3) {
            const double lo = key(vals[0]), mid = key(vals[1]), hi = key(vals[2]);
            if (mid < lo)
                return vals[0];
            if (mid > hi)
                return vals[2];
            return vals[1];
        }
        CssValue best = vals.first();
        for (const CssValue& v : vals) {
            if (name == u"min" ? key(v) < key(best) : key(v) > key(best))
                best = v;
        }
        return best;
    }

    static double channel(const CssValue& v, double scale255)
    {
        if (v.kind == CssValue::Percent)
            return v.num / 100.0;
        return v.toNumber() / scale255;
    }

    static CssValue colorFunction(const QString& name, const QList<CssValue>& a)
    {
        CssValue out;
        if (a.size() < 3)
            return out;
        double alpha = 1.0;
        if (a.size() >= 4)
            alpha = a[3].kind == CssValue::Percent ? a[3].num / 100.0 : a[3].toNumber(1.0);
        if (name.startsWith(u"rgb")) {
            out.kind = CssValue::Color;
            out.color = QColor::fromRgbF(float(std::clamp(channel(a[0], 255.0), 0.0, 1.0)), float(std::clamp(channel(a[1], 255.0), 0.0, 1.0)),
                                         float(std::clamp(channel(a[2], 255.0), 0.0, 1.0)), float(std::clamp(alpha, 0.0, 1.0)));
            return out;
        }
        const double h = a[0].kind == CssValue::Angle ? a[0].num : a[0].toNumber();
        auto pct = [](const CssValue& v) { return v.kind == CssValue::Percent ? v.num / 100.0 : v.toNumber() / 100.0; };
        out.kind = CssValue::Color;
        out.color = hslColor(h, pct(a[1]), pct(a[2]), alpha);
        return out;
    }

    CssValue colorMix()
    {
        // color-mix(in <space> [<hue-method> hue], <color> [<p>], <color> [<p>])
        QString space = QStringLiteral("srgb");
        struct Part {
            QColor c;
            double p = -1;
        };
        QList<Part> parts;
        int section = 0;
        while (peek().t != Tok::End && peek().t != Tok::RParen) {
            const Tok& t = peek();
            if (t.t == Tok::Comma) {
                ++m_p;
                ++section;
                continue;
            }
            if (section == 0) {
                if (t.t == Tok::Ident && t.text != u"in" && t.text != u"hue" && t.text != u"shorter" && t.text != u"longer")
                    space = t.text.toLower();
                ++m_p;
                continue;
            }
            CssValue v = parseTerm();
            if (parts.size() < section)
                parts.resize(section);
            Part& part = parts[section - 1];
            if (v.kind == CssValue::Color)
                part.c = v.color;
            else if (v.kind == CssValue::Percent)
                part.p = v.num / 100.0;
        }
        if (peek().t == Tok::RParen)
            ++m_p;
        if (parts.size() != 2 || !parts[0].c.isValid() || !parts[1].c.isValid())
            return {};
        double p1 = parts[0].p, p2 = parts[1].p;
        if (p1 < 0 && p2 < 0)
            p1 = p2 = 0.5;
        else if (p1 < 0)
            p1 = 1.0 - p2;
        else if (p2 < 0)
            p2 = 1.0 - p1;
        CssValue out;
        out.kind = CssValue::Color;
        out.color = CssEvaluator::mix(parts[0].c, p1, parts[1].c, p2, space);
        return out;
    }

    QList<Tok> m_t;
    qsizetype m_p = 0;
};

} // namespace

CssEvaluator::CssEvaluator(Lookup lookup)
    : m_lookup(std::move(lookup))
{
}

QString CssEvaluator::substitute(const QString& value) const { return substituteImpl(value, 0); }

QString CssEvaluator::substituteImpl(const QString& value, int depth) const
{
    if (depth > 32)
        return QString(); // reference cycle / runaway expansion: invalid at computed-value time
    if (!value.contains(u"var("))
        return value;
    QString out;
    out.reserve(value.size());
    qsizetype i = 0;
    while (i < value.size()) {
        const qsizetype v = value.indexOf(u"var(", i);
        if (v < 0) {
            out += QStringView(value).mid(i);
            break;
        }
        out += QStringView(value).mid(i, v - i);
        // find matching ')'
        qsizetype j = v + 4, level = 1;
        while (j < value.size() && level > 0) {
            if (value[j] == u'(')
                ++level;
            else if (value[j] == u')')
                --level;
            ++j;
        }
        const QString inner = value.mid(v + 4, (j - 1) - (v + 4));
        // split name / fallback at first top-level comma
        qsizetype comma = -1, lvl = 0;
        for (qsizetype k = 0; k < inner.size(); ++k) {
            if (inner[k] == u'(')
                ++lvl;
            else if (inner[k] == u')')
                --lvl;
            else if (inner[k] == u',' && lvl == 0) {
                comma = k;
                break;
            }
        }
        const QString name = (comma < 0 ? inner : inner.left(comma)).trimmed();
        bool found = false;
        QString raw = m_lookup ? m_lookup(name, &found) : QString();
        if (!found || raw.trimmed() == u"'??'" || raw.trimmed() == u"\"??\"") {
            raw = comma < 0 ? QString() : inner.mid(comma + 1).trimmed();
            found = comma >= 0;
        }
        out += substituteImpl(raw, depth + 1);
        i = j;
    }
    return out;
}

CssValue CssEvaluator::evaluate(const QString& value) const
{
    const QString expanded = substitute(value);
    Lexer lx(expanded);
    Parser p(lx.all());
    return p.parseValue();
}

QColor CssEvaluator::parseHexColor(QStringView s)
{
    auto hex = [](QChar c) -> int {
        const char16_t u = c.unicode();
        if (u >= '0' && u <= '9')
            return u - '0';
        if (u >= 'a' && u <= 'f')
            return u - 'a' + 10;
        if (u >= 'A' && u <= 'F')
            return u - 'A' + 10;
        return -1;
    };
    for (QChar c : s)
        if (hex(c) < 0)
            return {};
    auto two = [&](int i) { return hex(s[i]) * 16 + hex(s[i + 1]); };
    auto one = [&](int i) { return hex(s[i]) * 17; };
    switch (s.size()) {
    case 3:
        return QColor(one(0), one(1), one(2));
    case 4:
        return QColor(one(0), one(1), one(2), one(3));
    case 6:
        return QColor(two(0), two(2), two(4));
    case 8:
        return QColor(two(0), two(2), two(4), two(6));
    default:
        return {};
    }
}

QColor CssEvaluator::namedColor(const QString& ident)
{
    static const QHash<QString, QColor> names = {
        {QStringLiteral("transparent"), QColor(0, 0, 0, 0)},
        {QStringLiteral("white"), QColor(255, 255, 255)},
        {QStringLiteral("black"), QColor(0, 0, 0)},
        {QStringLiteral("red"), QColor(255, 0, 0)},
        {QStringLiteral("green"), QColor(0, 128, 0)},
        {QStringLiteral("blue"), QColor(0, 0, 255)},
        {QStringLiteral("gray"), QColor(128, 128, 128)},
        {QStringLiteral("grey"), QColor(128, 128, 128)},
        {QStringLiteral("silver"), QColor(192, 192, 192)},
        {QStringLiteral("orange"), QColor(255, 165, 0)},
        {QStringLiteral("yellow"), QColor(255, 255, 0)},
        {QStringLiteral("purple"), QColor(128, 0, 128)},
        {QStringLiteral("pink"), QColor(255, 192, 203)},
        {QStringLiteral("cyan"), QColor(0, 255, 255)},
        {QStringLiteral("magenta"), QColor(255, 0, 255)},
    };
    return names.value(ident);
}

QColor CssEvaluator::mix(const QColor& a, double pa, const QColor& b, double pb, const QString& space)
{
    double sum = pa + pb;
    if (sum <= 0)
        return QColor(0, 0, 0, 0);
    const double alphaMult = sum < 1.0 ? sum : 1.0;
    pa /= sum;
    pb /= sum;
    const double aa = a.alphaF(), ab = b.alphaF();
    const double alpha = aa * pa + ab * pb;
    if (alpha <= 0)
        return QColor(0, 0, 0, 0);
    if (space == u"oklch" || space == u"oklab") {
        Lab la = toOklab(a), lb = toOklab(b);
        if (space == u"oklch") {
            // Polar interpolation, powerless hue for achromatic/transparent colors (shorter hue path).
            auto polar = [](const Lab& l) {
                const double c = std::hypot(l.a, l.b);
                const double h = std::atan2(l.b, l.a);
                return std::pair{c, h};
            };
            auto [ca, ha] = polar(la);
            auto [cb, hb] = polar(lb);
            const bool aPowerless = ca < 1e-4 || aa <= 0;
            const bool bPowerless = cb < 1e-4 || ab <= 0;
            if (aPowerless && !bPowerless)
                ha = hb;
            if (bPowerless && !aPowerless)
                hb = ha;
            double dh = hb - ha;
            if (dh > std::numbers::pi)
                dh -= 2 * std::numbers::pi;
            else if (dh < -std::numbers::pi)
                dh += 2 * std::numbers::pi;
            // premultiplied L and C
            const double L = (la.L * aa * pa + lb.L * ab * pb) / alpha;
            const double C = (ca * aa * pa + cb * ab * pb) / alpha;
            const double H = ha + dh * pb;
            return fromOklab({L, C * std::cos(H), C * std::sin(H)}, alpha * alphaMult);
        }
        const Lab r{(la.L * aa * pa + lb.L * ab * pb) / alpha, (la.a * aa * pa + lb.a * ab * pb) / alpha,
                    (la.b * aa * pa + lb.b * ab * pb) / alpha};
        return fromOklab(r, alpha * alphaMult);
    }
    // srgb (gamma-encoded), premultiplied
    auto ch = [&](double ca, double cb) { return std::clamp((ca * aa * pa + cb * ab * pb) / alpha, 0.0, 1.0); };
    return QColor::fromRgbF(float(ch(a.redF(), b.redF())), float(ch(a.greenF(), b.greenF())), float(ch(a.blueF(), b.blueF())),
                            float(std::clamp(alpha * alphaMult, 0.0, 1.0)));
}

} // namespace oc
