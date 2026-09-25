// Golden tests for the minimal frontmatter patches (core/properties/frontmatter_editor.h): bytes in,
// one or more property edits, bytes out. Every line the edit does not concern must keep its bytes and
// its line terminator (CRLF, BOM, comments, quoting, flow collections, ...).
#include "core/properties/frontmatter_editor.h"
#include "text/codec.h"

#include <gtest/gtest.h>

#include <functional>

using namespace oc;
using namespace oc::props;

namespace {

using Op = std::function<void(FrontmatterEditor&)>;

QByteArray edited(const QByteArray& in, const Op& op)
{
    const DecodedFile d = decodeFile(in);
    FrontmatterEditor ed(d.text);
    op(ed);
    const Text out = ed.apply(d.text.dominantEol());
    return encodeFile(out, d.bom);
}

QVariant list(std::initializer_list<const char*> items)
{
    QVariantList l;
    for (const char* s : items)
        l << QString::fromUtf8(s);
    return l;
}

QString s(const char* utf8) { return QString::fromUtf8(utf8); }

#define GOLDEN(in, op, out) EXPECT_EQ(edited(QByteArray(in), op).toStdString(), std::string(out))

} // namespace

// ---- values ----------------------------------------------------------------------------------------

TEST(PropertiesPatch, SetInlineScalar)
{
    GOLDEN("---\ntitle: Old\ncount: 3\n---\nBody\n", [](auto& e) { e.set("title", s("New")); },
           "---\ntitle: New\ncount: 3\n---\nBody\n");
}

TEST(PropertiesPatch, SetKeepsCommentsAndOtherKeys)
{
    GOLDEN("---\n# header comment\na: 'single'  # why\nb: \"double\"\n\n# about c\nc: plain\n---\n",
           [](auto& e) { e.set("b", s("changed")); },
           "---\n# header comment\na: 'single'  # why\nb: changed\n\n# about c\nc: plain\n---\n");
}

TEST(PropertiesPatch, SetKeepsInlineComment)
{
    GOLDEN("---\nstatus: draft # workflow\n---\n", [](auto& e) { e.set("status", s("done")); }, "---\nstatus: done # workflow\n---\n");
}

TEST(PropertiesPatch, SetKeepsAlignment)
{
    GOLDEN("---\ntitle:    Foo\ndate:     2024-01-01\n---\n", [](auto& e) { e.set("title", s("Bar baz")); },
           "---\ntitle:    Bar baz\ndate:     2024-01-01\n---\n");
}

TEST(PropertiesPatch, SetTabAfterColon)
{
    GOLDEN("---\na:\tx\nb: y\n---\n", [](auto& e) { e.set("a", s("z")); }, "---\na:\tz\nb: y\n---\n");
}

TEST(PropertiesPatch, SetQuotesWhenNeeded)
{
    GOLDEN("---\na: x\n---\n", [](auto& e) { e.set("a", s("#tag")); }, "---\na: \"#tag\"\n---\n");
    GOLDEN("---\na: x\n---\n", [](auto& e) { e.set("a", s("[[Note]]")); }, "---\na: \"[[Note]]\"\n---\n");
    GOLDEN("---\na: x\n---\n", [](auto& e) { e.set("a", s("123")); }, "---\na: \"123\"\n---\n");
    GOLDEN("---\na: x\n---\n", [](auto& e) { e.set("a", s("say \"hi\": now")); }, "---\na: 'say \"hi\": now'\n---\n");
    GOLDEN("---\na: x\n---\n", [](auto& e) { e.set("a", s("it's: here")); }, "---\na: \"it's: here\"\n---\n");
}

TEST(PropertiesPatch, SetTypedScalars)
{
    GOLDEN("---\nn: 1\nc: false\nd: 2024-01-01\n---\n",
           [](auto& e) {
               e.set("n", 2.5);
               e.set("c", true);
               e.set("d", s("2024-02-29"));
           },
           "---\nn: 2.5\nc: true\nd: 2024-02-29\n---\n");
    GOLDEN("---\nwhen: 2024-01-01T10:30\n---\n", [](auto& e) { e.set("when", s("2024-01-01T11:45:00")); },
           "---\nwhen: 2024-01-01T11:45:00\n---\n");
}

TEST(PropertiesPatch, SetNullAndBack)
{
    GOLDEN("---\na: x # note\nb: y\n---\n", [](auto& e) { e.set("a", QVariant()); }, "---\na: # note\nb: y\n---\n");
    GOLDEN("---\na:\nb: y\n---\n", [](auto& e) { e.set("a", s("x")); }, "---\na: x\nb: y\n---\n");
    GOLDEN("---\na: # note\n---\n", [](auto& e) { e.set("a", s("x")); }, "---\na: x # note\n---\n");
}

TEST(PropertiesPatch, SetSameValueChangesNothing)
{
    GOLDEN("---\na: 'x'\nb: [1, 2]\n---\n", [](auto& e) { e.set("a", s("x")); }, "---\na: x\nb: [1, 2]\n---\n");
    const QByteArray in("---\ntitle: Same\n---\n");
    const DecodedFile d = decodeFile(in);
    FrontmatterEditor ed(d.text);
    ed.set(QStringLiteral("title"), QStringLiteral("Same"));
    EXPECT_TRUE(ed.changes().isEmpty());
}

TEST(PropertiesPatch, UnicodeKeysAndValues)
{
    GOLDEN("---\nnaïve: café\n日本: 語\n---\n", [](auto& e) { e.set(s("日本"), s("東京 😀")); }, "---\nnaïve: café\n日本: 東京 😀\n---\n");
}

// ---- lists ---------------------------------------------------------------------------------------

TEST(PropertiesPatch, ScalarToList)
{
    GOLDEN("---\ntags: one\nx: 1\n---\n", [](auto& e) { e.set("tags", list({"one", "two"})); },
           "---\ntags:\n  - one\n  - two\nx: 1\n---\n");
}

TEST(PropertiesPatch, ListAddItemKeepsOtherLines)
{
    GOLDEN("---\ntags:\n  - a\n  - 'b'\nnext: 1\n---\n", [](auto& e) { e.set("tags", list({"a", "b", "c"})); },
           "---\ntags:\n  - a\n  - 'b'\n  - c\nnext: 1\n---\n");
}

TEST(PropertiesPatch, ListRemoveItem)
{
    GOLDEN("---\naliases:\n  - One\n  - \"Two\"\n  - Three\n---\n", [](auto& e) { e.set("aliases", list({"One", "Three"})); },
           "---\naliases:\n  - One\n  - Three\n---\n");
}

TEST(PropertiesPatch, ListInsertInTheMiddle)
{
    GOLDEN("---\nl:\n  - a\n  - c\n---\n", [](auto& e) { e.set("l", list({"a", "b", "c"})); }, "---\nl:\n  - a\n  - b\n  - c\n---\n");
}

TEST(PropertiesPatch, CompactListKeepsItsIndentation)
{
    GOLDEN("---\ntags:\n- a\n- b\n---\n", [](auto& e) { e.set("tags", list({"a", "b", "#c"})); }, "---\ntags:\n- a\n- b\n- \"#c\"\n---\n");
    GOLDEN("---\ntags:\n    - a\n---\n", [](auto& e) { e.set("tags", list({"x", "a"})); }, "---\ntags:\n    - x\n    - a\n---\n");
}

TEST(PropertiesPatch, ListWithCommentsBetweenItems)
{
    GOLDEN("---\nl:\n  - a\n  # keep me\n  - b\n---\n", [](auto& e) { e.set("l", list({"a", "b", "c"})); },
           "---\nl:\n  - a\n  # keep me\n  - b\n  - c\n---\n");
}

TEST(PropertiesPatch, ListToNullRemovesItems)
{
    GOLDEN("---\nl:\n  - a\n  - b\nz: 1\n---\n", [](auto& e) { e.set("l", QVariant()); }, "---\nl:\nz: 1\n---\n");
}

TEST(PropertiesPatch, ListToScalar)
{
    GOLDEN("---\nl:\n  - a\n  - b\n---\n", [](auto& e) { e.set("l", s("a")); }, "---\nl: a\n---\n");
}

TEST(PropertiesPatch, FlowSequenceBecomesBlockList)
{
    GOLDEN("---\ntags: [a, b]\nnext: x\n---\n", [](auto& e) { e.set("tags", list({"a", "b", "c"})); },
           "---\ntags:\n  - a\n  - b\n  - c\nnext: x\n---\n");
}

TEST(PropertiesPatch, FlowSequenceUntouchedWhenOtherKeyChanges)
{
    GOLDEN("---\ntags: [a, \"b\", {x: 1}]\nother: 1\n---\n", [](auto& e) { e.set("other", 2); },
           "---\ntags: [a, \"b\", {x: 1}]\nother: 2\n---\n");
}

TEST(PropertiesPatch, MultiLineFlowSequence)
{
    GOLDEN("---\ntags: [\n  a,\n  b\n]\nz: 0\n---\n", [](auto& e) { e.set("tags", list({"b"})); }, "---\ntags:\n  - b\nz: 0\n---\n");
}

TEST(PropertiesPatch, ListItemsNeedingQuotes)
{
    GOLDEN("---\nrelated:\n---\n", [](auto& e) { e.set("related", list({"[[A]]", "plain", "#t", "a: b"})); },
           "---\nrelated:\n  - \"[[A]]\"\n  - plain\n  - \"#t\"\n  - \"a: b\"\n---\n");
}

// ---- multi-line strings --------------------------------------------------------------------------

TEST(PropertiesPatch, BlockScalarReplaced)
{
    GOLDEN("---\ndesc: |\n  line one\n\n  line two\nnext: 1\n---\n", [](auto& e) { e.set("desc", s("single")); },
           "---\ndesc: single\nnext: 1\n---\n");
}

TEST(PropertiesPatch, MultiLineValueBecomesBlockLiteral)
{
    GOLDEN("---\ndesc: x\n---\n", [](auto& e) { e.set("desc", s("first\nsecond")); }, "---\ndesc: |-\n  first\n  second\n---\n");
}

TEST(PropertiesPatch, UntouchedBlockScalarsStayByteExact)
{
    GOLDEN("---\na: >-\n  folded\n   more   \nb: |+\n  kept\n\nc: 1\n---\n", [](auto& e) { e.set("c", 2); },
           "---\na: >-\n  folded\n   more   \nb: |+\n  kept\n\nc: 2\n---\n");
}

TEST(PropertiesPatch, MultiLinePlainScalar)
{
    GOLDEN("---\ntitle: a long\n  folded title\nnext: 1\n---\n", [](auto& e) { e.set("title", s("short")); },
           "---\ntitle: short\nnext: 1\n---\n");
}

TEST(PropertiesPatch, MultiLineQuotedScalar)
{
    GOLDEN("---\nq: \"first\n  second\"\nz: 1\n---\n", [](auto& e) { e.set("q", s("x")); }, "---\nq: x\nz: 1\n---\n");
}

// ---- nested maps ------------------------------------------------------------------------------------

TEST(PropertiesPatch, NestedMapUntouched)
{
    GOLDEN("---\nmeta:\n  author: Me   # me!\n  tags: [x]\ntitle: T\n---\n", [](auto& e) { e.set("title", s("U")); },
           "---\nmeta:\n  author: Me   # me!\n  tags: [x]\ntitle: U\n---\n");
}

TEST(PropertiesPatch, NestedMapRewrittenKeepsIndent)
{
    GOLDEN("---\nmeta:\n    a: 1\n---\n", [](auto& e) { e.set("meta", QVariantMap{{"a", 1}, {"b", s("x")}}); }, "---\nmeta:\n    a: 1\n    b: x\n---\n");
}

// ---- adding ---------------------------------------------------------------------------------------

TEST(PropertiesPatch, AddAppendsAtTheEnd)
{
    GOLDEN("---\na: 1\n---\nBody\n", [](auto& e) { e.add("b", s("two")); }, "---\na: 1\nb: two\n---\nBody\n");
}

TEST(PropertiesPatch, AddBeforeTrailingBlankLines)
{
    GOLDEN("---\na: 1\n\n\n---\n", [](auto& e) { e.add("b", QVariant()); }, "---\na: 1\nb:\n\n\n---\n");
}

TEST(PropertiesPatch, AddAfterTrailingComment)
{
    GOLDEN("---\na: 1\n# the end\n---\n", [](auto& e) { e.add("b", 2); }, "---\na: 1\n# the end\nb: 2\n---\n");
}

TEST(PropertiesPatch, AddToEmptyFrontmatter)
{
    GOLDEN("---\n---\ntext\n", [](auto& e) { e.add("k", QVariant()); }, "---\nk:\n---\ntext\n");
    GOLDEN("---\n# only a comment\n---\n", [](auto& e) { e.add("k", s("v")); }, "---\n# only a comment\nk: v\n---\n");
}

TEST(PropertiesPatch, AddCreatesFrontmatter)
{
    GOLDEN("# Heading\n\nText\n", [](auto& e) { e.add("tags", list({"x"})); }, "---\ntags:\n  - x\n---\n# Heading\n\nText\n");
    GOLDEN("", [](auto& e) { e.add("key", QVariant()); }, "---\nkey:\n---\n");
    GOLDEN("no newline", [](auto& e) { e.add("a", true); }, "---\na: true\n---\nno newline");
}

TEST(PropertiesPatch, AddRefusesExistingKey)
{
    GOLDEN("---\na: 1\n---\n", [](auto& e) { EXPECT_FALSE(e.add("a", 2)); }, "---\na: 1\n---\n");
}

TEST(PropertiesPatch, AddQuotedKey)
{
    GOLDEN("---\na: 1\n---\n", [](auto& e) { e.add("#weird key", 1); }, "---\na: 1\n\"#weird key\": 1\n---\n");
}

// ---- renaming -------------------------------------------------------------------------------------

TEST(PropertiesPatch, RenameKeepsValueText)
{
    GOLDEN("---\nold:   'quoted value'   # c\nz: 1\n---\n", [](auto& e) { EXPECT_TRUE(e.rename("old", "new")); },
           "---\nnew:   'quoted value'   # c\nz: 1\n---\n");
}

TEST(PropertiesPatch, RenameBlockValue)
{
    GOLDEN("---\nl:\n  - a\n  - b\n---\n", [](auto& e) { e.rename("l", "list"); }, "---\nlist:\n  - a\n  - b\n---\n");
}

TEST(PropertiesPatch, RenameQuotedKeys)
{
    GOLDEN("---\n\"my key\": 1\n---\n", [](auto& e) { e.rename("my key", "#other"); }, "---\n\"#other\": 1\n---\n");
    GOLDEN("---\n'it''s': 1\n---\n", [](auto& e) { e.rename("it's", "plain"); }, "---\nplain: 1\n---\n");
}

TEST(PropertiesPatch, RenameKeyWithSpaceBeforeColon)
{
    GOLDEN("---\nkey : value\n---\n", [](auto& e) { e.rename("key", "k2"); }, "---\nk2 : value\n---\n");
}

TEST(PropertiesPatch, RenameRefusesDuplicate)
{
    GOLDEN("---\na: 1\nb: 2\n---\n", [](auto& e) { EXPECT_FALSE(e.rename("a", "b")); }, "---\na: 1\nb: 2\n---\n");
}

// ---- removing -------------------------------------------------------------------------------------

TEST(PropertiesPatch, RemoveTakesItsLeadingComment)
{
    GOLDEN("---\na: 1\n# about b\nb:\n  - x\n\nc: 3\n---\n", [](auto& e) { e.remove("b"); }, "---\na: 1\n\nc: 3\n---\n");
}

TEST(PropertiesPatch, RemoveKeepsDetachedComment)
{
    GOLDEN("---\na: 1\n# detached\n\nb: 2\n---\n", [](auto& e) { e.remove("b"); }, "---\na: 1\n# detached\n\n---\n");
}

TEST(PropertiesPatch, RemoveLastPropertyRemovesTheBlock)
{
    GOLDEN("---\nonly: 1\n---\nBody\n", [](auto& e) { e.remove("only"); }, "Body\n");
    GOLDEN("---\n# c\nonly: 1\n---", [](auto& e) { e.remove("only"); }, "");
}

TEST(PropertiesPatch, ClearRemovesTheBlock)
{
    GOLDEN("---\na: 1\nb: 2\n---\n\nBody\n", [](auto& e) { e.clear(); }, "\nBody\n");
    GOLDEN("No frontmatter\n", [](auto& e) { e.clear(); }, "No frontmatter\n");
}

// ---- ordering and merging -------------------------------------------------------------------------

TEST(PropertiesPatch, ReorderMovesWholeEntries)
{
    GOLDEN("---\n# about b\nb: 2 # two\n\na:\n  - x\nc: 3\n---\n", [](auto& e) { EXPECT_TRUE(e.reorder({"a", "b", "c"})); },
           "---\na:\n  - x\n\n# about b\nb: 2 # two\nc: 3\n---\n");
}

TEST(PropertiesPatch, MergeLists)
{
    GOLDEN("---\ntags:\n  - a\n  - b\n---\n", [](auto& e) { e.merge("tags", list({"b", "c"})); }, "---\ntags:\n  - a\n  - b\n  - c\n---\n");
    GOLDEN("---\ntitle:\n---\n", [](auto& e) { e.merge("title", s("x")); }, "---\ntitle: x\n---\n");
    GOLDEN("---\ntitle: keep\n---\n", [](auto& e) { e.merge("title", QVariant()); }, "---\ntitle: keep\n---\n");
    GOLDEN("---\na: 1\n---\n", [](auto& e) { e.merge("b", s("new")); }, "---\na: 1\nb: new\n---\n");
}

// ---- line terminators, BOM, odd files -------------------------------------------------------------

TEST(PropertiesPatch, CrlfFile)
{
    GOLDEN("---\r\ntitle: Old\r\ntags:\r\n  - a\r\n---\r\nBody\r\n",
           [](auto& e) {
               e.set("title", s("New"));
               e.set("tags", list({"a", "b"}));
               e.add("extra", 1);
           },
           "---\r\ntitle: New\r\ntags:\r\n  - a\r\n  - b\r\nextra: 1\r\n---\r\nBody\r\n");
}

TEST(PropertiesPatch, CrlfCreateAndRemove)
{
    GOLDEN("Line 1\r\nLine 2\r\n", [](auto& e) { e.add("k", s("v")); }, "---\r\nk: v\r\n---\r\nLine 1\r\nLine 2\r\n");
    GOLDEN("---\r\na: 1\r\nb: 2\r\n---\r\nx\r\n", [](auto& e) { e.remove("a"); }, "---\r\nb: 2\r\n---\r\nx\r\n");
}

TEST(PropertiesPatch, BomKept)
{
    GOLDEN("\xEF\xBB\xBF---\ntitle: a\n---\n", [](auto& e) { e.set("title", s("b")); }, "\xEF\xBB\xBF---\ntitle: b\n---\n");
    GOLDEN("\xEF\xBB\xBF# Note\n", [](auto& e) { e.add("k", 1); }, "\xEF\xBB\xBF---\nk: 1\n---\n# Note\n");
}

TEST(PropertiesPatch, MixedLineEndingsUntouchedLinesKeepThem)
{
    GOLDEN("---\r\na: 1\nb: 2\r\nc: 3\n---\n", [](auto& e) { e.set("b", 20); }, "---\r\na: 1\nb: 20\r\nc: 3\n---\n");
}

TEST(PropertiesPatch, InvalidUtf8ElsewhereIsKept)
{
    GOLDEN("---\na: 1\n---\nbad \xFF\xFE bytes\n", [](auto& e) { e.set("a", 2); }, "---\na: 2\n---\nbad \xFF\xFE bytes\n");
}

TEST(PropertiesPatch, DelimiterWithTrailingBlanksAndNoFinalNewline)
{
    GOLDEN("---  \na: 1\n---\t", [](auto& e) { e.set("a", 2); }, "---  \na: 2\n---\t");
}

TEST(PropertiesPatch, TrailingSpacesElsewhereKept)
{
    GOLDEN("---\na: 1   \nb: 2\t\n---\n", [](auto& e) { e.set("b", 3); }, "---\na: 1   \nb: 3\n---\n");
}

// ---- files Obsidian can't / we can't map -----------------------------------------------------------

TEST(PropertiesPatch, InvalidYamlIsNotEditable)
{
    const QByteArray in("---\na: [unclosed\nb: 1\n---\n");
    const DecodedFile d = decodeFile(in);
    FrontmatterEditor ed(d.text);
    EXPECT_EQ(ed.state(), FrontmatterEditor::State::Invalid);
    EXPECT_FALSE(ed.set(QStringLiteral("b"), 2));
    EXPECT_FALSE(ed.add(QStringLiteral("c"), 2));
    EXPECT_TRUE(ed.changes().isEmpty());
    EXPECT_FALSE(ed.error().isEmpty());
}

TEST(PropertiesPatch, DuplicateKeysAndNonMappingRootsAreInvalid)
{
    for (const char* in : {"---\na: 1\na: 2\n---\n", "---\n- a\n- b\n---\n", "---\njust text\n---\n"}) {
        FrontmatterEditor ed(decodeFile(QByteArray(in)).text);
        EXPECT_EQ(ed.state(), FrontmatterEditor::State::Invalid) << in;
    }
    FrontmatterEditor empty(decodeFile(QByteArray("---\n# nothing\n---\n")).text);
    EXPECT_EQ(empty.state(), FrontmatterEditor::State::Valid);
    EXPECT_TRUE(empty.properties().isEmpty());
}

TEST(PropertiesPatch, AnchorsAreRewrittenLikeObsidian)
{
    // Aliases can't be edited line by line: the block is serialized again (resolved), like Obsidian does.
    const QByteArray in("---\nbase: &b hello\ncopy: *b\n---\nBody\n");
    FrontmatterEditor probe(decodeFile(in).text);
    EXPECT_EQ(probe.state(), FrontmatterEditor::State::Valid);
    EXPECT_FALSE(probe.isMappable());
    GOLDEN(in, [](auto& e) { e.set("copy", s("changed")); }, "---\nbase: hello\ncopy: changed\n---\nBody\n");
}

TEST(PropertiesPatch, TopLevelFlowMapIsRewritten)
{
    GOLDEN("---\n{a: 1, b: x}\n---\n", [](auto& e) { e.set("b", s("y")); }, "---\na: 1\nb: y\n---\n");
}

TEST(PropertiesPatch, UnmappedButUneditedStaysByteExact)
{
    const QByteArray in("---\n{a: 1}\n---\n");
    FrontmatterEditor ed(decodeFile(in).text);
    EXPECT_EQ(ed.properties().size(), 1);
    EXPECT_TRUE(ed.changes().isEmpty());
}

TEST(PropertiesPatch, ParsedValuesAndSpan)
{
    FrontmatterEditor ed(decodeFile(QByteArray("---\ntitle: T\ncount: 2\ndone: true\ntags: [a, b]\nnothing:\n---\n")).text);
    ASSERT_EQ(ed.state(), FrontmatterEditor::State::Valid);
    const QList<Property> p = ed.properties();
    ASSERT_EQ(p.size(), 5);
    EXPECT_EQ(p[0].value.toString(), "T");
    EXPECT_EQ(p[1].value.toDouble(), 2.0);
    EXPECT_EQ(p[2].value.toBool(), true);
    EXPECT_EQ(p[3].value.toList().size(), 2);
    EXPECT_FALSE(p[4].value.isValid());
    EXPECT_EQ(ed.span().closeLine, 6);
}

TEST(PropertiesPatch, SequentialEditsCompose)
{
    GOLDEN("---\nb: 1\na: 2\n---\n",
           [](auto& e) {
               e.rename("b", "c");
               e.set("c", list({"x"}));
               e.add("d", 4);
               e.remove("a");
               e.reorder({"d", "c"});
           },
           "---\nd: 4\nc:\n  - x\n---\n");
}
