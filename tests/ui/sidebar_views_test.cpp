// The sidebar views in a real window: the generic TreeList, Backlinks (linked / unlinked mentions, the
// Link action), Outgoing links, Tags, Outline, Bookmarks (+ modal and commands), All properties, the
// sidebar commands, and screenshots (only with OC_SHOT_DIR).
#include "app_test_support.h"
#include "core/bookmarks/bookmarks.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/sidebar/all_properties_view.h"
#include "ui/views/sidebar/backlink_view.h"
#include "ui/views/sidebar/bookmarks_view.h"
#include "ui/views/sidebar/outgoing_link_view.h"
#include "ui/views/sidebar/outline_view.h"
#include "ui/views/sidebar/sidebar_registration.h"
#include "ui/views/sidebar/tag_view.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/tree_list.h"
#include "ui/workspace/file_menu.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollBar>

#include <ostream>

using namespace oc;
using namespace oc::test;

inline void PrintTo(const QString& s, std::ostream* os) { *os << '"' << s.toStdString() << '"'; }

namespace {

void sendMouse(QWidget* w, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, mods);
    QApplication::sendEvent(w, &ev);
}

void click(QWidget* w, QPoint pos, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, mods);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
}

void key(QWidget* w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(w, &press);
}

TreeList::NodePtr item(const QString& id, const QString& title, bool collapsible = false)
{
    auto n = std::make_unique<TreeList::Node>();
    n->id = id;
    n->title = title;
    n->collapsible = collapsible;
    return n;
}

int rowWithTitle(TreeList* t, const QString& title)
{
    for (int i = 0; i < t->rowCount(); ++i)
        if (t->nodeAt(i)->title == title)
            return i;
    return -1;
}

QStringList trimmedTitles(TreeList* t)
{
    QStringList out;
    for (const QString& s : t->visibleTitles())
        out << s;
    return out;
}

void ensureTheme()
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = new ThemeManager(qApp);
        tm->setMode(ThemeManager::Mode::Dark);
        tm->apply();
    }
}

} // namespace

// ---- TreeList ---------------------------------------------------------------------------------------

class TreeListTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureTheme();
        tree = std::make_unique<TreeList>();
        tree->resize(300, 400);
        tree->show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(tree.get()));
        std::vector<TreeList::NodePtr> roots;
        auto a = item("a", "Alpha", true);
        a->add(item("a1", "One"));
        TreeList::Node* a2 = a->add(item("a2", "Two", true));
        a2->add(item("a2x", "Deep"));
        roots.push_back(std::move(a));
        roots.push_back(item("b", "Beta"));
        roots.back()->flair = "7";
        tree->setRoots(std::move(roots));
    }
    std::unique_ptr<TreeList> tree;
};

TEST_F(TreeListTest, RowsCollapseAndState)
{
    EXPECT_EQ(trimmedTitles(tree.get()), (QStringList{"Alpha", "  One", "  Two", "    Deep", "Beta"}));
    // the chevron toggles
    const QRect chevron = tree->chevronRect(0);
    ASSERT_FALSE(chevron.isNull());
    click(tree->viewport(), chevron.center());
    EXPECT_EQ(trimmedTitles(tree.get()), (QStringList{"Alpha", "Beta"}));
    click(tree->viewport(), tree->chevronRect(0).center());
    EXPECT_EQ(tree->rowCount(), 5);
    // a rebuild keeps expansion by id
    tree->setCollapsed(tree->findNode("a2"), true);
    std::vector<TreeList::NodePtr> roots;
    auto a = item("a", "Alpha", true);
    TreeList::Node* a2 = a->add(item("a2", "Two", true));
    a2->add(item("a2x", "Deep"));
    roots.push_back(std::move(a));
    tree->setRoots(std::move(roots));
    EXPECT_EQ(trimmedTitles(tree.get()), (QStringList{"Alpha", "  Two"}));
    // collapse all / expand all
    tree->setAllCollapsed(false);
    EXPECT_EQ(tree->rowCount(), 3);
    tree->setAllCollapsed(true);
    EXPECT_EQ(tree->rowCount(), 1);
}

TEST_F(TreeListTest, ClicksKeysAndSelection)
{
    QList<QString> activated;
    tree->handlers().activate = [&](TreeList::Node* n, Qt::MouseButton, Qt::KeyboardModifiers, bool keyboard) {
        activated << n->id + (keyboard ? "!" : "");
    };
    click(tree->viewport(), tree->rowRect(rowWithTitle(tree.get(), "Beta")).center());
    EXPECT_EQ(activated, (QList<QString>{"b"}));
    // keyboard: focus moves down from the top, Enter activates, ← collapses the parent, → expands
    tree->setFocus();
    key(tree.get(), Qt::Key_Down);
    EXPECT_EQ(tree->focusedNode()->id, "a");
    key(tree.get(), Qt::Key_Down);
    EXPECT_EQ(tree->focusedNode()->id, "a1");
    key(tree.get(), Qt::Key_Return);
    EXPECT_EQ(activated.last(), "a1!");
    key(tree.get(), Qt::Key_Left); // a leaf: collapses its parent and focuses it
    EXPECT_EQ(tree->focusedNode()->id, "a");
    EXPECT_TRUE(tree->findNode("a")->collapsed);
    key(tree.get(), Qt::Key_Right);
    EXPECT_FALSE(tree->findNode("a")->collapsed);
    // selection (Alt+click toggles, Shift+click extends) when enabled
    tree->setSelectionEnabled(true);
    activated.clear();
    click(tree->viewport(), tree->rowRect(rowWithTitle(tree.get(), "One")).center(), Qt::AltModifier);
    click(tree->viewport(), tree->rowRect(rowWithTitle(tree.get(), "Beta")).center(), Qt::ShiftModifier);
    EXPECT_TRUE(activated.isEmpty());
    QStringList sel;
    for (TreeList::Node* n : tree->selectedNodes())
        sel << n->id;
    EXPECT_EQ(sel, (QStringList{"a1", "a2", "a2x", "b"}));
    key(tree.get(), Qt::Key_Escape);
    EXPECT_TRUE(tree->selectedNodes().isEmpty());
    QList<TreeList::Node*> removed;
    tree->handlers().remove = [&](const QList<TreeList::Node*>& nodes) { removed = nodes; };
    tree->setFocusedNode(tree->findNode("b"));
    key(tree.get(), Qt::Key_Delete);
    ASSERT_EQ(removed.size(), 1);
    EXPECT_EQ(removed.first()->id, "b");
}

TEST_F(TreeListTest, DragToReorder)
{
    tree->setDragEnabled(true);
    TreeList::Node* dropped = nullptr;
    TreeList::DropPlacement where = TreeList::DropPlacement::Inside;
    QString moved;
    tree->handlers().drop = [&](const QList<TreeList::Node*>& nodes, TreeList::Node* target, TreeList::DropPlacement p) {
        moved = nodes.first()->id;
        dropped = target;
        where = p;
    };
    const QRect beta = tree->rowRect(rowWithTitle(tree.get(), "Beta"));
    const QRect one = tree->rowRect(rowWithTitle(tree.get(), "One"));
    sendMouse(tree->viewport(), QEvent::MouseButtonPress, beta.center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(tree->viewport(), QEvent::MouseMove, QPoint(one.center().x(), one.top() + 2), Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(tree->isDragging());
    EXPECT_EQ(tree->dropTarget(), tree->findNode("a1"));
    EXPECT_EQ(tree->dropPlacement(), TreeList::DropPlacement::Before);
    sendMouse(tree->viewport(), QEvent::MouseButtonRelease, QPoint(one.center().x(), one.top() + 2), Qt::LeftButton, Qt::NoButton);
    EXPECT_FALSE(tree->isDragging());
    EXPECT_EQ(moved, "b");
    EXPECT_EQ(dropped ? dropped->id : QString(), "a1");
    EXPECT_EQ(where, TreeList::DropPlacement::Before);
    // onto the middle of a collapsible row: into it; onto itself: nothing
    const QRect two = tree->rowRect(rowWithTitle(tree.get(), "Two"));
    sendMouse(tree->viewport(), QEvent::MouseButtonPress, tree->rowRect(rowWithTitle(tree.get(), "Beta")).center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(tree->viewport(), QEvent::MouseMove, two.center(), Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(tree->dropPlacement(), TreeList::DropPlacement::Inside);
    tree->cancelDrag();
    sendMouse(tree->viewport(), QEvent::MouseButtonRelease, two.center(), Qt::LeftButton, Qt::NoButton);
}

TEST_F(TreeListTest, MatchRowsAndButtons)
{
    auto content = std::make_shared<const QString>("some text with a match in it\nand a second line that is long enough to wrap");
    std::vector<TreeList::NodePtr> roots;
    auto file = item("f", "File", true);
    TreeList::Node* m = file->add(TreeList::Kind::Match, "m1");
    m->content = content;
    m->from = 0;
    m->to = int(content->size());
    m->matches = {{17, 22}};
    m->button = "Link";
    m->moreAfter = true;
    m->chips = {"Target"};
    roots.push_back(std::move(file));
    auto msg = std::make_unique<TreeList::Node>();
    msg->kind = TreeList::Kind::Message;
    msg->title = "Nothing";
    roots.push_back(std::move(msg));
    tree->setRoots(std::move(roots));
    ASSERT_EQ(tree->rowCount(), 3);
    const QRect item = tree->rowRect(0);
    const QRect match = tree->rowRect(1);
    EXPECT_GT(match.height(), item.height()); // two wrapped lines + padding + a chip row
    EXPECT_GE(match.top(), item.bottom() + 4); // the matches box margin
    int buttons = 0, chips = 0;
    tree->handlers().button = [&](TreeList::Node*) { ++buttons; };
    tree->handlers().chip = [&](TreeList::Node*, int) { ++chips; };
    // buttons only react under the pointer (they're drawn on hover)
    sendMouse(tree->viewport(), QEvent::MouseMove, match.center(), Qt::NoButton, Qt::NoButton);
    click(tree->viewport(), tree->buttonRect(1).center());
    click(tree->viewport(), tree->chipRect(1, 0).center());
    EXPECT_EQ(buttons, 1);
    EXPECT_EQ(chips, 1);
    EXPECT_FALSE(tree->moreButtonRect(1, false).isNull());
    EXPECT_TRUE(tree->moreButtonRect(1, true).isNull());
}

// ---- the views in a window --------------------------------------------------------------------------

class SidebarTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {
            {"Target.md", "---\naliases: [Tgt]\ntags: [project/alpha]\nstatus: draft\n---\n# Target\n\nBody text.\n## Section A\nText\n### Sub\nMore\n## Section B\nEnd\n"},
            {"Linker.md", "---\nrelated: \"[[Target]]\"\nnext: \"[[Missing]]\"\ntags: [project/beta, misc]\nstatus: done\n---\nThis mentions Target in plain text and links [[Target]].\n- an item about tgt\n"},
            {"Other.md", "---\ncount: 3\n---\nNothing here about targets. Target again.\n"},
            {"Folder/Deep.md", "Deep note mentions target.\n| a | target |\n|---|---|\n"},
        };
    }
    void SetUp() override
    {
        ensureTheme();
        AppFixture::SetUp();
        registerSidebarViews(*registry);
        commands = std::make_unique<CommandRegistry>();
        session->setCommands(commands.get());
        registerSidebarCommands(*commands, window);
        commands->setEditorProvider([this]() -> EditorContext {
            WorkspaceLeaf* l = window->workspace()->activeRootLeaf();
            auto* mv = l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
            return mv ? EditorContext{mv->editor(), mv} : EditorContext{};
        });
        globalSearchHooks().openSearch = [this](MainWindow*, const QString& q) { searched << q; };
        globalSearchHooks().currentQuery = [this](MainWindow*) { return currentQuery; };
    }
    void TearDown() override
    {
        globalSearchHooks() = GlobalSearchHooks{};
        session->setCommands(nullptr);
        AppFixture::TearDown();
        commands.reset();
    }
    template <typename T> T* side(const char* type, Workspace::Side s = Workspace::Side::Right)
    {
        WorkspaceLeaf* l = window->workspace()->ensureSideLeaf(QString::fromLatin1(type), s, true);
        QApplication::processEvents();
        return l ? qobject_cast<T*>(l->view()) : nullptr;
    }
    static void settle()
    {
        for (int i = 0; i < 5; ++i) {
            QApplication::processEvents();
            QTest::qWait(20);
        }
    }
    std::unique_ptr<CommandRegistry> commands;
    QStringList searched;
    QString currentQuery;
};

TEST_F(SidebarTest, BacklinksLinkedAndUnlinked)
{
    open("Target.md");
    auto* view = side<BacklinkView>("backlink");
    ASSERT_TRUE(view);
    BacklinkPane* pane = view->pane();
    EXPECT_EQ(view->displayText(), "Backlinks");
    ASSERT_TRUE(QTest::qWaitFor([&] { return pane->linkedFiles().contains("Linker.md"); }, 5000));
    EXPECT_EQ(pane->linkedFiles(), (QStringList{"Linker.md"}));
    EXPECT_GE(pane->linkedCount(), 1); // the property link (and the body link once inline links are parsed)
    settle();
    QStringList titles = pane->tree()->visibleTitles();
    EXPECT_EQ(titles.value(0), "Linked mentions");
    EXPECT_TRUE(titles.contains("Linker"));
    EXPECT_EQ(titles.last(), "Unlinked mentions"); // collapsed by default
    // unlinked mentions: the name / aliases as whole words, case-insensitive
    pane->setUnlinkedCollapsed(false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->isSearchingUnlinked(); }, 5000));
    settle();
    EXPECT_EQ(pane->unlinkedFiles(), (QStringList{"Folder/Deep.md", "Linker.md", "Other.md"}));
    titles = pane->tree()->visibleTitles();
    EXPECT_TRUE(titles.contains("Deep"));
    EXPECT_FALSE(titles.contains("No unlinked mentions found."));
    // the state keys round-trip
    const QJsonObject st = view->state();
    for (const char* k : {"file", "collapseAll", "extraContext", "sortOrder", "showSearch", "searchQuery", "backlinkCollapsed", "unlinkedCollapsed"})
        EXPECT_TRUE(st.contains(QString::fromLatin1(k))) << k;
    EXPECT_EQ(st.value("unlinkedCollapsed").toBool(), false);
    pane->setState(QJsonObject{{"collapseAll", true}, {"sortOrder", "alphabeticalReverse"}, {"extraContext", true}});
    settle();
    EXPECT_EQ(pane->unlinkedFiles(), (QStringList{"Other.md", "Linker.md", "Folder/Deep.md"}));
    EXPECT_TRUE(pane->collapseButton()->isActive());
    for (TreeList::Node* n : pane->tree()->allNodes()) {
        if (n->kind == TreeList::Kind::Item) {
            EXPECT_TRUE(n->collapsed);
        }
    }
}

TEST_F(SidebarTest, BacklinksBodyLinks)
{
    const md::CachedMetadata* m = meta->fileCache("Linker.md");
    ASSERT_TRUE(m);
    if (m->links.isEmpty())
        GTEST_SKIP() << "inline links are not parsed yet";
    open("Target.md");
    auto* view = side<BacklinkView>("backlink");
    ASSERT_TRUE(QTest::qWaitFor([&] { return view->pane()->linkedCount() >= 2; }, 5000));
}

TEST_F(SidebarTest, BacklinksFilterAndLinkAction)
{
    open("Target.md");
    auto* view = side<BacklinkView>("backlink");
    BacklinkPane* pane = view->pane();
    pane->setUnlinkedCollapsed(false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->isSearchingUnlinked() && pane->unlinkedFiles().size() == 3; }, 5000));
    // the search filter narrows both sections
    pane->header()->setSearchShown(true);
    pane->setSearchQuery("path:Folder");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->isSearchingUnlinked(); }, 5000));
    settle();
    EXPECT_EQ(pane->unlinkedFiles(), (QStringList{"Folder/Deep.md"}));
    EXPECT_TRUE(pane->linkedFiles().isEmpty());
    pane->setSearchQuery(QString());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->isSearchingUnlinked() && pane->unlinkedFiles().size() == 3; }, 5000));
    // "Link": the mention becomes [[Target]]; the rest of the file stays byte-identical
    const QByteArray before = readFile(root + "/Other.md");
    ASSERT_TRUE(pane->linkUnlinkedMention("Other.md", 0));
    const QByteArray after = readFile(root + "/Other.md");
    EXPECT_EQ(after, QByteArray("---\ncount: 3\n---\nNothing here about targets. [[Target]] again.\n"));
    EXPECT_NE(before, after);
    // in a table the link's pipe would be escaped; here a plain [[Target|target]] keeps the case
    ASSERT_TRUE(pane->linkUnlinkedMention("Folder/Deep.md", 0));
    // FileManager::generateMarkdownLink drops a display text that only differs from the link in case
    EXPECT_EQ(readFile(root + "/Folder/Deep.md"), QByteArray("Deep note mentions [[target]].\n| a | target |\n|---|---|\n"));
}

TEST_F(SidebarTest, BacklinksFollowChanges)
{
    open("Target.md");
    auto* view = side<BacklinkView>("backlink");
    BacklinkPane* pane = view->pane();
    pane->setUnlinkedCollapsed(false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->isSearchingUnlinked() && pane->unlinkedFiles().size() == 3; }, 5000));
    // a new note mentioning the target shows up (re-checked by the search engine)
    QString err;
    ASSERT_TRUE(vault->createFile("New.md", "A new note about Target.\n", &err)) << qPrintable(err);
    ASSERT_TRUE(QTest::qWaitFor([&] { return pane->unlinkedFiles().contains("New.md"); }, 5000));
    // linking it moves it to the linked mentions (once its links are parsed)
    ASSERT_TRUE(pane->linkUnlinkedMention("New.md", 0));
    EXPECT_EQ(readFile(root + "/New.md"), QByteArray("A new note about [[Target]].\n"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->unlinkedFiles().contains("New.md") && !pane->isSearchingUnlinked(); }, 5000));
    if (!meta->fileCache("Linker.md")->links.isEmpty()) {
        EXPECT_TRUE(QTest::qWaitFor([&] { return pane->linkedFiles().contains("New.md"); }, 5000));
    }
    // a deleted note disappears
    ASSERT_TRUE(vault->trash(vault->file("Other.md"), Vault::TrashMode::Permanent, &err)) << qPrintable(err);
    EXPECT_TRUE(QTest::qWaitFor([&] { return !pane->unlinkedFiles().contains("Other.md"); }, 5000));
}

TEST_F(SidebarTest, BacklinksClickOpensMatch)
{
    open("Target.md");
    auto* view = side<BacklinkView>("backlink");
    BacklinkPane* pane = view->pane();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->linkedFiles().isEmpty(); }, 5000));
    settle();
    TreeList* t = pane->tree();
    int row = -1; // the last match: the first one is the link in the properties, which get the focus instead
    for (int i = 0; i < t->rowCount(); ++i)
        if (t->nodeAt(i)->kind == TreeList::Kind::Match)
            row = i;
    ASSERT_GE(row, 0);
    const TreeList::Node* match = t->nodeAt(row);
    const MatchRange range = match->matches.first();
    click(t->viewport(), t->rowRect(row).center());
    settle();
    auto* mv = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(mv);
    EXPECT_EQ(mv->file()->path(), "Linker.md");
    EXPECT_EQ(mv->editor()->selection().main().from(), range.from);
    EXPECT_EQ(mv->editor()->selection().main().to(), range.to);
}

TEST_F(SidebarTest, BacklinksInDocument)
{
    MarkdownView* mv = open("Target.md");
    ASSERT_TRUE(mv);
    EXPECT_FALSE(mv->showBacklinks());
    EXPECT_FALSE(mv->editor()->footerWidget());
    const double plainHeight = mv->editor()->documentHeight();
    ASSERT_TRUE(session->commands()->execute("backlink:toggle-backlinks-in-document"));
    BacklinkPane* pane = mv->embeddedBacklinks();
    ASSERT_TRUE(pane);
    EXPECT_TRUE(pane->isEmbedded());
    EXPECT_EQ(mv->editor()->footerWidget(), pane);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pane->linkedFiles().isEmpty(); }, 5000));
    settle();
    // natural height below the text: the note gets taller, the pane doesn't scroll
    EXPECT_TRUE(pane->tree()->autoHeight());
    EXPECT_GT(mv->editor()->documentHeight(), plainHeight + pane->tree()->height() - 1);
    EXPECT_EQ(pane->tree()->verticalScrollBar()->maximum(), 0);
    // per-view state, restored
    const QJsonObject st = mv->state();
    EXPECT_TRUE(st.value("backlinks").toBool());
    EXPECT_TRUE(st.value("backlinkOpts").isObject());
    mv->setState(QJsonObject{{"file", "Target.md"}, {"backlinks", false}});
    EXPECT_FALSE(mv->showBacklinks());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_FALSE(mv->editor()->footerWidget());
    EXPECT_DOUBLE_EQ(mv->editor()->documentHeight(), plainHeight);
    // new views follow the Backlinks option
    session->config()->setValue("backlink", "backlinkInDocument", true);
    window->workspace()->openFile(session->vault()->file("Linker.md"), Workspace::NewLeaf::Tab);
    settle();
    auto* other = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(other);
    EXPECT_TRUE(other->showBacklinks());
}

TEST_F(SidebarTest, OutgoingLinks)
{
    open("Linker.md");
    auto* view = side<OutgoingLinkView>("outgoing-link");
    ASSERT_TRUE(view);
    EXPECT_EQ(view->displayText(), "Outgoing links");
    ASSERT_TRUE(QTest::qWaitFor([&] { return view->links().size() >= 2; }, 5000));
    const QList<OutgoingLinkView::LinkRow> links = view->links();
    EXPECT_EQ(links[0].file, "Target.md");
    EXPECT_EQ(links[0].title, "Target");
    EXPECT_EQ(links[0].icon, "link");
    EXPECT_EQ(links[1].title, "Missing");
    EXPECT_TRUE(links[1].unresolved);
    EXPECT_EQ(links[1].icon, "file-plus");
    // unlinked mentions of other notes' names in this note, with a chip per destination
    view->setUnlinkedCollapsed(false);
    settle();
    ASSERT_GE(view->unlinkedCount(), 2); // "Target" and the alias "tgt"
    TreeList::Node* first = nullptr;
    for (TreeList::Node* n : view->tree()->allNodes())
        if (n->kind == TreeList::Kind::Match && !first)
            first = n;
    ASSERT_TRUE(first);
    EXPECT_EQ(first->chips, (QStringList{"Target"}));
    ASSERT_TRUE(view->linkMention(0, 0));
    EXPECT_TRUE(readFile(root + "/Linker.md").contains("This mentions [[Target]] in plain text"));
    const QJsonObject st = view->state();
    EXPECT_EQ(st.value("file").toString(), "Linker.md");
    EXPECT_EQ(st.value("unlinkedCollapsed").toBool(), false);
    EXPECT_EQ(st.value("linksCollapsed").toBool(), false);
}

TEST_F(SidebarTest, Tags)
{
    auto* view = side<TagView>("tag");
    ASSERT_TRUE(view);
    view->updateTags();
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"project", "  alpha", "  beta", "misc"}));
    EXPECT_EQ(view->tree()->findNode("#project")->flair, "2");
    view->setUseHierarchy(false);
    view->updateTags();
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"project", "misc", "project/alpha", "project/beta"}));
    view->setSortOrder(CountSortOrder::AlphabeticalReverse);
    view->updateTags();
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"project/beta", "project/alpha", "project", "misc"}));
    // the filter keeps matches and their parents
    view->setUseHierarchy(true);
    view->setSortOrder(CountSortOrder::Frequency);
    view->setSearchQuery("alpha");
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"project", "  alpha"}));
    view->setSearchQuery(QString());
    // clicking searches tag:#x; Mod+click adds it to (or removes it from) the current query
    TreeList* t = view->tree();
    click(t->viewport(), t->rowRect(rowWithTitle(t, "alpha")).center());
    EXPECT_EQ(searched, (QStringList{"tag:#project/alpha"}));
    currentQuery = "foo";
    click(t->viewport(), t->rowRect(rowWithTitle(t, "misc")).center(), Qt::ControlModifier);
    EXPECT_EQ(searched.last(), "foo tag:#misc");
    currentQuery = "foo tag:#misc";
    click(t->viewport(), t->rowRect(rowWithTitle(t, "misc")).center(), Qt::ControlModifier);
    EXPECT_EQ(searched.last(), "foo");
    const QJsonObject st = view->state();
    EXPECT_EQ(st.value("sortOrder").toString(), "frequency");
    EXPECT_EQ(st.value("useHierarchy").toBool(), true);
}

TEST_F(SidebarTest, Outline)
{
    MarkdownView* mv = open("Target.md");
    ASSERT_TRUE(mv);
    auto* view = side<OutlineView>("outline");
    ASSERT_TRUE(view);
    view->update();
    EXPECT_EQ(view->displayText(), "Outline");
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"Target", "  Section A", "    Sub", "  Section B"}));
    // the current section follows the cursor
    const Text& text = mv->document()->text();
    mv->editor()->setCursorPosition(text.lineStart(11)); // "More" under "### Sub"
    view->updateActiveHeading();
    EXPECT_EQ(view->activeHeading(), 2);
    EXPECT_TRUE(view->tree()->findNode(view->tree()->nodeAt(2)->id)->active);
    // clicking a heading moves the note's cursor there
    TreeList* t = view->tree();
    click(t->viewport(), t->rowRect(rowWithTitle(t, "Section B")).center());
    settle();
    EXPECT_EQ(text.lineAt(mv->editor()->cursorPosition()), 12);
    EXPECT_EQ(view->activeHeading(), 3);
    // filter
    view->setSearchQuery("sub");
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"Target", "  Section A", "    Sub"}));
    view->setSearchQuery(QString());
    // "... for the current file": a linked outline next to the note
    ASSERT_TRUE(commands->execute("outline:open-for-current"));
    settle();
    auto* bound = qobject_cast<OutlineView*>(window->workspace()->activeLeaf()->view());
    ASSERT_TRUE(bound);
    EXPECT_EQ(bound->displayText(), "Outline of Target");
    EXPECT_EQ(bound->state().value("file").toString(), "Target.md");
}

TEST_F(SidebarTest, OutlineDragMovesSections)
{
    MarkdownView* mv = open("Target.md");
    auto* view = side<OutlineView>("outline");
    view->update();
    // Section B (index 3) before Section A (index 1)
    ASSERT_TRUE(view->moveSection(3, 1));
    EXPECT_EQ(mv->document()->text().toString(),
              "---\naliases: [Tgt]\ntags: [project/alpha]\nstatus: draft\n---\n# Target\n\nBody text.\n\n## Section B\nEnd\n\n## Section A\nText\n### Sub\nMore\n");
    EXPECT_FALSE(view->moveSection(1, 2)); // into its own section
}

TEST_F(SidebarTest, BookmarksViewAndModel)
{
    BookmarkModel* model = sessionBookmarks(session.get());
    ASSERT_TRUE(model);
    auto* view = side<BookmarksView>("bookmarks", Workspace::Side::Left);
    ASSERT_TRUE(view);
    view->update();
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"No bookmarks found"}));
    Bookmark* file = model->add(Bookmark::file("Target.md"));
    Bookmark* group = model->add(Bookmark::group("Reading"));
    model->add(Bookmark::file("Target.md", "#Section A"), group);
    model->add(Bookmark::search("tag:#misc"), group);
    model->add(Bookmark::folder("Folder"));
    model->add(Bookmark::file("Gone.md")); // hidden: no such file
    view->update();
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"Target", "Reading", "  Target#Section A", "  tag:#misc", "Folder"}));
    // saved in the clone's config dir as Obsidian's bookmarks.json
    session->config()->flush();
    const QJsonObject json = QJsonDocument::fromJson(readFile(session->config()->dir() + "/bookmarks.json")).object();
    ASSERT_EQ(json.value("items").toArray().size(), 4);
    EXPECT_EQ(json.value("items").toArray()[1].toObject().value("items").toArray()[0].toObject().value("subpath").toString(), "#Section A");
    // rename in place sets a custom title
    view->startRename(file);
    ASSERT_TRUE(view->tree()->renameEditor()->isVisible());
    view->tree()->renameEditor()->setText("My target");
    view->tree()->commitRename();
    settle();
    EXPECT_EQ(file->title.value_or(QString()), "My target");
    EXPECT_EQ(view->tree()->visibleTitles().value(0), "My target");
    // clicking opens; the open note's bookmark is active
    TreeList* t = view->tree();
    click(t->viewport(), t->rowRect(rowWithTitle(t, "My target")).center());
    settle();
    EXPECT_EQ(qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view())->file()->path(), "Target.md");
    EXPECT_TRUE(view->nodeFor(file)->active);
    // a search bookmark runs the search
    click(t->viewport(), t->rowRect(rowWithTitle(t, "tag:#misc")).center());
    EXPECT_EQ(searched.last(), "tag:#misc");
    // drag the file bookmark into the group (onto its middle)
    const QRect g = t->rowRect(rowWithTitle(t, "Reading"));
    sendMouse(t->viewport(), QEvent::MouseButtonPress, t->rowRect(rowWithTitle(t, "My target")).center(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(t->viewport(), QEvent::MouseMove, g.center(), Qt::NoButton, Qt::LeftButton);
    sendMouse(t->viewport(), QEvent::MouseButtonRelease, g.center(), Qt::LeftButton, Qt::NoButton);
    settle();
    EXPECT_EQ(file->parent, group);
    EXPECT_EQ(group->items.front().get(), file); // an expanded group: first
    // Delete removes the focused one
    t->setFocusedNode(view->nodeFor(file));
    t->setFocus();
    key(t, Qt::Key_Delete);
    settle();
    EXPECT_FALSE(model->contains(file));
}

TEST_F(SidebarTest, BookmarkCommandsAndModal)
{
    BookmarkModel* model = sessionBookmarks(session.get());
    MarkdownView* mv = open("Target.md");
    ASSERT_TRUE(commands->isAvailable("bookmarks:bookmark-current-view"));
    EXPECT_FALSE(commands->isAvailable("bookmarks:unbookmark-current-view"));
    ASSERT_TRUE(commands->execute("bookmarks:bookmark-current-view"));
    Modal* modal = window->overlay()->findChild<Modal*>();
    ASSERT_TRUE(modal);
    EXPECT_EQ(modal->title(), "Add bookmark");
    auto* title = modal->findChild<QLineEdit*>("ocBookmarkTitle");
    ASSERT_TRUE(title);
    EXPECT_EQ(title->placeholderText(), "Target");
    title->setText("The target");
    modal->button(modal->defaultButton())->click();
    settle();
    ASSERT_EQ(model->items().size(), 1u);
    EXPECT_EQ(model->items()[0]->path, "Target.md");
    EXPECT_EQ(model->items()[0]->title.value_or(QString()), "The target");
    // again: the existing bookmark is edited ("Edit bookmark" with Remove)
    ASSERT_TRUE(commands->execute("bookmarks:bookmark-current-view"));
    settle();
    Modal* edit = nullptr;
    for (Modal* m : window->overlay()->findChildren<Modal*>())
        if (m->isOpen())
            edit = m;
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->title(), "Edit bookmark");
    EXPECT_EQ(edit->buttonCount(), 3);
    edit->close();
    ASSERT_TRUE(commands->isAvailable("bookmarks:unbookmark-current-view"));
    ASSERT_TRUE(commands->execute("bookmarks:unbookmark-current-view"));
    EXPECT_TRUE(model->items().empty());
    // the heading under the cursor
    mv->editor()->setCursorPosition(mv->document()->text().lineStart(8)); // "Text" under "## Section A"
    ASSERT_TRUE(commands->execute("bookmarks:bookmark-current-heading"));
    settle();
    Modal* heading = nullptr;
    for (Modal* m : window->overlay()->findChildren<Modal*>())
        if (m->isOpen())
            heading = m;
    ASSERT_TRUE(heading);
    heading->button(heading->defaultButton())->click();
    settle();
    ASSERT_EQ(model->items().size(), 1u);
    EXPECT_EQ(model->items()[0]->subpath, "#Section A");
}

TEST_F(SidebarTest, AllProperties)
{
    auto* view = side<AllPropertiesView>("all-properties");
    ASSERT_TRUE(view);
    view->update();
    QStringList rows;
    for (const PropertyInfo& p : view->properties())
        rows << p.name + ":" + QString::number(p.occurrences);
    EXPECT_EQ(rows, (QStringList{"status:2", "tags:2", "aliases:1", "count:1", "next:1", "related:1", "cssclasses:0"}));
    TreeList* t = view->tree();
    EXPECT_EQ(t->findNode("count")->icon, "binary");
    EXPECT_EQ(t->findNode("tags")->icon, "tags");
    click(t->viewport(), t->rowRect(rowWithTitle(t, "status")).center());
    EXPECT_EQ(searched.last(), "[\"status\"]");
    view->setSortOrder(CountSortOrder::Alphabetical);
    EXPECT_EQ(view->properties().first().name, "aliases");
    view->setSearchQuery("stat");
    EXPECT_EQ(view->tree()->visibleTitles(), (QStringList{"status"}));
}

TEST_F(SidebarTest, Commands)
{
    for (const char* id : {"backlink:open", "backlink:open-backlinks", "backlink:toggle-backlinks-in-document", "outgoing-links:open",
                           "outgoing-links:open-for-current", "tag-pane:open", "outline:open", "outline:open-for-current", "bookmarks:open",
                           "bookmarks:bookmark-current-view", "bookmarks:bookmark-current-search", "bookmarks:unbookmark-current-view",
                           "bookmarks:bookmark-current-section", "bookmarks:bookmark-current-heading", "bookmarks:bookmark-all-tabs",
                           "properties:open"})
        EXPECT_TRUE(commands->find(QString::fromLatin1(id))) << id;
    EXPECT_EQ(commands->find("backlink:open")->name, "Backlinks: Show backlinks");
    EXPECT_EQ(commands->find("bookmarks:bookmark-current-section")->name, "Bookmarks: Bookmark block under cursor...");
    EXPECT_EQ(commands->find("tag-pane:open")->name, "Tags view: Show tags");
    EXPECT_EQ(commands->find("properties:open")->name, "Properties view: Show all properties");
    ASSERT_TRUE(commands->execute("tag-pane:open"));
    EXPECT_TRUE(qobject_cast<TagView*>(window->workspace()->activeLeaf()->view()));
    EXPECT_TRUE(window->workspace()->activeLeaf()->isInSidedock());
    ASSERT_TRUE(commands->execute("bookmarks:open"));
    EXPECT_TRUE(qobject_cast<BookmarksView*>(window->workspace()->activeLeaf()->view()));
    EXPECT_FALSE(commands->isAvailable("backlink:open-backlinks")); // no note open
    open("Target.md");
    ASSERT_TRUE(commands->execute("backlink:open-backlinks"));
    settle();
    auto* bound = qobject_cast<BacklinkView*>(window->workspace()->activeLeaf()->view());
    ASSERT_TRUE(bound);
    EXPECT_FALSE(bound->leaf()->isInSidedock());
    EXPECT_EQ(bound->displayText(), "Backlinks for Target");
    EXPECT_FALSE(commands->isAvailable("backlink:toggle-backlinks-in-document"));
}

TEST_F(SidebarTest, FileAndEditorMenus)
{
    MarkdownView* mv = open("Target.md");
    WorkspaceLeaf* leaf = mv->leaf();
    QList<MenuEntry> items;
    FileMenuHooks::collect(items, vault->file("Target.md"), "tab-header", leaf, window);
    QStringList titles;
    for (const MenuEntry& e : items)
        titles << e.section + "/" + e.title;
    for (const QString& t : {QString("action/Bookmark..."), QString("view.linked/Open backlinks"), QString("view.linked/Open outgoing links"),
                             QString("view.linked/Open outline")})
        EXPECT_TRUE(titles.contains(t)) << qPrintable(t) << qPrintable(titles.join(", "));
    // the explorer (no leaf) only gets the bookmark item; the vault root none of ours
    QList<MenuEntry> explorer;
    FileMenuHooks::collect(explorer, vault->folder("Folder"), "file-explorer-context-menu", nullptr, window);
    int ours = 0;
    for (const MenuEntry& e : explorer)
        ours += e.title == u"Bookmark..." || e.section == u"view.linked";
    EXPECT_EQ(ours, 1);
    // "Open outline": a linked outline next to the note
    for (const MenuEntry& e : items)
        if (e.title == u"Open outline")
            e.run();
    settle();
    auto* outline = qobject_cast<OutlineView*>(window->workspace()->activeLeaf()->view());
    ASSERT_TRUE(outline);
    EXPECT_EQ(outline->displayText(), "Outline of Target");
    EXPECT_EQ(outline->linkedLeaf(), leaf);
    // editor menu on a heading line
    mv->editor()->setCursorPosition(mv->document()->text().lineStart(8)); // "## Section A"
    QList<MenuEntry> editor;
    EditorMenuHooks::collect(editor, mv, mv->editor()->cursorPosition());
    bool found = false;
    for (const MenuEntry& e : editor)
        if (e.title == u"Bookmark this heading...") {
            found = true;
            e.run();
        }
    ASSERT_TRUE(found);
    settle();
    Modal* modal = nullptr;
    for (Modal* m : window->overlay()->findChildren<Modal*>())
        if (m->isOpen())
            modal = m;
    ASSERT_TRUE(modal);
    modal->button(modal->defaultButton())->click();
    BookmarkModel* model = sessionBookmarks(session.get());
    ASSERT_EQ(model->items().size(), 1u);
    EXPECT_EQ(model->items()[0]->subpath, "#Section A");
    // with a bookmark the file menu offers to edit it
    model->add(Bookmark::file("Target.md"));
    QList<MenuEntry> again;
    FileMenuHooks::collect(again, vault->file("Target.md"), "more-options", leaf, window);
    bool edit = false;
    for (const MenuEntry& e : again)
        edit = edit || e.title == u"Edit bookmark";
    EXPECT_TRUE(edit);
}

// ---- screenshots (OC_SHOT_DIR) ----------------------------------------------------------------------

TEST_F(SidebarTest, Screenshots)
{
    const QString dir = qEnvironmentVariable("OC_SHOT_DIR");
    if (dir.isEmpty())
        GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
    QDir().mkpath(dir);
    window->resize(1400, 850);
    BookmarkModel* model = sessionBookmarks(session.get());
    model->add(Bookmark::file("Target.md"));
    Bookmark* g = model->add(Bookmark::group("Reading list"));
    model->add(Bookmark::file("Linker.md"), g);
    model->add(Bookmark::file("Target.md", "#Section A"), g);
    model->add(Bookmark::search("tag:#project"));
    model->add(Bookmark::folder("Folder"));
    open("Linker.md");
    settle();
    struct Shot {
        const char* type;
        Workspace::Side side;
        const char* name;
    };
    for (const auto& [mode, suffix] : {std::pair{ThemeManager::Mode::Dark, "dark"}, std::pair{ThemeManager::Mode::Light, "light"}}) {
        ThemeManager::instance()->setMode(mode);
        ThemeManager::instance()->apply();
        for (const Shot& s : {Shot{"backlink", Workspace::Side::Right, "backlinks"}, Shot{"outgoing-link", Workspace::Side::Right, "outgoing"},
                              Shot{"tag", Workspace::Side::Right, "tags"}, Shot{"outline", Workspace::Side::Right, "outline"},
                              Shot{"all-properties", Workspace::Side::Right, "properties"}, Shot{"bookmarks", Workspace::Side::Left, "bookmarks"}}) {
            open(QString::fromLatin1(s.type) == u"outgoing-link" ? "Linker.md" : "Target.md");
            WorkspaceLeaf* l = window->workspace()->ensureSideLeaf(QString::fromLatin1(s.type), s.side, true);
            if (auto* b = qobject_cast<BacklinkView*>(l->view())) {
                b->pane()->setUnlinkedCollapsed(false);
                EXPECT_TRUE(QTest::qWaitFor([&] { return !b->pane()->isSearchingUnlinked(); }, 5000));
            }
            if (auto* o = qobject_cast<OutgoingLinkView*>(l->view()))
                o->setUnlinkedCollapsed(false);
            settle();
            window->grab().save(dir + QStringLiteral("/sidebar-%1-%2.png").arg(QString::fromLatin1(s.name), QString::fromLatin1(suffix)));
            l->view()->grab().save(dir + QStringLiteral("/sidebar-%1-%2-view.png").arg(QString::fromLatin1(s.name), QString::fromLatin1(suffix)));
            // states: a hovered match (Link / more-context buttons), the focus ring, the current section
            TreeList* t = l->view()->findChild<TreeList*>();
            if (!t)
                continue;
            window->workspace()->setActiveLeaf(l, false);
            int hover = -1, focus = -1;
            for (int i = 0; i < t->rowCount(); ++i) {
                if (t->nodeAt(i)->kind == TreeList::Kind::Match)
                    hover = i; // the last one
                else if (t->nodeAt(i)->kind == TreeList::Kind::Item && focus < 0)
                    focus = i;
            }
            if (auto* o = qobject_cast<OutlineView*>(l->view())) {
                auto* mv = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
                mv->editor()->setCursorPosition(mv->document()->text().lineStart(11));
                o->updateActiveHeading();
            }
            if (focus >= 0)
                t->setFocusedNode(t->nodeAt(focus), false);
            if (hover >= 0)
                sendMouse(t->viewport(), QEvent::MouseMove, t->rowRect(hover).center(), Qt::NoButton, Qt::NoButton);
            else if (focus >= 0 && t->rowCount() > focus + 1)
                sendMouse(t->viewport(), QEvent::MouseMove, t->rowRect(focus + 1).center(), Qt::NoButton, Qt::NoButton);
            settle();
            l->view()->grab().save(dir + QStringLiteral("/sidebar-%1-%2-states.png").arg(QString::fromLatin1(s.name), QString::fromLatin1(suffix)));
            t->setFocusedNode(nullptr, false);
            sendMouse(t->viewport(), QEvent::MouseMove, QPoint(-10, -10), Qt::NoButton, Qt::NoButton);
        }
        // the bookmark modal
        open("Target.md");
        commands->execute("bookmarks:bookmark-current-view");
        settle();
        window->grab().save(dir + QStringLiteral("/sidebar-bookmark-modal-%1.png").arg(QString::fromLatin1(suffix)));
        for (Modal* m : window->overlay()->findChildren<Modal*>())
            if (m->isOpen())
                m->close();
        settle();
    }
    ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
    ThemeManager::instance()->apply();
}
