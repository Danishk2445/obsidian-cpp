// In-file search: DocSearcher matching rules and the EditorSearch controller (Obsidian's search bar logic).
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/search/doc_searcher.h"
#include "editor/search/editor_search.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

namespace {
Text T(const char* s) { return Text::fromString(QString::fromUtf8(s)); }
SearchQuery Q(const QString& s) { return SearchQuery{s}; }
} // namespace

TEST(DocSearcher, CaseInsensitivePlainTextByDefault)
{
    const Text t = T("Foo bar\nfoo FOO\nnothing");
    DocSearcher s(Q("foo"));
    const QList<SearchMatch> all = s.all(t);
    ASSERT_EQ(all.size(), 3);
    EXPECT_EQ(all[0], (SearchMatch{0, 3}));
    EXPECT_EQ(all[1], (SearchMatch{8, 11}));
    EXPECT_EQ(all[2], (SearchMatch{12, 15}));
    SearchQuery cs = Q("foo");
    cs.caseSensitive = true;
    EXPECT_EQ(DocSearcher(cs).all(t).size(), 1);
    // regex metacharacters are literal in plain mode
    EXPECT_EQ(DocSearcher(Q(".")).all(T("a.b")).size(), 1);
}

TEST(DocSearcher, NextWrapsAndPreviousUsesMatchEnds)
{
    const Text t = T("ab ab\nab");
    DocSearcher s(Q("ab"));
    EXPECT_EQ(*s.next(t, 0), (SearchMatch{0, 2}));
    EXPECT_EQ(*s.next(t, 1), (SearchMatch{3, 5}));
    EXPECT_EQ(*s.next(t, 5), (SearchMatch{6, 8}));
    EXPECT_EQ(*s.next(t, 7), (SearchMatch{0, 2})); // wrapped
    EXPECT_FALSE(s.next(t, 7, false).has_value());
    EXPECT_EQ(*s.previous(t, 6), (SearchMatch{3, 5}));
    EXPECT_EQ(*s.previous(t, 4), (SearchMatch{0, 2})); // [3,5) doesn't end before 4
    EXPECT_EQ(*s.previous(t, 0), (SearchMatch{6, 8})); // wrapped to the last one
    EXPECT_FALSE(s.previous(t, 0, false).has_value());
    EXPECT_FALSE(DocSearcher(Q("zz")).next(t, 0).has_value());
}

TEST(DocSearcher, MatchesStayWithinLinesAndAreNonOverlapping)
{
    DocSearcher s(Q("aa"));
    EXPECT_EQ(s.all(T("aaaa")).size(), 2);
    EXPECT_EQ(s.all(T("a\na")).size(), 0);
}

TEST(DocSearcher, RegexAndWholeWord)
{
    SearchQuery re = Q("b+");
    re.regex = true;
    const QList<SearchMatch> m = DocSearcher(re).all(T("abbb b"));
    ASSERT_EQ(m.size(), 2);
    EXPECT_EQ(m[0], (SearchMatch{1, 4}));
    SearchQuery empty = Q("x*");
    empty.regex = true;
    EXPECT_EQ(DocSearcher(empty).all(T("axxb")).size(), 1); // empty matches are skipped
    SearchQuery bad = Q("(");
    bad.regex = true;
    EXPECT_FALSE(DocSearcher(bad).isValid());
    SearchQuery ww = Q("cat");
    ww.wholeWord = true;
    const QList<SearchMatch> w = DocSearcher(ww).all(T("cat concat cat_x cat."));
    ASSERT_EQ(w.size(), 2);
    EXPECT_EQ(w[1].from, 17);
}

TEST(DocSearcher, IndexAndCount)
{
    const Text t = T("x x\nx");
    DocSearcher s(Q("x"));
    EXPECT_EQ(s.indexAndCount(t, std::nullopt), (std::pair{0, 3}));
    EXPECT_EQ(s.indexAndCount(t, SearchMatch{2, 3}), (std::pair{2, 3}));
    EXPECT_EQ(s.indexAndCount(t, SearchMatch{4, 5}, 2), (std::pair{2, 2})); // capped
}

class EditorSearchTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        s_themes = new ThemeManager();
        s_themes->apply();
    }
    static void TearDownTestSuite()
    {
        delete s_themes;
        s_themes = nullptr;
    }
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        storage = std::make_unique<LocalFileStorage>(dir.path());
    }
    void open(const QByteArray& content)
    {
        QFile f(dir.path() + "/t.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
        doc = std::make_shared<Document>(storage.get(), QStringLiteral("t.md"));
        ASSERT_TRUE(doc->load());
        view = std::make_unique<EditorView>();
        view->resize(700, 400);
        view->setDocument(doc);
        view->show();
        QApplication::processEvents();
    }
    // Search marks the view applies to `line` (via its decoration layers).
    QList<Mark> searchMarks(EditorSearch& s, int line)
    {
        QList<Mark> out;
        const Text& t = doc->text();
        const qsizetype ls = t.lineStart(line), le = t.lineEnd(line);
        for (const SearchMatch& m : s.highlights())
            if (m.from < le && m.to > ls)
                out.append(Mark{int(m.from - ls), int(m.to - ls), StyleId::SearchMatch});
        return out;
    }

    static ThemeManager* s_themes;
    QTemporaryDir dir;
    std::unique_ptr<LocalFileStorage> storage;
    std::shared_ptr<Document> doc;
    std::unique_ptr<EditorView> view;
};
ThemeManager* EditorSearchTest::s_themes = nullptr;

TEST_F(EditorSearchTest, QuerySearchesFromTheSelectionEndAndLeavesSelectionAlone)
{
    open("one two one\ntwo one");
    view->setSelection(Selection::single(0, 3)); // "one" selected
    EditorSearch s(view.get());
    QSignalSpy results(&s, &EditorSearch::resultsChanged);
    ASSERT_TRUE(s.setQuery(Q("one")));
    EXPECT_EQ(*s.current(), (SearchMatch{8, 11})); // the selected occurrence is skipped
    EXPECT_EQ(view->selection().main().from(), 0);  // search doesn't move the editor selection
    EXPECT_EQ(view->selection().main().to(), 3);
    EXPECT_GE(results.count(), 1);
    EXPECT_EQ(s.indexAndCount(), (std::pair{2, 3}));
    ASSERT_TRUE(s.findNext());
    EXPECT_EQ(*s.current(), (SearchMatch{16, 19}));
    ASSERT_TRUE(s.findNext());
    EXPECT_EQ(*s.current(), (SearchMatch{0, 3})); // wraps
    ASSERT_TRUE(s.findPrevious());
    EXPECT_EQ(*s.current(), (SearchMatch{16, 19}));
    EXPECT_EQ(s.highlights().size(), 1);
    s.selectCurrent();
    EXPECT_EQ(view->selection().main().from(), 16);
    EXPECT_EQ(view->selection().main().to(), 19);
}

TEST_F(EditorSearchTest, NoMatchAndEmptyQuery)
{
    open("abc");
    EditorSearch s(view.get());
    EXPECT_FALSE(s.setQuery(Q("zzz")));
    EXPECT_FALSE(s.current().has_value());
    EXPECT_EQ(s.indexAndCount(), (std::pair{0, 0}));
    EXPECT_FALSE(s.setQuery(Q("")));
    EXPECT_TRUE(s.highlights().isEmpty());
}

TEST_F(EditorSearchTest, FindAllRingsEveryMatch)
{
    open("a b a\nb a");
    EditorSearch s(view.get());
    s.setQuery(Q("a"));
    EXPECT_EQ(s.findAll(), 3);
    EXPECT_EQ(s.highlights().size(), 3);
    EXPECT_EQ(searchMarks(s, 0).size(), 2);
    EXPECT_EQ(searchMarks(s, 1).size(), 1);
}

TEST_F(EditorSearchTest, ReplaceCurrentThenNextAndUndo)
{
    open("cat cat cat");
    EditorSearch s(view.get());
    s.setQuery(Q("cat"));
    EXPECT_EQ(*s.current(), (SearchMatch{0, 3}));
    ASSERT_TRUE(s.replaceCurrent("dog"));
    EXPECT_EQ(doc->text().toString(), "dog cat cat");
    EXPECT_EQ(*s.current(), (SearchMatch{4, 7}));
    // A replacement containing the query isn't matched again.
    ASSERT_TRUE(s.replaceCurrent("cats"));
    EXPECT_EQ(doc->text().toString(), "dog cats cat");
    EXPECT_EQ(*s.current(), (SearchMatch{9, 12}));
    view->undo();
    EXPECT_EQ(doc->text().toString(), "dog cat cat");
    view->undo();
    EXPECT_EQ(doc->text().toString(), "cat cat cat");
}

TEST_F(EditorSearchTest, ReplaceAllIsOneUndoStep)
{
    open("x1 x2\nx3");
    EditorSearch s(view.get());
    s.setQuery(Q("X"));
    EXPECT_EQ(s.replaceAll("yy"), 3);
    EXPECT_EQ(doc->text().toString(), "yy1 yy2\nyy3");
    EXPECT_TRUE(s.highlights().isEmpty());
    view->undo();
    EXPECT_EQ(doc->text().toString(), "x1 x2\nx3");
}

TEST_F(EditorSearchTest, HighlightsFollowEdits)
{
    open("hello world");
    EditorSearch s(view.get());
    s.setQuery(Q("world"));
    EXPECT_EQ(*s.current(), (SearchMatch{6, 11}));
    view->setCursorPosition(0);
    view->insertText(">> ");
    EXPECT_EQ(*s.current(), (SearchMatch{9, 14}));
    EXPECT_EQ(s.highlights().first(), (SearchMatch{9, 14}));
    ASSERT_TRUE(s.findNext()); // only one match: stays on it
    EXPECT_EQ(*s.current(), (SearchMatch{9, 14}));
}

TEST_F(EditorSearchTest, ReplaceSkipsStaleMatch)
{
    open("foo bar");
    EditorSearch s(view.get());
    s.setQuery(Q("foo"));
    // Edit inside the match: it no longer matches, so replace just moves on.
    view->setSelection(Selection::single(1, 2));
    view->insertText("X");
    EXPECT_FALSE(s.replaceCurrent("baz"));
    EXPECT_EQ(doc->text().toString(), "fXo bar");
}

TEST_F(EditorSearchTest, LayerDecoratesVisibleLines)
{
    open("alpha beta alpha");
    EditorSearch s(view.get());
    s.setQuery(Q("alpha"));
    s.findAll();
    // Ask the view to lay the line out again and check it carries SearchMatch marks via the layer.
    int seen = 0;
    const int id = view->addDecorationLayer([&](int line, LineDecoration& d) {
        if (line != 0)
            return;
        for (const Mark& m : d.marks)
            if (m.style == StyleId::SearchMatch)
                ++seen;
    });
    seen = 0;
    view->layout().invalidateLines(0, 1);
    ASSERT_NE(view->layout().layoutFor(0), nullptr);
    EXPECT_EQ(seen, 2);
    view->removeDecorationLayer(id);
}
