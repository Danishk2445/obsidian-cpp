// Workspace model tests: default layout, persistence round trips, getLeaf modes, split/close rules,
// moves between groups and sidedocks, navigation history, undo close, recent files.

#include "ui/workspace_test_support.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "ui/shell/empty_view.h"

#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

using namespace oc;
using namespace oc::test;

namespace {
using NewLeaf = Workspace::NewLeaf;
using Side = Workspace::Side;
} // namespace

class WorkspaceModelTest : public WorkspaceFixture {};

TEST_F(WorkspaceModelTest, DefaultLayout)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    EXPECT_EQ(describe(d->root), "V[T(_*)]");
    EXPECT_EQ(describe(d->left), "H[T(Files*,Search,Bookmarks)]");
    EXPECT_EQ(describe(d->right), "H[T(Backlinks*,Outgoing links,Tags,Outline)]");
    EXPECT_FALSE(ws->isSideCollapsed(Side::Left));
    EXPECT_TRUE(ws->isSideCollapsed(Side::Right)); // Obsidian starts new vaults with it collapsed
    EXPECT_EQ(ws->sideWidth(Side::Left), 300);
    ASSERT_TRUE(ws->activeLeaf());
    EXPECT_EQ(ws->activeLeaf(), ws->activeRootLeaf());
    EXPECT_EQ(ws->activeLeaf()->view()->viewType(), "empty");
    EXPECT_FALSE(ws->activeLeaf()->isInSidedock());
    EXPECT_TRUE(ws->leavesOfType("file-explorer").value(0)->isInSidedock());

    const QJsonObject j = ws->serialize();
    EXPECT_EQ(j["main"].toObject()["type"].toString(), "split");
    EXPECT_EQ(j["main"].toObject()["direction"].toString(), "vertical");
    EXPECT_EQ(j["left"].toObject()["direction"].toString(), "horizontal");
    EXPECT_EQ(j["left"].toObject()["width"].toInt(), 300);
    EXPECT_FALSE(j["left"].toObject().contains("collapsed"));
    EXPECT_TRUE(j["right"].toObject()["collapsed"].toBool());
    EXPECT_TRUE(j["left-ribbon"].toObject()["hiddenItems"].isObject());
    EXPECT_EQ(j["active"].toString(), ws->activeLeaf()->id());
    EXPECT_TRUE(j["lastOpenFiles"].isArray());
    const QJsonObject leaf = j["main"].toObject()["children"].toArray()[0].toObject()["children"].toArray()[0].toObject();
    EXPECT_EQ(leaf["type"].toString(), "leaf");
    EXPECT_EQ(leaf["id"].toString().size(), 16);
    EXPECT_EQ(leaf["state"].toObject()["type"].toString(), "empty");
    EXPECT_EQ(leaf["state"].toObject()["title"].toString(), "New tab");
}

TEST_F(WorkspaceModelTest, DefaultLayoutOnlyAddsRegisteredSidebarViews)
{
    m_registry.unregisterView("search");
    auto ws = makeWorkspace();
    EXPECT_EQ(describe(impl(ws.get())->left), "H[T(Files*,Bookmarks)]");
}

TEST_F(WorkspaceModelTest, SerializeRestoreSerializeIsIdentical)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(note("Projects/Alpha.md"));
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::Tab);
    b->setViewState(graph("Graph one"));
    b->setPinned(true);
    WorkspaceLeaf* c = ws->getLeaf(NewLeaf::SplitRight);
    c->setViewState(note("Welcome.md"));
    WorkspaceLeaf* e = ws->getLeaf(NewLeaf::SplitDown);
    e->setViewState(note("Daily/2026-09-22.md"));
    d->root->children[0]->dimension = 60;
    d->root->children[1]->dimension = 40;
    ws->setSideWidth(Side::Left, 250);
    ws->setSideCollapsed(Side::Right, false);
    ws->setSideCollapsed(Side::Left, true);
    ws->ensureSideLeaf("outline", Side::Right, true);
    ws->setActiveLeaf(c, false);
    EXPECT_EQ(describe(d->root), "V[T(Alpha,Graph one*),H[T(Welcome*),T(2026-09-22*)]]");

    const QJsonObject first = ws->serialize();
    auto ws2 = makeWorkspace(false);
    ASSERT_TRUE(ws2->restore(first));
    const QJsonObject second = ws2->serialize();
    EXPECT_EQ(first, second) << QJsonDocument(first).toJson().constData() << "\n----\n" << QJsonDocument(second).toJson().constData();
    WorkspaceImpl* d2 = impl(ws2.get());
    EXPECT_EQ(describe(d2->root), describe(d->root));
    EXPECT_TRUE(ws2->activeLeaf()->id() == c->id());
    EXPECT_TRUE(ws2->isSideCollapsed(Side::Left));
    EXPECT_EQ(ws2->sideWidth(Side::Left), 250);
    EXPECT_TRUE(d2->leafById(b->id())->isPinned());
    EXPECT_DOUBLE_EQ(d2->root->children[0]->dimension, 60);
}

TEST_F(WorkspaceModelTest, UnknownViewTypesAndKeysRoundTripUnchanged)
{
    const QJsonObject json = parseJson(R"({
      "main": {"id": "m0000000000000001", "type": "split", "children": [
        {"id": "t000000000000001", "type": "tabs", "children": [
          {"id": "l000000000000001", "type": "leaf", "group": "g1",
           "state": {"type": "kanban", "state": {"file": "Board.md", "lanes": [1, 2]}, "icon": "lucide-trello", "title": "Board"}},
          {"id": "l000000000000002", "type": "leaf", "pinned": true,
           "state": {"type": "empty", "state": {}, "pinned": true, "icon": "file", "title": "New tab"}}
        ], "currentTab": 1, "stacked": true, "future": 3}
      ], "direction": "vertical"},
      "left": {"id": "s000000000000001", "type": "split", "children": [
        {"id": "t000000000000002", "type": "tabs", "children": [
          {"id": "l000000000000003", "type": "leaf",
           "state": {"type": "file-explorer", "state": {"sortOrder": "alphabetical", "autoReveal": false}, "icon": "folder-closed", "title": "Files"}}
        ]}
      ], "direction": "horizontal", "width": 280},
      "right": {"id": "s000000000000002", "type": "split", "children": [], "direction": "horizontal", "width": 200, "collapsed": true},
      "left-ribbon": {"hiddenItems": {"graph:Open graph view": true, "switcher:Open quick switcher": false}},
      "floating": {"id": "f000000000000001", "type": "split", "children": [], "direction": "vertical"},
      "active": "l000000000000003",
      "lastOpenFiles": ["Board.md", "Welcome.md"]
    })");
    auto ws = makeWorkspace(false);
    ASSERT_TRUE(ws->restore(json));
    const QJsonObject out = ws->serialize();
    EXPECT_EQ(out, json) << QJsonDocument(out).toJson().constData();
    WorkspaceLeaf* unknown = impl(ws.get())->leafById("l000000000000001");
    ASSERT_TRUE(unknown);
    EXPECT_TRUE(qobject_cast<PlaceholderView*>(unknown->view()));
    EXPECT_EQ(unknown->view()->viewType(), "kanban");
    EXPECT_EQ(unknown->displayText(), "kanban");
    EXPECT_EQ(unknown->icon(), "ghost");
    EXPECT_EQ(ws->leavesOfType("kanban").size(), 1);
    EXPECT_EQ(ws->lastOpenFiles(), QStringList({"Board.md", "Welcome.md"}));
}

TEST_F(WorkspaceModelTest, RestoreRejectsLayoutsWithoutMain)
{
    auto ws = makeWorkspace();
    const QString before = describe(impl(ws.get())->root);
    EXPECT_FALSE(ws->restore(QJsonObject()));
    EXPECT_FALSE(ws->restore(parseJson(R"({"main": 3})")));
    EXPECT_FALSE(ws->restore(parseJson(R"({"main": {"type": "nonsense"}})")));
    EXPECT_EQ(describe(impl(ws.get())->root), before);
}

TEST_F(WorkspaceModelTest, RestoreForcesDirectionsAndWrapsLegacyLeaves)
{
    auto ws = makeWorkspace(false);
    ASSERT_TRUE(ws->restore(parseJson(R"({
      "main": {"id": "m", "type": "split", "direction": "horizontal", "children": [
        {"id": "l1", "type": "leaf", "state": {"type": "graph", "state": {"name": "G"}}},
        {"id": "t1", "type": "tabs", "children": []}
      ]}})")));
    WorkspaceImpl* d = impl(ws.get());
    EXPECT_TRUE(d->root->vertical);
    EXPECT_EQ(describe(d->root), "V[T(G*)]"); // the empty tab group was dropped
    EXPECT_EQ(describe(d->left), "H[]");
    EXPECT_TRUE(ws->isSideCollapsed(Side::Right)); // no right sidedock in the layout
}

TEST_F(WorkspaceModelTest, LoadLayoutFromDiskAndDebouncedSave)
{
    const QString file = ClonePaths::vaultConfigDir(m_session->id()) + "/workspace.json";
    {
        auto ws = makeWorkspace();
        EXPECT_FALSE(QFile::exists(file)); // loading alone doesn't write
        ws->activeRootLeaf()->setViewState(graph("Persisted"));
        ws->setSideCollapsed(Side::Left, true);
        ws->setSideWidth(Side::Right, 222);
        ASSERT_TRUE(QTest::qWaitFor([&] { return QFile::exists(file); }, 3000)); // ~1 s debounce
        ws->getLeaf(NewLeaf::Tab)->setViewState(graph("Unsaved yet")); // flushed by the destructor
    }
    const QJsonObject onDisk = JsonFile::read(file);
    EXPECT_TRUE(onDisk["left"].toObject()["collapsed"].toBool());
    EXPECT_EQ(onDisk["right"].toObject()["width"].toInt(), 222);
    auto ws = makeWorkspace();
    EXPECT_EQ(describe(impl(ws.get())->root), "V[T(Persisted,Unsaved yet*)]");
    EXPECT_TRUE(ws->isSideCollapsed(Side::Left));
    EXPECT_EQ(ws->sideWidth(Side::Right), 222);
}

TEST_F(WorkspaceModelTest, CorruptLayoutFileFallsBackToDefault)
{
    const QString dir = ClonePaths::ensureDir(ClonePaths::vaultConfigDir(m_session->id()));
    QFile f(dir + "/workspace.json");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("{ not json");
    f.close();
    auto ws = makeWorkspace();
    EXPECT_EQ(describe(impl(ws.get())->root), "V[T(_*)]");
    EXPECT_EQ(describe(impl(ws.get())->left), "H[T(Files*,Search,Bookmarks)]");
}

TEST_F(WorkspaceModelTest, GetLeafModes)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* r = ws->activeRootLeaf();
    EXPECT_EQ(ws->getLeaf(NewLeaf::No), r); // an empty tab can navigate
    r->setViewState(graph("R"));
    EXPECT_EQ(ws->getLeaf(NewLeaf::No), r);

    // Pinned: a new tab at the end of the same group.
    r->setPinned(true);
    WorkspaceLeaf* n = ws->getLeaf(NewLeaf::No);
    EXPECT_NE(n, r);
    EXPECT_EQ(n->tabs(), r->tabs());
    EXPECT_EQ(ws->activeLeaf(), n);
    EXPECT_EQ(describe(d->root), "V[T(R,_*)]");
    EXPECT_EQ(ws->getLeaf(NewLeaf::No), n);
    n->setViewState(graph("N"));

    // Tab: right after the active leaf; an empty current tab is reused.
    ws->setActiveLeaf(r, false);
    WorkspaceLeaf* t = ws->getLeaf(NewLeaf::Tab);
    EXPECT_EQ(describe(d->root), "V[T(R,_*,N)]");
    EXPECT_EQ(ws->getLeaf(NewLeaf::Tab), t);
    t->setViewState(graph("T"));

    // Splits.
    WorkspaceLeaf* s = ws->getLeaf(NewLeaf::SplitRight);
    EXPECT_EQ(describe(d->root), "V[T(R,T*,N),T(_*)]");
    EXPECT_EQ(ws->activeLeaf(), s);
    s->setViewState(graph("S"));
    ws->getLeaf(NewLeaf::SplitDown);
    EXPECT_EQ(describe(d->root), "V[T(R,T*,N),H[T(S*),T(_*)]]");

    // Window falls back to a tab.
    WorkspaceLeaf* w = ws->getLeaf(NewLeaf::Window);
    EXPECT_EQ(w, ws->activeLeaf());
    EXPECT_EQ(w->view()->viewType(), "empty");
}

TEST_F(WorkspaceModelTest, GetLeafNoPrefersAnotherUnpinnedPaneOverANewTab)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* left = ws->activeRootLeaf();
    left->setViewState(graph("L"));
    WorkspaceLeaf* right = ws->getLeaf(NewLeaf::SplitRight);
    right->setViewState(graph("R"));
    left->setPinned(true);
    ws->setActiveLeaf(left, false);
    EXPECT_EQ(ws->getLeaf(NewLeaf::No), right); // Obsidian's getUnpinnedLeaf()
    EXPECT_EQ(describe(d->root), "V[T(L*),T(R*)]");
}

TEST_F(WorkspaceModelTest, SplitAndCloseCollapseRules)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* r = ws->activeRootLeaf();
    r->setViewState(graph("R"));
    WorkspaceLeaf* s = ws->splitLeaf(r, Qt::Horizontal);
    s->setViewState(graph("S"));
    WorkspaceLeaf* s2 = ws->splitLeaf(s, Qt::Vertical);
    s2->setViewState(graph("S2"));
    WorkspaceLeaf* before = ws->splitLeaf(r, Qt::Horizontal, true);
    before->setViewState(graph("B"));
    EXPECT_EQ(describe(d->root), "V[T(B*),T(R*),H[T(S*),T(S2*)]]");

    d->root->children[2]->dimension = 50;
    s->detach(); // H[...] with one child collapses into it and keeps its dimension
    EXPECT_EQ(describe(d->root), "V[T(B*),T(R*),T(S2*)]");
    EXPECT_DOUBLE_EQ(d->root->children[2]->dimension, 50);
    s2->detach();
    before->detach();
    EXPECT_EQ(describe(d->root), "V[T(R*)]");
    const QString rid = r->id();
    r->detach(); // the root always keeps a group with an empty leaf
    EXPECT_EQ(describe(d->root), "V[T(_*)]");
    EXPECT_NE(ws->activeRootLeaf()->id(), rid);
    EXPECT_EQ(ws->activeLeaf(), ws->activeRootLeaf());
}

TEST_F(WorkspaceModelTest, ClosingTheActiveTabSelectsItsRightNeighbour)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::Tab);
    b->setViewState(graph("B"));
    WorkspaceLeaf* c = ws->getLeaf(NewLeaf::Tab);
    c->setViewState(graph("C"));
    ws->setActiveLeaf(b, false);
    EXPECT_EQ(describe(d->root), "V[T(A,B*,C)]");
    b->detach();
    EXPECT_EQ(describe(d->root), "V[T(A,C*)]");
    EXPECT_EQ(ws->activeLeaf(), c);
    c->detach();
    EXPECT_EQ(describe(d->root), "V[T(A*)]");
    EXPECT_EQ(ws->activeLeaf(), a);
}

TEST_F(WorkspaceModelTest, MoveLeavesBetweenGroupsAndSidedocks)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::Tab);
    b->setViewState(graph("B"));
    WorkspaceLeaf* c = ws->getLeaf(NewLeaf::Tab);
    c->setViewState(graph("C"));
    WorkspaceLeaf* x = ws->getLeaf(NewLeaf::SplitRight);
    x->setViewState(graph("X"));
    WorkspaceTabs* A = a->tabs();
    WorkspaceTabs* X = x->tabs();
    EXPECT_EQ(describe(d->root), "V[T(A,B,C*),T(X*)]");

    d->moveLeaf(b, X, 0);
    EXPECT_EQ(describe(d->root), "V[T(A,C*),T(B*,X)]");
    d->moveLeaf(a, A, 2); // reorder within the group (index counts the moved tab itself)
    EXPECT_EQ(describe(d->root), "V[T(C,A*),T(B*,X)]");
    EXPECT_EQ(describe(A), "T(C,A*)");

    // Split a group with one of its own tabs; the dragged group keeps the rest.
    ASSERT_TRUE(d->splitLeafTo(x, X, DropEdge::Bottom));
    EXPECT_EQ(describe(d->root), "V[T(C,A*),H[T(B*),T(X*)]]");
    // A group can't be split with its only tab.
    EXPECT_FALSE(d->splitLeafTo(x, x->tabs(), DropEdge::Left));
    ASSERT_TRUE(d->splitLeafTo(c, x->tabs(), DropEdge::Left));
    EXPECT_EQ(describe(d->root), "V[T(A*),H[T(B*),V[T(C*),T(X*)]]]");

    // Into the sidedocks: appended to their first group, empty groups are cleaned up.
    d->moveLeafToSide(c, d->right);
    EXPECT_EQ(describe(d->right), "H[T(Backlinks,Outgoing links,Tags,Outline,C*)]");
    EXPECT_EQ(describe(d->root), "V[T(A*),H[T(B*),T(X*)]]");
    EXPECT_TRUE(c->isInSidedock());
    d->moveLeaf(c, a->tabs(), 0);
    EXPECT_EQ(describe(d->root), "V[T(C*,A),H[T(B*),T(X*)]]");
    EXPECT_FALSE(c->isInSidedock());
    // Sidedock groups can be split top/bottom; an emptied sidedock stays (empty).
    WorkspaceLeaf* files = ws->leavesOfType("file-explorer").value(0);
    ASSERT_TRUE(d->splitLeafTo(files, files->tabs(), DropEdge::Bottom));
    EXPECT_EQ(describe(d->left), "H[T(Search*,Bookmarks),T(Files*)]");
    d->moveLeaf(files, a->tabs(), -1);
    EXPECT_EQ(describe(d->left), "H[T(Search*,Bookmarks)]");
}

TEST_F(WorkspaceModelTest, NavigationHistoryWithEphemeralState)
{
    auto ws = makeWorkspace();
    WorkspaceLeaf* leaf = ws->activeRootLeaf();
    QSignalSpy history(leaf, &WorkspaceLeaf::historyChanged);
    leaf->setViewState(note("A.md"));
    EXPECT_FALSE(leaf->canGoBack()); // leaving an empty tab isn't recorded
    static_cast<TestNoteView*>(leaf->view())->eState = QJsonObject{{"scroll", 5}};
    leaf->setViewState(note("B.md"));
    EXPECT_TRUE(leaf->canGoBack());
    EXPECT_GE(history.count(), 1);
    static_cast<TestNoteView*>(leaf->view())->eState = QJsonObject{{"scroll", 7}};
    leaf->setViewState(graph("G"));
    leaf->setViewState(note("B.md"), QJsonObject{{"scroll", 9}}, true); // same view type again
    EXPECT_EQ(leaf->displayText(), "B");

    leaf->goBack();
    EXPECT_EQ(leaf->view()->viewType(), "graph");
    leaf->goBack();
    EXPECT_EQ(leaf->displayText(), "B");
    EXPECT_EQ(static_cast<TestNoteView*>(leaf->view())->eState, QJsonObject({{"scroll", 7}}));
    leaf->goBack();
    EXPECT_EQ(leaf->displayText(), "A");
    EXPECT_EQ(static_cast<TestNoteView*>(leaf->view())->eState, QJsonObject({{"scroll", 5}}));
    EXPECT_FALSE(leaf->canGoBack());
    EXPECT_TRUE(leaf->canGoForward());
    leaf->goForward();
    leaf->goForward();
    EXPECT_EQ(leaf->view()->viewType(), "graph");
    // A new navigation clears the forward stack.
    leaf->setViewState(note("C.md"));
    EXPECT_FALSE(leaf->canGoForward());

    // Re-opening the current file doesn't create an entry; the stack is capped at 50.
    const QList<QPair<QString, QString>> before = impl(ws.get())->historyItems(leaf, true);
    leaf->setViewState(note("C.md"));
    EXPECT_EQ(impl(ws.get())->historyItems(leaf, true).size(), before.size());
    for (int i = 0; i < 70; ++i)
        leaf->setViewState(note(QStringLiteral("N%1.md").arg(i)));
    EXPECT_EQ(impl(ws.get())->historyItems(leaf, true).size(), 50);
    EXPECT_EQ(impl(ws.get())->historyItems(leaf, true).first().first, "N68");
}

TEST_F(WorkspaceModelTest, NonNavigableViewsDontRecordHistory)
{
    auto ws = makeWorkspace();
    WorkspaceLeaf* files = ws->leavesOfType("file-explorer").value(0);
    files->setViewState(QJsonObject{{"type", "search"}});
    EXPECT_FALSE(files->canGoBack());
}

TEST_F(WorkspaceModelTest, UndoCloseReopensInTheOldGroup)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::Tab);
    b->setViewState(note("B.md"));
    b->setViewState(note("B2.md"));
    static_cast<TestNoteView*>(b->view())->eState = QJsonObject{{"scroll", 3}};
    WorkspaceLeaf* c = ws->getLeaf(NewLeaf::Tab);
    c->setViewState(graph("C"));
    const QString bid = b->id();
    EXPECT_FALSE(ws->canUndoCloseLeaf());
    b->detach();
    EXPECT_TRUE(ws->canUndoCloseLeaf());
    EXPECT_EQ(describe(d->root), "V[T(A,C*)]");
    ws->undoCloseLeaf();
    EXPECT_EQ(describe(d->root), "V[T(A,B2*,C)]");
    WorkspaceLeaf* back = ws->activeLeaf();
    EXPECT_EQ(back->id(), bid);
    EXPECT_TRUE(back->canGoBack()); // history restored
    EXPECT_EQ(static_cast<TestNoteView*>(back->view())->eState, QJsonObject({{"scroll", 3}}));

    // Group gone: reopened in a new split next to the most recent leaf.
    WorkspaceLeaf* s = ws->getLeaf(NewLeaf::SplitRight);
    s->setViewState(graph("S"));
    s->detach();
    EXPECT_EQ(describe(d->root), "V[T(A,B2*,C)]");
    ws->undoCloseLeaf();
    EXPECT_EQ(describe(d->root), "V[T(A,B2*,C),T(S*)]");
    EXPECT_FALSE(ws->canUndoCloseLeaf());

    // Empty tabs aren't remembered.
    ws->getLeaf(NewLeaf::Tab)->detach();
    EXPECT_FALSE(ws->canUndoCloseLeaf());
}

TEST_F(WorkspaceModelTest, LastOpenFilesOrderAndCaps)
{
    auto ws = makeWorkspace();
    QSignalSpy opened(ws.get(), &Workspace::fileOpened);
    std::vector<std::unique_ptr<TFile>> files;
    auto open = [&](const QString& path, NewLeaf mode = NewLeaf::No) {
        files.push_back(makeFile(path));
        ws->openFile(files.back().get(), mode);
    };
    open("A.md");
    open("B.md");
    open("Folder/C.md");
    EXPECT_EQ(ws->lastOpenFiles(), QStringList({"Folder/C.md", "B.md", "A.md"}));
    EXPECT_EQ(opened.count(), 3);
    EXPECT_EQ(opened.last().at(0).value<TFile*>(), files.back().get());
    open("A.md");
    EXPECT_EQ(ws->lastOpenFiles(), QStringList({"A.md", "Folder/C.md", "B.md"}));
    // Unknown extensions don't open.
    files.push_back(makeFile("archive.zip"));
    ws->openFile(files.back().get());
    EXPECT_EQ(ws->lastOpenFiles().first(), "A.md");
    // Switching tabs changes the active file too (Obsidian's file-open).
    open("D.md", NewLeaf::Tab);
    WorkspaceLeaf* dLeaf = ws->activeLeaf();
    ws->previousTab();
    EXPECT_EQ(ws->lastOpenFiles().first(), "A.md");
    ws->setActiveLeaf(dLeaf, false);
    EXPECT_EQ(ws->lastOpenFiles().first(), "D.md");

    // Obsidian keeps at most 25 other notes behind the newest entry; the list never exceeds 50.
    for (int i = 0; i < 40; ++i)
        open(QStringLiteral("n%1.md").arg(i));
    QStringList recent = ws->lastOpenFiles();
    EXPECT_EQ(recent.size(), 26);
    EXPECT_EQ(recent.first(), "n39.md");
    EXPECT_EQ(recent.last(), "n14.md");
}

TEST_F(WorkspaceModelTest, RecentFilesCapPerCategory)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    for (int i = 0; i < 15; ++i)
        d->recordRecent(QStringLiteral("p%1.png").arg(i));
    for (int i = 0; i < 30; ++i)
        d->recordRecent(QStringLiteral("n%1.md").arg(i));
    for (int i = 0; i < 12; ++i)
        d->recordRecent(QStringLiteral("c%1.canvas").arg(i));
    const QStringList r = ws->lastOpenFiles();
    EXPECT_EQ(r.first(), "c11.canvas");
    EXPECT_EQ(r.filter(".canvas").size(), 11); // newest + 10
    EXPECT_EQ(r.filter(".md").size(), 25);
    EXPECT_EQ(r.filter(".png").size(), 10);
    EXPECT_LE(r.size(), 50);
}

TEST_F(WorkspaceModelTest, SidedockCollapseAndWidthPersist)
{
    auto ws = makeWorkspace();
    QSignalSpy layout(ws.get(), &Workspace::layoutChanged);
    ws->toggleSide(Side::Right);
    EXPECT_FALSE(ws->isSideCollapsed(Side::Right));
    ws->setSideCollapsed(Side::Left, true);
    ws->setSideWidth(Side::Right, 260);
    EXPECT_GE(layout.count(), 3);
    const QJsonObject j = ws->serialize();
    EXPECT_TRUE(j["left"].toObject()["collapsed"].toBool());
    EXPECT_FALSE(j["right"].toObject().contains("collapsed"));
    EXPECT_EQ(j["right"].toObject()["width"].toInt(), 260);
    auto ws2 = makeWorkspace(false);
    ASSERT_TRUE(ws2->restore(j));
    EXPECT_TRUE(ws2->isSideCollapsed(Side::Left));
    EXPECT_FALSE(ws2->isSideCollapsed(Side::Right));
    EXPECT_EQ(ws2->sideWidth(Side::Right), 260);
}

TEST_F(WorkspaceModelTest, CollapsingTheActiveSidedockActivatesTheRoot)
{
    auto ws = makeWorkspace();
    WorkspaceLeaf* root = ws->activeRootLeaf();
    WorkspaceLeaf* files = ws->leavesOfType("file-explorer").value(0);
    ws->setActiveLeaf(files, false);
    EXPECT_EQ(ws->activeLeaf(), files);
    EXPECT_EQ(ws->activeRootLeaf(), root); // most recent root leaf
    ws->setSideCollapsed(Side::Left, true);
    EXPECT_EQ(ws->activeLeaf(), root);
}

TEST_F(WorkspaceModelTest, EnsureAndRevealSideLeaf)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* outline = ws->ensureSideLeaf("outline", Side::Right, false);
    EXPECT_EQ(outline, ws->leavesOfType("outline").value(0)); // existing leaf
    EXPECT_TRUE(ws->isSideCollapsed(Side::Right));
    ws->revealLeaf(outline);
    EXPECT_FALSE(ws->isSideCollapsed(Side::Right));
    EXPECT_EQ(describe(d->right), "H[T(Backlinks,Outgoing links,Tags,Outline*)]");
    WorkspaceLeaf* g = ws->ensureSideLeaf("graph", Side::Left, true);
    EXPECT_EQ(describe(d->left), "H[T(Files,Search,Bookmarks,Graph*)]");
    EXPECT_TRUE(g->isInSidedock());
}

TEST_F(WorkspaceModelTest, CommandHelpers)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    for (const char* n : {"B", "C"})
        ws->getLeaf(NewLeaf::Tab)->setViewState(graph(n));
    EXPECT_EQ(describe(d->root), "V[T(A,B,C*)]");
    ws->nextTab();
    EXPECT_EQ(describe(d->root), "V[T(A*,B,C)]"); // wraps
    ws->previousTab();
    EXPECT_EQ(describe(d->root), "V[T(A,B,C*)]");
    ws->goToTab(1);
    EXPECT_EQ(ws->activeLeaf()->displayText(), "B");
    ws->goToTab(7); // out of range: ignored
    EXPECT_EQ(ws->activeLeaf()->displayText(), "B");
    ws->goToLastTab();
    EXPECT_EQ(ws->activeLeaf()->displayText(), "C");
    ws->togglePinActiveLeaf();
    EXPECT_TRUE(ws->activeLeaf()->isPinned());
    ws->closeActiveLeaf(); // unpins first
    EXPECT_FALSE(ws->activeLeaf()->isPinned());
    ws->closeActiveLeaf();
    EXPECT_EQ(describe(d->root), "V[T(A,B*)]");
    WorkspaceLeaf* nt = ws->newTab();
    EXPECT_EQ(describe(d->root), "V[T(A,B,_*)]");
    EXPECT_EQ(ws->activeLeaf(), nt);
    ws->setActiveLeaf(a, false);
    WorkspaceLeaf* dup = ws->duplicateLeaf(a, NewLeaf::SplitRight);
    ASSERT_TRUE(dup);
    EXPECT_EQ(dup->viewState(), a->viewState());
    EXPECT_EQ(describe(d->root), "V[T(A*,B,_),T(A*)]");
}

TEST_F(WorkspaceModelTest, ActiveLeafSignalsAndFileViewState)
{
    auto ws = makeWorkspace();
    QSignalSpy active(ws.get(), &Workspace::activeLeafChanged);
    WorkspaceLeaf* a = ws->activeRootLeaf();
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::SplitRight);
    EXPECT_EQ(active.count(), 1);
    EXPECT_EQ(active.last().at(0).value<WorkspaceLeaf*>(), b);
    ws->setActiveLeaf(a, false);
    EXPECT_EQ(active.count(), 2);
    ws->setActiveLeaf(a, false);
    EXPECT_EQ(active.count(), 2);

    // Without a vault a FileView keeps the path and shows the file name.
    b->setViewState(note("Projects/Alpha.md"));
    EXPECT_EQ(b->displayText(), "Alpha");
    EXPECT_EQ(b->viewState()["state"].toObject()["file"].toString(), "Projects/Alpha.md");
    EXPECT_EQ(b->viewState()["title"].toString(), "Alpha");
    EXPECT_EQ(b->viewState()["icon"].toString(), "file-text");
    auto* view = qobject_cast<FileView*>(b->view());
    ASSERT_TRUE(view);
    EXPECT_EQ(view->file(), nullptr);
}

TEST_F(WorkspaceModelTest, DeletedFileGoesBackOrCloses)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    auto fa = makeFile("A.md"), fb = makeFile("B.md");
    WorkspaceLeaf* leaf = ws->activeRootLeaf();
    leaf->setViewState(note("A.md"));
    leaf->setViewState(note("B.md"));
    auto* v = static_cast<TestNoteView*>(leaf->view());
    v->loadFile(fb.get());
    v->onDelete(fb.get()); // back to A
    EXPECT_EQ(leaf->displayText(), "A");
    auto* v2 = static_cast<TestNoteView*>(leaf->view());
    v2->loadFile(fa.get());
    WorkspaceLeaf* other = ws->getLeaf(NewLeaf::Tab);
    other->setViewState(graph("G"));
    v2->onDelete(fa.get()); // no history left: the empty tab closes since the group has others
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(describe(d->root), "V[T(G*)]");
}

TEST_F(WorkspaceModelTest, PinnedAndCloseMenusSkipPinnedTabs)
{
    auto ws = makeWorkspace();
    WorkspaceImpl* d = impl(ws.get());
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    for (const char* n : {"B", "C", "D"})
        ws->getLeaf(NewLeaf::Tab)->setViewState(graph(n));
    WorkspaceLeaf* b = d->leavesUnder(d->root).at(1);
    WorkspaceLeaf* c = d->leavesUnder(d->root).at(2);
    b->setPinned(true);
    EXPECT_EQ(c->displayText(), "C");
    d->closeAfter(b);
    EXPECT_EQ(describe(d->root), "V[T(A,B*)]");
    d->closeOthers(b);
    EXPECT_EQ(d->leavesUnder(d->root).size(), 1);
    EXPECT_TRUE(d->leavesUnder(d->root).first()->isPinned());
    d->closeAll(b->tabs());
    EXPECT_EQ(d->leavesUnder(d->root).size(), 1); // pinned tabs survive "Close all"
}
