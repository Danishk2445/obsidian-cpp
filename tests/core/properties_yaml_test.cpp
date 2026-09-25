// Obsidian's frontmatter serializer (core/properties/property_yaml.h) against reference vectors
// produced by the serializer Obsidian uses.
#include "core/properties/property_yaml.h"
#include "core/properties_yaml_vectors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace oc;
using namespace oc::props;

namespace {

QJsonObject vectors()
{
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray(kObsidianYamlVectors), &err);
    EXPECT_EQ(err.error, QJsonParseError::NoError) << err.errorString().toStdString();
    return d.object();
}

std::string show(const QJsonValue& v) { return QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact).toStdString(); }

} // namespace

TEST(PropertiesYaml, ValuesMatchObsidian)
{
    const QJsonArray cases = vectors().value(QStringLiteral("values")).toArray();
    ASSERT_GE(cases.size(), 100);
    for (const QJsonValue& c : cases) {
        const QJsonValue v = c.toObject().value(QStringLiteral("v"));
        const QString expected = c.toObject().value(QStringLiteral("out")).toString();
        EXPECT_EQ(yamlPairValue(v.toVariant()).toStdString(), expected.toStdString()) << "value " << show(v);
    }
}

TEST(PropertiesYaml, KeysMatchObsidian)
{
    const QJsonArray cases = vectors().value(QStringLiteral("keys")).toArray();
    ASSERT_GE(cases.size(), 30);
    for (const QJsonValue& c : cases) {
        const QString key = c.toObject().value(QStringLiteral("k")).toString();
        const QString expected = c.toObject().value(QStringLiteral("out")).toString();
        EXPECT_EQ(yamlKey(key).toStdString(), expected.toStdString()) << "key " << key.toStdString();
    }
}

TEST(PropertiesYaml, DocumentsMatchObsidian)
{
    const QJsonArray cases = vectors().value(QStringLiteral("docs")).toArray();
    for (const QJsonValue& c : cases) {
        QList<QPair<QString, QVariant>> entries;
        for (const QJsonValue& e : c.toObject().value(QStringLiteral("d")).toArray())
            entries.append({e.toArray().at(0).toString(), e.toArray().at(1).toVariant()});
        EXPECT_EQ(yamlDocument(entries).toStdString(), c.toObject().value(QStringLiteral("out")).toString().toStdString());
    }
}

TEST(PropertiesYaml, JsNumber)
{
    EXPECT_EQ(jsNumber(0), "0");
    EXPECT_EQ(jsNumber(-0.0), "-0");
    EXPECT_EQ(jsNumber(1), "1");
    EXPECT_EQ(jsNumber(1.5), "1.5");
    EXPECT_EQ(jsNumber(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(jsNumber(1e21), "1e+21");
    EXPECT_EQ(jsNumber(1e20), "100000000000000000000");
    EXPECT_EQ(jsNumber(123456789.125), "123456789.125");
    EXPECT_EQ(jsNumber(1e-6), "0.000001");
    EXPECT_EQ(jsNumber(1.5e-7), "1.5e-7");
    EXPECT_EQ(jsNumber(-2.5), "-2.5");
    EXPECT_EQ(jsNumber(12345678901234567890.0), "12345678901234567000");
    EXPECT_EQ(jsNumber(std::numeric_limits<double>::quiet_NaN()), ".nan");
    EXPECT_EQ(jsNumber(std::numeric_limits<double>::infinity()), ".inf");
    EXPECT_EQ(jsNumber(-std::numeric_limits<double>::infinity()), "-.inf");
}

TEST(PropertiesYaml, EntriesAndItems)
{
    EXPECT_EQ(yamlEntry(QStringLiteral("tags"), QStringList{QStringLiteral("a"), QStringLiteral("#b")}), "tags:\n  - a\n  - \"#b\"");
    EXPECT_EQ(yamlEntry(QStringLiteral("empty"), QVariant()), "empty:");
    EXPECT_EQ(yamlEntry(QStringLiteral("#k"), 1), "\"#k\": 1");
    bool single = false;
    EXPECT_EQ(yamlInlineItem(QStringLiteral("[[Note]]"), &single), "\"[[Note]]\"");
    EXPECT_TRUE(single);
    yamlInlineItem(QStringLiteral("a\nb"), &single);
    EXPECT_FALSE(single);
    EXPECT_EQ(jsonText(QVariantMap{{QStringLiteral("a"), 1}, {QStringLiteral("b"), QVariantList{QStringLiteral("x"), true}}}),
              "{\"a\":1,\"b\":[\"x\",true]}");
    EXPECT_EQ(jsonText(QVariant()), "null");
}
