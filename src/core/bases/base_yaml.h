#pragma once

// The YAML of .base files: an ordered tree (mappings keep their key order, like the JavaScript objects
// Obsidian builds), read with yaml-cpp using the YAML 1.2 core schema of the `yaml` library Obsidian
// uses (null / true / 1 / 1.5 / .inf / 0x1F resolve, quoted scalars stay strings), and written the
// way Obsidian writes .base files: yaml.stringify(object, {nullStr: "", lineWidth: 0}) — block
// mappings, sequences indented by two spaces, the scalar rules of props::yamlString().

#include <QList>
#include <QPair>
#include <QString>
#include <QVariant>

#include <optional>

namespace oc::bases {

class YamlNode {
public:
    enum class Type { Null, Bool, Number, String, List, Map };
    using Entry = QPair<QString, YamlNode>;

    YamlNode() = default;
    static YamlNode boolean(bool b);
    static YamlNode number(double n);
    static YamlNode string(const QString& s);
    static YamlNode list(const QList<YamlNode>& items = {});
    static YamlNode map(const QList<Entry>& entries = {});
    static YamlNode fromVariant(const QVariant& v); // QVariantMap keys come out sorted

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isList() const { return m_type == Type::List; }
    bool isMap() const { return m_type == Type::Map; }

    bool toBool() const { return m_bool; }
    double toNumber() const { return m_number; }
    const QString& toString() const { return m_string; } // strings only ("" otherwise)

    const QList<YamlNode>& items() const { return m_items; }
    QList<YamlNode>& items() { return m_items; }
    const QList<Entry>& entries() const { return m_entries; }
    QList<Entry>& entries() { return m_entries; }

    // Mappings.
    bool contains(const QString& key) const { return find(key) != nullptr; }
    const YamlNode* find(const QString& key) const;
    YamlNode* find(const QString& key);
    YamlNode value(const QString& key) const; // null when missing
    void set(const QString& key, const YamlNode& value); // in place, or appended
    bool remove(const QString& key);

    QVariant toVariant() const; // null -> invalid QVariant, maps -> QVariantMap
    bool operator==(const YamlNode& o) const;
    bool operator!=(const YamlNode& o) const { return !(*this == o); }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    QString m_string;
    QList<YamlNode> m_items;
    QList<Entry> m_entries;
};

// Parse a YAML document. nullopt (and *error, yaml-cpp's message) on a syntax error; an empty
// document is a null node.
std::optional<YamlNode> parseYaml(const QString& text, QString* error = nullptr);

// The document text Obsidian writes for `root` (ends with "\n"; "" for null).
QString dumpYaml(const YamlNode& root);

// A scalar written as in flow context text (numbers like JavaScript, strings plain), for messages.
QString scalarText(const YamlNode& node);

} // namespace oc::bases
