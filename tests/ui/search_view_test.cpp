// Global search view: typing runs queries (debounced, streamed, cancelled on restart), sort orders, collapse,
// snippets and "more context", explain panel, clicking a match opens the note with the match eState, view state
// round trip, live updates on vault changes, the command, tag searches and copying results.
#include "navigation_test_support.h"

#include "editor/editor_view.h"
#include "ui/navigation/vault_search_source.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QSignalSpy>

using namespace oc;
using namespace oc::test;

namespace {

SearchView* searchView(NavigationEnv& env)
{
    SearchView* v = openGlobalSearch(env.session);
    EXPECT_NE(v, nullptr);
    return v;
}

// The scratch vault gets `n` generated notes: every note has "common" on many lines, every third one "needle".
void addGenerated(const QString& root, int n)
{
    for (int i = 0; i < n; ++i) {
        QByteArray body;
        for (int line = 0; line < 30; ++line)
            body += "common words on line " + QByteArray::number(line) + " of note " + QByteArray::number(i) + "\n";
        if (i % 3 == 0)
            body += "the needle is here\n";
        writeFile(root + QStringLiteral("/Gen/note-%1.md").arg(i, 4, 10, QLatin1Char('0')), body);
    }
}

} // namespace

TEST(SearchView, IsRegisteredAsTheSearchSidebarView)
{
    NavigationEnv env;
    const QList<WorkspaceLeaf*> leaves = env.workspace->leavesOfType("search");
    ASSERT_EQ(leaves.size(), 1); // the default layout has one in the left sidebar
    auto* v = qobject_cast<SearchView*>(leaves[0]->view());
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->displayText(), "Search");
    EXPECT_EQ(v->icon(), "search");
    EXPECT_FALSE(v->showsViewHeader());
    EXPECT_TRUE(leaves[0]->isInSidedock());
    EXPECT_EQ(v->input()->placeholderText(), "Search...");
}

TEST(SearchView, TypingRunsTheQueryAfterADebounce)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    QSignalSpy started(v, &SearchView::searchStarted);
    v->focusInput();
    QTest::keyClicks(v->input(), "callout");
    EXPECT_EQ(started.count(), 0); // debounced
    EXPECT_FALSE(v->isIdle());
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(started.count(), 1);
    EXPECT_TRUE(v->resultPaths().contains("Quotes and callouts.md"));
    EXPECT_EQ(v->resultCountText(), QStringLiteral("%1 result%2").arg(v->resultCount()).arg(v->resultCount() == 1 ? "" : "s"));
    const SearchResultGroup* g = v->result("Quotes and callouts.md");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->title, "Quotes and callouts");
    EXPECT_FALSE(g->snippets.isEmpty());
    for (const search::Snippet& s : g->snippets) {
        ASSERT_FALSE(s.matches.isEmpty());
        EXPECT_EQ(s.text.mid(s.matches[0].from, s.matches[0].to - s.matches[0].from).toLower(), "callout");
    }
    // file-name hits are highlighted on the title
    EXPECT_FALSE(g->titleMatches.isEmpty());

    // Enter runs at once; clearing the input clears the results immediately
    v->input()->setText("zzzz-nothing-matches");
    QTest::keyClick(v->input(), Qt::Key_Return);
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultCount(), 0);
    EXPECT_EQ(v->resultsList()->emptyText(), "No matches found.");
    v->input()->clear();
    EXPECT_TRUE(v->isIdle());
    EXPECT_EQ(v->resultCount(), 0);
    EXPECT_TRUE(v->resultsList()->emptyText().isEmpty());
}

TEST(SearchView, StreamsResultsAndCancelsTheSearchItReplaces)
{
    NavigationEnv env([](const QString& root) { addGenerated(root, 300); });
    SearchView* v = searchView(env);
    QSignalSpy finished(v, &SearchView::searchFinished);
    QSignalSpy changed(v, &SearchView::resultsChanged);
    v->setQuery("common");
    EXPECT_TRUE(v->isSearching());
    v->setQuery("needle"); // restarts: nothing of the first search may show up
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultCount(), 100);
    for (const QString& p : v->resultPaths())
        EXPECT_TRUE(p.startsWith("Gen/")) << qPrintable(p);
    for (const SearchResultGroup& g : v->resultsList()->groups())
        EXPECT_EQ(g.contentMatches.size(), 1);

    v->setQuery("common");
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultCount(), 300);
    // many matches per file: snippets are capped with "... and N more matches."
    const SearchResultGroup* g = v->result("Gen/note-0001.md");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->contentMatches.size(), 30);
    EXPECT_EQ(g->snippets.size(), SearchView::kMaxSnippets);
    EXPECT_EQ(g->moreMatches, 30 - SearchView::kMaxSnippets);
    EXPECT_GT(changed.count(), 1);
    EXPECT_GE(finished.count(), 2);
}

TEST(SearchView, SortOrders)
{
    NavigationEnv env([](const QString& root) {
        writeFile(root + "/Sort/b-note.md", "sortable b\n");
        writeFile(root + "/Sort/a-note.md", "sortable a\n");
        writeFile(root + "/Sort/c-note.md", "sortable c\n");
        writeFile(root + "/Sort/note 10.md", "sortable 10\n");
        writeFile(root + "/Sort/note 9.md", "sortable 9\n");
        // modification times: c oldest, a newest
        const QDateTime base = QDateTime::currentDateTime().addDays(-10);
        auto touch = [&](const char* name, int days) {
            QFile f(root + "/Sort/" + name);
            if (f.open(QIODevice::ReadWrite))
                f.setFileTime(base.addDays(days), QFileDevice::FileModificationTime);
        };
        touch("c-note.md", 0);
        touch("b-note.md", 1);
        touch("note 9.md", 2);
        touch("note 10.md", 3);
        touch("a-note.md", 4);
    });
    SearchView* v = searchView(env);
    v->setQuery("sortable");
    ASSERT_TRUE(env.waitIdle(v));
    const QStringList az{"Sort/a-note.md", "Sort/b-note.md", "Sort/c-note.md", "Sort/note 9.md", "Sort/note 10.md"}; // numeric-aware
    EXPECT_EQ(v->resultPaths(), az);
    v->setSortOrder(SearchView::SortOrder::AlphabeticalReverse);
    QStringList za = az;
    std::reverse(za.begin(), za.end());
    EXPECT_EQ(v->resultPaths(), za);
    v->setSortOrder(SearchView::SortOrder::ByModifiedTime);
    EXPECT_EQ(v->resultPaths(), (QStringList{"Sort/a-note.md", "Sort/note 10.md", "Sort/note 9.md", "Sort/b-note.md", "Sort/c-note.md"}));
    v->setSortOrder(SearchView::SortOrder::ByModifiedTimeReverse);
    EXPECT_EQ(v->resultPaths(), (QStringList{"Sort/c-note.md", "Sort/b-note.md", "Sort/note 9.md", "Sort/note 10.md", "Sort/a-note.md"}));
    // streamed results land in sorted position
    v->setQuery("sortable");
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultPaths().first(), "Sort/c-note.md");
    EXPECT_EQ(SearchView::sortOrderLabel(SearchView::SortOrder::ByCreatedTime), "Created time (new to old)");
}

TEST(SearchView, CollapseAllAndPerFileToggles)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    v->setQuery("welcome");
    ASSERT_TRUE(env.waitIdle(v));
    ASSERT_GE(v->resultCount(), 2);
    SearchResultsList* list = v->resultsList();
    EXPECT_FALSE(list->itemRect(0, 0).isEmpty());
    v->setCollapseAll(true);
    for (const SearchResultGroup& g : list->groups())
        EXPECT_TRUE(g.collapsed);
    EXPECT_TRUE(list->itemRect(0, 0).isEmpty());
    EXPECT_FALSE(list->itemRect(1, -1).isEmpty());

    // the arrow of one file expands just that one
    const QRect title = list->itemRect(0, -1);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(title.left() + 10, title.center().y()));
    EXPECT_FALSE(list->groups()[0].collapsed);
    EXPECT_TRUE(list->groups()[1].collapsed);
    EXPECT_FALSE(list->itemRect(0, 0).isEmpty());
    v->setCollapseAll(false);
    for (const SearchResultGroup& g : list->groups())
        EXPECT_FALSE(g.collapsed);
}

TEST(SearchView, ClickingAMatchOpensTheNoteWithTheMatchSelected)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    v->setQuery("strikethrough");
    ASSERT_TRUE(env.waitIdle(v));
    const int gi = int(v->resultPaths().indexOf("Formatting.md"));
    ASSERT_GE(gi, 0);
    SearchResultsList* list = v->resultsList();
    list->scrollToGroup(gi);
    const QRect r = list->itemRect(gi, 0);
    ASSERT_FALSE(r.isEmpty());

    const QJsonObject e = v->matchEState(gi, 0);
    const QJsonObject match = e.value("match").toObject();
    const QString content = match.value("content").toString();
    const QJsonArray first = match.value("matches").toArray().first().toArray();
    const int from = first.at(0).toInt(), to = first.at(1).toInt();
    EXPECT_EQ(content.mid(from, to - from).toLower(), "strikethrough");
    EXPECT_EQ(content, readFile(env.root + "/Formatting.md")); // the searched text (LF note)

    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), "Formatting.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_NE(mv, nullptr);
    const SelRange sel = mv->editor()->selection().main();
    EXPECT_EQ(std::min(sel.anchor, sel.head), from);
    EXPECT_EQ(std::max(sel.anchor, sel.head), to);

    // Mod+click opens a new tab; a click on the title opens the file without a selection
    const int leaves = env.rootLeafCount();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier, list->itemRect(gi, 0).center());
    QApplication::processEvents();
    EXPECT_EQ(env.rootLeafCount(), leaves + 1);
    env.openNote("Welcome.md");
    const QRect title = list->itemRect(gi, -1);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(title.center().x(), title.center().y()));
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), "Formatting.md");
}

TEST(SearchView, MoreContextExplainAndMatchCase)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    v->setQuery("Welcome");
    ASSERT_TRUE(env.waitIdle(v));
    const int ignoreCase = v->resultCount();
    ASSERT_GT(ignoreCase, 0);
    const SearchResultGroup* g = v->result("Welcome.md");
    ASSERT_NE(g, nullptr);
    const int lines = int(g->snippets.value(0).text.count('\n'));
    v->setExtraContext(true);
    g = v->result("Welcome.md");
    EXPECT_GT(int(g->snippets.value(0).text.count('\n')), lines);
    v->setExtraContext(false);

    v->setMatchCase(true);
    EXPECT_TRUE(v->input()->matchCaseButton()->isChecked());
    ASSERT_TRUE(env.waitIdle(v));
    for (const SearchResultGroup& r : v->resultsList()->groups())
        for (const search::ContentMatch& m : r.contentMatches)
            EXPECT_EQ(r.doc->content.mid(m.from, m.to - m.from), "Welcome");
    v->setQuery("welcome");
    ASSERT_TRUE(env.waitIdle(v));
    for (const SearchResultGroup& r : v->resultsList()->groups())
        EXPECT_TRUE(r.contentMatches.isEmpty()) << qPrintable(r.path); // only lowercase hits, e.g. none in content
    v->input()->matchCaseButton()->click(); // the Aa toggle
    EXPECT_FALSE(v->matchCase());

    EXPECT_TRUE(v->explanation().isEmpty());
    v->setSettingsVisible(true);
    EXPECT_TRUE(v->settingsVisible());
    v->setExplainSearch(true);
    v->setQuery("meeting -file:draft");
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->explanation(), search::explainSearchQuery(search::parseSearchQuery("meeting -file:draft")));
    EXPECT_TRUE(v->explanation().startsWith("Match all of:"));
}

TEST(SearchView, StateRoundTripsWithObsidiansKeys)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    QSignalSpy stateChanged(v, &View::stateChanged);
    const QJsonObject state{{"query", "tag:#fixture"},
                            {"matchingCase", true},
                            {"explainSearch", true},
                            {"collapseAll", true},
                            {"extraContext", true},
                            {"sortOrder", "byCreatedTimeReverse"}};
    v->setState(state);
    EXPECT_EQ(v->state(), state);
    EXPECT_EQ(v->query(), "tag:#fixture");
    EXPECT_TRUE(v->matchCase());
    EXPECT_TRUE(v->collapseAll());
    EXPECT_EQ(v->sortOrder(), SearchView::SortOrder::ByCreatedTimeReverse);
    EXPECT_EQ(stateChanged.count(), 0); // applying a state doesn't echo it
    ASSERT_TRUE(env.waitIdle(v));
    v->setSortOrder(SearchView::SortOrder::Alphabetical);
    EXPECT_EQ(stateChanged.count(), 1);
    EXPECT_EQ(v->state().value("sortOrder").toString(), "alphabetical");

    // the workspace persists it in the leaf's state
    const QJsonObject ws = env.workspace->serialize();
    const QByteArray json = QJsonDocument(ws).toJson(QJsonDocument::Compact);
    EXPECT_TRUE(json.contains("\"query\":\"tag:#fixture\"")) << json.constData();
    EXPECT_TRUE(json.contains("\"type\":\"search\""));
}

TEST(SearchView, FollowsVaultChanges)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    v->setQuery("zebracorn");
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultCount(), 0);

    TFile* f = env.vault->createFile("Zoo/Animals.md", "A zebracorn appears.\n");
    ASSERT_NE(f, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return v->resultCount() == 1 && v->isIdle(); }, 10000));
    EXPECT_EQ(v->resultPaths(), QStringList{"Zoo/Animals.md"});

    // content edits (external writes are picked up by the watcher; ours by the vault signal)
    DiskState st;
    ASSERT_TRUE(env.vault->read("Zoo/Animals.md", &st, nullptr).has_value());
    DiskState ns;
    ASSERT_EQ(env.vault->write("Zoo/Animals.md", "Two zebracorn and a zebracorn.\n", st, &ns, nullptr), DocumentStorage::WriteResult::Ok);
    ASSERT_TRUE(QTest::qWaitFor([&] { const auto* g = v->result("Zoo/Animals.md"); return g && g->contentMatches.size() == 2 && v->isIdle(); }, 10000));

    // renames move the result, deletions remove it
    ASSERT_TRUE(env.vault->rename(env.vault->file("Zoo/Animals.md"), "Zoo/Beasts.md"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return v->resultPaths() == QStringList{"Zoo/Beasts.md"} && v->isIdle(); }, 10000));
    ASSERT_TRUE(env.vault->trash(env.vault->file("Zoo/Beasts.md"), Vault::TrashMode::Local));
    ASSERT_TRUE(QTest::qWaitFor([&] { return v->resultCount() == 0 && v->isIdle(); }, 10000));
}

TEST(SearchView, SearchesTheLiveTextOfOpenNotes)
{
    NavigationEnv env;
    env.openNote("Math.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_NE(mv, nullptr);
    Document* doc = mv->document();
    ASSERT_NE(doc, nullptr);
    doc->setAutosaveDelayMs(60 * 60 * 1000); // keep the edit unsaved
    mv->editor()->setSelection(Selection::single(0, 0), false);
    mv->editor()->insertText("quokkaword ");
    ASSERT_TRUE(doc->isDirty());
    SearchView* v = searchView(env);
    v->setQuery("quokkaword");
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_EQ(v->resultPaths(), QStringList{"Math.md"});
    EXPECT_EQ(readFile(env.root + "/Math.md").indexOf("quokkaword"), -1); // not saved: the view searched live text
}

TEST(SearchView, GlobalSearchCommandTagSearchAndCopy)
{
    NavigationEnv env;
    const Command* c = env.commands->find("global-search:open");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "Search: Search in all files");
    EXPECT_EQ(env.commands->hotkeys("global-search:open").value(0).toString(), "Ctrl + Shift + F");

    // with a one-line selection in the editor, the search is prefilled
    env.openNote("Welcome.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_NE(mv, nullptr);
    const Text& t = mv->document()->text();
    const qsizetype at = t.toString().indexOf("Formatting");
    ASSERT_GE(at, 0);
    mv->editor()->setSelection(Selection::single(at, at + 10), false);
    ASSERT_TRUE(env.commands->execute("global-search:open"));
    SearchView* v = qobject_cast<SearchView*>(env.workspace->leavesOfType("search").value(0)->view());
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->query(), "Formatting");
    EXPECT_TRUE(v->input()->hasFocus());
    EXPECT_FALSE(env.workspace->isSideCollapsed(Workspace::Side::Left));
    ASSERT_TRUE(env.waitIdle(v));
    EXPECT_TRUE(v->resultPaths().contains("Formatting.md"));

    // tag clicks
    SearchView* tv = openGlobalSearchForTag(env.session, "#fixture");
    ASSERT_EQ(tv, v);
    EXPECT_EQ(v->query(), "tag:#fixture");
    ASSERT_TRUE(env.waitIdle(v));
    if (env.meta->fileCache("Frontmatter.md") && !env.meta->fileCache("Frontmatter.md")->allTags().isEmpty()) {
        EXPECT_EQ(v->resultPaths(), QStringList{"Frontmatter.md"});
    }

    // copy results
    v->setQuery("strikethrough");
    ASSERT_TRUE(env.waitIdle(v));
    const QString text = v->resultsText();
    EXPECT_TRUE(text.contains("- [[Formatting]]")) << qPrintable(text);
    EXPECT_EQ(v->resultsText(true, SearchView::LinkStyle::None, SearchView::ListPrefix::Numbered).split('\n').first().left(3), "1. ");
    v->copyResults();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), text);
}

TEST(SearchView, ExcludedFilesAreHidden)
{
    NavigationEnv env;
    env.session->vaultConfig()->set("userIgnoreFilters", QJsonArray{"Projects/"});
    SearchView* v = searchView(env);
    v->setQuery("alpha");
    ASSERT_TRUE(env.waitIdle(v));
    for (const QString& p : v->resultPaths())
        EXPECT_FALSE(p.startsWith("Projects/")) << qPrintable(p);
}

TEST(SearchView, KeyboardNavigatesTheResults)
{
    NavigationEnv env;
    SearchView* v = searchView(env);
    v->setQuery("welcome");
    ASSERT_TRUE(env.waitIdle(v));
    ASSERT_GE(v->resultCount(), 2);
    SearchResultsList* list = v->resultsList();
    auto focused = [&] { return std::make_pair(list->focusedRow().group, list->focusedRow().snippet); };

    QTest::keyClick(v->input(), Qt::Key_Down); // from the input into the results
    EXPECT_EQ(env.window->focusWidget(), list);
    EXPECT_EQ(focused(), std::make_pair(0, -1));
    QTest::keyClick(list, Qt::Key_Down);
    EXPECT_EQ(focused(), std::make_pair(0, 0));
    QTest::keyClick(list, Qt::Key_Left); // match -> its file
    EXPECT_EQ(focused(), std::make_pair(0, -1));
    QTest::keyClick(list, Qt::Key_Left); // collapses
    EXPECT_TRUE(list->groups()[0].collapsed);
    QTest::keyClick(list, Qt::Key_Down); // skips the collapsed matches
    EXPECT_EQ(focused(), std::make_pair(1, -1));
    QTest::keyClick(list, Qt::Key_Up);
    QTest::keyClick(list, Qt::Key_Right); // expands
    EXPECT_FALSE(list->groups()[0].collapsed);
    QTest::keyClick(list, Qt::Key_Up); // back to the input
    EXPECT_EQ(env.window->focusWidget(), v->input());

    // Enter on a match opens the note there
    QTest::keyClick(v->input(), Qt::Key_Down);
    QTest::keyClick(list, Qt::Key_Down);
    const QString path = list->groups()[0].path;
    QTest::keyClick(list, Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), path);
}
