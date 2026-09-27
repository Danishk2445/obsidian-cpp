#include "core/bases/base_value.h"

#include "core/properties/property_yaml.h"
#include "core/time/moment_format.h"

#include <QDateTime>
#include <QTimeZone>

#include <cmath>
#include <limits>

namespace oc::bases {

// ---------------------------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------------------------
struct Value::Payload {
    virtual ~Payload() = default;
};

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct ListPayload : Value::Payload {
    QList<Value> items;
};

struct ObjectPayload : Value::Payload {
    std::shared_ptr<ObjectData> data;
};

struct DurationPayload : Value::Payload {
    DurationParts d;
};

struct LinkPayload : Value::Payload {
    const DataSource* ds = nullptr;
    QString sourcePath;
    std::optional<Value> display;
};

struct UrlPayload : Value::Payload {
    std::optional<Value> display;
};

struct RegExpPayload : Value::Payload {
    QRegularExpression re;
    QString flags;
};

struct FilePayload : Value::Payload {
    const DataSource* ds = nullptr;
    std::shared_ptr<Scope> scope; // ThisFile
    mutable std::optional<Value> links, embeds, backlinks, tags, props;
};

struct FormulasPayload : Value::Payload {
    std::shared_ptr<Scope> scope;
    QStringList names;
};

const QList<Value>& emptyList()
{
    static const QList<Value> l;
    return l;
}

// JSON.stringify for a string.
QString jsonQuote(const QString& s)
{
    QString out;
    out.reserve(s.size() + 2);
    out += u'"';
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'"': out += QLatin1String("\\\""); break;
        case u'\\': out += QLatin1String("\\\\"); break;
        case u'\b': out += QLatin1String("\\b"); break;
        case u'\f': out += QLatin1String("\\f"); break;
        case u'\n': out += QLatin1String("\\n"); break;
        case u'\r': out += QLatin1String("\\r"); break;
        case u'\t': out += QLatin1String("\\t"); break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    out += u'"';
    return out;
}

QString pad2(int v) { return QString::number(v).rightJustified(2, u'0'); }

// Local calendar fields of a time value (JavaScript's getFullYear()/getMonth()/...).
struct LocalParts {
    qint64 year = 0;
    qint64 month = 0; // 0-based
    qint64 day = 1;
    qint64 hour = 0, minute = 0, second = 0, ms = 0;
};

LocalParts localParts(double ms)
{
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(qint64(ms));
    const QDate d = dt.date();
    const QTime t = dt.time();
    LocalParts p;
    p.year = d.year() < 0 ? d.year() + 1 : d.year(); // Qt has no year 0
    p.month = d.month() - 1;
    p.day = d.day();
    p.hour = t.hour();
    p.minute = t.minute();
    p.second = t.second();
    p.ms = t.msec();
    return p;
}

qint64 floorDiv(qint64 a, qint64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// JavaScript's new Date(y, m, d, h, mi, s, ms) (fields overflow into the larger ones).
double fromLocalParts(double y, double mo, double d, double h, double mi, double s, double ms)
{
    if (!std::isfinite(y) || !std::isfinite(mo) || !std::isfinite(d) || !std::isfinite(h) || !std::isfinite(mi) || !std::isfinite(s)
        || !std::isfinite(ms))
        return kNaN;
    const qint64 yi = qint64(std::trunc(y)), moi = qint64(std::trunc(mo)), di = qint64(std::trunc(d));
    const qint64 year = yi + floorDiv(moi, 12);
    const int month = int(moi - floorDiv(moi, 12) * 12);
    if (year < -271820 || year > 275759)
        return kNaN;
    const QDate first(int(year <= 0 ? year - 1 : year), month + 1, 1);
    if (!first.isValid())
        return kNaN;
    qint64 t = ((qint64(std::trunc(h)) * 60 + qint64(std::trunc(mi))) * 60 + qint64(std::trunc(s))) * 1000 + qint64(std::trunc(ms));
    const qint64 carry = floorDiv(t, 86400000);
    t -= carry * 86400000;
    const QDate date = first.addDays(di - 1 + carry);
    const QDateTime dt(date, QTime::fromMSecsSinceStartOfDay(int(t)));
    return double(dt.toMSecsSinceEpoch());
}

// Human-readable duration (moment.duration(...).humanize() with the default thresholds).
QString humanizeDuration(const DurationParts& p)
{
    // moment's duration internals: _milliseconds, _days, _months
    const double ms = std::abs(p.milliseconds + p.seconds * 1e3 + p.minutes * 6e4 + p.hours * 36e5);
    const double days = std::abs(p.days);
    const double months = std::abs(p.months + p.years * 12);
    const auto round = [](double v) { return qint64(std::floor(v + 0.5)); };
    const double daysTotal = days + std::round(months * 146097 / 4800); // monthsToDays, rounded like as()
    const qint64 seconds = round((daysTotal * 864e5 + ms) / 1000);
    const qint64 minutes = round((daysTotal * 864e5 + ms) / 6e4);
    const qint64 hours = round((daysTotal * 864e5 + ms) / 36e5);
    const qint64 d = round(daysTotal + ms / 864e5);
    const double monthsExact = months + (days + ms / 864e5) * 4800 / 146097;
    const qint64 M = round(monthsExact);
    const qint64 y = round(monthsExact / 12);
    if (seconds <= 44)
        return QStringLiteral("a few seconds");
    if (seconds < 45)
        return QStringLiteral("%1 seconds").arg(seconds);
    if (minutes <= 1)
        return QStringLiteral("a minute");
    if (minutes < 45)
        return QStringLiteral("%1 minutes").arg(minutes);
    if (hours <= 1)
        return QStringLiteral("an hour");
    if (hours < 22)
        return QStringLiteral("%1 hours").arg(hours);
    if (d <= 1)
        return QStringLiteral("a day");
    if (d < 26)
        return QStringLiteral("%1 days").arg(d);
    if (M <= 1)
        return QStringLiteral("a month");
    if (M < 11)
        return QStringLiteral("%1 months").arg(M);
    if (y <= 1)
        return QStringLiteral("a year");
    return QStringLiteral("%1 years").arg(y);
}

bool isPrimitiveType(ValueType t)
{
    switch (t) {
    case ValueType::Boolean:
    case ValueType::Number:
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Link:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
        return true;
    default:
        return false;
    }
}

bool isStringType(ValueType t) { return isPrimitiveType(t) && t != ValueType::Boolean && t != ValueType::Number; }

// JavaScript `a == b` between two primitives (booleans, numbers, strings).
bool primitiveLooseEquals(const Value& a, const Value& b)
{
    auto kind = [](const Value& v) { return v.type() == ValueType::Boolean ? 0 : v.type() == ValueType::Number ? 1 : 2; };
    const int ka = kind(a), kb = kind(b);
    if (ka == kb) {
        if (ka == 0)
            return a.boolValue() == b.boolValue();
        if (ka == 1)
            return a.number() == b.number();
        return a.text() == b.text();
    }
    auto toNum = [](const Value& v) -> double {
        switch (v.type()) {
        case ValueType::Boolean:
            return v.boolValue() ? 1 : 0;
        case ValueType::Number:
            return v.number();
        default:
            return Value::jsToNumber(v.text());
        }
    };
    return toNum(a) == toNum(b);
}

const QList<Value>& listItems(const Value& v) { return v.items(); }

} // namespace

// ---------------------------------------------------------------------------------------------
// ObjectData
// ---------------------------------------------------------------------------------------------
int ObjectData::indexOf(const QString& key) const { return int(keys.indexOf(key)); }

int ObjectData::indexOfInsensitive(const QString& key) const
{
    const int exact = indexOf(key);
    if (exact >= 0)
        return exact;
    for (int i = 0; i < keys.size(); ++i)
        if (keys[i].compare(key, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

Value ObjectData::at(int index) const
{
    if (index < 0 || index >= keys.size())
        return Value();
    if (values.size() < keys.size())
        values.resize(keys.size());
    if (!values[index]) {
        const QVariant raw = index < this->raw.size() ? this->raw[index] : QVariant();
        values[index] = std::make_shared<Value>(frontmatter ? Value::fromFrontmatter(ds, sourcePath, keys[index], raw) : Value::fromVariant(raw));
    }
    return *values[index];
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------
Value Value::error(const QString& message)
{
    Value v;
    v.m_type = ValueType::Error;
    v.m_str = message;
    return v;
}

Value Value::boolean(bool b)
{
    Value v;
    v.m_type = ValueType::Boolean;
    v.m_flag = b;
    return v;
}

Value Value::number(double n)
{
    Value v;
    v.m_type = ValueType::Number;
    v.m_num = n;
    return v;
}

Value Value::string(const QString& s)
{
    Value v;
    v.m_type = ValueType::String;
    v.m_str = s;
    return v;
}

Value Value::tag(const QString& tag)
{
    Value v;
    v.m_type = ValueType::Tag;
    v.m_str = tag.startsWith(u'#') ? tag : u'#' + tag;
    return v;
}

Value Value::link(const DataSource* ds, const QString& path, const QString& sourcePath, const Value* display)
{
    Value v;
    v.m_type = ValueType::Link;
    v.m_str = path;
    auto p = std::make_shared<LinkPayload>();
    p->ds = ds;
    p->sourcePath = sourcePath;
    if (display && !display->isNull())
        p->display = *display;
    v.m_p = std::move(p);
    return v;
}

Value Value::url(const QString& url, const Value* display)
{
    Value v;
    v.m_type = ValueType::Url;
    v.m_str = url;
    if (display && !display->isNull()) {
        auto p = std::make_shared<UrlPayload>();
        p->display = *display;
        v.m_p = std::move(p);
    }
    return v;
}

Value Value::image(const QString& src)
{
    Value v;
    v.m_type = ValueType::Image;
    v.m_str = src;
    return v;
}

Value Value::icon(const QString& name)
{
    Value v;
    v.m_type = ValueType::Icon;
    v.m_str = name;
    return v;
}

Value Value::html(const QString& html)
{
    Value v;
    v.m_type = ValueType::Html;
    v.m_str = html;
    return v;
}

Value Value::date(double ms, bool time)
{
    Value v;
    v.m_type = ValueType::Date;
    v.m_num = ms;
    v.m_flag = time;
    return v;
}

Value Value::relativeDate(double ms, bool time)
{
    Value v = date(ms, time);
    v.m_type = ValueType::RelativeDate;
    return v;
}

Value Value::duration(const DurationParts& d)
{
    Value v;
    v.m_type = ValueType::Duration;
    auto p = std::make_shared<DurationPayload>();
    p->d = d;
    v.m_p = std::move(p);
    return v;
}

Value Value::list(const QList<Value>& items)
{
    Value v;
    v.m_type = ValueType::List;
    auto p = std::make_shared<ListPayload>();
    p->items = items;
    v.m_p = std::move(p);
    return v;
}

Value Value::tagList(const QStringList& tags)
{
    Value v;
    v.m_type = ValueType::TagList;
    auto p = std::make_shared<ListPayload>();
    p->items.reserve(tags.size());
    for (const QString& t : tags)
        p->items << tag(t);
    v.m_p = std::move(p);
    return v;
}

Value Value::object(std::shared_ptr<ObjectData> data)
{
    Value v;
    v.m_type = ValueType::Object;
    auto p = std::make_shared<ObjectPayload>();
    p->data = data ? std::move(data) : std::make_shared<ObjectData>();
    v.m_p = std::move(p);
    return v;
}

Value Value::regexp(const QString& pattern, const QString& flags)
{
    Value v;
    v.m_type = ValueType::RegExp;
    v.m_str = pattern;
    auto p = std::make_shared<RegExpPayload>();
    QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
    if (flags.contains(u'i'))
        opts |= QRegularExpression::CaseInsensitiveOption;
    if (flags.contains(u'm'))
        opts |= QRegularExpression::MultilineOption;
    if (flags.contains(u's'))
        opts |= QRegularExpression::DotMatchesEverythingOption;
    p->re = QRegularExpression(pattern, opts);
    p->flags = flags;
    v.m_p = std::move(p);
    return v;
}

Value Value::file(const DataSource* ds, const QString& path)
{
    Value v;
    v.m_type = ValueType::File;
    v.m_str = path;
    auto p = std::make_shared<FilePayload>();
    p->ds = ds;
    v.m_p = std::move(p);
    return v;
}

Value Value::thisFile(const DataSource* ds, const QString& path, std::shared_ptr<Scope> scope)
{
    Value v = file(ds, path);
    v.m_type = ValueType::ThisFile;
    auto p = std::make_shared<FilePayload>();
    p->ds = ds;
    p->scope = std::move(scope);
    v.m_p = std::move(p);
    return v;
}

Value Value::formulas(std::shared_ptr<Scope> scope, const QStringList& names)
{
    Value v;
    v.m_type = ValueType::Formulas;
    auto p = std::make_shared<FormulasPayload>();
    p->scope = std::move(scope);
    p->names = names;
    v.m_p = std::move(p);
    return v;
}

Value Value::fromVariant(const QVariant& v)
{
    if (!v.isValid() || v.isNull())
        return Value();
    switch (v.typeId()) {
    case QMetaType::Bool:
        return boolean(v.toBool());
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return number(v.toDouble());
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        QList<Value> items;
        for (const QVariant& item : v.toList())
            items << fromVariant(item);
        return list(items);
    }
    case QMetaType::QVariantMap: {
        auto data = std::make_shared<ObjectData>();
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            data->keys << it.key();
            data->raw << it.value();
        }
        return object(data);
    }
    default:
        return string(v.toString());
    }
}

Value Value::fromFrontmatter(const DataSource* ds, const QString& sourcePath, const QString& key, const QVariant& v)
{
    const bool isList = v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList;
    if (!key.isNull() && key.compare(QLatin1String("tags"), Qt::CaseInsensitive) == 0) {
        if (isList) {
            bool strings = true;
            QStringList tags;
            for (const QVariant& item : v.toList()) {
                if (!item.isValid() || item.isNull())
                    continue;
                if (item.typeId() != QMetaType::QString) {
                    strings = false;
                    break;
                }
                tags << item.toString();
            }
            if (strings)
                return tagList(tags);
        } else if (v.typeId() == QMetaType::QString) {
            return tagList({v.toString()});
        }
    }
    if (v.typeId() == QMetaType::QString) {
        const QString s = v.toString();
        if (auto l = parseLink(ds, s, sourcePath))
            return *l;
        if (!s.contains(u' ') && isUrl(s))
            return url(s);
        if (auto d = parseDate(s))
            return *d;
        return string(s);
    }
    if (isList) {
        QList<Value> items;
        for (const QVariant& item : v.toList())
            items << fromFrontmatter(ds, sourcePath, QString(), item);
        return list(items);
    }
    if (v.typeId() == QMetaType::QVariantMap) {
        auto data = std::make_shared<ObjectData>();
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            data->keys << it.key();
            data->raw << it.value();
        }
        data->frontmatter = true;
        data->ds = ds;
        data->sourcePath = sourcePath;
        return object(data);
    }
    return fromVariant(v);
}

Value Value::frontmatterObject(const DataSource* ds, const QString& sourcePath, const QList<md::FrontmatterValue>& fm)
{
    auto data = std::make_shared<ObjectData>();
    data->keys.reserve(fm.size());
    data->raw.reserve(fm.size());
    for (const md::FrontmatterValue& e : fm) {
        const int existing = data->indexOf(e.key);
        if (existing >= 0) {
            data->raw[existing] = e.value;
            continue;
        }
        data->keys << e.key;
        data->raw << e.value;
    }
    data->frontmatter = true;
    data->ds = ds;
    data->sourcePath = sourcePath;
    return object(data);
}

// ---------------------------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------------------------
bool Value::is(ValueClass c) const
{
    const ValueType t = m_type;
    switch (c) {
    case ValueClass::Any:
        return true;
    case ValueClass::NotNull:
        return t != ValueType::Null && t != ValueType::Error;
    case ValueClass::Primitive:
        return isPrimitiveType(t);
    case ValueClass::Null:
        return t == ValueType::Null;
    case ValueClass::Error:
        return t == ValueType::Error;
    case ValueClass::Boolean:
        return t == ValueType::Boolean;
    case ValueClass::Number:
        return t == ValueType::Number;
    case ValueClass::String:
        return isStringType(t);
    case ValueClass::Tag:
        return t == ValueType::Tag;
    case ValueClass::Link:
        return t == ValueType::Link;
    case ValueClass::Url:
        return t == ValueType::Url;
    case ValueClass::Image:
        return t == ValueType::Image;
    case ValueClass::Icon:
        return t == ValueType::Icon;
    case ValueClass::Html:
        return t == ValueType::Html;
    case ValueClass::Date:
        return t == ValueType::Date || t == ValueType::RelativeDate;
    case ValueClass::Duration:
        return t == ValueType::Duration;
    case ValueClass::List:
        return t == ValueType::List || t == ValueType::TagList;
    case ValueClass::TagList:
        return t == ValueType::TagList;
    case ValueClass::Object:
        return t == ValueType::Object;
    case ValueClass::RegExp:
        return t == ValueType::RegExp;
    case ValueClass::File:
        return t == ValueType::File || t == ValueType::ThisFile;
    case ValueClass::Formulas:
        return t == ValueType::Formulas;
    }
    return false;
}

QString Value::typeName() const
{
    switch (m_type) {
    case ValueType::Null: return QStringLiteral("Null");
    case ValueType::Error: return QStringLiteral("Error");
    case ValueType::Boolean: return QStringLiteral("Boolean");
    case ValueType::Number: return QStringLiteral("Number");
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Icon: return QStringLiteral("String");
    case ValueType::Link: return QStringLiteral("Link");
    case ValueType::Url: return QStringLiteral("URL");
    case ValueType::Image: return QStringLiteral("Image");
    case ValueType::Html: return QStringLiteral("HTML");
    case ValueType::Date:
    case ValueType::RelativeDate: return QStringLiteral("Date");
    case ValueType::Duration: return QStringLiteral("Duration");
    case ValueType::List:
    case ValueType::TagList: return QStringLiteral("List");
    case ValueType::Object: return QStringLiteral("Object");
    case ValueType::RegExp: return QStringLiteral("RegExp");
    case ValueType::File: return QStringLiteral("File");
    case ValueType::ThisFile: return QStringLiteral("ThisFile");
    case ValueType::Formulas: return QStringLiteral("Formulas");
    }
    return QStringLiteral("Any");
}

bool Value::isOfType(const QString& name) const
{
    const QString n = name.toLower();
    if (n == u"any")
        return true;
    if (typeName().toLower() == n)
        return true;
    switch (m_type) {
    case ValueType::Link:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Html:
        return n == u"string";
    case ValueType::ThisFile:
        return n == u"file";
    default:
        return false;
    }
}

bool Value::isTruthy() const
{
    switch (m_type) {
    case ValueType::Null:
    case ValueType::Error:
        return false;
    case ValueType::Boolean:
        return m_flag;
    case ValueType::Number:
        return m_num != 0 && !std::isnan(m_num);
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
        return !m_str.isEmpty();
    case ValueType::Link:
    case ValueType::Date:
    case ValueType::RelativeDate:
    case ValueType::RegExp:
    case ValueType::File:
    case ValueType::ThisFile:
    case ValueType::Formulas:
        return true;
    case ValueType::Duration: {
        const DurationParts& d = durationParts();
        return d.years != 0 || d.months != 0 || d.days != 0 || d.hours != 0 || d.minutes != 0 || d.seconds != 0 || d.milliseconds != 0;
    }
    case ValueType::List:
    case ValueType::TagList:
        return !items().isEmpty();
    case ValueType::Object:
        return !objectData()->keys.isEmpty();
    }
    return false;
}

QString Value::toString() const
{
    switch (m_type) {
    case ValueType::Null:
        return QStringLiteral("null");
    case ValueType::Error:
        return QStringLiteral("Error: ") + m_str;
    case ValueType::Boolean:
        return m_flag ? QStringLiteral("true") : QStringLiteral("false");
    case ValueType::Number:
        return jsNumber(m_num);
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Url:
    case ValueType::Icon:
    case ValueType::Html:
        return m_str;
    case ValueType::Link: {
        const Value* d = display();
        return QStringLiteral("[[") + m_str + (d ? u'|' + d->toString() : QString()) + QStringLiteral("]]");
    }
    case ValueType::Image: {
        const bool vaultPath = m_str.startsWith(QLatin1String("./")) || m_str.startsWith(QLatin1String("../")) || !m_str.contains(u':');
        return vaultPath ? QString(QStringLiteral("![[") + m_str + QStringLiteral("]]")) : QString(QStringLiteral("![](") + m_str + u')');
    }
    case ValueType::Date:
        return m_flag ? printDate() + u'T' + printTime() : printDate();
    case ValueType::RelativeDate:
        return relative();
    case ValueType::Duration:
        return humanize();
    case ValueType::List:
    case ValueType::TagList: {
        QStringList parts;
        for (const Value& v : items())
            parts << v.toString();
        return parts.join(QLatin1String(", "));
    }
    case ValueType::Object: {
        const ObjectData* o = objectData();
        QStringList parts;
        for (int i = 0; i < o->keys.size(); ++i)
            parts << jsonQuote(o->keys[i]) + u':' + jsonQuote(o->at(i).toString());
        return u'{' + parts.join(u',') + u'}';
    }
    case ValueType::RegExp:
        return u'/' + m_str + u'/' + regexFlags();
    case ValueType::File:
    case ValueType::ThisFile:
        return m_str;
    case ValueType::Formulas: {
        const auto* p = static_cast<const FormulasPayload*>(m_p.get());
        QStringList parts;
        for (const QString& n : p->names)
            parts << jsonQuote(n);
        return QStringLiteral("Formulas[") + parts.join(u',') + u']';
    }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Data accessors
// ---------------------------------------------------------------------------------------------
const DurationParts& Value::durationParts() const
{
    static const DurationParts zero;
    return m_type == ValueType::Duration ? static_cast<const DurationPayload*>(m_p.get())->d : zero;
}

const QList<Value>& Value::items() const
{
    if ((m_type == ValueType::List || m_type == ValueType::TagList) && m_p)
        return static_cast<const ListPayload*>(m_p.get())->items;
    return emptyList();
}

const ObjectData* Value::objectData() const
{
    static const ObjectData empty;
    return m_type == ValueType::Object && m_p ? static_cast<const ObjectPayload*>(m_p.get())->data.get() : &empty;
}

const Value* Value::display() const
{
    if (m_type == ValueType::Link && m_p) {
        const auto* p = static_cast<const LinkPayload*>(m_p.get());
        return p->display ? &*p->display : nullptr;
    }
    if (m_type == ValueType::Url && m_p) {
        const auto* p = static_cast<const UrlPayload*>(m_p.get());
        return p->display ? &*p->display : nullptr;
    }
    return nullptr;
}

QString Value::linkSourcePath() const
{
    return m_type == ValueType::Link && m_p ? static_cast<const LinkPayload*>(m_p.get())->sourcePath : QString();
}

const DataSource* Value::dataSource() const
{
    if (m_type == ValueType::Link && m_p)
        return static_cast<const LinkPayload*>(m_p.get())->ds;
    if ((m_type == ValueType::File || m_type == ValueType::ThisFile) && m_p)
        return static_cast<const FilePayload*>(m_p.get())->ds;
    return nullptr;
}

const QRegularExpression& Value::regex() const
{
    static const QRegularExpression none;
    return m_type == ValueType::RegExp && m_p ? static_cast<const RegExpPayload*>(m_p.get())->re : none;
}

QString Value::regexFlags() const
{
    return m_type == ValueType::RegExp && m_p ? static_cast<const RegExpPayload*>(m_p.get())->flags : QString();
}

std::shared_ptr<Scope> Value::scope() const
{
    if (m_type == ValueType::ThisFile && m_p)
        return static_cast<const FilePayload*>(m_p.get())->scope;
    if (m_type == ValueType::Formulas && m_p)
        return static_cast<const FormulasPayload*>(m_p.get())->scope;
    return nullptr;
}

QString Value::resolve() const
{
    if (m_type != ValueType::Link)
        return {};
    const DataSource* ds = dataSource();
    if (!ds)
        return {};
    const qsizetype hash = m_str.indexOf(u'#');
    return ds->resolveLink(hash < 0 ? m_str : m_str.left(hash), linkSourcePath());
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------
Value Value::fileLinks() const
{
    const auto* p = static_cast<const FilePayload*>(m_p.get());
    if (!p->links) {
        QList<Value> out;
        if (const md::CachedMetadata* m = p->ds ? p->ds->metadata(m_str) : nullptr) {
            auto add = [&](const QList<md::LinkCache>& refs) {
                for (const md::LinkCache& l : refs) {
                    const Value display = l.displayText.isEmpty() ? Value() : string(l.displayText);
                    out << link(p->ds, l.link, m_str, &display);
                }
            };
            add(m->frontmatterLinks);
            add(m->links);
            add(m->embeds);
        }
        p->links = list(out);
    }
    return *p->links;
}

Value Value::fileEmbeds() const
{
    const auto* p = static_cast<const FilePayload*>(m_p.get());
    if (!p->embeds) {
        QList<Value> out;
        if (const md::CachedMetadata* m = p->ds ? p->ds->metadata(m_str) : nullptr) {
            for (const md::LinkCache& l : m->embeds) {
                const Value display = l.displayText.isEmpty() ? Value() : string(l.displayText);
                out << link(p->ds, l.link, m_str, &display);
            }
        }
        p->embeds = list(out);
    }
    return *p->embeds;
}

Value Value::fileBacklinks() const
{
    const auto* p = static_cast<const FilePayload*>(m_p.get());
    if (!p->backlinks) {
        QList<Value> out;
        if (p->ds) {
            QStringList sources = p->ds->backlinks(m_str);
            std::sort(sources.begin(), sources.end());
            for (const QString& s : sources) {
                QString name = s.mid(s.lastIndexOf(u'/') + 1);
                if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
                    name.chop(3);
                const Value display = string(name);
                out << link(p->ds, s, QString(), &display);
            }
        }
        p->backlinks = list(out);
    }
    return *p->backlinks;
}

Value Value::fileTags() const
{
    const auto* p = static_cast<const FilePayload*>(m_p.get());
    if (!p->tags) {
        QStringList tags;
        if (const md::CachedMetadata* m = p->ds ? p->ds->metadata(m_str) : nullptr) {
            for (const md::TagCache& t : m->tags)
                tags << t.tag;
            if (m->frontmatter) {
                for (const md::FrontmatterValue& fv : *m->frontmatter) {
                    if (fv.key.compare(QLatin1String("tags"), Qt::CaseInsensitive) != 0)
                        continue;
                    QStringList fm;
                    if (fv.value.typeId() == QMetaType::QString) {
                        fm << fv.value.toString().trimmed();
                    } else if (fv.value.typeId() == QMetaType::QVariantList) {
                        for (const QVariant& item : fv.value.toList())
                            if (item.typeId() == QMetaType::QString)
                                fm << item.toString().trimmed();
                    }
                    for (const QString& t : std::as_const(fm)) {
                        if (t.isEmpty() || t.contains(u' '))
                            continue;
                        tags << (t.startsWith(u'#') ? t : u'#' + t);
                    }
                    break;
                }
            }
        }
        QStringList unique;
        for (const QString& t : std::as_const(tags))
            if (!unique.contains(t))
                unique << t;
        p->tags = tagList(unique);
    }
    return *p->tags;
}

Value Value::fileProperties() const
{
    const auto* p = static_cast<const FilePayload*>(m_p.get());
    if (!p->props) {
        const md::CachedMetadata* m = p->ds ? p->ds->metadata(m_str) : nullptr;
        p->props = frontmatterObject(p->ds, m_str, m && m->frontmatter ? *m->frontmatter : QList<md::FrontmatterValue>());
    }
    return *p->props;
}

// ---------------------------------------------------------------------------------------------
// Members
// ---------------------------------------------------------------------------------------------
std::optional<Value> Value::objectAccess(const QString& name) const
{
    const QString key = name.toLower();
    switch (m_type) {
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Link:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
        if (key == u"length")
            return number(double(m_str.size()));
        return std::nullopt;
    case ValueType::List:
    case ValueType::TagList:
        if (key == u"length")
            return number(double(items().size()));
        return std::nullopt;
    case ValueType::Object: {
        const ObjectData* o = objectData();
        return o->at(o->indexOfInsensitive(name));
    }
    case ValueType::Date:
    case ValueType::RelativeDate: {
        if (std::isnan(m_num)) {
            static const QStringList parts = {QStringLiteral("year"), QStringLiteral("month"), QStringLiteral("day"), QStringLiteral("hour"),
                                              QStringLiteral("minute"), QStringLiteral("second"), QStringLiteral("millisecond"),
                                              QStringLiteral("timestamp")};
            if (parts.contains(key))
                return number(kNaN);
            return std::nullopt;
        }
        if (key == u"timestamp")
            return number(m_num);
        const LocalParts p = localParts(m_num);
        if (key == u"year")
            return number(double(p.year));
        if (key == u"month")
            return number(double(p.month + 1));
        if (key == u"day")
            return number(double(p.day));
        if (key == u"hour")
            return number(double(p.hour));
        if (key == u"minute")
            return number(double(p.minute));
        if (key == u"second")
            return number(double(p.second));
        if (key == u"millisecond")
            return number(double(p.ms));
        return std::nullopt;
    }
    case ValueType::Duration: {
        static const QStringList units = {QStringLiteral("years"), QStringLiteral("months"), QStringLiteral("weeks"), QStringLiteral("days"),
                                          QStringLiteral("hours"), QStringLiteral("minutes"), QStringLiteral("seconds"), QStringLiteral("milliseconds")};
        if (!units.contains(key))
            return std::nullopt;
        // moment(now + duration).diff(now, unit, true)
        const double now = double(QDateTime::currentMSecsSinceEpoch());
        const Value later = addToDate(date(now, true), false);
        const double delta = later.dateMs() - now;
        if (key == u"milliseconds")
            return number(delta);
        if (key == u"seconds")
            return number(delta / 1e3);
        if (key == u"minutes")
            return number(delta / 6e4);
        if (key == u"hours")
            return number(delta / 36e5);
        const qint64 zoneDelta = (QDateTime::fromMSecsSinceEpoch(qint64(later.dateMs())).offsetFromUtc()
                                  - QDateTime::fromMSecsSinceEpoch(qint64(now)).offsetFromUtc())
                                 * 1000LL;
        if (key == u"days")
            return number((delta + double(zoneDelta)) / 864e5);
        if (key == u"weeks")
            return number((delta + double(zoneDelta)) / 6048e5);
        // monthDiff(later, now) (moment's months/years diff)
        const LocalParts a = localParts(now), b = localParts(later.dateMs());
        const qint64 whole = (b.year - a.year) * 12 + (b.month - a.month);
        auto addMonths = [&](qint64 n) {
            const LocalParts p = localParts(now);
            // moment's add(n, "months"): the day is clamped to the target month's length
            const qint64 y = p.year + floorDiv(p.month + n, 12);
            const int m = int(p.month + n - floorDiv(p.month + n, 12) * 12);
            const QDate first(int(y <= 0 ? y - 1 : y), m + 1, 1);
            const int day = std::min<int>(int(p.day), first.daysInMonth());
            return fromLocalParts(double(y), double(m), double(day), double(p.hour), double(p.minute), double(p.second), double(p.ms));
        };
        const double anchor = addMonths(whole);
        const double target = later.dateMs();
        double adjust;
        if (target - anchor < 0) {
            const double anchor2 = addMonths(whole - 1);
            adjust = (target - anchor) / (anchor - anchor2);
        } else {
            const double anchor2 = addMonths(whole + 1);
            adjust = (target - anchor) / (anchor2 - anchor);
        }
        const double months = double(whole) + adjust;
        return number(key == u"months" ? months : months / 12);
    }
    case ValueType::File: {
        const auto* p = static_cast<const FilePayload*>(m_p.get());
        const std::optional<FileStat> st = p->ds ? p->ds->stat(m_str) : std::nullopt;
        if (key == u"file")
            return *this;
        if (key == u"name")
            return string(st ? (st->extension == u"md" ? st->basename : st->name) : m_str.mid(m_str.lastIndexOf(u'/') + 1));
        if (key == u"basename")
            return string(st ? st->basename : QString());
        if (key == u"fullname")
            return string(st ? st->name : QString());
        if (key == u"path")
            return string(m_str);
        if (key == u"folder")
            return string(st ? st->folder : QString());
        if (key == u"ext")
            return string(st ? st->extension : QString());
        if (key == u"ctime")
            return date(st ? double(st->ctime) : kNaN, true);
        if (key == u"mtime")
            return date(st ? double(st->mtime) : kNaN, true);
        if (key == u"size")
            return number(st ? double(st->size) : 0);
        if (key == u"links")
            return fileLinks();
        if (key == u"embeds")
            return fileEmbeds();
        if (key == u"backlinks")
            return fileBacklinks();
        if (key == u"tags")
            return fileTags();
        if (key == u"properties")
            return fileProperties();
        return std::nullopt;
    }
    case ValueType::ThisFile: {
        const std::shared_ptr<Scope> s = scope();
        return s ? s->getByIdentifier(name) : Value();
    }
    case ValueType::Formulas: {
        const std::shared_ptr<Scope> s = scope();
        if (!s)
            return std::nullopt;
        const std::optional<Value> v = s->formulaValue(name);
        if (v && v->isError())
            throw EvalError{v->text()};
        return v;
    }
    default:
        return std::nullopt;
    }
}

QStringList Value::keys() const
{
    switch (m_type) {
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Link:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
    case ValueType::List:
    case ValueType::TagList:
        return {QStringLiteral("length")};
    case ValueType::Object:
        return objectData()->keys;
    case ValueType::Date:
    case ValueType::RelativeDate:
        return {QStringLiteral("year"), QStringLiteral("month"), QStringLiteral("day"), QStringLiteral("hour"), QStringLiteral("minute"),
                QStringLiteral("second"), QStringLiteral("millisecond"), QStringLiteral("timestamp")};
    case ValueType::Duration:
        return {QStringLiteral("years"), QStringLiteral("months"), QStringLiteral("days"), QStringLiteral("weeks"), QStringLiteral("hours"),
                QStringLiteral("minutes"), QStringLiteral("seconds"), QStringLiteral("milliseconds")};
    case ValueType::File:
        return {QStringLiteral("file"), QStringLiteral("name"), QStringLiteral("basename"), QStringLiteral("fullname"), QStringLiteral("path"),
                QStringLiteral("folder"), QStringLiteral("ext"), QStringLiteral("ctime"), QStringLiteral("mtime"), QStringLiteral("size"),
                QStringLiteral("links"), QStringLiteral("embeds"), QStringLiteral("backlinks"), QStringLiteral("tags"),
                QStringLiteral("properties")};
    case ValueType::ThisFile: {
        const std::shared_ptr<Scope> s = scope();
        return s ? s->keys() : QStringList();
    }
    case ValueType::Formulas: {
        const std::shared_ptr<Scope> s = scope();
        return s ? s->formulaNames() : QStringList();
    }
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------------------------
// Equality
// ---------------------------------------------------------------------------------------------
namespace {

bool sameClassEquals(const Value& a, const Value& b)
{
    switch (a.type()) {
    case ValueType::Null:
        return true;
    case ValueType::Error:
        return a.text() == b.text();
    case ValueType::Boolean:
        return a.boolValue() == b.boolValue();
    case ValueType::Number:
        return a.number() == b.number();
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
        return a.text() == b.text();
    case ValueType::Url: {
        const Value* da = a.display();
        const Value* db = b.display();
        return a.text() == b.text() && ((!da && !db) || (da && db && Value::equals(*da, *db)));
    }
    case ValueType::Link: {
        const Value* da = a.display();
        const Value* db = b.display();
        return a.text() == b.text() && a.linkSourcePath() == b.linkSourcePath() && ((!da && !db) || (da && db && Value::equals(*da, *db)));
    }
    case ValueType::Date:
    case ValueType::RelativeDate:
        return a.hasTime() == b.hasTime() && a.dateMs() == b.dateMs();
    case ValueType::Duration:
        return a.durationParts() == b.durationParts();
    case ValueType::List:
    case ValueType::TagList: {
        const QList<Value>& x = a.items();
        const QList<Value>& y = b.items();
        if (x.size() != y.size())
            return false;
        for (qsizetype i = 0; i < x.size(); ++i)
            if (!Value::equals(x[i], y[i]))
                return false;
        return true;
    }
    case ValueType::Object: {
        const ObjectData* x = a.objectData();
        const ObjectData* y = b.objectData();
        if (x == y)
            return true;
        if (x->keys.size() != y->keys.size())
            return false;
        for (const QString& k : x->keys)
            if (y->indexOf(k) < 0)
                return false;
        for (int i = 0; i < x->keys.size(); ++i)
            if (!Value::equals(x->at(i), y->at(y->indexOf(x->keys[i]))))
                return false;
        return true;
    }
    case ValueType::File:
    case ValueType::ThisFile:
        return a.filePath() == b.filePath();
    case ValueType::RegExp:
    case ValueType::Formulas:
        return false;
    }
    return false;
}

// The class-specific looseEquals(e) of `a` (Obsidian's prototype method).
bool classLooseEquals(const Value& a, const Value& b)
{
    switch (a.type()) {
    case ValueType::Boolean:
    case ValueType::Number:
    case ValueType::String:
    case ValueType::Tag:
    case ValueType::Url:
    case ValueType::Image:
    case ValueType::Icon:
    case ValueType::Html:
        return b.is(ValueClass::Primitive) && primitiveLooseEquals(a, b);
    case ValueType::Link: {
        if (b.type() == ValueType::Link) {
            const QString ra = a.resolve(), rb = b.resolve();
            return !ra.isEmpty() && !rb.isEmpty() ? ra == rb : a.text() == b.text();
        }
        if (b.is(ValueClass::String)) {
            if (auto parsed = Value::parseLink(a.dataSource(), b.text(), QString()))
                return classLooseEquals(a, *parsed);
        }
        if (b.is(ValueClass::File))
            return a.resolve() == b.filePath();
        return false;
    }
    case ValueType::Date:
    case ValueType::RelativeDate: {
        Value other = b;
        if (b.is(ValueClass::String)) {
            if (auto d = Value::parseDate(b.text()))
                other = *d;
        }
        if (!other.is(ValueClass::Date))
            return false;
        if (a.hasTime() && other.hasTime())
            return a.dateMs() == other.dateMs();
        return a.dateOnly().dateMs() == other.dateOnly().dateMs();
    }
    case ValueType::Duration: {
        Value other = b;
        if (b.is(ValueClass::String)) {
            if (auto d = Value::parseDuration(b.text()))
                other = *d;
        }
        return other.type() == ValueType::Duration && a.durationMs() == other.durationMs();
    }
    case ValueType::List:
    case ValueType::TagList: {
        const QList<Value>& x = a.items();
        if (b.is(ValueClass::List)) {
            const QList<Value>& y = b.items();
            if (x.size() != y.size())
                return false;
            for (qsizetype i = 0; i < x.size(); ++i)
                if (!Value::looseEquals(x[i], y[i]))
                    return false;
            return true;
        }
        return x.size() == 1 && Value::looseEquals(x[0], b);
    }
    case ValueType::File:
    case ValueType::ThisFile:
        return b.is(ValueClass::String) && a.filePath() == b.text();
    default:
        return false;
    }
}

} // namespace

bool Value::equals(const Value& a, const Value& b) { return a.type() == b.type() && sameClassEquals(a, b); }

bool Value::looseEquals(const Value& a, const Value& b)
{
    if (a.isNull() && b.isNull())
        return true;
    return equals(a, b) || classLooseEquals(a, b) || classLooseEquals(b, a);
}

bool Value::tagMatches(const Value& query) const
{
    if (m_type != ValueType::Tag || !query.is(ValueClass::String))
        return false;
    const QString n = m_str.toLower();
    QString i = query.type() == ValueType::Tag ? query.text() : query.text();
    if (!i.startsWith(u'#'))
        i.prepend(u'#');
    i = i.toLower();
    return n.startsWith(i) && (n.size() == i.size() || n.at(i.size()) == u'/');
}

bool Value::listIncludes(const Value& v) const
{
    const QList<Value>& list = items();
    if (m_type == ValueType::TagList) {
        for (const Value& t : list)
            if (t.tagMatches(v))
                return true;
        return false;
    }
    for (const Value& item : list)
        if (looseEquals(v, item))
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Dates, durations, links
// ---------------------------------------------------------------------------------------------
std::optional<Value> Value::parseDate(const QString& s)
{
    static const QRegularExpression dateTimeRe(QStringLiteral(
        "\\A(\\d{4})-(\\d{2})-(\\d{2})[ T](\\d{2}):(\\d{2})(?::(\\d{2})(?:\\.(\\d{1,9}))?)?(Z|([+-])(\\d{2})(?::?(\\d{2}))?)?\\z"));
    static const QRegularExpression dateRe(QStringLiteral("\\A(\\d{4})-(\\d{2})-(\\d{2})\\z"));
    if (s.size() < 10 || !s.at(0).isDigit())
        return std::nullopt;
    if (auto m = dateTimeRe.match(s); m.hasMatch()) {
        const int y = m.captured(1).toInt(), mo = m.captured(2).toInt(), d = m.captured(3).toInt();
        const int h = m.captured(4).toInt(), mi = m.captured(5).toInt(), sec = m.captured(6).toInt();
        const QString frac = m.captured(7);
        const int ms = frac.isEmpty() ? 0 : QString(frac + QStringLiteral("00")).left(3).toInt();
        const QDate date(y, mo, d);
        if (!date.isValid() || mo < 1 || mo > 12 || h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms)))
            return Value::date(kNaN, true);
        if (m.captured(8).isEmpty())
            return Value::date(fromLocalParts(y, mo - 1, d, h, mi, sec, ms), true);
        qint64 offsetMin = 0;
        if (m.captured(8) != u"Z") {
            offsetMin = m.captured(10).toInt() * 60 + m.captured(11).toInt();
            if (m.captured(9) == u"-")
                offsetMin = -offsetMin;
        }
        const QDateTime utc(date, QTime(0, 0), QTimeZone::UTC);
        const double t = double(utc.toMSecsSinceEpoch()) + ((h * 60.0 + mi) * 60 + sec) * 1000 + ms - offsetMin * 60000.0;
        return Value::date(t, true);
    }
    if (auto m = dateRe.match(s); m.hasMatch()) {
        const int y = m.captured(1).toInt(), mo = m.captured(2).toInt(), d = m.captured(3).toInt();
        if (!QDate(y, mo, d).isValid())
            return Value::date(kNaN, false);
        return Value::date(fromLocalParts(y, mo - 1, d, 0, 0, 0, 0), false);
    }
    return std::nullopt;
}

Value Value::dateOnly() const
{
    if (!m_flag || std::isnan(m_num))
        return date(m_num, false);
    const LocalParts p = localParts(m_num);
    return date(fromLocalParts(double(p.year), double(p.month), double(p.day), 0, 0, 0, 0), false);
}

QString Value::printDate() const
{
    if (std::isnan(m_num))
        return QStringLiteral("0NaN-NaN-NaN");
    const LocalParts p = localParts(m_num);
    QString y = QString::number(std::abs(p.year)).rightJustified(4, u'0');
    if (p.year < 0)
        y.prepend(u'-');
    return y + u'-' + pad2(int(p.month + 1)) + u'-' + pad2(int(p.day));
}

QString Value::printTime() const
{
    if (std::isnan(m_num))
        return QStringLiteral("NaN:NaN:NaN");
    const LocalParts p = localParts(m_num);
    return pad2(int(p.hour)) + u':' + pad2(int(p.minute)) + u':' + pad2(int(p.second));
}

QString Value::relative() const
{
    if (std::isnan(m_num))
        return QStringLiteral("Invalid date");
    return momentFromNow(QDateTime::fromMSecsSinceEpoch(qint64(m_num)));
}

std::optional<Value> Value::parseDuration(const QString& s)
{
    static const QRegularExpression isoRe(QStringLiteral(
        "P(?:([.,\\d]+)Y)?(?:([.,\\d]+)M)?(?:([.,\\d]+)W)?(?:([.,\\d]+)D)?T(?:([.,\\d]+)H)?(?:([.,\\d]+)M)?(?:([.,\\d]+)S)?"));
    // (Obsidian's pattern: "ms" / "milliseconds" are handled below but the pattern never matches them)
    static const QRegularExpression simpleRe(QStringLiteral("\\A(-?\\d+) ?([dhmswMy]|(?:second|minute|hour|day|week|month|year)s?)\\z"));
    if (auto m = isoRe.match(s); m.hasMatch()) {
        auto num = [&](int i) -> double {
            const QString c = m.captured(i);
            if (c.isEmpty())
                return 0;
            // parseInt(): the leading digits
            qsizetype n = 0;
            while (n < c.size() && c[n].isDigit())
                ++n;
            return n ? c.left(n).toDouble() : kNaN;
        };
        DurationParts d;
        d.years = num(1);
        d.months = num(2);
        d.days = 7 * num(3) + num(4);
        d.hours = num(5);
        d.minutes = num(6);
        d.seconds = num(7);
        return duration(d);
    }
    if (auto m = simpleRe.match(s); m.hasMatch()) {
        const double n = m.captured(1).toDouble();
        const QString u = m.captured(2);
        DurationParts d;
        if (u == u"y" || u == u"year" || u == u"years")
            d.years = n;
        else if (u == u"M" || u == u"month" || u == u"months")
            d.months = n;
        else if (u == u"w" || u == u"week" || u == u"weeks")
            d.days = 7 * n;
        else if (u == u"d" || u == u"day" || u == u"days")
            d.days = n;
        else if (u == u"h" || u == u"hour" || u == u"hours")
            d.hours = n;
        else if (u == u"m" || u == u"minute" || u == u"minutes")
            d.minutes = n;
        else if (u == u"s" || u == u"second" || u == u"seconds")
            d.seconds = n;
        else if (u == u"ms" || u == u"millisecond" || u == u"milliseconds")
            d.milliseconds = n;
        else
            return std::nullopt;
        return duration(d);
    }
    return std::nullopt;
}

double Value::durationMs() const
{
    const double now = double(QDateTime::currentMSecsSinceEpoch());
    return addToDate(date(now, true), false).dateMs() - now;
}

Value Value::addToDate(const Value& d, bool subtract) const
{
    const DurationParts& p = durationParts();
    const double n = subtract ? -1 : 1;
    double t = d.dateMs();
    bool time = d.hasTime();
    auto step = [&](int field, double amount) {
        if (amount == 0 || std::isnan(t))
            return;
        LocalParts lp = localParts(t);
        double f[7] = {double(lp.year), double(lp.month), double(lp.day), double(lp.hour), double(lp.minute), double(lp.second), double(lp.ms)};
        f[field] += n * amount;
        t = fromLocalParts(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    };
    step(0, p.years);
    step(1, p.months);
    step(2, p.days);
    if (p.hours != 0) {
        step(3, p.hours);
        time = true;
    }
    if (p.minutes != 0) {
        step(4, p.minutes);
        time = true;
    }
    if (p.seconds != 0) {
        step(5, p.seconds);
        time = true;
    }
    if (p.milliseconds != 0) {
        step(6, p.milliseconds);
        time = true;
    }
    return date(t, time);
}

QString Value::humanize() const { return humanizeDuration(durationParts()); }

std::optional<Value> Value::parseLink(const DataSource* ds, const QString& s, const QString& sourcePath)
{
    if (!s.startsWith(QLatin1String("[[")) || !s.endsWith(QLatin1String("]]")) || s.size() < 4)
        return std::nullopt;
    QString inner = s.mid(2, s.size() - 4);
    const qsizetype pipe = inner.lastIndexOf(u'|');
    if (pipe >= 0) {
        const Value display = string(inner.mid(pipe + 1));
        return link(ds, inner.left(pipe), sourcePath, &display);
    }
    return link(ds, inner, sourcePath);
}

// ---------------------------------------------------------------------------------------------
// JavaScript helpers
// ---------------------------------------------------------------------------------------------
QString Value::jsNumber(double n)
{
    if (std::isnan(n))
        return QStringLiteral("NaN");
    if (std::isinf(n))
        return n < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
    if (n == 0)
        return QStringLiteral("0");
    return props::jsNumber(n);
}

namespace {
bool isJsSpace(QChar c)
{
    return c.isSpace() || c == QChar(0xFEFF);
}

QString jsTrim(const QString& s)
{
    qsizetype a = 0, b = s.size();
    while (a < b && isJsSpace(s[a]))
        ++a;
    while (b > a && isJsSpace(s[b - 1]))
        --b;
    return s.mid(a, b - a);
}
} // namespace

double Value::jsToNumber(const QString& s)
{
    const QString t = jsTrim(s);
    if (t.isEmpty())
        return 0;
    if (t == u"Infinity" || t == u"+Infinity")
        return std::numeric_limits<double>::infinity();
    if (t == u"-Infinity")
        return -std::numeric_limits<double>::infinity();
    static const QRegularExpression radixRe(QStringLiteral("\\A0([xXoObB])([0-9a-fA-F]+)\\z"));
    if (auto m = radixRe.match(t); m.hasMatch()) {
        const QChar r = m.captured(1).at(0).toLower();
        const int base = r == u'x' ? 16 : r == u'o' ? 8 : 2;
        bool ok = false;
        const qulonglong v = m.captured(2).toULongLong(&ok, base);
        return ok ? double(v) : kNaN;
    }
    static const QRegularExpression decRe(QStringLiteral("\\A[+-]?(?:\\d+\\.?\\d*(?:[eE][+-]?\\d+)?|\\.\\d+(?:[eE][+-]?\\d+)?)\\z"));
    if (!decRe.match(t).hasMatch())
        return kNaN;
    return t.toDouble();
}

double Value::jsParseFloat(const QString& s)
{
    qsizetype i = 0;
    while (i < s.size() && isJsSpace(s[i]))
        ++i;
    static const QRegularExpression re(QStringLiteral("\\A[+-]?(?:Infinity|(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?)"));
    const QRegularExpressionMatch m = re.match(QStringView(s).mid(i));
    if (!m.hasMatch())
        return kNaN;
    const QString t = m.captured(0);
    if (t.endsWith(QLatin1String("Infinity")))
        return t.startsWith(u'-') ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    return t.toDouble();
}

bool Value::isUrl(const QString& raw)
{
    const QString s = raw.trimmed();
    static const QRegularExpression re(QStringLiteral("\\A([A-Za-z][A-Za-z0-9+.\\-]*):(.*)\\z"), QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return false;
    const QString scheme = m.captured(1).toLower();
    static const QStringList special = {QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("ws"), QStringLiteral("wss"),
                                        QStringLiteral("ftp"), QStringLiteral("file")};
    if (!special.contains(scheme))
        return true;
    if (scheme == u"file")
        return true;
    QString rest = m.captured(2);
    qsizetype i = 0;
    while (i < rest.size() && (rest[i] == u'/' || rest[i] == u'\\'))
        ++i;
    rest = rest.mid(i);
    qsizetype end = 0;
    while (end < rest.size() && rest[end] != u'/' && rest[end] != u'\\' && rest[end] != u'?' && rest[end] != u'#')
        ++end;
    QString host = rest.left(end);
    const qsizetype at = host.lastIndexOf(u'@');
    if (at >= 0)
        host = host.mid(at + 1);
    if (host.isEmpty())
        return false;
    for (const QChar c : std::as_const(host))
        if (c.isSpace() || c == u'<' || c == u'>' || c == u'^' || c == u'|' || c == u'%' || c == u'"')
            return false;
    return true;
}

} // namespace oc::bases
