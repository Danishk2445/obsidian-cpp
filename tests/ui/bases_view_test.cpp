// The Bases core plugin in a real session and window, on a copy of tests/fixtures/vault-bases:
// opening .base files (no rewrite on open), the table (rows, header sort toggling, column resizing,
// group-by, in-place cell editing writing the note's frontmatter byte-exactly), the toolbar menus,
// search, live updates, "#View" subpaths, parse errors, ```base code blocks and ![[x.base]] embeds
// in notes (and their write-back), the "bases:" commands and the folder menu's "New base".
#include "ui/app_test_support.h"

#include "core/bases/base_file.h"
#include "core/commands/command_registry.h"
#include "editor/embeds/hosted_embeds.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/popout_window.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_embed.h"
#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_plugin.h"
#include "ui/views/bases/bases_table.h"
#include "ui/views/bases/bases_view.h"
#include "ui/views/bases/bases_widgets.h"
#include "ui/views/markdown/properties/property_editors.h"
#include "ui/workspace/file_menu.h"

#include <QApplication>
#include <QClipboard>
#include <QDirIterator>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>

using namespace oc;
using namespace oc::test;
using namespace oc::basesui;

namespace {

ThemeManager* themes()
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = [] {
            auto* m = new ThemeManager(qApp);
            FontSettings fonts;
            fonts.interfaceFamily = Theme::firstInstalled({QStringLiteral("Inter")}, QString());
            fonts.textFamily = fonts.interfaceFamily;
            m->setFonts(fonts);
            m->setMode(ThemeManager::Mode::Dark);
            m->apply();
            return m;
        }();
        return tm;
    }
    return ThemeManager::instance();
}

QString fixtureDir() { return QStringLiteral(OC_FIXTURES_DIR "/vault-bases"); }

QByteArray fixtureBytes(const QString& path) { return readFile(fixtureDir() + u'/' + path); }

// A table grouped by status with a view filter and a limit.
const QByteArray kGrouped = "filters:\n"
                            "  and:\n"
                            "    - file.inFolder(\"Books\")\n"
                            "views:\n"
                            "  - type: table\n"
                            "    name: By status\n"
                            "    groupBy:\n"
                            "      property: status\n"
                            "      direction: ASC\n"
                            "    order:\n"
                            "      - file.name\n"
                            "      - status\n"
                            "      - pages\n"
                            "    summaries:\n"
                            "      pages: Sum\n";

class BasesTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> out;
        QDirIterator it(fixtureDir(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            out.append({QDir(fixtureDir()).relativeFilePath(p), readFile(p)});
        }
        out.append({QStringLiteral("Grouped.base"), kGrouped});
        out.append({QStringLiteral("Broken.base"), QByteArray("views: [\n  - type: table\n")});
        out.append({QStringLiteral("Empty.base"), QByteArray()});
        return out;
    }
    void SetUp() override
    {
        themes();
        AppFixture::SetUp();
        if (HasFatalFailure())
            return;
        commands = new CommandRegistry;
        session->setCommands(commands);
        registerBases(window);
        window->resize(1400, 850);
        QApplication::processEvents();
    }
    void TearDown() override
    {
        AppFixture::TearDown();
        delete commands;
    }

    Workspace* ws() const { return window->workspace(); }
    QByteArray disk(const QString& path) const { return readFile(root + u'/' + path); }

    static bool waitReady(BasesController* c)
    {
        return c && QTest::qWaitFor([c] { return c->isIdle(); }, 5000);
    }
    BasesFileView* openBase(const QString& path)
    {
        TFile* f = vault->file(path);
        if (!f)
            return nullptr;
        ws()->openFile(f, Workspace::NewLeaf::No);
        auto* v = qobject_cast<BasesFileView*>(ws()->activeRootLeaf()->view());
        if (v && !waitReady(v->controller()))
            return nullptr;
        QApplication::processEvents();
        return v;
    }
    static QStringList names(const QList<bases::EntryPtr>& rows)
    {
        QStringList out;
        for (const bases::EntryPtr& e : rows)
            out << QFileInfo(e->path()).completeBaseName();
        return out;
    }
    static QStringList rowNames(BasesController* c) { return names(c->data().rows); }
    static TableLayout* table(BasesController* c) { return qobject_cast<TableLayout*>(c->layoutWidget()); }
    static int rowOf(BasesController* c, const QString& name) { return int(rowNames(c).indexOf(name)); }
    static int columnOf(BasesController* c, const QString& id) { return int(c->data().columns.indexOf(id)); }
    static void click(QWidget* w, const QPoint& pos)
    {
        QTest::mouseMove(w, pos);
        QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, pos);
    }
    // Pump the editor so that its hosted embeds exist and are placed.
    static void pump(EditorView* e)
    {
        for (int i = 0; i < 4; ++i) {
            e->viewport()->repaint();
            QApplication::processEvents();
            if (HostedEmbeds* h = HostedEmbeds::of(e))
                h->sync();
        }
    }
    static QList<BaseEmbedWidget*> embeds(EditorView* e)
    {
        QList<BaseEmbedWidget*> out;
        if (HostedEmbeds* h = HostedEmbeds::of(e))
            for (QWidget* w : h->liveWidgets())
                if (auto* b = qobject_cast<BaseEmbedWidget*>(w))
                    out << b;
        return out;
    }
    QList<BaseEmbedWidget*> waitEmbeds(EditorView* e, int count)
    {
        QList<BaseEmbedWidget*> out;
        (void)QTest::qWaitFor([&] {
            pump(e);
            out = embeds(e);
            if (out.size() != count)
                return false;
            for (BaseEmbedWidget* w : out)
                if (!w->controller()->isIdle())
                    return false;
            return true;
        }, 5000);
        return out;
    }

    CommandRegistry* commands = nullptr;
};

const QStringList kByRating = {"Piranesi", "The Left Hand of Darkness", "Dune", "The Name of the Wind",
                               "Foundation", "Hyperion", "Neuromancer", "Snow Crash"};

} // namespace

// ---------------------------------------------------------------------------------------------
// The .base view
// ---------------------------------------------------------------------------------------------

TEST_F(BasesTest, OpensBaseFilesWithoutRewritingThem)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->viewType(), "bases");
    EXPECT_EQ(v->icon(), "table"); // the view type's icon (Obsidian: layout-list without a view)
    BasesController* c = v->controller();
    EXPECT_EQ(c->viewName(), "All books");
    EXPECT_EQ(c->viewsButton()->label(), "All books");
    ASSERT_TRUE(table(c));
    EXPECT_EQ(rowNames(c), kByRating);
    EXPECT_EQ(c->resultsButton()->label(), "8 results");
    EXPECT_EQ(c->data().columns, (QStringList{"file.name", "note.author", "note.status", "note.rating", "note.pages", "note.genre",
                                             "note.published", "note.owned", "formula.pages_per_star"}));
    EXPECT_TRUE(c->sortButton()->isActive());
    EXPECT_FALSE(c->filterButton()->isActive()); // only the base's filters, none in the view
    EXPECT_TRUE(table(c)->hasFooter());
    EXPECT_EQ(v->state().value("viewName").toString(), "All books");
    QTest::qWait(700);
    EXPECT_FALSE(v->hasPendingSave());
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base"));

    // an empty file shows Obsidian's default view and stays empty
    BasesFileView* empty = openBase("Empty.base");
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->controller()->viewName(), "Table");
    EXPECT_EQ(empty->controller()->data().rows.size(), vault->files().size());
    QTest::qWait(700);
    EXPECT_EQ(disk("Empty.base"), QByteArray());
}

TEST_F(BasesTest, ParseErrorsShowAMessage)
{
    BasesFileView* v = qobject_cast<BasesFileView*>(([&] {
        ws()->openFile(vault->file("Broken.base"), Workspace::NewLeaf::No);
        return ws()->activeRootLeaf()->view();
    })());
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->controller()->hasError());
    EXPECT_EQ(v->controller()->errorText(), "Illegal block entry at line 2, column 3");
    // fixing the file on disk reloads it
    QString err;
    ASSERT_TRUE(fileManager->editFile("Broken.base", [](const Text& t) -> QList<ChangeSpec> {
        return {ChangeSpec{0, t.length(), QStringLiteral("views:\n  - type: list\n    name: Fixed\n")}};
    }, &err)) << err.toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !v->controller()->hasError() && v->controller()->viewName() == "Fixed"; }, 3000));
    ASSERT_TRUE(waitReady(v->controller()));
    EXPECT_EQ(v->controller()->layoutWidget()->type(), "list");
}

TEST_F(BasesTest, HeaderClicksToggleTheSortAndSaveLikeObsidian)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    TableLayout* t = table(c);
    ASSERT_TRUE(t);
    const int pages = columnOf(c, "note.pages");
    ASSERT_GE(pages, 0);
    const QByteArray original = fixtureBytes("Books.base");

    click(t->viewport(), t->headerRect(pages).center());
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(rowNames(c), (QStringList{"Foundation", "Neuromancer", "Piranesi", "The Left Hand of Darkness", "Dune", "Snow Crash",
                                        "Hyperion", "The Name of the Wind"}));
    ASSERT_TRUE(v->hasPendingSave());
    ASSERT_TRUE(v->save());
    QByteArray expected = original;
    expected.replace("    sort:\n      - property: rating\n",
                     "    sort:\n      - property: pages\n        direction: ASC\n      - property: rating\n");
    EXPECT_EQ(disk("Books.base"), expected);

    click(t->viewport(), t->headerRect(pages).center());
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(rowNames(c).first(), "The Name of the Wind");
    ASSERT_TRUE(v->save());
    expected = original;
    expected.replace("    sort:\n      - property: rating\n",
                     "    sort:\n      - property: pages\n        direction: DESC\n      - property: rating\n");
    EXPECT_EQ(disk("Books.base"), expected);

    // the third click removes the sort: the file is Obsidian's original again
    click(t->viewport(), t->headerRect(pages).center());
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(rowNames(c), kByRating);
    ASSERT_TRUE(v->save());
    EXPECT_EQ(disk("Books.base"), original);
}

TEST_F(BasesTest, ColumnResizeIsSavedInTheView)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    TableLayout* t = table(c);
    ASSERT_TRUE(t);
    const int before = t->columnWidths().value(1);
    const QRect h = t->headerRect(1);
    const QPoint from(h.right() - 1, h.center().y());
    const QPoint to = from + QPoint(50, 0);
    QTest::mouseMove(t->viewport(), from);
    QTest::mousePress(t->viewport(), Qt::LeftButton, Qt::NoModifier, from);
    for (int i = 1; i <= 5; ++i)
        QTest::mouseMove(t->viewport(), from + (to - from) * i / 5);
    QTest::mouseRelease(t->viewport(), Qt::LeftButton, Qt::NoModifier, to);
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(t->columnWidths().value(1), before + 50);
    const bases::YamlNode sizes = c->currentView()->option("columnSize");
    ASSERT_TRUE(sizes.isMap());
    ASSERT_TRUE(sizes.find("note.author"));
    EXPECT_EQ(sizes.find("note.author")->toVariant().toInt(), before + 50);
    ASSERT_TRUE(v->save());
    EXPECT_TRUE(disk("Books.base").contains(QStringLiteral("    columnSize:\n      note.author: %1\n").arg(before + 50).toUtf8()));
}

TEST_F(BasesTest, GroupsRowsAndSummarizesEachGroup)
{
    BasesFileView* v = openBase("Grouped.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    const bases::ViewData& d = c->data();
    ASSERT_EQ(d.groups.size(), 4);
    QStringList keys;
    QList<int> counts;
    for (const bases::ViewGroup& g : d.groups) {
        keys << g.key.toString();
        counts << int(g.entries.size());
    }
    EXPECT_EQ(keys, (QStringList{"abandoned", "read", "reading", "to read"}));
    EXPECT_EQ(counts, (QList<int>{1, 3, 2, 2}));
    TableLayout* t = table(c);
    ASSERT_TRUE(t);
    for (int g = 1; g < 4; ++g)
        EXPECT_GT(t->groupTop(g), t->groupTop(g - 1));
    EXPECT_TRUE(t->hasFooter());
    // the rows of a group are in the view's sort (file name by default)
    EXPECT_EQ(names(d.groups[1].entries), (QStringList{"Dune", "Neuromancer", "The Left Hand of Darkness"}));

    // group descending, then no grouping (written back like Obsidian)
    c->editView([](bases::BaseView& view) { view.groupBy->direction = QStringLiteral("DESC"); });
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(c->data().groups.first().key.toString(), "to read");
    c->editView([](bases::BaseView& view) { view.groupBy.reset(); });
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(c->data().groups.size(), 1);
    ASSERT_TRUE(v->save());
    QByteArray expected = kGrouped;
    expected.replace("    groupBy:\n      property: status\n      direction: ASC\n", "");
    EXPECT_EQ(disk("Grouped.base"), expected);
}

TEST_F(BasesTest, CellEditsWriteTheNotesFrontmatterByteExactly)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    TableLayout* t = table(c);
    ASSERT_TRUE(t);
    const QByteArray dune = fixtureBytes("Books/Dune.md");

    // a number: the property editor in place, Enter commits
    const int pages = columnOf(c, "note.pages");
    ASSERT_TRUE(t->editCell({0, rowOf(c, "Dune"), pages}));
    auto* num = qobject_cast<NumberPropertyEditor*>(t->editor());
    ASSERT_TRUE(num);
    num->focusEditor(QStringLiteral("both"));
    QTest::keyClicks(num->input(), "413");
    QTest::keyClick(num->input(), Qt::Key_Return);
    QByteArray expected = dune;
    expected.replace("pages: 412\n", "pages: 413\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return disk("Books/Dune.md") == expected; }, 3000)) << disk("Books/Dune.md").toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        const int r = rowOf(c, "Dune");
        return r >= 0 && c->data().rows[r]->valueOrError("note.pages").toString() == "413";
    }, 3000));

    // a checkbox toggles on click
    ASSERT_TRUE(waitReady(c));
    t->closeEditor();
    const int owned = columnOf(c, "note.owned");
    click(t->viewport(), t->cellRect(0, rowOf(c, "Dune"), owned).center());
    expected.replace("owned: true\n", "owned: false\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return disk("Books/Dune.md") == expected; }, 3000)) << disk("Books/Dune.md").toStdString();

    // text
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && !c->data().rows.isEmpty(); }, 3000));
    const QByteArray piranesi = fixtureBytes("Books/Piranesi.md");
    ASSERT_TRUE(t->editCell({0, rowOf(c, "Piranesi"), columnOf(c, "note.status")}));
    auto* text = qobject_cast<TextPropertyEditor*>(t->editor());
    ASSERT_TRUE(text);
    text->focusEditor(QStringLiteral("both"));
    QTest::keyClicks(text->input(), "reading");
    QTest::keyClick(text->input(), Qt::Key_Return);
    QByteArray expectedP = piranesi;
    expectedP.replace("status: to read\n", "status: reading\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return disk("Books/Piranesi.md") == expectedP; }, 3000)) << disk("Books/Piranesi.md").toStdString();

    // nothing else was written
    QTest::qWait(700);
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base"));
    EXPECT_EQ(disk("Books/Foundation.md"), fixtureBytes("Books/Foundation.md"));
}

TEST_F(BasesTest, ResultsFollowTheVault)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    QString err;
    ASSERT_TRUE(fileManager->editFile("Books/Snow Crash.md", [](const Text& t) -> QList<ChangeSpec> {
        const QString s = t.toString();
        const qsizetype at = s.indexOf(QStringLiteral("rating: 3\n"));
        return {ChangeSpec{at, at + 9, QStringLiteral("rating: 6")}};
    }, &err)) << err.toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && rowNames(c).value(0) == "Snow Crash"; }, 3000)) << rowNames(c).join(", ").toStdString();

    // a new tagged note shows up; untagging removes it again
    ASSERT_TRUE(vault->createFile("Books/New.md", "---\ntags: [book]\nrating: 1\n---\n", &err)) << err.toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && c->data().rows.size() == 9; }, 3000));
    EXPECT_EQ(rowNames(c).last(), "New");
    EXPECT_EQ(c->resultsButton()->label(), "9 results");
    ASSERT_TRUE(fileManager->editFile("Books/New.md", [](const Text& t) -> QList<ChangeSpec> {
        return {ChangeSpec{0, t.length(), QStringLiteral("---\nrating: 1\n---\n")}};
    }, &err));
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && c->data().rows.size() == 8; }, 3000));

    // the base changed on disk (another editor): re-read
    ASSERT_TRUE(fileManager->editFile("Books.base", [](const Text& t) -> QList<ChangeSpec> {
        const QString s = t.toString();
        const qsizetype at = s.indexOf(QStringLiteral("    summaries:\n      pages: Sum\n"));
        return {ChangeSpec{at, at, QStringLiteral("    limit: 3\n")}};
    }, &err));
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && c->data().rows.size() == 3; }, 3000));
    EXPECT_EQ(c->resultsButton()->label(), "3 results");
    EXPECT_TRUE(c->resultsButton()->isActive());
}

TEST_F(BasesTest, SearchNarrowsTheRows)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    c->toggleSearch();
    c->setSearchText("wind");
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && c->data().rows.size() == 1; }, 3000));
    EXPECT_EQ(rowNames(c), QStringList{"The Name of the Wind"});
    c->setSearchText(QString());
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->isIdle() && c->data().rows.size() == 8; }, 3000));
    QTest::qWait(700);
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base")); // searching isn't an edit
}

TEST_F(BasesTest, ToolbarMenusOpenAndClose)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();
    using Opener = MenuPanel* (BasesController::*)();
    for (Opener open : {&BasesController::openViewsMenu, &BasesController::openResultsMenu, &BasesController::openSortMenu,
                        &BasesController::openFilterMenu, &BasesController::openPropertiesMenu}) {
        QPointer<MenuPanel> panel = (c->*open)();
        ASSERT_TRUE(panel);
        QApplication::processEvents();
        EXPECT_TRUE(panel->isOpen());
        EXPECT_EQ(MenuPanel::openPanel(window), panel.data());
        QTest::keyClick(panel, Qt::Key_Escape);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !panel || !panel->isOpen(); }, 2000));
    }
    QPointer<MenuPanel> cfg = c->openConfigureView("All books");
    ASSERT_TRUE(cfg);
    QApplication::processEvents();
    EXPECT_TRUE(cfg->isOpen());
    cfg->close();
    QTest::qWait(700);
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base")); // looking at menus isn't an edit
}

TEST_F(BasesTest, ViewSubpathsAndStateSelectTheView)
{
    ws()->openFile(vault->file("Welcome.md"), Workspace::NewLeaf::No);
    MarkdownView::openLinkText(ws(), "Books.base#Gallery", 0, "Welcome.md", false);
    auto* v = qobject_cast<BasesFileView*>(ws()->activeRootLeaf()->view());
    ASSERT_TRUE(v);
    ASSERT_TRUE(waitReady(v->controller()));
    EXPECT_EQ(v->controller()->viewName(), "Gallery");
    EXPECT_EQ(v->controller()->layoutWidget()->type(), "cards");
    EXPECT_EQ(v->state().value("viewName").toString(), "Gallery");
    v->controller()->selectView("Reading list");
    ASSERT_TRUE(waitReady(v->controller()));
    EXPECT_EQ(v->controller()->layoutWidget()->type(), "list");
    EXPECT_EQ(v->controller()->data().groups.size(), 4);
    EXPECT_EQ(v->state().value("viewName").toString(), "Reading list");
    QTest::qWait(700);
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base")); // switching views isn't an edit

    // another base in the same tab shows its first view; so does the first one opened again
    BasesFileView* other = openBase("Grouped.base");
    ASSERT_EQ(other, v);
    EXPECT_EQ(v->controller()->viewName(), "By status");
    ASSERT_EQ(openBase("Books.base"), v);
    EXPECT_EQ(v->controller()->viewName(), "All books");
    // the state's view wins
    ws()->activeRootLeaf()->setViewState(QJsonObject{{"type", "bases"}, {"state", QJsonObject{{"file", "Grouped.base"}}}});
    ws()->activeRootLeaf()->setViewState(
        QJsonObject{{"type", "bases"}, {"state", QJsonObject{{"file", "Books.base"}, {"viewName", "Gallery"}}}});
    ASSERT_TRUE(waitReady(v->controller()));
    EXPECT_EQ(v->controller()->viewName(), "Gallery");
}

// ---------------------------------------------------------------------------------------------
// Bases in notes
// ---------------------------------------------------------------------------------------------

TEST_F(BasesTest, CodeBlocksAndEmbedsRenderInNotes)
{
    MarkdownView* mv = open("Reading.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setSelection(Selection::cursor(0));
    const QList<BaseEmbedWidget*> live = waitEmbeds(e, 2);
    ASSERT_EQ(live.size(), 2);
    BaseEmbedWidget* block = live[0]->isCodeBlock() ? live[0] : live[1];
    BaseEmbedWidget* file = live[0]->isCodeBlock() ? live[1] : live[0];
    ASSERT_TRUE(block->isCodeBlock());
    ASSERT_FALSE(file->isCodeBlock());
    EXPECT_EQ(block->controller()->viewName(), "Now");
    EXPECT_EQ(rowNames(block->controller()), (QStringList{"Hyperion", "The Name of the Wind"}));
    EXPECT_EQ(file->controller()->viewName(), "Gallery");
    EXPECT_EQ(file->controller()->layoutWidget()->type(), "cards");
    EXPECT_EQ(file->controller()->data().rows.size(), 8);
    // embeds are as tall as their content
    EXPECT_GE(block->height(), block->controller()->preferredHeight());

    // Reading view shows them too
    mv->setMode(EditorView::Mode::Reading);
    EXPECT_EQ(waitEmbeds(e, 2).size(), 2);
    QTest::qWait(500);
    EXPECT_EQ(disk("Reading.md"), fixtureBytes("Reading.md"));
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base"));
}

TEST_F(BasesTest, EditingACodeBlockRewritesOnlyItsFence)
{
    MarkdownView* mv = open("Reading.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setSelection(Selection::cursor(0));
    const QList<BaseEmbedWidget*> live = waitEmbeds(e, 2);
    ASSERT_EQ(live.size(), 2);
    BaseEmbedWidget* block = live[0]->isCodeBlock() ? live[0] : live[1];
    block->controller()->editView([](bases::BaseView& v) { v.setSortProperty(QStringLiteral("note.pages"), QStringLiteral("DESC")); });
    block->flush();
    QString expected = QString::fromUtf8(fixtureBytes("Reading.md"));
    expected.replace("      - pages\n```", "      - pages\n    sort:\n      - property: pages\n        direction: DESC\n```");
    EXPECT_EQ(e->document()->text().toString(), expected);
    // the rewritten block is still a live base, sorted
    const QList<BaseEmbedWidget*> after = waitEmbeds(e, 2);
    ASSERT_EQ(after.size(), 2);
    BaseEmbedWidget* block2 = after[0]->isCodeBlock() ? after[0] : after[1];
    EXPECT_EQ(rowNames(block2->controller()), (QStringList{"The Name of the Wind", "Hyperion"}));
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base"));
}

TEST_F(BasesTest, EditingAFileEmbedWritesTheBaseFile)
{
    MarkdownView* mv = open("Reading.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setSelection(Selection::cursor(0));
    const QList<BaseEmbedWidget*> live = waitEmbeds(e, 2);
    ASSERT_EQ(live.size(), 2);
    BaseEmbedWidget* file = live[0]->isCodeBlock() ? live[1] : live[0];
    file->controller()->editView([](bases::BaseView& v) { v.limit = 2; });
    file->flush();
    QByteArray expected = fixtureBytes("Books.base");
    expected.replace("    image: note.cover\n", "    limit: 2\n    image: note.cover\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return disk("Books.base") == expected; }, 3000)) << disk("Books.base").toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return file->controller()->isIdle() && file->controller()->data().rows.size() == 2; }, 3000));
    EXPECT_EQ(e->document()->text().toString().toUtf8(), fixtureBytes("Reading.md")); // the note is untouched
}

// ---------------------------------------------------------------------------------------------
// Commands and menus
// ---------------------------------------------------------------------------------------------

TEST_F(BasesTest, NewFileCommandCreatesAnEmptyBase)
{
    ASSERT_TRUE(commands->execute("bases:new-file"));
    ASSERT_TRUE(vault->file("Untitled.base"));
    auto* v = qobject_cast<BasesFileView*>(ws()->activeRootLeaf()->view());
    ASSERT_TRUE(v);
    ASSERT_TRUE(waitReady(v->controller()));
    EXPECT_EQ(v->file()->path(), "Untitled.base");
    EXPECT_EQ(v->controller()->viewName(), "Table");
    ASSERT_TRUE(commands->execute("bases:new-file"));
    EXPECT_TRUE(vault->file("Untitled 1.base"));
    QTest::qWait(700);
    EXPECT_EQ(disk("Untitled.base"), QByteArray());
    EXPECT_EQ(disk("Untitled 1.base"), QByteArray());

    // the folder menu of the file explorer
    QList<MenuEntry> items;
    FileMenuHooks::collect(items, vault->folder("Books"), "file-explorer-context-menu", nullptr, window);
    const auto it = std::find_if(items.begin(), items.end(), [](const MenuEntry& m) { return m.title == "New base"; });
    ASSERT_NE(it, items.end());
    it->run();
    EXPECT_TRUE(vault->file("Books/Untitled.base"));
}

TEST_F(BasesTest, InsertCommandEmbedsANewBase)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    commands->setEditorProvider([mv] { return EditorContext{mv->editor(), mv}; });
    EditorView* e = mv->editor();
    const Text& t = e->document()->text();
    e->setSelection(Selection::cursor(t.length()));
    ASSERT_TRUE(commands->execute("bases:insert"));
    ASSERT_TRUE(vault->file("Untitled.base"));
    EXPECT_EQ(e->document()->text().toString(), "# Welcome\n\nOpen [[Books.base]].\n![[Untitled.base]]\n");
    e->setSelection(Selection::cursor(0));
    const QList<BaseEmbedWidget*> live = waitEmbeds(e, 1);
    ASSERT_EQ(live.size(), 1);
    EXPECT_EQ(live[0]->controller()->viewName(), "Table");
}

TEST_F(BasesTest, ViewCommands)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    BasesController* c = v->controller();

    // Switch view...
    ASSERT_TRUE(commands->execute("bases:change-view"));
    auto* modal = window->findChild<SuggestModal*>("ocBasesSwitchView");
    ASSERT_TRUE(modal);
    modal->setQuery("gal");
    ASSERT_FALSE(modal->suggestions().isEmpty());
    EXPECT_EQ(modal->suggestions().first().title, "Gallery");
    modal->chooseSelected();
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(c->viewName(), "Gallery");
    EXPECT_FALSE(commands->isAvailable("bases:copy-table")); // cards
    c->selectView("All books");
    ASSERT_TRUE(waitReady(c));

    // Copy table to clipboard
    ASSERT_TRUE(commands->execute("bases:copy-table"));
    const QString tsv = QApplication::clipboard()->text();
    const QStringList lines = tsv.split(u'\n');
    ASSERT_GE(lines.size(), 9);
    EXPECT_TRUE(lines[0].startsWith("file name\tAuthor\tstatus\trating\tpages")) << lines[0].toStdString();
    EXPECT_TRUE(lines[1].startsWith("Piranesi\t")) << lines[1].toStdString();

    // Add view: a new table view, selected and written
    ASSERT_TRUE(commands->execute("bases:add-view"));
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(c->viewName(), "View");
    EXPECT_EQ(c->base().views.size(), 4);
    if (MenuPanel* p = MenuPanel::openPanel(window))
        p->close();
    ASSERT_TRUE(v->save());
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base") + "  - type: table\n    name: View\n");

    // Add item: a note with the properties the filters imply
    const int before = int(vault->files().size());
    ASSERT_TRUE(commands->execute("bases:add-item"));
    ASSERT_EQ(int(vault->files().size()), before + 1);
    TFile* created = vault->file("Untitled.md");
    ASSERT_TRUE(created);
    EXPECT_EQ(disk("Untitled.md"), "---\ntags:\n  - book\n---\n");
}

TEST_F(BasesTest, WorksInPopoutWindows)
{
    BasesFileView* v = openBase("Books.base");
    ASSERT_TRUE(v);
    WorkspaceLeaf* leaf = ws()->activeRootLeaf();
    PopoutWindow* pop = ws()->moveLeafToPopout(leaf, QRect(100, 100, 900, 640));
    ASSERT_TRUE(pop);
    ASSERT_TRUE(QTest::qWaitForWindowExposed(pop));
    auto* moved = qobject_cast<BasesFileView*>(leaf->view());
    ASSERT_TRUE(moved);
    BasesController* c = moved->controller();
    ASSERT_TRUE(waitReady(c));
    EXPECT_EQ(rowNames(c), kByRating);
    // toolbar menus open in the pop-out's own overlay layer
    QPointer<MenuPanel> panel = c->openSortMenu();
    ASSERT_TRUE(panel);
    QApplication::processEvents();
    EXPECT_EQ(panel->parentWidget(), pop->overlay());
    EXPECT_EQ(MenuPanel::openPanel(pop), panel.data());
    EXPECT_EQ(MenuPanel::openPanel(window), nullptr);
    QTest::keyClick(panel, Qt::Key_Escape);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !panel || !panel->isOpen(); }, 2000));
    // the folder menu hook finds the main window from a widget in the pop-out
    QList<MenuEntry> items;
    FileMenuHooks::collect(items, vault->folder("Books"), "file-explorer-context-menu", nullptr, c);
    const auto it = std::find_if(items.begin(), items.end(), [](const MenuEntry& m) { return m.title == "New base"; });
    ASSERT_NE(it, items.end());
    it->run();
    EXPECT_TRUE(vault->file("Books/Untitled.base"));
    QTest::qWait(700);
    EXPECT_EQ(disk("Books.base"), fixtureBytes("Books.base"));
}
