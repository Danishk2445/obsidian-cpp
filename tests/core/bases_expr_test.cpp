// The Bases formula language: parsing, operators, functions, null propagation and errors, checked
// against Obsidian 1.13's semantics (expected values follow its implementation).

#include "core/bases/base_expr.h"
#include "core/bases_test_support.h"

#include <gtest/gtest.h>

using namespace oc::bases;
using bases_test::Env;

namespace {

struct Case {
    const char* expr;
    const char* expected; // toString() of the value ("null" for null, "Error: ..." for errors)
    const char* type = nullptr; // typeName() when given
};

void PrintTo(const Case& c, std::ostream* os) { *os << c.expr; }

class ExprTable : public ::testing::TestWithParam<Case> {};

TEST_P(ExprTable, Evaluates)
{
    Env env;
    const Case& c = GetParam();
    const Value v = env.eval(QString::fromUtf8(c.expr));
    EXPECT_EQ(v.toString(), QString::fromUtf8(c.expected)) << c.expr;
    if (c.type) {
        EXPECT_EQ(v.typeName(), QString::fromUtf8(c.type)) << c.expr;
    }
}

const Case kLiterals[] = {
    {"1", "1", "Number"},
    {"1.5", "1.5", "Number"},
    {"-2", "-2", "Number"},
    {"1e3", "1000", "Number"},
    {"\"text\"", "text", "String"},
    {"'say \"hi\"'", "say \"hi\"", "String"},
    {"\"a\\nb\"", "a\nb", "String"},
    {"'\\u0041'", "A", "String"},
    {"true", "true", "Boolean"},
    {"false", "false", "Boolean"},
    {"null", "null", "Null"},
    {"[1, \"a\", true]", "1, a, true", "List"},
    {"[]", "", "List"},
    {"[1, 2,]", "1, 2", "List"},
    {"/ab+c/i", "/ab+c/i", "RegExp"},
    {"{a: 1, \"b c\": \"x\"}", "{\"a\":\"1\",\"b c\":\"x\"}", "Object"},
    {"{}", "{}", "Object"},
};
INSTANTIATE_TEST_SUITE_P(Literals, ExprTable, ::testing::ValuesIn(kLiterals));

const Case kArithmetic[] = {
    {"1 + 2 * 3", "7"},
    {"(1 + 2) * 3", "9"},
    {"7 % 3", "1"},
    {"-7 % 3", "-1"},
    {"10 / 4", "2.5"},
    {"1 / 0", "Infinity"},
    {"-1 / 0", "-Infinity"},
    {"0 / 0", "NaN"},
    {"0.1 + 0.2", "0.30000000000000004"},
    {"1e21 * 1", "1e+21"},
    {"2 - 3 - 4", "-5"},
    {"-(2 + 3)", "-5"},
    {"\"a\" + 1", "a1", "String"},
    {"1 + \"a\"", "1a", "String"},
    {"\"a\" + missing", "a", "String"},
    {"missing + \"a\"", "a", "String"},
    {"missing + 1", "null"},
    {"1 + true", "Error: Invalid operator between Number and Boolean"},
    {"[1, 2] + [3]", "1, 2, 3", "List"},
    {"-\"a\"", "Error: Unable to negate a"},
    {"-missing", "null"},
};
INSTANTIATE_TEST_SUITE_P(Arithmetic, ExprTable, ::testing::ValuesIn(kArithmetic));

const Case kLogic[] = {
    {"1 < 2", "true"},
    {"2 <= 2", "true"},
    {"\"b\" > \"a\"", "true"},
    {"\"10\" < 9", "false"},
    {"\"a\" < 1", "false"},
    {"\"a\" >= 1", "false"},
    {"1 == \"1\"", "true"},
    {"true == 1", "true"},
    {"\"true\" == true", "false"},
    {"\"\" == 0", "true"},
    {"null == null", "true"},
    {"missing == null", "true"},
    {"missing == \"\"", "false"},
    {"\"a\" != \"b\"", "true"},
    {"missing > 5", "null"},
    {"5 < missing", "null"},
    {"1 && 0", "false"},
    {"0 || \"x\"", "true"},
    {"!0", "true"},
    {"!missing", "null"},
    {"!\"\"", "true"},
    {"true || false && false", "true"},
    {"1 < 2 == true", "true"},
    {"[1] == 1", "true"},
    {"[1, 2] == [1, 2]", "true"},
    {"[1, 2] == [2, 1]", "false"},
    {"if(true, \"a\", \"b\")", "a"},
    {"if(1 > 2, \"a\", \"b\")", "b"},
    {"if(false, \"a\")", "null"},
    {"if(missing, \"a\", \"b\")", "b"},
    {"true.if(\"y\", \"n\")", "y"},
    {"if(1)", "Error: Cannot call function \"if\", not enough arguments."},
    {"if(1, 2, 3, 4)", "Error: Cannot call function \"if\", too many arguments."},
};
INSTANTIATE_TEST_SUITE_P(Logic, ExprTable, ::testing::ValuesIn(kLogic));

const Case kStrings[] = {
    {"\"Hello\".lower()", "hello"},
    {"\"hello world\".title()", "Hello World"},
    {"\"hELLO\".title()", "Hello"},
    {"\"  x \".trim()", "x"},
    {"\"abc\".contains(\"B\")", "true"},
    {"\"abc\".contains(\"\")", "false"},
    {"\"abc\".containsAny(\"x\", \"b\")", "true"},
    {"\"abc\".containsAll(\"a\", \"x\")", "false"},
    {"\"abc\".containsAll(\"A\", \"c\")", "true"},
    {"\"a,b,c\".split(\",\")", "a, b, c", "List"},
    {"\"a,b,c\".split(\",\", 2).length", "2"},
    {"\"a1b2\".split(/\\d/)", "a, b, "},
    {"\"abc\".split(\"\")", "a, b, c"},
    {"\"hello\".slice(1, 3)", "el"},
    {"\"hello\".slice(-3)", "llo"},
    {"\"hello\".slice(4, 1)", ""},
    {"\"ab\".repeat(3)", "ababab"},
    {"\"ab\".repeat(-1)", "Error: Invalid count value: -1"},
    {"\"hello\".replace(\"l\", \"L\")", "heLLo"},
    {"\"hello\".replace(/l/, \"L\")", "heLlo"},
    {"\"hello\".replace(/l/g, \"L\")", "heLLo"},
    {"\"2024-01\".replace(/(\\d+)-(\\d+)/, \"$2/$1\")", "01/2024"},
    {"\"hello\".reverse()", "olleh"},
    {"\"hello\".startsWith(\"he\")", "true"},
    {"\"hello\".endsWith(\"lo\")", "true"},
    {"\"abc\".length", "3"},
    {"\"\".isEmpty()", "true"},
    {"\"abc\"[1]", "b"},
    {"\"abc\"[5]", ""},
    {"\"abc\".isType(\"string\")", "true"},
    {"\"a\".contains(1)", "Error: Type error in \"contains\", parameter \"value\". Expected String, given Number."},
    {"\"a\".contains()", "Error: Cannot call function \"contains\", not enough arguments."},
    {"\"a\".trim(1)", "Error: Cannot call function \"trim\", too many arguments."},
    {"\"abc\".foo()", "Error: Cannot find function \"foo\" on type String"},
    {"\"abc\".x", "Error: Cannot find \"x\" on type String"},
    {"\"abc\".UPPER()", "Error: Cannot find function \"UPPER\" on type String"},
    {"\"Abc\".LOWER()", "abc"},
    {"escapeHTML(\"<b>&\")", "&lt;b&gt;&amp;"},
    {"\"x\".contains(missing)", "null"},
};
INSTANTIATE_TEST_SUITE_P(Strings, ExprTable, ::testing::ValuesIn(kStrings));

const Case kNumbers[] = {
    {"2.5.round()", "3"},
    {"(-2.5).round()", "-2"},
    {"3.14159.round(2)", "3.14"},
    {"3.7.floor()", "3"},
    {"3.2.ceil()", "4"},
    {"(-3).abs()", "3"},
    {"3.14159.toFixed(2)", "3.14"},
    {"2.5.toFixed(0)", "3"},
    {"1.005.toFixed(2)", "1.00"},
    {"(-1.5).toFixed(1)", "-1.5"},
    {"1.toFixed(101)", "Error: toFixed() digits argument must be between 0 and 100"},
    {"1.isEmpty()", "false"},
    {"min(3, 1, 2)", "1"},
    {"max(3, 1, 2)", "3"},
    {"min(1, missing)", "null"},
    {"number(\"12.5abc\")", "12.5"},
    {"number(\"abc\")", "Error: Unable to parse \"abc\" as a number."},
    {"number(true)", "1"},
    {"number([1])", "Error: Unable to convert \"List\" to a number."},
    {"number(date(\"1970-01-02\"))", "86400000"},
    {"5.toString()", "5", "String"},
    {"0.isTruthy()", "false"},
};
INSTANTIATE_TEST_SUITE_P(Numbers, ExprTable, ::testing::ValuesIn(kNumbers));

const Case kLists[] = {
    {"[1, 2, 3].length", "3"},
    {"[1, 2, 3].sum()", "6"},
    {"[1, 2, 3].mean()", "2"},
    {"[1, \"a\", 3].mean()", "1.3333333333333333"},
    {"[1, 2, 3, 4].median()", "2.5"},
    {"[3, 5].min()", "3"},
    {"[3, 5].max()", "5"},
    {"[].sum()", "null"},
    {"[1, 2, 3].stddev().round(2)", "0.82"},
    {"[3, 1, 2].sort()", "1, 2, 3"},
    {"[\"b\", \"a10\", \"a9\"].sort()", "a9, a10, b"},
    {"[1, [2, [3]]].flat()", "1, 2, 3"},
    {"[1, 2, 2, 3, 1].unique()", "1, 2, 3"},
    {"[\"b\", 3, \"a\", 1].unique()", "1, 3, b, a"},
    {"[1, 2, 3].reverse()", "3, 2, 1"},
    {"[1, 2, 3].contains(2)", "true"},
    {"[1, 2, 3].contains(\"2\")", "true"},
    {"[1, 2, 3].containsAny(5, 3)", "true"},
    {"[1, 2, 3].containsAny([5, 3])", "true"},
    {"[1, 2, 3].containsAll(1, 5)", "false"},
    {"[1, 2, 3].join(\"-\")", "1-2-3"},
    {"[1, 2, 3].join()", "Error: Cannot call function \"join\", not enough arguments."},
    {"[1, 2, 3].map(value * 2)", "2, 4, 6"},
    {"[1, 2, 3].map(index)", "0, 1, 2"},
    {"[1, 2, 3].filter(value > 1)", "2, 3"},
    {"[1, 2, 3].reduce(acc + value, 0)", "6"},
    {"[1, 2, 3].reduce(acc + value)", "Error: Cannot call function \"reduce\", not enough arguments."},
    {"[1, 2, 3][0]", "1"},
    {"[1, 2, 3][-1]", "3"},
    {"[1, 2, 3][9]", "null"},
    {"[1, 2, 3][\"a\"]", "Error: Array index must be an integer. Got String"},
    {"[1, 2, 3].slice(1)", "2, 3"},
    {"[1, 2, 3].slice(-2, -1)", "2"},
    {"[].isEmpty()", "true"},
    {"list(\"a\")", "a", "List"},
    {"list([1, 2])", "1, 2", "List"},
    {"[\"2024-01-01\", \"2024-03-01\"].latest()", "2024-03-01T00:00:00", "Date"},
    {"[\"2024-01-01\", \"2024-03-01\"].earliest()", "2024-01-01T00:00:00"},
    {"[].latest()", "null"},
    {"[1, 2].isTruthy()", "true"},
    {"[1, 2].toString()", "1, 2", "String"},
};
INSTANTIATE_TEST_SUITE_P(Lists, ExprTable, ::testing::ValuesIn(kLists));

const Case kObjects[] = {
    {"{a: 1, b: \"x\"}.keys()", "a, b"},
    {"{a: 1, b: 2}.values()", "1, 2"},
    {"{a: 1}.a", "1"},
    {"{a: 1}.A", "1"},
    {"{a: 1}[\"a\"]", "1"},
    {"{a: 1}.missing", "null"},
    {"{}.isEmpty()", "true"},
    {"{a: 1, b: 2}.map(value + 1)", "2, 3"},
    {"{a: 1, b: 2}.filter(key == \"b\")", "{\"b\":\"2\"}"},
    {"{a: 1}[1]", "Error: Object access must be an string. Got Number"},
};
INSTANTIATE_TEST_SUITE_P(Objects, ExprTable, ::testing::ValuesIn(kObjects));

const Case kDates[] = {
    {"date(\"2024-01-15\")", "2024-01-15", "Date"},
    {"date(\"2024-01-15 10:30\")", "2024-01-15T10:30:00"},
    {"date(\"2024-01-15T10:30:00Z\")", "2024-01-15T10:30:00"},
    {"date(\"2024-01-15T10:30:00+02:00\")", "2024-01-15T08:30:00"},
    {"date(\"20240115\")", "2024-01-15"},
    {"date(\"202401151030\")", "2024-01-15T10:30:00"},
    {"date(\"garbage\")", "null"},
    {"date(\"2024-01-15\") + \"1 week\"", "2024-01-22"},
    {"date(\"2024-01-15\") - \"3d\"", "2024-01-12"},
    {"date(\"2024-01-31\") + \"1 month\"", "2024-03-02"},
    {"date(\"2024-01-15\") + \"1y\"", "2025-01-15"},
    {"date(\"2024-01-15 10:30\") + \"2h\"", "2024-01-15T12:30:00"},
    {"date(\"2024-01-15\") + \"2h\"", "2024-01-15T02:00:00"},
    {"date(\"2024-01-15\") + duration(\"1 day\")", "2024-01-16"},
    {"date(\"2024-01-15\") * 2", "Error: Invalid operator between Date and Number"},
    {"date(\"2024-01-15\") * \"1d\"", "Error: Invalid date operator"},
    {"(date(\"2024-01-20\") - date(\"2024-01-15\")).days", "5"},
    {"(date(\"2024-01-20\") - date(\"2024-01-15\")).hours", "120"},
    {"date(\"2024-01-15\") > date(\"2024-01-10\")", "true"},
    {"date(\"2024-01-15\") > \"2024-01-10\"", "true"},
    {"date(\"2024-01-15\") == \"2024-01-15\"", "true"},
    {"date(\"2024-01-15 10:00\") == date(\"2024-01-15\")", "true"},
    {"date(\"2024-01-15 10:00\") == date(\"2024-01-15 11:00\")", "false"},
    {"date(\"2024-01-15\").format(\"YYYY/MM/DD\")", "2024/01/15"},
    {"date(\"2024-01-15\").format(\"dddd, MMMM Do\")", "Monday, January 15th"},
    {"date(\"2024-01-15\").year", "2024"},
    {"date(\"2024-01-15\").month", "1"},
    {"date(\"2024-01-15\").day", "15"},
    {"date(\"2024-01-15T10:30:05\").minute", "30"},
    {"date(\"1970-01-02\").timestamp", "86400000"},
    {"date(\"2024-01-15T10:30:00\").time()", "10:30:00"},
    {"date(\"2024-01-15T10:30:00\").date()", "2024-01-15"},
    {"date(\"2024-01-15\").isEmpty()", "false"},
    {"now() > date(\"2000-01-01\")", "true"},
    {"today() <= now()", "true"},
    {"today().hour", "0"},
    {"date(\"2024-01-15\").foo", "Error: Cannot find \"foo\" on type Date"},
};
INSTANTIATE_TEST_SUITE_P(Dates, ExprTable, ::testing::ValuesIn(kDates));

const Case kDurations[] = {
    {"duration(\"1d\")", "a day", "Duration"},
    {"duration(\"3 days\")", "3 days"},
    {"duration(\"2 hours\").hours", "2"},
    {"duration(\"1 week\").days", "7"},
    {"duration(\"90m\").hours", "1.5"},
    {"duration(\"P1DT2H\").hours", "26"},
    {"duration(\"P1D\")", "null"},
    {"duration(\"5ms\")", "null"},
    {"duration(\"nope\")", "null"},
    {"duration(\"1d\") * 2", "2 days"},
    {"duration(\"2d\") / 2", "a day"},
    {"duration(\"1d\") + duration(\"2d\")", "3 days"},
    {"duration(\"2d\") > duration(\"1d\")", "true"},
    {"duration(\"2d\") > \"1d\"", "true"},
    {"duration(\"1d\") == \"24h\"", "true"},
    {"duration(\"1 year\")", "a year"},
    {"duration(\"45 minutes\")", "an hour"},
};
INSTANTIATE_TEST_SUITE_P(Durations, ExprTable, ::testing::ValuesIn(kDurations));

const Case kNotes[] = {
    {"author", "Frank Herbert", "String"},
    {"note.author", "Frank Herbert"},
    {"AUTHOR", "Frank Herbert"},
    {"note[\"author\"]", "Frank Herbert"},
    {"pages", "412", "Number"},
    {"pages > 300", "true"},
    {"pages * price", "4115.88"},
    {"rating.round()", "5"},
    {"status == \"read\"", "true"},
    {"published", "1965-08-01", "Date"},
    {"published.year", "1965"},
    {"started", "2024-01-15T10:30:00", "Date"},
    {"series", "[[Dune Chronicles]]", "Link"},
    {"series.isType(\"string\")", "true"},
    {"site", "https://example.com/dune", "URL"},
    {"tags", "#book, #scifi/classic", "List"},
    {"tags.contains(\"scifi\")", "true"},
    {"tags.contains(\"#sci\")", "false"},
    {"missing", "null", "Null"},
    {"missing.contains(\"x\")", "null"},
    {"missing.isEmpty()", "true"},
    {"missing.foo", "null"},
    {"missing.length", "null"},
    {"note.keys().length", "10"},
    {"file.name", "Dune", "String"},
    {"file.basename", "Dune"},
    {"file.fullname", "Dune.md"},
    {"file.path", "Books/Dune.md"},
    {"file.folder", "Books"},
    {"file.ext", "md"},
    {"file.size > 0", "true"},
    {"file.mtime.year", "2024"},
    {"file.ctime", "2023-11-14T22:13:20"},
    {"file.file", "Books/Dune.md", "File"},
    {"file", "Books/Dune.md", "File"},
    {"file.tags", "#desert, #book, #scifi/classic", "List"},
    {"file.hasTag(\"book\")", "true"},
    {"file.hasTag(\"#scifi\")", "true"},
    {"file.hasTag(\"sci\")", "false"},
    {"file.hasTag(\"x\", \"desert\")", "true"},
    {"file.inFolder(\"Books\")", "true"},
    {"file.inFolder(\"/Books/\")", "true"},
    {"file.inFolder(\"Book\")", "false"},
    {"file.inFolder(\"\")", "true"},
    {"file.hasProperty(\"author\")", "true"},
    {"file.hasProperty(\"Author\")", "false"},
    {"file.hasLink(\"Hyperion\")", "true"},
    {"file.hasLink(\"Frank Herbert\")", "true"},
    {"file.hasLink(link(\"Books/Neuromancer\"))", "false"},
    {"file.hasLink(file(\"Hyperion\"))", "true"},
    {"file.links.length", "3"},
    {"file.links[1]", "[[Frank Herbert|Frank Herbert]]", "Link"},
    {"file.backlinks", "[[Authors/Frank Herbert.md|Frank Herbert]], [[Books/Hyperion.md|Hyperion]]"},
    {"file.embeds", ""},
    {"file.properties.author", "Frank Herbert"},
    {"file.asLink()", "[[Books/Dune.md]]", "Link"},
    {"file.asLink(\"D\")", "[[Books/Dune.md|D]]"},
    {"file.foo", "Error: Cannot find \"foo\" on type File"},
    {"file.name.lower()", "dune"},
    {"link(\"Dune\")", "[[Dune]]", "Link"},
    {"link(\"Dune\", \"The book\")", "[[Dune|The book]]"},
    {"link(\"[[Dune|x]]\")", "[[Dune|x]]"},
    {"link(\"https://obsidian.md\")", "https://obsidian.md", "URL"},
    {"link(\"Hyperion\").asFile().name", "Hyperion"},
    {"link(\"Nope\").asFile()", "null"},
    {"link(\"Hyperion\").linksTo(file(\"Dune\"))", "false"},
    {"link(\"Dune\") == \"Dune\"", "true"},
    {"link(\"Dune\") == \"[[Dune]]\"", "true"},
    {"link(\"Dune\") == file", "true"},
    {"file == \"Books/Dune.md\"", "true"},
    {"file(\"Hyperion\").name", "Hyperion"},
    {"file(\"Nope\")", "null"},
    {"file(\"Hyperion\").pages", "Error: Cannot find \"pages\" on type File"},
    {"file(\"Hyperion\").properties.pages", "482"},
    {"image(\"cover.png\")", "![[cover.png]]", "Image"},
    {"image(\"https://x.org/a.png\")", "![](https://x.org/a.png)"},
    {"icon(\"book\")", "book", "String"},
    {"html(\"<b>x</b>\")", "<b>x</b>", "HTML"},
    {"this", "Books.base", "ThisFile"},
    {"this.file.name", "Books.base"},
    {"this.file.ext", "base"},
    {"this.author", "null"},
    {"file.tags[1].matches(\"#book\")", "true"},
    {"file.tags[2].matches(\"scifi\")", "true"},
    {"formula", "Formulas[]", "Formulas"},
    {"formula.nope", "Error: Cannot find \"nope\" on type Formulas"},
    {"random() < 1", "true"},
    {"nope()", "Error: Cannot find function \"nope\""},
    {"today.x", "null"},
    {"(1)(2)", "null"}, // invalid formulas evaluate to null (their error shows in the formula editor)
    {"", "null"},
    {"   ", "null"},
};
INSTANTIATE_TEST_SUITE_P(Notes, ExprTable, ::testing::ValuesIn(kNotes));

TEST(BasesExpr, ParseErrorsAreInvalidFormulas)
{
    const struct {
        const char* text;
        const char* message;
    } cases[] = {
        {"'it''s'", "Unable to parse formula. (position 4)"},
        {"(1)(2)", "Invalid function name"},
        {"a[0]()", "Invalid function name"},
        {"1 +", "Unable to parse formula. (position 0)"},
        {"a b", "Unable to parse formula. (position 1)"},
        {"\"unterminated", "Unable to parse formula. (position 0)"},
        {"\"bad \\q escape\"", "Unable to parse formula. (position 0)"},
        {"x.true", "Unable to parse formula. (position 0)"},
        {"#", "Unable to parse formula. (position 0)"},
    };
    for (const auto& c : cases) {
        const Formula f(QString::fromUtf8(c.text));
        EXPECT_TRUE(f.isInvalid()) << c.text;
        EXPECT_EQ(f.errorMessage(), QString::fromUtf8(c.message)) << c.text;
    }
    const Formula bad(QStringLiteral("1 + (2"));
    EXPECT_TRUE(bad.isInvalid());
    EXPECT_EQ(bad.errorMessage(), QStringLiteral("Unable to parse formula. (position 0)"));
    const Formula trailing(QStringLiteral("  a )"));
    EXPECT_EQ(trailing.errorMessage(), QStringLiteral("Unable to parse formula. (position 3)"));
    const Formula empty(QStringLiteral(" "));
    EXPECT_TRUE(empty.isEmpty());
    Env env;
    EXPECT_FALSE(bad.test(*env.entry()));
    EXPECT_FALSE(empty.test(*env.entry()));
}

TEST(BasesExpr, PrecedenceMatchesJavaScript)
{
    Env env;
    EXPECT_EQ(env.eval(QStringLiteral("2 + 3 * 4 - 1")).toString(), QStringLiteral("13"));
    EXPECT_EQ(env.eval(QStringLiteral("1 < 2 && 3 > 4 || true")).toString(), QStringLiteral("true"));
    EXPECT_EQ(env.eval(QStringLiteral("!true == false")).toString(), QStringLiteral("true"));
    EXPECT_EQ(env.eval(QStringLiteral("-2.abs()")).toString(), QStringLiteral("-2"));
}

TEST(BasesExpr, FormulasCacheAndDetectLoops)
{
    Env env;
    env.base.setFormula(QStringLiteral("ppp"), QStringLiteral("(price / pages).toFixed(3)"));
    env.base.setFormula(QStringLiteral("twice"), QStringLiteral("formula.ppp + formula.ppp"));
    env.base.setFormula(QStringLiteral("a"), QStringLiteral("formula.b"));
    env.base.setFormula(QStringLiteral("b"), QStringLiteral("formula.a + 1"));
    env.base.setFormula(QStringLiteral("broken"), QStringLiteral("pages.foo()"));
    env.base.setFormula(QStringLiteral("usesBroken"), QStringLiteral("formula.broken + 1"));
    env.reset();
    EXPECT_EQ(env.eval(QStringLiteral("formula.ppp")).toString(), QStringLiteral("0.024"));
    EXPECT_EQ(env.eval(QStringLiteral("formula.PPP")).toString(), QStringLiteral("0.024"));
    EXPECT_EQ(env.eval(QStringLiteral("formula.twice")).toString(), QStringLiteral("0.0240.024"));
    const EntryPtr e = env.entry();
    EXPECT_EQ(e->value(QStringLiteral("formula.a")).toString(), QStringLiteral("Error: Infinite loop detected in formula"));
    EXPECT_EQ(e->value(QStringLiteral("formula.broken")).toString(), QStringLiteral("Error: Cannot find function \"foo\" on type Number"));
    // an error read through "formula.x" is thrown to the reader
    EXPECT_EQ(e->value(QStringLiteral("formula.usesBroken")).toString(), QStringLiteral("Error: Cannot find function \"foo\" on type Number"));
    EXPECT_TRUE(e->value(QStringLiteral("formula.missing")).isNull());
    EXPECT_EQ(env.eval(QStringLiteral("formula")).toString(),
              QStringLiteral("Formulas[\"ppp\",\"twice\",\"a\",\"b\",\"broken\",\"usesBroken\"]"));
}

TEST(BasesExpr, ThisFollowsTheContext)
{
    Env env;
    env.reset(QStringLiteral("Books/Hyperion.md"));
    EXPECT_EQ(env.eval(QStringLiteral("this.author")).toString(), QStringLiteral("Dan Simmons"));
    EXPECT_EQ(env.eval(QStringLiteral("file.hasLink(this.file)")).toString(), QStringLiteral("true"));
    EXPECT_EQ(env.eval(QStringLiteral("pages > this.pages")).toString(), QStringLiteral("false"));
    EXPECT_TRUE(env.ctx->localUsed());
    env.reset(QString());
    EXPECT_EQ(env.eval(QStringLiteral("this")).toString(), QStringLiteral("null"));
}

TEST(BasesExpr, PropertyExpressions)
{
    EXPECT_EQ(propertyExpression(QStringLiteral("note.status")), QStringLiteral("status"));
    EXPECT_EQ(propertyExpression(QStringLiteral("note.my prop")), QStringLiteral("note[\"my prop\"]"));
    EXPECT_EQ(propertyExpression(QStringLiteral("formula.total")), QStringLiteral("formula.total"));
    EXPECT_EQ(propertyExpression(QStringLiteral("file.file")), QStringLiteral("file"));
    EXPECT_EQ(propertyExpression(QStringLiteral("file.name")), QStringLiteral("file.name"));
}

TEST(BasesExpr, RegistersObsidiansFunctions)
{
    const QList<FunctionInfo> fns = allFunctions();
    EXPECT_EQ(fns.size(), 80);
    int globals = 0;
    for (const FunctionInfo& f : fns)
        globals += f.type.isEmpty();
    EXPECT_EQ(globals, 16);
}

} // namespace
