// Ordered JSON for canvas files: strict parsing, lexeme preservation, JavaScript number/string
// formatting (checked against node's String(number) / JSON.stringify) and Obsidian's canvas layout.
#include "core/canvas/canvas_json.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace oc::canvas;

namespace {
Json parse(const char* s)
{
    JsonError err;
    std::optional<Json> j = parseJson(QByteArray(s), &err);
    EXPECT_TRUE(j.has_value()) << s << ": " << err.message.toStdString() << " at " << err.offset;
    return j ? *j : Json();
}
} // namespace

TEST(CanvasJson, RejectsInvalidJson)
{
    for (const char* bad : {"", "{", "{\"a\":1,}", "[1,]", "{'a':1}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":.5}", "\"a\nb\"",
                            "{\"a\":1} x", "[1 2]", "{\"a\" 1}", "tru", "\"\\x\"", "\"\\u12\"", "+1", "NaN"}) {
        JsonError err;
        EXPECT_FALSE(parseJson(QByteArray(bad), &err).has_value()) << bad;
        EXPECT_GE(err.offset, 0) << bad;
    }
}

TEST(CanvasJson, ParsesValuesAndKeepsOrder)
{
    const Json j = parse("\xEF\xBB\xBF {\"z\":1,\"a\":[true,false,null],\"m\":{\"k\":\"v\"},\"n\":-1.5e2} ");
    ASSERT_TRUE(j.isObject());
    ASSERT_EQ(j.size(), 4);
    EXPECT_EQ(j.members()[0].key, "z");
    EXPECT_EQ(j.members()[1].key, "a");
    EXPECT_EQ(j.members()[2].key, "m");
    EXPECT_EQ(j.value(u"n").toDouble(), -150);
    EXPECT_TRUE(j.value(u"a").items()[2].isNull());
    EXPECT_EQ(j.value(u"m").value(u"k").toString(), "v");
}

TEST(CanvasJson, LeadingBomOnlyAtStart)
{
    EXPECT_TRUE(parseJson(QByteArray("\xEF\xBB\xBF{}")).has_value());
    EXPECT_FALSE(parseJson(QByteArray("{}\xEF\xBB\xBF")).has_value());
}

TEST(CanvasJson, DuplicateKeysLastValueFirstPosition)
{
    const Json j = parse("{\"a\":1,\"b\":2,\"a\":3}");
    ASSERT_EQ(j.size(), 2);
    EXPECT_EQ(j.members()[0].key, "a");
    EXPECT_EQ(j.members()[0].value.toDouble(), 3);
    EXPECT_EQ(stringifyCompact(j), "{\"a\":3,\"b\":2}");
}

TEST(CanvasJson, StringEscapesDecode)
{
    const Json j = parse(R"(["a\"b\\c\/d\b\f\n\r\t", "\u00e9\u20AC", "\ud83d\ude00", "\ud800x", "raw é 😀"])");
    const auto& it = j.items();
    EXPECT_EQ(it[0].toString(), QString::fromUtf8("a\"b\\c/d\b\f\n\r\t"));
    EXPECT_EQ(it[1].toString(), QString::fromUtf8("é€"));
    EXPECT_EQ(it[2].toString(), QString::fromUtf8("😀"));
    EXPECT_EQ(it[3].toString().size(), 2);
    EXPECT_TRUE(it[3].toString()[0].isHighSurrogate()); // a lone surrogate stays (JS strings allow it)
    EXPECT_EQ(it[4].toString(), QString::fromUtf8("raw é 😀"));
}

TEST(CanvasJson, LexemesAreWrittenBack)
{
    const QByteArray src = R"({"a":1.50,"b":"caf\u00e9 \/","c":-0,"d":1E2,"e":true})";
    const Json j = parse(src.constData());
    EXPECT_EQ(stringifyCompact(j), src);
    EXPECT_EQ(j.value(u"d").toDouble(), 100);
    Json k = j;
    k.insert(QStringLiteral("a"), Json(2.5));
    k.insert(QStringLiteral("b"), Json(QStringLiteral("café /")));
    EXPECT_EQ(stringifyCompact(k), R"({"a":2.5,"b":"café /","c":-0,"d":1E2,"e":true})");
    EXPECT_TRUE(j == parse(R"({"a":1.5,"b":"café /","c":0,"d":100,"e":true})")); // equality ignores lexemes
}

TEST(CanvasJson, JsNumberMatchesJavaScript)
{
    // Expected values from node: String(v).
    const std::pair<double, const char*> cases[] = {
        {0, "0"},
        {-0.0, "0"},
        {1, "1"},
        {-1, "-1"},
        {0.1, "0.1"},
        {1e21, "1e+21"},
        {1e-7, "1e-7"},
        {123456789012345680000.0, "123456789012345680000"},
        {1.5e-6, "0.0000015"},
        {5e-324, "5e-324"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {100, "100"},
        {0.000001, "0.000001"},
        {12.345, "12.345"},
        {-2.5, "-2.5"},
        {1.0 / 3, "0.3333333333333333"},
        {9007199254740992.0, "9007199254740992"},
        {1e20, "100000000000000000000"},
        {123e-20, "1.23e-18"},
        {0.5, "0.5"},
        {4.35, "4.35"},
    };
    for (const auto& [v, want] : cases)
        EXPECT_EQ(jsNumber(v), QByteArray(want)) << want;
    EXPECT_EQ(jsNumber(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(jsNumber(std::nan("")), "null");
}

TEST(CanvasJson, JsonQuoteMatchesJsonStringify)
{
    QString s = QString::fromUtf8("\"\\\b\f\n\r\t\x01\x1f\x7f\u2028/é");
    s += QChar(0xD800);
    s += u'x';
    s += QChar(0xDC00);
    s += QString::fromUtf8("😀");
    // node: JSON.stringify("\"\\\b\f\n\r\t\u0001\u001f\u007f\u2028/é\ud800x\udc00😀")
    EXPECT_EQ(jsonQuote(s), QByteArray("\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xe2\x80\xa8/\xc3\xa9\\ud800x\\udc00\xf0\x9f\x98\x80\""));
}

TEST(CanvasJson, CanvasLayout)
{
    EXPECT_EQ(stringifyCanvas(parse("{}")), "{}");
    EXPECT_EQ(stringifyCanvas(parse("[]")), "[]");
    EXPECT_EQ(stringifyCanvas(parse("{\"nodes\":[],\"edges\":[]}")), "{\n\t\"nodes\":[],\n\t\"edges\":[]\n}");
    // Plain values stay on one line; nested objects/arrays (and null) break the parent up.
    EXPECT_EQ(stringifyCanvas(parse("{\"a\":1,\"b\":\"x\",\"c\":true}")), "{\"a\":1,\"b\":\"x\",\"c\":true}");
    EXPECT_EQ(stringifyCanvas(parse("{\"a\":1,\"b\":null}")), "{\n\t\"a\":1,\n\t\"b\":null\n}");
    EXPECT_EQ(stringifyCanvas(parse("[1,\"a\",false]")), "[1,\"a\",false]");
    EXPECT_EQ(stringifyCanvas(parse("[1,null]")), "[\n\t1,\n\tnull\n]");
    EXPECT_EQ(stringifyCanvas(parse("{\"nodes\":[{\"id\":\"a\",\"x\":1},{\"id\":\"b\",\"k\":[1,2]}]}")),
              "{\n\t\"nodes\":[\n\t\t{\"id\":\"a\",\"x\":1},\n\t\t{\n\t\t\t\"id\":\"b\",\n\t\t\t\"k\":[1,2]\n\t\t}\n\t]\n}");
    EXPECT_EQ(stringifyCanvas(parse("{\"m\":{\"v\":\"1\",\"f\":{}}}")), "{\n\t\"m\":{\n\t\t\"v\":\"1\",\n\t\t\"f\":{}\n\t}\n}");
}

TEST(CanvasJson, EditingMembers)
{
    Json o = Json::object();
    o.insert(QStringLiteral("b"), Json(1));
    o.insert(QStringLiteral("c"), Json(2));
    o.insertAt(0, QStringLiteral("a"), Json(0));
    o.insert(QStringLiteral("b"), Json(5)); // in place
    EXPECT_EQ(stringifyCompact(o), "{\"a\":0,\"b\":5,\"c\":2}");
    EXPECT_TRUE(o.remove(u"b"));
    EXPECT_FALSE(o.remove(u"b"));
    EXPECT_EQ(stringifyCompact(o), "{\"a\":0,\"c\":2}");
    EXPECT_EQ(o.indexOf(u"c"), 1);
}

TEST(CanvasJson, DeepNestingIsRejected)
{
    const QByteArray deep = QByteArray(2000, '[') + QByteArray(2000, ']');
    EXPECT_FALSE(parseJson(deep).has_value());
}
