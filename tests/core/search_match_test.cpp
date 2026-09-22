// Global search evaluation semantics (plain terms, boolean logic, case, operators, regions, tags, tasks,
// properties), highlight ranges, cancellation, the CachedMetadata adapter and context snippets.
#include "core/search/search_match.h"
#include "core/search/search_snippet.h"
#include "markdown/metadata.h"

#include <gtest/gtest.h>

#include <atomic>

using namespace oc;
using namespace oc::search;

namespace {

SearchDocument note(const QString& path, const QString& content)
{
    SearchDocument d;
    d.path = path;
    d.content = content;
    return d;
}

std::optional<SearchResult> run(const QString& query, const SearchDocument& doc, const SearchOptions& options = {})
{
    const SearchQuery q = parseSearchQuery(query, options);
    const std::optional<SearchResult> r = matchDocument(q, doc);
    // The fast yes/no path must always agree with the full evaluation.
    EXPECT_EQ(documentMatches(q, doc), r.has_value()) << "query: " << query.toStdString() << " doc: " << doc.path.toStdString();
    return r;
}

bool hit(const QString& query, const SearchDocument& doc, const SearchOptions& options = {})
{
    return run(query, doc, options).has_value();
}

// Matched content substrings, in order.
QStringList contentHits(const QString& query, const SearchDocument& doc)
{
    QStringList out;
    if (const auto r = run(query, doc))
        for (const ContentMatch& m : r->contentMatches)
            out << doc.content.mid(m.from, m.to - m.from);
    return out;
}

QStringList fileHits(const QString& query, const SearchDocument& doc)
{
    QStringList out;
    if (const auto r = run(query, doc))
        for (const MatchRange& m : r->fileMatches)
            out << doc.path.mid(m.from, m.to - m.from);
    return out;
}

// [from, to) of the n-th occurrence of `what` in `text`.
TextRange rangeOf(const QString& text, const QString& what, int n = 0)
{
    qsizetype pos = -1;
    for (int i = 0; i <= n; ++i)
        pos = text.indexOf(what, pos + 1);
    EXPECT_GE(pos, 0) << what.toStdString();
    return {int(pos), int(pos + what.size())};
}

int lineOf(const QString& text, int offset) { return int(text.left(offset).count(u'\n')); }

md::Pos posAt(const QString& text, qsizetype offset)
{
    const qsizetype lineStart = offset > 0 ? text.lastIndexOf(u'\n', offset - 1) + 1 : 0;
    return {int(text.left(offset).count(u'\n')), int(offset - lineStart), offset};
}

md::Range mdRange(const QString& text, TextRange r) { return {posAt(text, r.from), posAt(text, r.to)}; }

} // namespace

TEST(SearchMatch, PlainTermsMatchThePathWithoutExtensionOrTheContent)
{
    const SearchDocument d = note("Projects/Meeting notes.md", "Discussed the kernel config.\nNext steps.");
    EXPECT_TRUE(hit("kernel", d));
    EXPECT_TRUE(hit("KERNEL", d)); // case-insensitive by default
    EXPECT_TRUE(hit("meeting", d));  // file name
    EXPECT_TRUE(hit("projects", d)); // folder
    EXPECT_TRUE(hit("notes steps", d));
    EXPECT_FALSE(hit("md", d)); // the extension is not part of a plain term's target
    EXPECT_FALSE(hit("xyz", d));
    EXPECT_FALSE(hit("", d)); // an empty query matches nothing

    const auto r = run("meeting config", d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->path, d.path);
    EXPECT_EQ(r->fileMatches, (QList<MatchRange>{{9, 16}}));
    EXPECT_EQ(r->contentMatches, (QList<ContentMatch>{{21, 27, 0}}));
    EXPECT_EQ(contentHits("steps", d), QStringList{"steps"});
    EXPECT_EQ(run("steps", d)->contentMatches.value(0).line, 1);
    EXPECT_EQ(fileHits("notes", d), QStringList{"notes"});
}

TEST(SearchMatch, BooleanLogicFollowsTheHelpExamples)
{
    const QList<SearchDocument> docs = {note("A.md", "meeting work"), note("B.md", "meeting meetup personal"),
                                        note("C.md", "work"), note("D.md", "meeting work meetup")};
    const auto matching = [&](const QString& q) {
        QString names;
        for (const SearchDocument& d : docs)
            if (hit(q, d))
                names += d.path.left(1);
        return names;
    };
    EXPECT_EQ(matching("meeting work"), "AD");
    EXPECT_EQ(matching("meeting OR work"), "ABCD");
    EXPECT_EQ(matching("meeting work OR meetup personal"), "ABD");
    EXPECT_EQ(matching("meeting (work OR meetup) personal"), "B");
    EXPECT_EQ(matching("meeting -work"), "B");
    EXPECT_EQ(matching("meeting -work -meetup"), "");
    EXPECT_EQ(matching("meeting -(work meetup)"), "AB");
    EXPECT_EQ(matching("-meeting"), "C");
    EXPECT_EQ(matching("--work"), "ACD");
}

TEST(SearchMatch, HighlightsComeFromPositiveTermsOnly)
{
    const SearchDocument d = note("n.md", "foobar\nbar foo\nbanana split");
    // Hits are sorted, overlapping ones merge, touching ones stay separate; each range knows its line.
    const auto r = run("foo bar", d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->contentMatches, (QList<ContentMatch>{{0, 3, 0}, {3, 6, 0}, {7, 10, 1}, {11, 14, 1}}));
    EXPECT_EQ(contentHits("foo OR foo", d), (QStringList{"foo", "foo"})); // duplicates collapse
    EXPECT_EQ(contentHits("apple OR banana", d), QStringList{"banana"});
    EXPECT_EQ(contentHits("split -apple", d), QStringList{"split"});
    EXPECT_EQ(contentHits("split -(apple OR pear)", d), QStringList{"split"});
    EXPECT_TRUE(run("-apple", d)->contentMatches.isEmpty());
    EXPECT_EQ(contentHits("an", d), (QStringList{"an", "an"})); // non-overlapping occurrences
}

TEST(SearchMatch, CaseSensitivity)
{
    const SearchDocument d = note("n.md", "HappyCat and IKEA");
    EXPECT_TRUE(hit("happycat", d));
    EXPECT_FALSE(hit("match-case:happycat", d));
    EXPECT_TRUE(hit("match-case:HappyCat", d));
    EXPECT_TRUE(hit("/happycat/", d));
    EXPECT_FALSE(hit("match-case:/happycat/", d));
    const SearchOptions matchCase{true};
    EXPECT_FALSE(hit("happycat", d, matchCase));
    EXPECT_TRUE(hit("HappyCat", d, matchCase));
    EXPECT_TRUE(hit("ignore-case:ikea", d, matchCase));
    EXPECT_TRUE(hit("ignore-case:(happycat ikea)", d, matchCase));
    EXPECT_TRUE(hit("über", note("n.md", "ÜBER alles"))); // Unicode case folding
}

TEST(SearchMatch, PhrasesAndRegexes)
{
    const SearchDocument d = note("Daily/2022-01-01.md", "they said \"hello\" to each other\nwars and star\n# Heading\nDate: 2022-01-01");
    EXPECT_TRUE(hit("\"said \\\"hello\\\"\"", d));
    EXPECT_TRUE(hit("\"to each other\"", d));
    EXPECT_FALSE(hit("\"star wars\"", d));
    EXPECT_TRUE(hit("star wars", d)); // words in any order
    EXPECT_EQ(contentHits(R"(/\d{4}-\d{2}-\d{2}/)", d), QStringList{"2022-01-01"});
    EXPECT_TRUE(hit(R"(path:/\d{4}-\d{2}-\d{2}/)", d));
    EXPECT_FALSE(hit(R"(content:/^\d{4}/)", d));
    EXPECT_EQ(contentHits("/^# .*$/", d), QStringList{"# Heading"}); // ^ and $ at line boundaries
    EXPECT_EQ(contentHits("/^/", d), QStringList{});                // empty matches match but do not highlight
    EXPECT_TRUE(hit("/^/", d));
    EXPECT_FALSE(hit("/(/", d)); // invalid: never matches
    EXPECT_TRUE(hit("\"\"", d)); // an empty phrase matches everything
}

TEST(SearchMatch, FilePathAndContentOperators)
{
    const SearchDocument d = note("Daily notes/2022-07-01.md", "went to IKEA today");
    EXPECT_TRUE(hit("file:2022", d));
    EXPECT_EQ(fileHits("file:2022", d), QStringList{"2022"});
    EXPECT_EQ(run("file:2022", d)->fileMatches, (QList<MatchRange>{{12, 16}})); // path coordinates
    EXPECT_FALSE(hit("file:daily", d)); // folders are not part of the file name
    EXPECT_TRUE(hit("path:daily", d));
    EXPECT_TRUE(hit("path:\"Daily notes/2022-07\"", d));
    EXPECT_TRUE(hit("file:.md", d)); // file: and path: include the extension
    EXPECT_TRUE(hit("path:md", d));
    EXPECT_TRUE(hit("content:ikea", d));
    EXPECT_FALSE(hit("content:daily", d));
    EXPECT_TRUE(hit("daily", d));
    EXPECT_TRUE(hit("file:", d)); // empty argument: every file
    EXPECT_TRUE(hit("-file:draft", d));
    EXPECT_FALSE(hit("file:-2022", d));
    EXPECT_TRUE(hit("file:(2022 07)", d));
    EXPECT_TRUE(hit("file:2022 content:ikea", d));
}

TEST(SearchMatch, FilesWithoutContentNeedAFileOrPathTerm)
{
    SearchDocument img = note("img/cat.png", QString());
    img.hasContent = false;
    EXPECT_FALSE(hit("cat", img));
    EXPECT_TRUE(hit("file:.png", img));
    EXPECT_TRUE(hit("file:.png cat", img));  // the plain term still looks at the path
    EXPECT_FALSE(hit("file:.png dog", img));
    EXPECT_TRUE(hit("path:img", img));
    EXPECT_FALSE(hit("-file:.jpg", img)); // no positive file term: only notes are searched
    EXPECT_FALSE(hit("file:.png content:cat", img));
    EXPECT_FALSE(hit("file:.png line:cat", img));
    SearchDocument withText = img; // content is ignored for such files
    withText.content = "cat";
    EXPECT_FALSE(hit("file:png content:cat", withText));
}

TEST(SearchMatch, TagsCompareHierarchically)
{
    const QString content = "intro\nwork on #work/project today\nat #Home\n```\n#code\n```";
    SearchDocument d = note("n.md", content);
    const TextRange work = rangeOf(content, "#work/project");
    const TextRange home = rangeOf(content, "#Home");
    d.tags = {{"#work/project", 1, work.from, work.to}, {"#Home", 2, home.from, home.to}, {"#inbox", -1, -1, -1}};
    EXPECT_TRUE(hit("tag:#work", d));
    EXPECT_TRUE(hit("tag:work", d));
    EXPECT_TRUE(hit("tag:#work/project", d));
    EXPECT_TRUE(hit("tag:#work/", d));
    EXPECT_FALSE(hit("tag:#work/proj", d));
    EXPECT_FALSE(hit("tag:#project", d));
    EXPECT_FALSE(hit("tag:#wor", d));
    EXPECT_FALSE(hit("tag:#code", d)); // only real tags count, not text in code blocks
    EXPECT_TRUE(hit("#code", d));      // ...which a plain search finds
    EXPECT_TRUE(hit("tag:#home", d));
    EXPECT_TRUE(hit("tag:#HOME", d, SearchOptions{true})); // the Match case toggle does not apply to tags
    EXPECT_FALSE(hit("match-case:tag:#home", d));
    EXPECT_TRUE(hit("match-case:tag:#Home", d));
    EXPECT_TRUE(hit("tag:#inbox", d)); // frontmatter tag
    EXPECT_TRUE(hit("tag:/^#w.*t$/", d));
    EXPECT_TRUE(hit("tag:", d));
    EXPECT_FALSE(hit("-tag:#work", d));
    EXPECT_TRUE(hit("tag:#work OR tag:#nothing", d));
    EXPECT_EQ(contentHits("tag:#work", d), QStringList{"#work/project"});
    EXPECT_TRUE(contentHits("tag:#inbox", d).isEmpty()); // no position to highlight

    SearchDocument other = note("o.md", "x");
    other.tags = {{"#homework", 0, -1, -1}, {"#myjob/work", 0, -1, -1}};
    EXPECT_FALSE(hit("tag:#work", other));
    EXPECT_TRUE(hit("tag:#myjob", other));
}

TEST(SearchMatch, LineOperator)
{
    const SearchDocument d = note("n.md", "mix the flour\nadd sugar and mix\r\nflour only");
    EXPECT_TRUE(hit("line:(mix flour)", d));
    EXPECT_EQ(contentHits("line:(mix flour)", d), (QStringList{"mix", "flour"})); // only the matching line
    EXPECT_EQ(run("line:(mix flour)", d)->contentMatches.value(1).line, 0);
    EXPECT_FALSE(hit("line:(sugar flour)", d));
    EXPECT_TRUE(hit("sugar flour", d));
    EXPECT_TRUE(hit("-line:(sugar flour)", d)); // no line has both
    EXPECT_FALSE(hit("-line:mix", d));
    EXPECT_TRUE(hit("line:(flour -mix)", d));
    EXPECT_EQ(contentHits("line:(flour -mix)", d), QStringList{"flour"});
    EXPECT_EQ(run("line:(flour -mix)", d)->contentMatches.value(0).line, 2);
    EXPECT_TRUE(hit("line:(mix -flour)", d));
    EXPECT_TRUE(hit("line:/^add.*mix$/", d)); // the line view excludes "\r"
    EXPECT_TRUE(hit("line:(mix OR nothing) line:only", d));
    EXPECT_FALSE(hit("line:(mix only)", d));
    EXPECT_FALSE(hit("line:x", note("e.md", QString())));
    // Trailing newline: no phantom empty last line.
    EXPECT_FALSE(hit("line:(-mix)", note("t.md", "mix\nmix\n")));
    EXPECT_TRUE(hit("line:(-mix)", note("t.md", "mix\n\nmix")));
}

TEST(SearchMatch, BlockOperator)
{
    // Without block structure, paragraphs separated by blank lines are the blocks.
    const SearchDocument paras = note("n.md", "dog\ncat\n\nonly dog\n  \t\ncat alone");
    EXPECT_TRUE(hit("block:(dog cat)", paras));
    EXPECT_FALSE(hit("line:(dog cat)", paras));
    EXPECT_FALSE(hit("block:(only cat)", paras));
    EXPECT_TRUE(hit("block:(cat alone)", paras));
    EXPECT_EQ(contentHits("block:(dog cat)", paras), (QStringList{"dog", "cat"}));
    // Explicit blocks win.
    SearchDocument d = paras;
    d.blocks = {rangeOf(d.content, "dog"), rangeOf(d.content, "cat"), rangeOf(d.content, "only dog"), rangeOf(d.content, "cat alone")};
    EXPECT_FALSE(hit("block:(dog cat)", d));
    EXPECT_TRUE(hit("block:(only dog)", d));
}

TEST(SearchMatch, SectionOperator)
{
    const QString content = "intro dog\n# Alpha\ndog\n## Beta\ncat\n# Gamma\ndog and cat";
    SearchDocument d = note("n.md", content);
    const int a = int(content.indexOf("# Alpha")), b = int(content.indexOf("## Beta")), c = int(content.indexOf("# Gamma"));
    d.sections = {{0, a}, {a, b}, {b, c}, {c, int(content.size())}};
    EXPECT_TRUE(hit("section:(dog cat)", d));
    EXPECT_EQ(contentHits("section:(dog cat)", d), (QStringList{"dog", "cat"}));
    EXPECT_EQ(run("section:(dog cat)", d)->contentMatches.value(0).line, 6);
    EXPECT_TRUE(hit("section:(alpha dog)", d)); // the heading belongs to its section
    EXPECT_FALSE(hit("section:(alpha cat)", d));
    EXPECT_FALSE(hit("section:(intro cat)", d));
    EXPECT_TRUE(hit("section:(line:(dog cat))", d));
    EXPECT_FALSE(hit("section:(line:(dog cat) intro)", d));
    d.sections.removeLast();
    EXPECT_FALSE(hit("section:(dog cat)", d));
    // Without sections the whole note is one section.
    EXPECT_TRUE(hit("section:(intro cat)", note("n.md", content)));
}

TEST(SearchMatch, TaskOperators)
{
    const QString content = "- [ ] call mom\n- [x] email boss\n- [-] call bank\n\t- [ ] write *call* notes\nnot a task: call";
    SearchDocument d = note("n.md", content);
    d.tasks = {{rangeOf(content, "- [ ] call mom"), u' '},
               {rangeOf(content, "- [x] email boss"), u'x'},
               {rangeOf(content, "- [-] call bank"), u'-'},
               {rangeOf(content, "- [ ] write *call* notes"), u' '}};
    EXPECT_TRUE(hit("task:call", d));
    EXPECT_EQ(contentHits("task:call", d), (QStringList{"call", "call", "call"})); // not the one outside tasks
    EXPECT_TRUE(hit("task-todo:call", d));
    EXPECT_FALSE(hit("task-todo:email", d));
    EXPECT_TRUE(hit("task-done:email", d));
    EXPECT_TRUE(hit("task-done:bank", d)); // any status other than ' ' counts as done
    EXPECT_FALSE(hit("task-todo:bank", d));
    EXPECT_TRUE(hit("task:(call mom)", d));
    EXPECT_FALSE(hit("task:(call boss)", d));
    EXPECT_TRUE(hit("task:(call OR email) task-todo:write", d));
    EXPECT_TRUE(hit("task-todo:", d));
    EXPECT_TRUE(hit("-task:dentist", d));
    SearchDocument allDone = note("done.md", "- [x] a");
    allDone.tasks = {{{0, 7}, u'x'}};
    EXPECT_FALSE(hit("task-todo:", allDone));
    EXPECT_TRUE(hit("task-done:", allDone));
    EXPECT_FALSE(hit("task:", note("none.md", "- [ ] looks like a task but no metadata")));
}

TEST(SearchMatch, TagsInsideRegions)
{
    const QString content = "meeting notes\n#work follow-up\nmeeting #work";
    SearchDocument d = note("n.md", content);
    d.tags = {{"#work", 1, rangeOf(content, "#work").from, rangeOf(content, "#work").to},
              {"#work", 2, rangeOf(content, "#work", 1).from, rangeOf(content, "#work", 1).to},
              {"#fm", -1, -1, -1}};
    EXPECT_TRUE(hit("line:(tag:#work meeting)", d));
    EXPECT_EQ(run("line:(tag:#work meeting)", d)->contentMatches.value(0).line, 2);
    d.tags.removeLast();
    d.tags.removeLast();
    EXPECT_FALSE(hit("line:(tag:#work meeting)", d));
    EXPECT_TRUE(hit("tag:#work meeting", d));
    // Tags given by line only still count in their line.
    SearchDocument byLine = note("n.md", content);
    byLine.tags = {{"#work", 2, -1, -1}};
    EXPECT_TRUE(hit("line:(tag:#work meeting)", byLine));
    SearchDocument frontmatterOnly = note("n.md", content);
    frontmatterOnly.tags = {{"#work", -1, -1, -1}};
    EXPECT_TRUE(hit("tag:#work", frontmatterOnly));
    EXPECT_FALSE(hit("line:(tag:#work)", frontmatterOnly));
}

TEST(SearchMatch, Properties)
{
    SearchDocument d = note("n.md", "body");
    d.properties = {
        {"aliases", QVariantList{"Big Name", "Other"}},
        {"status", "Draft"},
        {"duration", 3.0},
        {"rating", 4.5},
        {"done", true},
        {"archived", false},
        {"empty", QVariant()},
        {"emptyStr", QString()},
        {"emptyList", QVariantList{}},
        {"parent_topic", "x"},
        {"topic", "y"},
        {"date", "2024-03-01"},
        {"tags", QVariantList{"alpha", "beta"}},
        {"count", "12"},
        {"meta", QVariantMap{{"k", "v"}}},
    };
    EXPECT_TRUE(hit("[aliases]", d));
    EXPECT_FALSE(hit("[missing]", d));
    EXPECT_TRUE(hit("[aliases:name]", d)); // substring, case-insensitive
    EXPECT_TRUE(hit("[aliases:\"big name\"]", d));
    EXPECT_FALSE(hit("[aliases:Big Other]", d)); // all words must match the same list element
    EXPECT_TRUE(hit("[aliases:Big OR Nope]", d));
    EXPECT_TRUE(hit("[status:Draft OR Published]", d));
    EXPECT_FALSE(hit("[status:Published]", d));
    EXPECT_FALSE(hit("[status:-Draft]", d));
    EXPECT_TRUE(hit("[status:/^dr/]", d));
    EXPECT_FALSE(hit("[status:match-case:draft]", d)); // operators are literal inside brackets
    EXPECT_TRUE(hit("[duration:<5]", d));
    EXPECT_FALSE(hit("[duration:>5]", d));
    EXPECT_TRUE(hit("[duration:\"<5\"]", d));
    EXPECT_TRUE(hit("[duration:3]", d)); // numbers print like JavaScript: "3", not "3.0"
    EXPECT_FALSE(hit("[duration:3.0]", d));
    EXPECT_TRUE(hit("[rating:>4]", d));
    EXPECT_TRUE(hit("[rating:4.5]", d));
    EXPECT_TRUE(hit("[count:>5]", d)); // numeric strings compare as numbers
    EXPECT_FALSE(hit("[status:>5]", d));
    EXPECT_TRUE(hit("[date:>2024-01-01]", d)); // otherwise as strings
    EXPECT_FALSE(hit("[date:<2024-01-01]", d));
    EXPECT_TRUE(hit("[empty:null]", d));
    EXPECT_TRUE(hit("[empty:EMPTY]", d));
    EXPECT_FALSE(hit("[emptyStr:null]", d));
    EXPECT_FALSE(hit("[emptyList:null]", d));
    EXPECT_FALSE(hit("[status:null]", d));
    EXPECT_TRUE(hit("[empty]", d));
    EXPECT_FALSE(hit("[empty:x]", d));
    EXPECT_TRUE(hit("[done:true]", d));
    EXPECT_FALSE(hit("[done:false]", d));
    EXPECT_TRUE(hit("[archived:false]", d));
    EXPECT_FALSE(hit("[status:true]", d));
    EXPECT_TRUE(hit("[tags:beta]", d));
    EXPECT_TRUE(hit("[meta:\"k\"]", d)); // maps match as JSON text
    EXPECT_TRUE(hit("-[status:Published]", d));
    EXPECT_FALSE(hit("-[status:Draft]", d));
    EXPECT_TRUE(hit("[:Draft]", d));
    EXPECT_TRUE(hit("[/^dur/:<5]", d));
    EXPECT_TRUE(hit("[status:draft] body", d));
    EXPECT_FALSE(hit("[status:draft] nobody", d));

    // [topic] also finds "parent_topic": keys are matched as text (Obsidian's behaviour).
    const auto topics = run("[topic]", d);
    ASSERT_TRUE(topics.has_value());
    ASSERT_EQ(topics->propertyMatches.size(), 2);
    EXPECT_EQ(topics->propertyMatches[0].key, "parent_topic");
    EXPECT_EQ(topics->propertyMatches[0].keyRanges, (QList<MatchRange>{{7, 12}}));
    EXPECT_EQ(topics->propertyMatches[1].key, "topic");

    const auto aliases = run("[aliases:name]", d);
    ASSERT_TRUE(aliases.has_value());
    ASSERT_EQ(aliases->propertyMatches.size(), 1);
    EXPECT_EQ(aliases->propertyMatches[0].value, "Big Name");
    EXPECT_EQ(aliases->propertyMatches[0].valueRanges, (QList<MatchRange>{{4, 8}}));
    EXPECT_TRUE(aliases->contentMatches.isEmpty());
    // Inside a region operator the property term matches in every region; it is reported once.
    SearchDocument lines = d;
    lines.content = "kernel one\nkernel two\nkernel three";
    const auto perLine = run("line:([status:draft] kernel)", lines);
    ASSERT_TRUE(perLine.has_value());
    EXPECT_EQ(perLine->propertyMatches.size(), 1);
    EXPECT_EQ(perLine->contentMatches.size(), 3);

    EXPECT_EQ(propertyValueText(3.0), "3");
    EXPECT_EQ(propertyValueText(-2.5), "-2.5");
    EXPECT_EQ(propertyValueText(0.1), "0.1");
    EXPECT_EQ(propertyValueText(true), "true");
    EXPECT_EQ(propertyValueText(QVariant()), "");
    EXPECT_EQ(propertyValueText(QVariantList{"a", 1.0}), "a, 1");
}

TEST(SearchMatch, CancellationIsPolledEveryThousandLines)
{
    QString big;
    for (int i = 0; i < 5000; ++i)
        big += QStringLiteral("line %1 with x\n").arg(i);
    const SearchDocument d = note("big.md", big + "the end");
    std::atomic<bool> cancel{true};
    // The request is noticed by the periodic checks (every ~1k lines, regions or occurrences), not up front.
    EXPECT_FALSE(matchDocument(parseSearchQuery("line:(-zzz)"), d, &cancel).has_value());
    EXPECT_FALSE(matchDocument(parseSearchQuery("x"), d, &cancel).has_value());
    EXPECT_FALSE(documentMatches(parseSearchQuery("line:(-with)"), d, &cancel));
    EXPECT_TRUE(documentMatches(parseSearchQuery("end"), d, &cancel)); // small work finishes before the next poll
    std::atomic<bool> go{false};
    EXPECT_TRUE(matchDocument(parseSearchQuery("line:(-zzz)"), d, &go).has_value());
    EXPECT_TRUE(documentMatches(parseSearchQuery("line:(-with)"), d, &go));
    const auto all = matchDocument(parseSearchQuery("x"), d, &go);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->contentMatches.size(), 5000);
    EXPECT_EQ(all->contentMatches.last().line, 4999);
}

TEST(SearchMatch, DocumentFromCachedMetadata)
{
    const QString content = "---\ntags: [inbox]\nstatus: Draft\n---\n# Title\nSome #work text\n- [ ] todo item\n- [x] done item\n\n"
                            "Para two\n## Sub\nmore";
    md::CachedMetadata meta;
    meta.frontmatter = QList<md::FrontmatterValue>{{"tags", QVariantList{"inbox"}}, {"status", "Draft"}};
    const TextRange yaml{0, int(content.indexOf("# Title")) - 1};
    const TextRange title = rangeOf(content, "# Title");
    const TextRange para1 = rangeOf(content, "Some #work text");
    const TextRange list{rangeOf(content, "- [ ] todo item").from, rangeOf(content, "- [x] done item").to};
    const TextRange para2 = rangeOf(content, "Para two");
    const TextRange sub = rangeOf(content, "## Sub");
    const TextRange more = rangeOf(content, "more");
    meta.tags = {{"#work", mdRange(content, rangeOf(content, "#work"))}};
    meta.headings = {{"Title", 1, mdRange(content, title)}, {"Sub", 2, mdRange(content, sub)}};
    meta.sections = {{"yaml", mdRange(content, yaml), {}},        {"heading", mdRange(content, title), {}},
                     {"paragraph", mdRange(content, para1), {}},  {"list", mdRange(content, list), {}},
                     {"paragraph", mdRange(content, para2), {}},  {"heading", mdRange(content, sub), {}},
                     {"paragraph", mdRange(content, more), {}}};
    md::ListItemCache todo;
    todo.position = mdRange(content, rangeOf(content, "- [ ] todo item"));
    todo.task = u' ';
    md::ListItemCache done;
    done.position = mdRange(content, rangeOf(content, "- [x] done item"));
    done.task = u'x';
    meta.listItems = {todo, done};

    const SearchDocument d = SearchDocument::fromMetadata("Notes/My note.md", content, meta);
    EXPECT_EQ(d.name().toString(), "My note.md");
    EXPECT_EQ(d.basename().toString(), "My note");
    EXPECT_EQ(d.pathNoExtension().toString(), "Notes/My note");
    EXPECT_EQ(note(".hidden", "").basename().toString(), ".hidden");
    EXPECT_EQ(note("a.b/c", "").pathNoExtension().toString(), "a.b/c");
    ASSERT_EQ(d.tags.size(), 2);
    EXPECT_EQ(d.tags[0].tag, "#inbox");
    EXPECT_EQ(d.tags[0].line, -1);
    EXPECT_EQ(d.tags[1].tag, "#work");
    EXPECT_EQ(d.tags[1].line, lineOf(content, rangeOf(content, "#work").from));
    EXPECT_EQ(d.tags[1].from, rangeOf(content, "#work").from);
    EXPECT_EQ(d.sections, (QList<TextRange>{{0, title.from}, {title.from, sub.from}, {sub.from, int(content.size())}}));
    EXPECT_EQ(d.blocks, (QList<TextRange>{title, para1, rangeOf(content, "- [ ] todo item"), rangeOf(content, "- [x] done item"),
                                          para2, sub, more}));
    ASSERT_EQ(d.tasks.size(), 2);
    EXPECT_EQ(d.tasks[0].status, u' ');
    EXPECT_EQ(d.tasks[1].status, u'x');
    ASSERT_EQ(d.properties.size(), 2);
    EXPECT_EQ(d.properties[1].key, "status");

    EXPECT_TRUE(hit("tag:#inbox tag:#work", d));
    EXPECT_TRUE(hit("task-todo:todo -task-todo:done task-done:done", d));
    EXPECT_TRUE(hit("section:(Title work)", d));
    EXPECT_FALSE(hit("section:(Title more)", d));
    EXPECT_TRUE(hit("block:(todo item)", d));
    EXPECT_FALSE(hit("block:(todo done)", d)); // list items are separate blocks
    EXPECT_TRUE(hit("[status:draft] \"My note\"", d));
    EXPECT_TRUE(hit("line:(status draft)", d)); // frontmatter text is content too
}

TEST(SearchSnippet, LineContextAroundMatches)
{
    const QString line = "Alpha bravo charlie delta echo foxtrot golf hotel india juliett kilo lima mike november oscar papa";
    const QString content = "first line\n" + line + "\nlast needle line";
    const int from = int(content.indexOf("india"));
    const QList<ContentMatch> matches = {{from, from + 5, 1}};
    SnippetOptions o;
    o.contextChars = 20;
    const QList<Snippet> s = buildSnippets(content, matches, o);
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s[0].line, 1);
    EXPECT_EQ(s[0].text, "foxtrot golf hotel india juliett kilo lima mike"); // cut at word boundaries
    EXPECT_TRUE(s[0].truncatedStart);
    EXPECT_TRUE(s[0].truncatedEnd);
    ASSERT_EQ(s[0].matches.size(), 1);
    EXPECT_EQ(s[0].text.mid(s[0].matches[0].from, s[0].matches[0].to - s[0].matches[0].from), "india");
    EXPECT_EQ(content.mid(s[0].from, s[0].to - s[0].from), s[0].text);

    // A whole short line is kept, without ellipses.
    const int n = int(content.indexOf("needle"));
    const QList<Snippet> last = buildSnippets(content, {{n, n + 6, 2}}, o);
    ASSERT_EQ(last.size(), 1);
    EXPECT_EQ(last[0].text, "last needle line");
    EXPECT_FALSE(last[0].truncatedStart);
    EXPECT_FALSE(last[0].truncatedEnd);
    EXPECT_EQ(last[0].matches, (QList<MatchRange>{{5, 11}}));
}

TEST(SearchSnippet, NearbyMatchesShareASnippetAndLinesSplit)
{
    const QString content = "one two three four\nfive six seven\neight";
    const auto m = [&](const QString& w, int line) {
        const int f = int(content.indexOf(w));
        return ContentMatch{f, f + int(w.size()), line};
    };
    SnippetOptions o;
    o.contextChars = 10;
    const QList<Snippet> s = buildSnippets(content, {m("two", 0), m("four", 0), m("six", 1), m("eight", 2)}, o);
    ASSERT_EQ(s.size(), 3);
    EXPECT_EQ(s[0].text, "one two three four");
    EXPECT_EQ(s[0].matches.size(), 2);
    EXPECT_EQ(s[1].line, 1);
    EXPECT_EQ(s[1].text, "five six seven");
    EXPECT_EQ(s[2].line, 2);
    EXPECT_EQ(s[2].text, "eight");

    o.maxSnippets = 2;
    EXPECT_EQ(buildSnippets(content, {m("two", 0), m("six", 1), m("eight", 2)}, o).size(), 2);

    // "Show more context": whole neighbouring lines.
    SnippetOptions more;
    more.contextLines = 1;
    const QList<Snippet> c = buildSnippets(content, {m("six", 1)}, more);
    ASSERT_EQ(c.size(), 1);
    EXPECT_EQ(c[0].line, 0);
    EXPECT_EQ(c[0].text, content);
    EXPECT_EQ(c[0].matches, (QList<MatchRange>{{int(content.indexOf("six")), int(content.indexOf("six")) + 3}}));
    EXPECT_FALSE(c[0].truncatedStart);
    const QList<Snippet> merged = buildSnippets(content, {m("two", 0), m("eight", 2)}, more);
    ASSERT_EQ(merged.size(), 1); // overlapping line windows merge
    EXPECT_EQ(merged[0].matches.size(), 2);
}

TEST(SearchSnippet, EdgesAndSurrogates)
{
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80"); // U+1F600, a surrogate pair
    const QString content = "x" + emoji.repeated(30) + "needle" + emoji.repeated(30) + "\r\nnext";
    const int f = int(content.indexOf("needle"));
    SnippetOptions o;
    o.contextChars = 5; // odd: would cut pairs in half
    const QList<Snippet> s = buildSnippets(content, {{f, f + 6, 0}}, o);
    ASSERT_EQ(s.size(), 1);
    EXPECT_FALSE(s[0].text.front().isLowSurrogate());
    EXPECT_FALSE(s[0].text.back().isHighSurrogate());
    EXPECT_FALSE(s[0].text.contains(u'\r'));
    EXPECT_TRUE(s[0].text.contains("needle"));

    const QString plain = "needle at the start";
    const QList<Snippet> start = buildSnippets(plain, {{0, 6, 0}}, o);
    ASSERT_EQ(start.size(), 1);
    EXPECT_FALSE(start[0].truncatedStart);
    EXPECT_EQ(start[0].matches, (QList<MatchRange>{{0, 6}}));
    EXPECT_TRUE(buildSnippets(plain, {}, o).isEmpty());

    // End-to-end with a search result.
    const SearchDocument d = note("n.md", "alpha\n  beta gamma\ndelta beta");
    const auto r = matchDocument(parseSearchQuery("beta"), d);
    ASSERT_TRUE(r.has_value());
    const QList<Snippet> snippets = buildSnippets(d.content, r->contentMatches);
    ASSERT_EQ(snippets.size(), 2);
    EXPECT_EQ(snippets[0].text, "beta gamma"); // leading indentation trimmed
    EXPECT_EQ(snippets[0].line, 1);
    EXPECT_EQ(snippets[1].text, "delta beta");
    EXPECT_EQ(snippets[1].line, 2);
}
