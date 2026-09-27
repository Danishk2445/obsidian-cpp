#include "core/bases/base_yaml.h"

#include "core/properties/property_yaml.h"

#include <yaml-cpp/yaml.h>

#include <QRegularExpression>

#include <cmath>
#include <limits>

namespace oc::bases {

// ---------------------------------------------------------------------------------------------
// YamlNode
// ---------------------------------------------------------------------------------------------
YamlNode YamlNode::boolean(bool b)
{
    YamlNode n;
    n.m_type = Type::Bool;
    n.m_bool = b;
    return n;
}

YamlNode YamlNode::number(double v)
{
    YamlNode n;
    n.m_type = Type::Number;
    n.m_number = v;
    return n;
}

YamlNode YamlNode::string(const QString& s)
{
    YamlNode n;
    n.m_type = Type::String;
    n.m_string = s;
    return n;
}

YamlNode YamlNode::list(const QList<YamlNode>& items)
{
    YamlNode n;
    n.m_type = Type::List;
    n.m_items = items;
    return n;
}

YamlNode YamlNode::map(const QList<Entry>& entries)
{
    YamlNode n;
    n.m_type = Type::Map;
    n.m_entries = entries;
    return n;
}

YamlNode YamlNode::fromVariant(const QVariant& v)
{
    if (!v.isValid() || v.isNull())
        return {};
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
        QList<YamlNode> items;
        for (const QVariant& item : v.toList())
            items << fromVariant(item);
        return list(items);
    }
    case QMetaType::QVariantMap: {
        QList<Entry> entries;
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            entries.append({it.key(), fromVariant(it.value())});
        return map(entries);
    }
    default:
        return string(v.toString());
    }
}

const YamlNode* YamlNode::find(const QString& key) const
{
    for (const Entry& e : m_entries)
        if (e.first == key)
            return &e.second;
    return nullptr;
}

YamlNode* YamlNode::find(const QString& key)
{
    for (Entry& e : m_entries)
        if (e.first == key)
            return &e.second;
    return nullptr;
}

YamlNode YamlNode::value(const QString& key) const
{
    const YamlNode* n = find(key);
    return n ? *n : YamlNode();
}

void YamlNode::set(const QString& key, const YamlNode& value)
{
    if (m_type != Type::Map) {
        *this = map();
    }
    if (YamlNode* n = find(key)) {
        *n = value;
        return;
    }
    m_entries.append({key, value});
}

bool YamlNode::remove(const QString& key)
{
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].first == key) {
            m_entries.removeAt(i);
            return true;
        }
    }
    return false;
}

QVariant YamlNode::toVariant() const
{
    switch (m_type) {
    case Type::Null:
        return {};
    case Type::Bool:
        return m_bool;
    case Type::Number:
        return m_number;
    case Type::String:
        return m_string;
    case Type::List: {
        QVariantList l;
        for (const YamlNode& n : m_items)
            l << n.toVariant();
        return l;
    }
    case Type::Map: {
        QVariantMap m;
        for (const Entry& e : m_entries)
            m.insert(e.first, e.second.toVariant());
        return m;
    }
    }
    return {};
}

bool YamlNode::operator==(const YamlNode& o) const
{
    if (m_type != o.m_type)
        return false;
    switch (m_type) {
    case Type::Null:
        return true;
    case Type::Bool:
        return m_bool == o.m_bool;
    case Type::Number:
        return m_number == o.m_number || (std::isnan(m_number) && std::isnan(o.m_number));
    case Type::String:
        return m_string == o.m_string;
    case Type::List:
        return m_items == o.m_items;
    case Type::Map:
        return m_entries == o.m_entries;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------
namespace {

// A plain scalar resolved with the YAML 1.2 core schema (the `yaml` library's default).
YamlNode resolvePlain(const QString& s)
{
    static const QRegularExpression nullRe(QStringLiteral("\\A(?:~|[Nn]ull|NULL)?\\z"));
    static const QRegularExpression trueRe(QStringLiteral("\\A(?:[Tt]rue|TRUE)\\z"));
    static const QRegularExpression falseRe(QStringLiteral("\\A(?:[Ff]alse|FALSE)\\z"));
    static const QRegularExpression octRe(QStringLiteral("\\A0o([0-7]+)\\z"));
    static const QRegularExpression intRe(QStringLiteral("\\A[-+]?[0-9]+\\z"));
    static const QRegularExpression hexRe(QStringLiteral("\\A0x([0-9a-fA-F]+)\\z"));
    static const QRegularExpression specialRe(QStringLiteral("\\A(?:([-+]?)\\.(?:inf|Inf|INF)|(\\.nan|\\.NaN|\\.NAN))\\z"));
    static const QRegularExpression expRe(QStringLiteral("\\A[-+]?(?:\\.[0-9]+|[0-9]+(?:\\.[0-9]*)?)[eE][-+]?[0-9]+\\z"));
    static const QRegularExpression floatRe(QStringLiteral("\\A[-+]?(?:\\.[0-9]+|[0-9]+\\.[0-9]*)\\z"));
    if (nullRe.match(s).hasMatch())
        return {};
    if (trueRe.match(s).hasMatch())
        return YamlNode::boolean(true);
    if (falseRe.match(s).hasMatch())
        return YamlNode::boolean(false);
    if (auto m = octRe.match(s); m.hasMatch())
        return YamlNode::number(double(m.captured(1).toULongLong(nullptr, 8)));
    if (intRe.match(s).hasMatch() || expRe.match(s).hasMatch() || floatRe.match(s).hasMatch())
        return YamlNode::number(s.toDouble());
    if (auto m = hexRe.match(s); m.hasMatch())
        return YamlNode::number(double(m.captured(1).toULongLong(nullptr, 16)));
    if (auto m = specialRe.match(s); m.hasMatch()) {
        if (!m.captured(2).isEmpty())
            return YamlNode::number(std::numeric_limits<double>::quiet_NaN());
        const double inf = std::numeric_limits<double>::infinity();
        return YamlNode::number(m.captured(1) == u"-" ? -inf : inf);
    }
    return YamlNode::string(s);
}

YamlNode convert(const YAML::Node& n)
{
    switch (n.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
        return {};
    case YAML::NodeType::Scalar: {
        const QString s = QString::fromStdString(n.Scalar());
        if (n.Tag() == "!")
            return YamlNode::string(s); // quoted or block scalar
        if (n.Tag() == "tag:yaml.org,2002:str")
            return YamlNode::string(s);
        return resolvePlain(s);
    }
    case YAML::NodeType::Sequence: {
        QList<YamlNode> items;
        for (const auto& it : n)
            items << convert(it);
        return YamlNode::list(items);
    }
    case YAML::NodeType::Map: {
        QList<YamlNode::Entry> entries;
        for (const auto& it : n) {
            const QString key = it.first.IsScalar() ? QString::fromStdString(it.first.Scalar()) : QString();
            YamlNode value = convert(it.second);
            bool replaced = false;
            for (YamlNode::Entry& e : entries) {
                if (e.first == key) { // a later duplicate wins (JavaScript object semantics)
                    e.second = value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                entries.append({key, std::move(value)});
        }
        return YamlNode::map(entries);
    }
    }
    return {};
}

} // namespace

std::optional<YamlNode> parseYaml(const QString& text, QString* error)
{
    try {
        const YAML::Node root = YAML::Load(text.toStdString());
        return convert(root);
    } catch (const YAML::Exception& e) {
        // like the yaml library's messages: "<problem> at line L, column C"
        if (error) {
            QString msg = QString::fromStdString(e.msg);
            if (!msg.isEmpty())
                msg[0] = msg[0].toUpper();
            if (!e.mark.is_null())
                msg += QStringLiteral(" at line %1, column %2").arg(e.mark.line + 1).arg(e.mark.column + 1);
            *error = msg;
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------
// Writing (the structure of property_yaml.cpp's node()/pair(), with ordered mappings)
// ---------------------------------------------------------------------------------------------
namespace {

const QString kStep = QStringLiteral("  ");

QString jsNumberYaml(double v) { return props::jsNumber(v); }

QString node(const YamlNode& v, const QString& indent);

QString pairValue(const YamlNode& value, const QString& indent)
{
    const QString childIndent = indent + kStep;
    const QString valueStr = node(value, childIndent);
    if (value.isList() || value.isMap()) {
        const bool empty = value.isList() ? value.items().isEmpty() : value.entries().isEmpty();
        return (empty ? QStringLiteral(" ") : u'\n' + childIndent) + valueStr;
    }
    if (valueStr.isEmpty() || valueStr.startsWith(u'\n'))
        return valueStr;
    return u' ' + valueStr;
}

QString pair(const QString& key, const YamlNode& value, const QString& indent)
{
    return props::yamlString(key, indent + kStep, true) + u':' + pairValue(value, indent);
}

QString joinItems(const QStringList& lines, const QString& indent)
{
    QString str = lines.value(0);
    for (qsizetype i = 1; i < lines.size(); ++i)
        str += lines[i].isEmpty() ? QStringLiteral("\n") : u'\n' + indent + lines[i];
    return str;
}

QString node(const YamlNode& v, const QString& indent)
{
    switch (v.type()) {
    case YamlNode::Type::Null:
        return QString(); // nullStr: ""
    case YamlNode::Type::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case YamlNode::Type::Number:
        return jsNumberYaml(v.toNumber());
    case YamlNode::Type::String:
        return props::yamlString(v.toString(), indent, false);
    case YamlNode::Type::List: {
        if (v.items().isEmpty())
            return QStringLiteral("[]");
        QStringList lines;
        for (const YamlNode& item : v.items())
            lines << QStringLiteral("- ") + node(item, indent + kStep);
        return joinItems(lines, indent);
    }
    case YamlNode::Type::Map: {
        if (v.entries().isEmpty())
            return QStringLiteral("{}");
        QStringList lines;
        for (const YamlNode::Entry& e : v.entries())
            lines << pair(e.first, e.second, indent);
        return joinItems(lines, indent);
    }
    }
    return {};
}

} // namespace

QString dumpYaml(const YamlNode& root)
{
    if (root.isNull())
        return {};
    return node(root, QString()) + u'\n';
}

QString scalarText(const YamlNode& n)
{
    switch (n.type()) {
    case YamlNode::Type::Null:
        return QStringLiteral("null");
    case YamlNode::Type::Bool:
        return n.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case YamlNode::Type::Number:
        return props::jsNumber(n.toNumber());
    case YamlNode::Type::String:
        return n.toString();
    default:
        return dumpYaml(n).trimmed();
    }
}

} // namespace oc::bases
