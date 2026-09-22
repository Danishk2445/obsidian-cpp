// Global search query language: parser structure, error recovery, JS regex translation and "Explain search terms".
#include "core/search/search_query.h"

#include <gtest/gtest.h>

#include <vector>

using namespace oc::search;

namespace {

QString tree(const QString& q, const SearchOptions& options = {})
{
    return parseSearchQuery(q, options).toDebugString();
}

QString explain(const QString& q, const SearchOptions& options = {})
{
    return explainSearchQuery(parseSearchQuery(q, options));
}

QString parseError(const QString& q)
{
    QString error;
    parseSearchQuery(q, &error);
    return error;
}

// First leaf in preorder.
const Node* firstLeaf(const NodePtr& n)
{
    if (!n)
        return nullptr;
    if (n->isLeaf())
        return n.get();
    for (const NodePtr& c : n->children)
        if (const Node* l = firstLeaf(c))
            return l;
    return nullptr;
}

} // namespace

TEST(SearchQueryParse, WordsAreAndedAndOrBindsLooser)
{
    EXPECT_EQ(tree("meeting"), R"("meeting")");
    EXPECT_EQ(tree("meeting work"), R"((and "meeting" "work"))");
    EXPECT_EQ(tree("meeting OR work"), R"((or "meeting" "work"))");
    EXPECT_EQ(tree("meeting work OR meetup personal"), R"((or (and "meeting" "work") (and "meetup" "personal")))");
    EXPECT_EQ(tree("meeting (work OR meetup) personal"), R"((and "meeting" (or "work" "meetup") "personal"))");
    EXPECT_EQ(tree("a or b"), R"((and "a" "or" "b"))"); // only uppercase OR is an operator
    EXPECT_EQ(tree("a ORb"), R"((and "a" "ORb"))");
    EXPECT_EQ(tree("OR a"), R"("a")");
    EXPECT_EQ(tree("a OR"), R"("a")");
    EXPECT_EQ(tree("a OR OR b"), R"((or "a" "b"))");
    EXPECT_EQ(tree("a OR(b c)"), R"((or "a" (and "b" "c")))");
    EXPECT_EQ(tree("(a b)OR c"), R"((or (and "a" "b") "c"))");
    EXPECT_EQ(tree("  \t a \n b  "), R"((and "a" "b"))");
}

TEST(SearchQueryParse, Negation)
{
    EXPECT_EQ(tree("meeting -work"), R"((and "meeting" (not "work")))");
    EXPECT_EQ(tree("meeting -work -meetup"), R"((and "meeting" (not "work") (not "meetup")))");
    EXPECT_EQ(tree("meeting -(work meetup)"), R"((and "meeting" (not (and "work" "meetup"))))");
    EXPECT_EQ(tree("-\"a b\""), R"((not (phrase "a b")))");
    EXPECT_EQ(tree("-/x/"), R"((not /x/))");
    EXPECT_EQ(tree("--x"), R"((not (not "x")))");
    EXPECT_EQ(tree("well-known"), R"("well-known")");
    EXPECT_EQ(tree("a - b"), R"((and "a" "-" "b"))"); // a lone hyphen is text
    EXPECT_EQ(tree("-"), R"("-")");
}

TEST(SearchQueryParse, GroupsAndParenthesesInWords)
{
    EXPECT_EQ(tree("((a))"), R"("a")");
    EXPECT_EQ(tree("(a (b OR (c d)))"), R"((and "a" (or "b" (and "c" "d"))))");
    EXPECT_EQ(tree("f(x)"), R"~("f(x)")~");
    EXPECT_EQ(tree("(f(x) y)"), R"~((and "f(x)" "y"))~");
    EXPECT_EQ(tree("a)"), R"~("a)")~"); // a stray ')' at the top level is text
    EXPECT_EQ(tree(")"), R"~(")")~");
    EXPECT_EQ(tree("()"), "(nothing)");
    EXPECT_EQ(tree("(a b"), R"((and "a" "b"))");
    EXPECT_EQ(parseError("(a b"), "Missing closing parenthesis");
    EXPECT_TRUE(parseError("(a) (b)").isEmpty());
}

TEST(SearchQueryParse, PhrasesAndEscapes)
{
    EXPECT_EQ(tree("\"star wars\""), R"((phrase "star wars"))");
    const SearchQuery q = parseSearchQuery(R"("they said \"hello\" to each other")");
    ASSERT_TRUE(q.root());
    EXPECT_EQ(q.root()->kind, NodeKind::Phrase);
    EXPECT_EQ(q.root()->text, R"(they said "hello" to each other)");
    EXPECT_EQ(parseSearchQuery(R"("a\\b")").root()->text, R"(a\b)");
    EXPECT_EQ(parseSearchQuery(R"("a\nb")").root()->text, R"(a\nb)"); // other backslashes are literal
    EXPECT_EQ(tree(R"(x "a (b) OR c" y)"), R"((and "x" (phrase "a (b) OR c") "y"))");
    EXPECT_EQ(tree("\"unterminated phrase"), R"((phrase "unterminated phrase"))");
    EXPECT_EQ(parseError("\"unterminated"), "Missing closing quote");
    EXPECT_EQ(tree("\"\""), R"((phrase ""))");
}

TEST(SearchQueryParse, RegularExpressions)
{
    EXPECT_EQ(tree(R"(/\d{4}-\d{2}-\d{2}/)"), R"(/\d{4}-\d{2}-\d{2}/)");
    EXPECT_EQ(tree(R"(path:/\d{4}/)"), R"((path /\d{4}/))");
    EXPECT_EQ(tree("/a b/ c"), R"((and /a b/ "c"))");
    EXPECT_EQ(tree(R"(/a\/b/)"), R"(/a\/b/)");
    EXPECT_EQ(tree("/[/]x/"), R"(/[/]x/)"); // a slash inside a class does not close
    EXPECT_EQ(tree("(/x/)"), R"(/x/)");
    // Not a regex: no closing slash that ends the term.
    EXPECT_EQ(tree("/usr/bin"), R"("/usr/bin")");
    EXPECT_EQ(tree("//"), R"("//")");
    EXPECT_EQ(tree("/abc/i"), R"("/abc/i")");
    EXPECT_EQ(tree("a/b/"), R"("a/b/")");
    // Invalid patterns parse (and never match) but are reported.
    EXPECT_EQ(tree("/(/"), "/(/");
    EXPECT_TRUE(parseError("/(/").startsWith("Invalid regular expression /(/"));
    EXPECT_FALSE(parseSearchQuery("/(/").root()->regex.isValid());
    // ^ and $ work per line; case follows the options.
    const SearchQuery q = parseSearchQuery("/^b$/");
    EXPECT_TRUE(q.root()->regex.match("a\nB\nc").hasMatch());
    const SearchQuery sensitive = parseSearchQuery("match-case:/^b$/");
    ASSERT_TRUE(firstLeaf(sensitive.root()));
    EXPECT_FALSE(firstLeaf(sensitive.root())->regex.match("a\nB\nc").hasMatch());
    EXPECT_TRUE(firstLeaf(sensitive.root())->regex.match("a\nb\nc").hasMatch());
}

TEST(SearchQueryParse, JavaScriptRegexTranslation)
{
    // Backslash-u sequences are written as escaped plain literals.
    EXPECT_EQ(jsRegexToPcre(u"\\u0041"), "\\x{0041}");
    EXPECT_EQ(jsRegexToPcre(u"\\u{41}"), "u{41}"); // no u flag: 'u' repeated 41 times
    EXPECT_EQ(jsRegexToPcre(u"\\uD83D\\uDE00"), "\\x{1f600}"); // a UTF-16 pair is one code point
    EXPECT_EQ(jsRegexToPcre(uR"(\u12)"), "u12");
    EXPECT_EQ(jsRegexToPcre(uR"(\u)"), "u");
    EXPECT_EQ(jsRegexToPcre(uR"(\x41\xg)"), R"(\x41xg)");
    EXPECT_EQ(jsRegexToPcre(uR"(\v)"), R"(\x{0B})");
    EXPECT_EQ(jsRegexToPcre(u"a[^]b"), R"(a[\s\S]b)");
    EXPECT_EQ(jsRegexToPcre(u"a[]b"), "a(?!)b");
    EXPECT_EQ(jsRegexToPcre(u"[[]"), R"([\[])");
    EXPECT_EQ(jsRegexToPcre(u"[[:alpha:]]"), R"([\[:alpha:]])");
    EXPECT_EQ(jsRegexToPcre(u"[^]]"), R"([\s\S]])");
    EXPECT_EQ(jsRegexToPcre(uR"(\A\Z\z\Q\E\K\R\X\h\e\a\g\N\o)"), "AZzQEKRXheagNo");
    EXPECT_EQ(jsRegexToPcre(uR"(\d\D\w\W\s\S\b\B\f\n\r\t\1\0\.\/)"), R"(\d\D\w\W\s\S\b\B\f\n\r\t\1\0\.\/)");
    EXPECT_EQ(jsRegexToPcre(uR"((?<y>a)\k<y>)"), R"((?<y>a)\k<y>)");
    EXPECT_EQ(jsRegexToPcre(uR"(\k)"), "k");
    EXPECT_EQ(jsRegexToPcre(uR"(\p{L}\p)"), R"(\p{L}p)");
    EXPECT_EQ(jsRegexToPcre(uR"([\b\B])"), R"([\bB])");
    EXPECT_EQ(jsRegexToPcre(uR"(\cJ\c1)"), R"(\cJ\\c1)");

    const auto matches = [](const QString& js, const QString& subject) {
        const QRegularExpression re(jsRegexToPcre(js));
        EXPECT_TRUE(re.isValid()) << js.toStdString() << ": " << re.errorString().toStdString();
        return re.match(subject).hasMatch();
    };
    EXPECT_TRUE(matches("\\u0041", "xAx"));
    EXPECT_FALSE(matches("\\u0041", "u0041"));
    EXPECT_TRUE(matches("x\\uD83D\\uDE00", QString::fromUtf8("x\xF0\x9F\x98\x80")));
    EXPECT_TRUE(matches(R"(\Qx)", "Qx"));
    EXPECT_TRUE(matches("a[^]b", "a\nb"));
    EXPECT_FALSE(matches("x[]", "x"));
    EXPECT_TRUE(matches("^\\u{2}$", "uu"));
    EXPECT_TRUE(matches(R"([[]x)", "[x"));
    EXPECT_TRUE(matches(R"(\v)", QString(QChar(0x0B))));
    EXPECT_FALSE(matches(R"(\v)", "\n"));
}

TEST(SearchQueryParse, Operators)
{
    EXPECT_EQ(tree("file:.jpg"), R"((file ".jpg"))");
    EXPECT_EQ(tree("file:202209"), R"((file "202209"))");
    EXPECT_EQ(tree("path:\"Daily notes/2022-07\""), R"((path (phrase "Daily notes/2022-07")))");
    EXPECT_EQ(tree("content:\"happy cat\""), R"((content (phrase "happy cat")))");
    EXPECT_EQ(tree("match-case:HappyCat"), R"((match-case "HappyCat"))");
    EXPECT_EQ(tree("ignore-case:ikea"), R"((ignore-case "ikea"))");
    EXPECT_EQ(tree("tag:#work"), R"((tag "#work"))");
    EXPECT_EQ(tree("line:(mix flour)"), R"((line (and "mix" "flour")))");
    EXPECT_EQ(tree("block:(dog cat)"), R"((block (and "dog" "cat")))");
    EXPECT_EQ(tree("section:(dog cat)"), R"((section (and "dog" "cat")))");
    EXPECT_EQ(tree("task:call"), R"((task "call"))");
    EXPECT_EQ(tree("task-todo:call"), R"((task-todo "call"))");
    EXPECT_EQ(tree("task-done:call"), R"((task-done "call"))");
    EXPECT_EQ(tree("task:(call OR email)"), R"((task (or "call" "email")))");
    EXPECT_EQ(tree("-file:draft"), R"((not (file "draft")))");
    EXPECT_EQ(tree("file:-draft"), R"((file (not "draft")))");
    EXPECT_EQ(tree("match-case:file:Foo"), R"((match-case (file "Foo")))");
    EXPECT_EQ(tree("section:(line:(a b) c)"), R"((section (and (line (and "a" "b")) "c")))");
    // An empty argument matches everything in the operator's scope.
    EXPECT_EQ(tree("task-todo:"), "(task-todo (empty))");
    EXPECT_EQ(tree("file: foo"), R"((and (file (empty)) "foo"))");
    EXPECT_EQ(tree("(file:)"), "(file (empty))");
    // Not operators.
    EXPECT_EQ(tree("foo:bar"), R"("foo:bar")");
    EXPECT_EQ(tree("File:x"), R"("File:x")");
    EXPECT_EQ(tree("filename:x"), R"("filename:x")");
    EXPECT_EQ(tree("tasks:x"), R"("tasks:x")");
    EXPECT_EQ(tree("x file:y"), R"((and "x" (file "y")))");
}

TEST(SearchQueryParse, Properties)
{
    EXPECT_EQ(tree("[aliases]"), R"((prop "aliases"))");
    EXPECT_EQ(tree("[aliases:Name]"), R"((prop "aliases" "Name"))");
    EXPECT_EQ(tree("[status:Draft OR Published]"), R"((prop "status" (or "Draft" "Published")))");
    EXPECT_EQ(tree("[status:(Draft OR Published) -old]"), R"((prop "status" (and (or "Draft" "Published") (not "old"))))");
    EXPECT_EQ(tree("meeting [duration:<5]"), R"((and "meeting" (prop "duration" (< 5))))");
    EXPECT_EQ(tree("[duration:>5]"), R"((prop "duration" (> 5)))");
    EXPECT_EQ(tree("[duration:\"<5\"]"), R"((prop "duration" (< 5)))");
    EXPECT_EQ(tree("[date:>2024-01-01]"), R"((prop "date" (> 2024-01-01)))");
    EXPECT_EQ(tree("[aliases:null]"), R"((prop "aliases" null))");
    EXPECT_EQ(tree("[aliases:EMPTY]"), R"((prop "aliases" null))");
    EXPECT_EQ(tree("[done:true] [done:FALSE]"), R"((and (prop "done" true) (prop "done" false)))");
    EXPECT_EQ(tree("[\"my key\":\"a b\"]"), R"((prop (phrase "my key") (phrase "a b")))");
    EXPECT_EQ(tree("[file:x]"), R"((prop "file" "x"))"); // operators are literal inside brackets
    EXPECT_EQ(tree("[time:10:30]"), R"((prop "time" "10:30"))");
    EXPECT_EQ(tree("[/^dur/:<5]"), R"((prop /^dur/ (< 5)))");
    EXPECT_EQ(tree("[:x]"), R"((prop (empty) "x"))");
    EXPECT_EQ(tree("[x:]"), R"((prop "x" (empty)))");
    EXPECT_EQ(tree("-[status:draft]"), R"((not (prop "status" "draft")))");
    EXPECT_EQ(tree("[a OR b]"), R"((prop (or "a" "b")))");
    EXPECT_EQ(tree("[status:draft"), R"((prop "status" "draft"))");
    EXPECT_EQ(parseError("[status:draft"), "Missing closing bracket");
    // "[[" is a wikilink search, not a property.
    EXPECT_EQ(tree("[[Note]]"), R"("[[Note]]")");
    // Comparison operands.
    const SearchQuery q = parseSearchQuery("[duration:<5.5]");
    const Node* cmp = q.root()->children.value(1).get();
    ASSERT_TRUE(cmp);
    EXPECT_EQ(cmp->kind, NodeKind::Compare);
    EXPECT_EQ(cmp->compareOp, '<');
    ASSERT_TRUE(cmp->number.has_value());
    EXPECT_DOUBLE_EQ(*cmp->number, 5.5);
    EXPECT_FALSE(parseSearchQuery("[date:>2024-01-01]").root()->children.value(1)->number.has_value());
}

TEST(SearchQueryParse, CaseSensitivityIsResolvedPerLeaf)
{
    // Leaf of a parsed query; the query is kept alive by the caller-provided storage.
    std::vector<SearchQuery> keep;
    const auto leaf = [&](const QString& q, const SearchOptions& options = {}) {
        keep.push_back(parseSearchQuery(q, options));
        return firstLeaf(keep.back().root());
    };
    const Node* l = leaf("match-case:HappyCat");
    ASSERT_TRUE(l);
    EXPECT_TRUE(l->caseSensitive);
    EXPECT_TRUE(l->caseExplicit);
    l = leaf("HappyCat");
    EXPECT_FALSE(l->caseSensitive);
    EXPECT_FALSE(l->caseExplicit);
    // The view's "Match case" toggle is the default; ignore-case: overrides it.
    const SearchOptions matchCase{true};
    EXPECT_TRUE(leaf("HappyCat", matchCase)->caseSensitive);
    EXPECT_FALSE(leaf("ignore-case:ikea", matchCase)->caseSensitive);
    // Tags ignore the toggle; only an explicit match-case: applies.
    l = leaf("tag:#Work/Sub", matchCase);
    EXPECT_FALSE(l->caseSensitive);
    EXPECT_EQ(l->tagName, "work/sub");
    l = leaf("match-case:tag:#Work");
    EXPECT_TRUE(l->caseSensitive);
    EXPECT_EQ(l->tagName, "Work");
    EXPECT_EQ(leaf("tag:work")->tagName, "work");
    // Scopes are fixed at parse time; the innermost operator wins.
    EXPECT_EQ(leaf("file:(path:x)")->scope, Scope::Path);
    EXPECT_EQ(leaf("line:x")->scope, Scope::Content);
    EXPECT_EQ(leaf("line:(tag:#x)")->scope, Scope::Tag);
    EXPECT_EQ(leaf("[k:v]")->scope, Scope::PropertyKey);
    EXPECT_EQ(leaf("x")->scope, Scope::Default);
}

TEST(SearchQueryParse, EmptyQueriesAndNonNoteFiles)
{
    EXPECT_TRUE(parseSearchQuery("").isEmpty());
    EXPECT_TRUE(parseSearchQuery("   ").isEmpty());
    EXPECT_TRUE(parseSearchQuery("OR").isEmpty());
    EXPECT_EQ(tree(""), "(nothing)");
    EXPECT_FALSE(parseSearchQuery("x").isEmpty());
    EXPECT_EQ(parseSearchQuery("a b").source(), "a b");

    EXPECT_TRUE(parseSearchQuery("file:.png").canMatchNonNotes());
    EXPECT_TRUE(parseSearchQuery("path:img OR cat").canMatchNonNotes());
    EXPECT_TRUE(parseSearchQuery("-(-file:x)").canMatchNonNotes());
    EXPECT_FALSE(parseSearchQuery("-file:.png").canMatchNonNotes());
    EXPECT_FALSE(parseSearchQuery("[file:x]").canMatchNonNotes());
    EXPECT_FALSE(parseSearchQuery("cat content:x").canMatchNonNotes());
}

TEST(SearchQueryExplain, MatchesObsidianWording)
{
    EXPECT_EQ(explain(""), "");
    EXPECT_EQ(explain("meeting"), R"(Matches text: "meeting")");
    EXPECT_EQ(explain("meeting -file:draft"),
              "Match all of:\n"
              "    Matches text: \"meeting\"\n"
              "    Excluding: Match file name: Matches text: \"draft\"");
    EXPECT_EQ(explain("meeting work OR meetup personal"),
              "Match any of:\n"
              "    Match all of:\n"
              "        Matches text: \"meeting\"\n"
              "        Matches text: \"work\"\n"
              "    Match all of:\n"
              "        Matches text: \"meetup\"\n"
              "        Matches text: \"personal\"");
    EXPECT_EQ(explain("meeting -(work meetup)"),
              "Match all of:\n"
              "    Matches text: \"meeting\"\n"
              "    Excluding:\n"
              "        Match all of:\n"
              "            Matches text: \"work\"\n"
              "            Matches text: \"meetup\"");
    EXPECT_EQ(explain("\"star wars\""), R"(Contains exact text: "star wars")");
    EXPECT_EQ(explain(R"("say \"hi\"")"), R"(Contains exact text: "say \"hi\"")");
    EXPECT_EQ(explain(R"(/\d{4}/)"), R"(Matches regex: /\d{4}/)");
    EXPECT_EQ(explain("match-case:HappyCat"), R"(Matches text: "HappyCat" (Case sensitive))");
    EXPECT_EQ(explain("ignore-case:ikea"), R"(Matches text: "ikea" (Case insensitive))");
    EXPECT_EQ(explain("HappyCat", SearchOptions{true}), R"(Matches text: "HappyCat" (Case sensitive))");
    EXPECT_EQ(explain("path:\"Daily notes/2022-07\""), R"(Match file path: Contains exact text: "Daily notes/2022-07")");
    EXPECT_EQ(explain("content:x"), R"(Match file content: Matches text: "x")");
    EXPECT_EQ(explain("tag:#work"), R"(Match tag: Matches text: "#work")");
    EXPECT_EQ(explain("line:(mix flour)"),
              "Match line:\n"
              "    Match all of:\n"
              "        Matches text: \"mix\"\n"
              "        Matches text: \"flour\"");
    EXPECT_EQ(explain("block:x"), R"(Match block: Matches text: "x")");
    EXPECT_EQ(explain("section:x"), R"(Match section: Matches text: "x")");
    EXPECT_EQ(explain("task:x"), R"(Match task: Matches text: "x")");
    EXPECT_EQ(explain("task-todo:x"), R"(Match task (todo): Matches text: "x")");
    EXPECT_EQ(explain("task-done:x"), R"(Match task (done): Matches text: "x")");
    EXPECT_EQ(explain("task-todo:"), R"(Match task (todo): Matches text: "")");
    EXPECT_EQ(explain("[aliases]"),
              "Contains property:\n"
              "    Matches text: \"aliases\"");
    EXPECT_EQ(explain("[status:Draft OR Published]"),
              "Contains property:\n"
              "    Matches text: \"status\"\n"
              "    Match any of:\n"
              "        Matches text: \"Draft\"\n"
              "        Matches text: \"Published\"");
    EXPECT_EQ(explain("[duration:<5]"), "Contains property:\n    Matches text: \"duration\"\n    Less than: 5");
    EXPECT_EQ(explain("[duration:>5]"), "Contains property:\n    Matches text: \"duration\"\n    Greater than: 5");
    EXPECT_EQ(explain("[aliases:null]"), "Contains property:\n    Matches text: \"aliases\"\n    Is empty");
    EXPECT_EQ(explain("[done:true]"), "Contains property:\n    Matches text: \"done\"\n    Is true");
    EXPECT_EQ(explain("[done:false]"), "Contains property:\n    Matches text: \"done\"\n    Is false");
    EXPECT_EQ(explain("-[x]"), "Excluding:\n    Contains property:\n        Matches text: \"x\"");
}
