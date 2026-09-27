// Pop-out windows (Obsidian's floating workspace windows): the workspace model (moving / opening tabs in
// new windows, getLeaf across windows, closing, undo), workspace.json round trips and restore on
// restart, tab drags between windows and out of every window, the active leaf following window focus,
// menus and modals of a window that goes away, a stacked tab group in a pop-out; with a full session: the
// window commands (always on top, stacked tabs), hotkeys and the command palette inside a pop-out, link
// opening from a pop-out, notices and page previews there.
#include "ui/navigation_test_support.h"
#include "ui/workspace_test_support.h"

#include "app/scenario_runner.h"
#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "editor/editor_view.h"
#include "ui/overlay/modal.h"
#include "ui/shell/popout_window.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/views/markdown/page_preview.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

using namespace oc;
using namespace oc::test;

namespace {
using NewLeaf = Workspace::NewLeaf;
using Target = WorkspaceImpl::DropTarget;

void sendMouse(QWidget* w, QEvent::Type type, QPoint local, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(local), QPointF(w->mapToGlobal(local)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void flush()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QStringList titles(Workspace* ws)
{
    QStringList out;
    ws->iterateAllLeaves([&](WorkspaceLeaf* l) { out << l->displayText(); });
    return out;
}

// Sends a key the way the window system does: ShortcutOverride, then KeyPress (HotkeyDispatcher).
void pressKey(QWidget* target, int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent so(QEvent::ShortcutOverride, key, mods);
    so.ignore();
    QApplication::sendEvent(target, &so);
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(target, &release);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Model + widgets (test views, no main window)
// ---------------------------------------------------------------------------------------------

class PopoutTest : public WorkspaceFixture {
protected:
    void SetUp() override
    {
        WorkspaceFixture::SetUp();
        m_ws = makeWorkspace();
        m_host = std::make_unique<QWidget>();
        auto* l = new QVBoxLayout(m_host.get());
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(m_ws->widget());
        m_host->setGeometry(0, 0, 1200, 800);
        m_host->show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(m_host.get()));
        flush();
    }
    void TearDown() override
    {
        m_ws.reset();
        m_host.reset();
        flush();
        WorkspaceFixture::TearDown();
    }
    WorkspaceImpl* d() const { return impl(m_ws.get()); }
    WorkspaceSplit* win(int i = 0) const { return d()->windows.value(i); }
    WorkspaceTabs* winTabs(int w = 0, int i = 0) const
    {
        QList<WorkspaceTabs*> all;
        d()->forEachTabs(win(w), [&](WorkspaceTabs* t) { all << t; });
        return all.value(i);
    }
    QWidget* ws() const { return m_ws->widget(); }
    // A (A, B, C) root group; returns {a, b, c}.
    QList<WorkspaceLeaf*> buildRoot()
    {
        WorkspaceLeaf* a = m_ws->activeRootLeaf();
        a->setViewState(graph("A"));
        WorkspaceLeaf* b = m_ws->getLeaf(NewLeaf::Tab);
        b->setViewState(graph("B"));
        WorkspaceLeaf* c = m_ws->getLeaf(NewLeaf::Tab);
        c->setViewState(graph("C"));
        flush();
        EXPECT_EQ(describe(d()->root), "V[T(A,B,C*)]");
        return {a, b, c};
    }
    // Drags tab `index` of `from` to a global point (a synthetic mouse drag, like the workspace tests).
    void dragTab(WorkspaceTabs* from, int index, QPoint globalTarget)
    {
        TabHeaderBar* h = from->w->header();
        const QPoint start = h->tabRect(index).center();
        sendMouse(h, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(h, QEvent::MouseMove, start + QPoint(12, 0), Qt::NoButton, Qt::LeftButton);
        EXPECT_TRUE(d()->drag.active);
        const QPoint target = h->mapFromGlobal(globalTarget);
        sendMouse(h, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        sendMouse(h, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(d()->drag.active);
        flush();
    }
    static QPoint globalIn(WorkspaceTabs* t, double fx, double fy)
    {
        const QRect r(t->w->mapToGlobal(QPoint(0, 0)), t->w->size());
        return QPoint(r.x() + int(r.width() * fx), r.y() + int(r.height() * fy));
    }
    // In the tab header, just before the end of the last tab: inserts after it.
    static QPoint afterLastTab(WorkspaceTabs* t)
    {
        TabHeaderBar* h = t->w->header();
        return h->mapToGlobal(QPoint(h->tabRect(h->tabCount() - 1).right() - 10, 25));
    }

    std::unique_ptr<Workspace> m_ws;
    std::unique_ptr<QWidget> m_host;
};

TEST_F(PopoutTest, MovingATabToANewWindow)
{
    const auto leaves = buildRoot();
    WorkspaceLeaf* b = leaves[1];
    m_ws->setActiveLeaf(b, true);
    PopoutWindow* p = m_ws->moveLeafToPopout(b, QRect(1300, 40, 700, 500));
    ASSERT_TRUE(p);
    flush();
    EXPECT_EQ(m_ws->popoutWindows(), QList<PopoutWindow*>{p});
    EXPECT_EQ(m_ws->popoutOf(b), p);
    EXPECT_EQ(m_ws->popoutOf(leaves[0]), nullptr);
    EXPECT_EQ(describe(d()->root), "V[T(A,C*)]");
    EXPECT_EQ(describe(win()), "V[T(B*)]");
    // The leaf keeps its view and is shown inside the new top-level window.
    EXPECT_EQ(b->displayText(), "B");
    EXPECT_TRUE(p->isVisible());
    EXPECT_TRUE(p->isWindow());
    EXPECT_EQ(p->parentWidget(), nullptr); // its own window, not kept above the main one
    EXPECT_EQ(p->size(), QSize(700, 500));
    EXPECT_EQ(p->pos(), QPoint(1300, 40));
    EXPECT_EQ(d()->leafWidget(b)->window(), p);
    EXPECT_TRUE(d()->leafWidget(b)->isVisible());
    EXPECT_TRUE(d()->leafWidget(b)->header()); // pop-out leaves have a view header, like the root's
    EXPECT_TRUE(p->windowTitle().startsWith("B - "));
    // Its top tab group is the title bar (no ribbon, sidedocks or status bar in the window).
    EXPECT_TRUE(winTabs()->w->header()->isTop());
    // The leaf takes part in iterateAllLeaves (last: Obsidian's root, left, right, floating) and in the
    // active leaf tracking.
    EXPECT_EQ(titles(m_ws.get()).last(), "B");
    EXPECT_TRUE(titles(m_ws.get()).contains("A"));
    EXPECT_EQ(m_ws->activeLeaf(), b);
    EXPECT_EQ(m_ws->activeRootLeaf(), b);
    EXPECT_EQ(m_ws->containerLeaves(b), QList<WorkspaceLeaf*>{b});
    EXPECT_EQ(m_ws->containerLeaves(leaves[0]).size(), 2);
    EXPECT_EQ(m_ws->mostRecentLeafIn(p), b);

    // Closing its last tab closes the window (Obsidian's WorkspaceWindow.removeChild()).
    QPointer<PopoutWindow> guard = p;
    b->detach();
    flush();
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
    EXPECT_FALSE(guard);
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "C");
}

TEST_F(PopoutTest, MovingTheLastRootTabLeavesAnEmptyTab)
{
    WorkspaceLeaf* a = m_ws->activeRootLeaf();
    a->setViewState(graph("A"));
    PopoutWindow* p = m_ws->moveLeafToPopout(a);
    ASSERT_TRUE(p);
    flush();
    EXPECT_EQ(describe(d()->root), "V[T(_*)]");
    EXPECT_EQ(describe(win()), "V[T(A*)]");
    // By default the window is as large as the leaf was, at least 600x600 (Obsidian).
    EXPECT_GE(p->width(), 600);
    EXPECT_GE(p->height(), 600);
}

TEST_F(PopoutTest, GetLeafAcrossWindows)
{
    buildRoot();
    // getLeaf("window"): a new window with an empty tab.
    WorkspaceLeaf* w = m_ws->getLeaf(NewLeaf::Window);
    ASSERT_TRUE(w);
    ASSERT_EQ(m_ws->popoutWindows().size(), 1);
    PopoutWindow* p = m_ws->popoutWindows().first();
    EXPECT_EQ(m_ws->popoutOf(w), p);
    EXPECT_EQ(m_ws->activeLeaf(), w);
    w->setViewState(graph("W"));
    // getLeaf(false) = Obsidian's getUnpinnedLeaf(): the active leaf when it can navigate...
    EXPECT_EQ(m_ws->getLeaf(NewLeaf::No), w);
    // ...else a leaf of its container: a new tab in the pop-out, not in the main window.
    w->setPinned(true);
    WorkspaceLeaf* n = m_ws->getLeaf(NewLeaf::No);
    EXPECT_NE(n, w);
    EXPECT_EQ(m_ws->popoutOf(n), p);
    EXPECT_EQ(describe(win()), "V[T(W,_*)]");
    EXPECT_EQ(describe(d()->root), "V[T(A,B,C*)]");
    // A new tab / split goes next to the most recent leaf: in the pop-out.
    n->setViewState(graph("N"));
    WorkspaceLeaf* t = m_ws->getLeaf(NewLeaf::Tab);
    EXPECT_EQ(m_ws->popoutOf(t), p);
    WorkspaceLeaf* s = m_ws->getLeaf(NewLeaf::SplitRight);
    EXPECT_EQ(m_ws->popoutOf(s), p);
    EXPECT_EQ(describe(win()), "V[T(W,N,_*),T(_*)]");
    // openFile(Window): each call its own window.
    auto file = makeFile("Notes/Plan.md");
    m_ws->openFile(file.get(), NewLeaf::Window);
    EXPECT_EQ(m_ws->popoutWindows().size(), 2);
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "Plan");
    EXPECT_EQ(m_ws->popoutOf(m_ws->activeLeaf()), m_ws->popoutWindows().last());
    // duplicateLeaf(Window): "Open current tab in new window".
    WorkspaceLeaf* copy = m_ws->duplicateLeaf(d()->root->children.first()->asTabs()->leaves[0], NewLeaf::Window);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->displayText(), "A");
    EXPECT_EQ(m_ws->popoutWindows().size(), 3);
}

TEST_F(PopoutTest, WorkspaceJsonRoundTrip)
{
    const auto leaves = buildRoot();
    WorkspaceLeaf* b = leaves[1];
    WorkspaceLeaf* c = leaves[2];
    m_ws->moveLeafToPopout(b, QRect(1300, 50, 640, 480));
    m_ws->setActiveLeaf(b, false);
    m_ws->moveLeafToPopout(c, QRect(1300, 600, 500, 400)); // a second window
    m_ws->setActiveLeaf(b, false);
    WorkspaceLeaf* d2 = m_ws->getLeaf(NewLeaf::Tab); // in b's window
    d2->setViewState(graph("D"));
    d2->setPinned(true);
    WorkspaceLeaf* e = m_ws->splitLeaf(d2, Qt::Vertical);
    e->setViewState(graph("E"));
    flush();
    ASSERT_EQ(describe(win(0)), "V[H[T(B,D*),T(E*)]]");

    QJsonObject json = m_ws->serialize();
    const QJsonObject floating = json["floating"].toObject();
    EXPECT_EQ(floating["type"].toString(), "floating");
    EXPECT_FALSE(floating["id"].toString().isEmpty());
    const QJsonArray windows = floating["children"].toArray();
    ASSERT_EQ(windows.size(), 2);
    const QJsonObject w0 = windows[0].toObject();
    EXPECT_EQ(w0["type"].toString(), "window");
    EXPECT_EQ(w0["direction"].toString(), "vertical");
    EXPECT_EQ(w0["x"].toInt(), 1300);
    EXPECT_EQ(w0["y"].toInt(), 50);
    EXPECT_EQ(w0["width"].toInt(), 640);
    EXPECT_EQ(w0["height"].toInt(), 480);
    EXPECT_EQ(w0["maximize"].toBool(), false);
    EXPECT_EQ(w0["zoom"].toDouble(), 0.0);
    const QJsonObject split = w0["children"].toArray()[0].toObject();
    EXPECT_EQ(split["type"].toString(), "split");
    EXPECT_EQ(split["direction"].toString(), "horizontal");
    const QJsonObject tabs = split["children"].toArray()[0].toObject();
    EXPECT_EQ(tabs["type"].toString(), "tabs");
    EXPECT_EQ(tabs["children"].toArray().size(), 2);
    EXPECT_TRUE(tabs["children"].toArray()[1].toObject()["pinned"].toBool());
    EXPECT_EQ(windows[1].toObject()["y"].toInt(), 600);
    EXPECT_EQ(json["active"].toString(), e->id());

    // Unknown keys of the floating split, a window and a leaf survive a round trip.
    QJsonObject f2 = floating;
    f2["future"] = 7;
    QJsonArray w2 = windows;
    QJsonObject win0 = w2[0].toObject();
    win0["someWindowKey"] = "kept";
    w2[0] = win0;
    f2["children"] = w2;
    json["floating"] = f2;
    auto ws2 = makeWorkspace(false);
    ASSERT_TRUE(ws2->restore(json));
    EXPECT_EQ(ws2->serialize(), json) << QJsonDocument(ws2->serialize()).toJson().constData();
    ASSERT_EQ(ws2->popoutWindows().size(), 2);
    EXPECT_EQ(describe(impl(ws2.get())->windows[0]), "V[H[T(B,D*),T(E*)]]");
    EXPECT_TRUE(impl(ws2.get())->leafById(d2->id())->isPinned());
    EXPECT_EQ(ws2->popoutWindows()[0]->size(), QSize(640, 480));
    EXPECT_TRUE(ws2->popoutWindows()[0]->isVisible());
    EXPECT_EQ(ws2->activeLeaf()->id(), e->id());

    // Obsidian drops the floating split's non-window children and windows without leaves.
    QJsonObject odd = json;
    odd["floating"] = parseJson(R"({"id": "f1", "type": "floating", "children": [
        {"type": "tabs", "children": [{"type": "leaf", "state": {"type": "graph", "state": {"name": "X"}}}]},
        {"id": "w1", "type": "window", "direction": "vertical", "children": []}]})");
    auto ws3 = makeWorkspace(false);
    ASSERT_TRUE(ws3->restore(odd));
    EXPECT_TRUE(ws3->popoutWindows().isEmpty());
    EXPECT_FALSE(ws3->serialize().contains("floating"));
}

TEST_F(PopoutTest, PopoutsAreRestoredOnRestart)
{
    QString bId;
    {
        const auto leaves = buildRoot();
        bId = leaves[1]->id();
        m_ws->moveLeafToPopout(leaves[1], QRect(1300, 50, 640, 480));
        m_ws->setActiveLeaf(leaves[1], false);
        m_ws->getLeaf(NewLeaf::Tab)->setViewState(graph("D"));
        flush();
        m_ws->requestSaveLayout();
        m_ws.reset(); // saves the pending layout
        flush();
    }
    const QJsonObject saved = JsonFile::read(ClonePaths::vaultConfigDir(m_session->id()) + "/workspace.json");
    EXPECT_EQ(saved["floating"].toObject()["children"].toArray().size(), 1);
    m_ws = makeWorkspace(); // loadLayout()
    flush();
    ASSERT_EQ(m_ws->popoutWindows().size(), 1);
    PopoutWindow* p = m_ws->popoutWindows().first();
    EXPECT_TRUE(p->isVisible());
    EXPECT_EQ(p->size(), QSize(640, 480));
    EXPECT_EQ(p->pos(), QPoint(1300, 50));
    EXPECT_EQ(describe(win()), "V[T(B,D*)]");
    EXPECT_EQ(describe(d()->root), "V[T(A,C*)]");
    EXPECT_EQ(m_ws->popoutOf(d()->leafById(bId)), p);
}

TEST_F(PopoutTest, ClosingAPopoutClosesItsLeavesPinnedOnesToo)
{
    const auto leaves = buildRoot();
    WorkspaceLeaf* b = leaves[1];
    PopoutWindow* p = m_ws->moveLeafToPopout(b, QRect(1300, 40, 700, 500));
    m_ws->setActiveLeaf(b, true);
    WorkspaceLeaf* dd = m_ws->getLeaf(NewLeaf::Tab);
    dd->setViewState(graph("D"));
    dd->setPinned(true);
    m_ws->setActiveLeaf(dd, true);
    flush();
    ASSERT_EQ(describe(win()), "V[T(B,D*)]");
    QPointer<PopoutWindow> guard = p;
    QPointer<WorkspaceLeaf> gb = b, gd = dd;
    p->close(); // its window button / Alt+F4
    flush();
    EXPECT_FALSE(guard);
    EXPECT_FALSE(gb);
    EXPECT_FALSE(gd); // Obsidian detaches every leaf, pinned or not
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
    EXPECT_EQ(describe(d()->root), "V[T(A,C*)]");
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "C"); // the most recent leaf elsewhere
    EXPECT_FALSE(m_ws->serialize().contains("floating"));
    // Both are in "Undo close tab"; their window is gone, so they reopen next to the most recent leaf.
    ASSERT_TRUE(m_ws->canUndoCloseLeaf());
    m_ws->undoCloseLeaf();
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "D");
    EXPECT_EQ(describe(d()->root), "V[T(A,C*),T(D*)]");
}

TEST_F(PopoutTest, UndoCloseTabReopensInItsWindow)
{
    const auto leaves = buildRoot();
    PopoutWindow* p = m_ws->moveLeafToPopout(leaves[1], QRect(1300, 40, 700, 500));
    m_ws->setActiveLeaf(leaves[1], true);
    WorkspaceLeaf* dd = m_ws->getLeaf(NewLeaf::Tab);
    dd->setViewState(graph("D"));
    flush();
    dd->detach();
    flush();
    EXPECT_EQ(describe(win()), "V[T(B*)]");
    m_ws->undoCloseLeaf();
    EXPECT_EQ(describe(win()), "V[T(B,D*)]");
    EXPECT_EQ(m_ws->popoutOf(m_ws->activeLeaf()), p);
    // A tab closed in a window that closed since: a new window.
    m_ws->activeLeaf()->detach();
    p->close();
    flush();
    ASSERT_TRUE(m_ws->popoutWindows().isEmpty());
    m_ws->undoCloseLeaf(); // B (closed with its window: next to the most recent leaf)
    m_ws->undoCloseLeaf(); // D (closed in the window: Obsidian opens a new window)
    ASSERT_EQ(m_ws->popoutWindows().size(), 1);
    EXPECT_EQ(describe(win()), "V[T(D*)]");
}

TEST_F(PopoutTest, ActiveLeafFollowsWindowFocus)
{
    const auto leaves = buildRoot();
    WorkspaceLeaf* b = leaves[1];
    PopoutWindow* p = m_ws->moveLeafToPopout(b, QRect(1300, 40, 700, 500));
    ASSERT_TRUE(p);
    flush();
    m_host->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(m_host.get()));
    flush();
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "C"); // the main window's most recent leaf
    p->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(p));
    flush();
    EXPECT_EQ(m_ws->activeLeaf(), b);
    // A sidedock leaf counts as the main window: focusing it there, then the main window, keeps it.
    WorkspaceLeaf* side = d()->left->children.first()->asTabs()->leaves[0];
    m_host->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(m_host.get()));
    m_ws->setActiveLeaf(side, false);
    flush();
    EXPECT_EQ(m_ws->activeLeaf(), side);
    // setActiveLeaf(focus) on a leaf in another window brings that window to the front.
    m_ws->setActiveLeaf(b, true);
    EXPECT_TRUE(QTest::qWaitForWindowActive(p));
}

TEST_F(PopoutTest, TabDragsBetweenWindows)
{
    const auto leaves = buildRoot();
    WorkspaceLeaf* a = leaves[0];
    m_ws->setActiveLeaf(leaves[2], true);
    WorkspaceLeaf* x = m_ws->openPopoutLeaf(QRect(1300, 40, 700, 500));
    x->setViewState(graph("X"));
    flush();
    WorkspaceTabs* root = rootTabs(m_ws.get());
    WorkspaceTabs* X = winTabs();
    ASSERT_EQ(describe(X), "T(X*)");

    // Main window -> pop-out: the targets are computed in the window under the pointer.
    Target t = d()->dropTargetAt(leaves[1], globalIn(X, 0.9, 0.6));
    EXPECT_EQ(t.kind, Target::Kind::Tabs);
    EXPECT_EQ(t.tabs, X);
    EXPECT_EQ(t.edge, DropEdge::Right);
    EXPECT_EQ(t.host, win()->w.data());
    dragTab(root, 1, afterLastTab(X)); // B after X
    EXPECT_EQ(describe(d()->root), "V[T(A,C*)]");
    EXPECT_EQ(describe(win()), "V[T(X,B*)]");
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "B");

    // Pop-out -> main window edge: a split there.
    dragTab(X, 0, globalIn(root, 0.95, 0.6));
    EXPECT_EQ(describe(d()->root), "V[T(A,C*),T(X*)]");
    EXPECT_EQ(describe(win()), "V[T(B*)]");

    // The pop-out's last tab into the main window: the window closes.
    QPointer<PopoutWindow> guard = m_ws->popoutWindows().value(0);
    dragTab(winTabs(), 0, afterLastTab(rootTabs(m_ws.get())));
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
    EXPECT_FALSE(guard);
    EXPECT_EQ(describe(d()->root), "V[T(A,C,B*),T(X*)]");

    // Released outside every window: a new window at the drop point (Obsidian), with the leaf's size.
    EXPECT_EQ(d()->dropTargetAt(a, QPoint(4000, 3000)).kind, Target::Kind::NewWindow);
    dragTab(rootTabs(m_ws.get()), 0, QPoint(4000, 3000));
    ASSERT_EQ(m_ws->popoutWindows().size(), 1);
    EXPECT_EQ(describe(win()), "V[T(A*)]");
    EXPECT_EQ(m_ws->popoutWindows().first()->pos(), QPoint(4000, 3000));
    EXPECT_EQ(m_ws->activeLeaf(), a);
    EXPECT_FALSE(d()->overlay && d()->overlay->isVisible());
    EXPECT_EQ(QApplication::overrideCursor(), nullptr);
}

TEST_F(PopoutTest, StackedGroupInAPopout)
{
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false); // reveals jump
    const auto leaves = buildRoot();
    PopoutWindow* p = m_ws->moveLeafToPopout(leaves[0], QRect(1300, 40, 900, 600));
    ASSERT_TRUE(p);
    WorkspaceTabs* t = winTabs();
    for (const char* name : {"Y", "Z"})
        d()->appendNewTab(t)->setViewState(graph(name));
    flush();
    ASSERT_EQ(describe(t), "T(A,Y,Z*)");
    ASSERT_TRUE(m_ws->setStacked(t->leaves[0], true));
    flush();
    // The pop-out's group renders stacked; its header row (the title bar) keeps "+" and the tab list.
    ASSERT_TRUE(t->w->isStacked());
    EXPECT_EQ(t->w->window(), p);
    EXPECT_TRUE(t->w->header()->isTop());
    EXPECT_EQ(t->w->header()->tabCount(), 0);
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(t->w->spine(i)) << i;
    EXPECT_TRUE(t->w->spine(2)->isVisible());
    EXPECT_EQ(t->w->stackScroll(), t->w->stackMaxScroll()); // Z (current) in view
    EXPECT_FALSE(d()->root->children.first()->asTabs()->stacked); // the main window's group isn't

    // workspace.json: in the floating split's window, restored on the next start.
    const QJsonObject json = m_ws->serialize();
    const QJsonObject winGroup = json["floating"].toObject()["children"].toArray()[0].toObject()["children"].toArray()[0].toObject();
    EXPECT_EQ(winGroup["stacked"], QJsonValue(true));
    ASSERT_TRUE(m_ws->restore(json));
    flush();
    WorkspaceTabs* t2 = winTabs();
    ASSERT_TRUE(t2);
    EXPECT_TRUE(t2->w->isStacked());
    ASSERT_TRUE(t2->w->spine(0));
    EXPECT_EQ(t2->w->spine(0)->leaf()->displayText(), "A");
    EXPECT_EQ(t2->w->window(), m_ws->popoutWindows().value(0));
    EXPECT_EQ(t2->w->stackScroll(), t2->w->stackMaxScroll());

    // A spine dragged into the main window's tab strip moves the tab there.
    StackedTabHeader* sy = t2->w->spine(1);
    ASSERT_EQ(sy->leaf()->displayText(), "Y");
    const QPoint start = sy->titleRect().center();
    sendMouse(sy, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(sy, QEvent::MouseMove, start + QPoint(0, 12), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(d()->drag.active);
    const QPoint target = sy->mapFromGlobal(afterLastTab(rootTabs(m_ws.get())));
    sendMouse(sy, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    sendMouse(sy, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    flush();
    EXPECT_EQ(describe(d()->root), "V[T(B,C,Y*)]");
    EXPECT_EQ(describe(win()), "V[T(A,Z*)]");
    EXPECT_TRUE(winTabs()->w->isStacked());
    EXPECT_EQ(winTabs()->w->spine(2), nullptr);
    // Closing the stacked pop-out closes its tabs (with the window).
    m_ws->closePopout(m_ws->popoutWindows().value(0));
    flush();
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
    EXPECT_EQ(describe(d()->root), "V[T(B,C,Y*)]");
}

TEST_F(PopoutTest, NativeDropsIntoAWindow)
{
    // Wayland: a tab leaving its window continues as a native drag; the window it is dropped on
    // accepts it through an event filter on its QWindow (window-local positions only).
    const auto leaves = buildRoot();
    WorkspaceLeaf* x = m_ws->openPopoutLeaf(QRect(1300, 40, 700, 500));
    x->setViewState(graph("X"));
    flush();
    d()->installNativeDrop();
    PopoutWindow* p = m_ws->popoutWindows().first();
    ASSERT_TRUE(p->windowHandle());
    QMimeData mime;
    mime.setData("application/x-obsidian-cpp-leaf", leaves[1]->id().toUtf8());
    const QPointF pos = QPointF(p->mapFromGlobal(globalIn(winTabs(), 0.5, 0.95)));
    QDragEnterEvent enter(pos.toPoint(), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(p->windowHandle(), &enter);
    EXPECT_TRUE(enter.isAccepted());
    EXPECT_TRUE(win()->dropOverlay && win()->dropOverlay->targetRect().isValid());
    // Other drags (files from another application...) are left alone.
    QMimeData other;
    other.setText("text");
    QDragEnterEvent foreign(pos.toPoint(), Qt::CopyAction, &other, Qt::LeftButton, Qt::NoModifier);
    foreign.ignore();
    QApplication::sendEvent(p->windowHandle(), &foreign);
    EXPECT_FALSE(foreign.isAccepted());
    QDropEvent drop(pos, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(p->windowHandle(), &drop);
    EXPECT_TRUE(drop.isAccepted());
    flush();
    EXPECT_EQ(describe(win()), "V[H[T(X*),T(B*)]]");
    EXPECT_EQ(describe(d()->root), "V[T(A,C*)]");
}

TEST_F(PopoutTest, TabMenuMovesToNewWindowAndSurvivesItsWindowClosing)
{
    const auto leaves = buildRoot();
    WorkspaceTabs* root = rootTabs(m_ws.get());
    TabHeaderBar* h = root->w->header();
    // "Move to new window" in the tab header menu.
    bool moved = false;
    QTimer::singleShot(50, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_TRUE(menu);
        for (QAction* act : menu->actions())
            if (act->text() == "Move to new window") {
                act->trigger();
                moved = true;
            }
        menu->close();
    });
    QContextMenuEvent e(QContextMenuEvent::Mouse, h->tabRect(0).center(), h->mapToGlobal(h->tabRect(0).center()));
    QApplication::sendEvent(h, &e);
    flush();
    EXPECT_TRUE(moved);
    ASSERT_EQ(m_ws->popoutWindows().size(), 1);
    EXPECT_EQ(describe(win()), "V[T(A*)]");

    // "Close" on the window's only tab closes the window while its menu runs its event loop.
    PopoutWindow* p = m_ws->popoutWindows().first();
    QPointer<PopoutWindow> guard = p;
    TabHeaderBar* ph = winTabs()->w->header();
    bool closed = false;
    QTimer::singleShot(50, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_TRUE(menu);
        EXPECT_EQ(menu->window(), menu); // a popup, parented to the pop-out
        for (QAction* act : menu->actions())
            if (act->text() == "Close" && !closed) {
                closed = true;
                act->trigger();
            }
    });
    QContextMenuEvent e2(QContextMenuEvent::Mouse, ph->tabRect(0).center(), ph->mapToGlobal(ph->tabRect(0).center()));
    QApplication::sendEvent(ph, &e2);
    flush();
    EXPECT_TRUE(closed);
    EXPECT_FALSE(guard);
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
    EXPECT_FALSE(QApplication::activePopupWidget());

    // The tab list's "Close all" closes the window too.
    WorkspaceLeaf* y = m_ws->openPopoutLeaf(QRect(1300, 40, 700, 500));
    y->setViewState(graph("Y"));
    flush();
    QPointer<PopoutWindow> guard2 = m_ws->popoutWindows().value(0);
    bool closedAll = false;
    QTimer::singleShot(50, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_TRUE(menu);
        for (QAction* act : menu->actions())
            if (act->text() == "Close all" && !closedAll) {
                closedAll = true;
                act->trigger();
            }
    });
    winTabs()->w->header()->tabListButton()->click();
    flush();
    EXPECT_TRUE(closedAll);
    EXPECT_FALSE(guard2);
    EXPECT_FALSE(QApplication::activePopupWidget());
}

TEST_F(PopoutTest, ClosingAWindowWithAnOpenModal)
{
    buildRoot();
    WorkspaceLeaf* x = m_ws->openPopoutLeaf(QRect(1300, 40, 700, 500));
    x->setViewState(graph("X"));
    flush();
    PopoutWindow* p = m_ws->popoutWindows().first();
    QPointer<Modal> modal = new Modal(p->overlay());
    modal->setTitle("Still open");
    modal->addButton("OK");
    modal->open();
    flush();
    EXPECT_TRUE(modal->isVisible());
    EXPECT_EQ(modal->window(), p);
    QPointer<PopoutWindow> guard = p;
    m_ws->closePopout(p);
    flush();
    EXPECT_FALSE(guard);
    EXPECT_FALSE(modal);
    // A synchronous modal (exec) whose window goes away returns.
    WorkspaceLeaf* y = m_ws->openPopoutLeaf(QRect(1300, 40, 700, 500));
    y->setViewState(graph("Y"));
    flush();
    PopoutWindow* p2 = m_ws->popoutWindows().first();
    auto* m2 = new Modal(p2->overlay());
    m2->addButton("OK");
    QTimer::singleShot(50, [&] { m_ws->closePopout(p2); });
    EXPECT_EQ(m2->exec(), -1);
    flush();
    EXPECT_TRUE(m_ws->popoutWindows().isEmpty());
}

// ---------------------------------------------------------------------------------------------
// A full session (commands, hotkeys, markdown views)
// ---------------------------------------------------------------------------------------------

namespace {
PopoutWindow* onlyPopout(NavigationEnv& env)
{
    const QList<PopoutWindow*> all = env.workspace->popoutWindows();
    return all.size() == 1 ? all.first() : nullptr;
}

void activate(QWidget* w)
{
    w->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(w));
    flush();
}
} // namespace

TEST(PopoutSession, MoveAndOpenInNewWindowCommands)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_TRUE(mv);
    const Command* move = env.commands->find("workspace:move-to-new-window");
    const Command* open = env.commands->find("workspace:open-in-new-window");
    ASSERT_TRUE(move && open);
    EXPECT_EQ(move->name, "Move current tab to new window");
    EXPECT_EQ(open->name, "Open current tab in new window");
    ASSERT_TRUE(env.commands->find("editor:open-link-in-new-window"));
    EXPECT_EQ(env.commands->hotkeys("editor:open-link-in-new-window").value(0).toString(), "Ctrl + Alt + Shift + Enter");

    // Open: a copy of the tab in a new window (the original stays).
    ASSERT_TRUE(env.commands->execute("workspace:open-in-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    EXPECT_EQ(env.rootLeafCount(), 1);
    EXPECT_EQ(env.activePath(), "Welcome.md");
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);
    EXPECT_NE(env.activeMarkdown(), mv);
    EXPECT_EQ(env.activeMarkdown()->window(), p);
    // The documents are shared: an edit in one window shows in the other.
    EXPECT_EQ(env.activeMarkdown()->document(), mv->document());

    // Move: the tab itself (same view) goes to a new window.
    env.workspace->setActiveLeaf(mv->leaf(), true);
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    ASSERT_EQ(env.workspace->popoutWindows().size(), 2);
    EXPECT_EQ(mv->window(), env.workspace->popoutWindows().last());
    EXPECT_EQ(env.workspace->activeLeaf(), mv->leaf());
    EXPECT_EQ(env.rootLeafCount(), 1); // an empty tab in the main window

    // workspace:close-window closes the focused pop-out (its tabs close), not the main window.
    activate(env.workspace->popoutWindows().last());
    ASSERT_TRUE(env.commands->execute("workspace:close-window"));
    flush();
    EXPECT_EQ(env.workspace->popoutWindows().size(), 1);
    EXPECT_TRUE(env.window->isVisible());
}

TEST(PopoutSession, StackedTabsAndAlwaysOnTopCommands)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    const Command* stack = env.commands->find("workspace:toggle-stacked-tabs");
    const Command* onTop = env.commands->find("window:toggle-always-on-top");
    ASSERT_TRUE(stack && onTop);
    EXPECT_EQ(stack->name, "Toggle stacked tabs");
    EXPECT_EQ(stack->icon, "layers");
    EXPECT_EQ(onTop->name, "Toggle window always on top");

    // workspace:toggle-stacked-tabs: the active leaf's group (not in a sidebar).
    activate(env.window);
    WorkspaceLeaf* welcome = env.workspace->activeLeaf();
    ASSERT_TRUE(welcome);
    ASSERT_TRUE(env.commands->execute("workspace:toggle-stacked-tabs"));
    EXPECT_TRUE(env.workspace->isStacked(welcome));
    EXPECT_TRUE(welcome->tabs()->w->isStacked());
    ASSERT_TRUE(env.commands->execute("workspace:toggle-stacked-tabs"));
    EXPECT_FALSE(env.workspace->isStacked(welcome));
    WorkspaceLeaf* files = env.workspace->leavesOfType("file-explorer").value(0);
    ASSERT_TRUE(files);
    env.workspace->setActiveLeaf(files, false);
    EXPECT_FALSE(env.commands->isAvailable("workspace:toggle-stacked-tabs"));
    env.workspace->setActiveLeaf(welcome, false);

    // window:toggle-always-on-top: never for the main window.
    EXPECT_FALSE(env.commands->isAvailable("window:toggle-always-on-top"));
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    activate(p);
    const QRect before = p->geometry();
    const Qt::WindowFlags flags = p->windowFlags();
    QSignalSpy changed(p, &PopoutWindow::alwaysOnTopChanged);
    ASSERT_TRUE(env.commands->execute("window:toggle-always-on-top"));
    EXPECT_TRUE(p->isAlwaysOnTop());
    EXPECT_TRUE(p->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    ASSERT_TRUE(p->windowHandle());
    EXPECT_TRUE(p->windowHandle()->flags().testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_EQ(p->windowFlags(), flags | Qt::WindowStaysOnTopHint); // the rest (frame) kept
    EXPECT_TRUE(p->isVisible()); // changed in place: not hidden / re-created
    EXPECT_EQ(p->geometry(), before);
    EXPECT_EQ(changed.count(), 1);
    ASSERT_TRUE(env.commands->execute("window:toggle-always-on-top"));
    EXPECT_FALSE(p->isAlwaysOnTop());
    EXPECT_FALSE(p->windowHandle()->flags().testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_EQ(changed.count(), 2);
    // The stacked tabs command works in a pop-out too.
    ASSERT_TRUE(env.commands->execute("workspace:toggle-stacked-tabs"));
    EXPECT_TRUE(env.workspace->isStacked(env.workspace->activeLeaf()));
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);
    // Not while the pop-out is maximized (Obsidian), nor once the main window has the focus again.
    p->showMaximized();
    flush();
    if (p->isMaximized()) {
        EXPECT_FALSE(env.commands->isAvailable("window:toggle-always-on-top"));
    }
    p->showNormal();
    flush();
    activate(env.window);
    EXPECT_FALSE(env.commands->isAvailable("window:toggle-always-on-top"));
}

TEST(PopoutSession, HotkeysAndCommandPaletteInsideAPopout)
{
    NavigationEnv env;
    HotkeyDispatcher dispatcher(env.commands, env.window);
    dispatcher.setWindow(env.window);
    env.openNote("Lists.md");
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    activate(p);
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_TRUE(mv);
    ASSERT_EQ(mv->window(), p);
    mv->editor()->setFocus();
    mv->editor()->setSelection(Selection::single(0, 0), false);
    flush();

    // Mod+P in the pop-out's editor: the palette opens in the pop-out (the focused window).
    pressKey(mv->editor(), Qt::Key_P, Qt::ControlModifier);
    auto* palette = p->overlay()->findChild<SuggestModal*>("ocCommandPalette");
    ASSERT_NE(palette, nullptr);
    EXPECT_TRUE(palette->isOpen());
    EXPECT_EQ(env.window->ownOverlay()->findChild<SuggestModal*>("ocCommandPalette"), nullptr);
    EXPECT_EQ(env.window->overlay(), p->overlay()); // MainWindow::overlay() follows the focus
    // The palette's editor commands run on the pop-out's editor.
    const QString before = mv->document()->text().toString();
    QTest::keyClicks(palette->input(), "toggle bold");
    ASSERT_EQ(palette->suggestions().value(0).data.toString(), "editor:toggle-bold");
    QTest::keyClick(palette->input(), Qt::Key_Return);
    flush();
    EXPECT_NE(mv->document()->text().toString(), before);

    // Mod+O: the quick switcher, there too; Enter opens the note in the pop-out's tab.
    mv->editor()->setFocus();
    pressKey(mv->editor(), Qt::Key_O, Qt::ControlModifier);
    auto* switcher = p->overlay()->findChild<SuggestModal*>("ocQuickSwitcher");
    ASSERT_NE(switcher, nullptr);
    QTest::keyClicks(switcher->input(), "Math");
    QTest::keyClick(switcher->input(), Qt::Key_Return);
    flush();
    EXPECT_EQ(env.activePath(), "Math.md");
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);

    // Mod+T / Mod+W act on the pop-out: a new tab there, then closing it.
    MarkdownView* math = env.activeMarkdown();
    ASSERT_TRUE(math);
    math->editor()->setFocus();
    pressKey(math->editor(), Qt::Key_T, Qt::ControlModifier);
    flush();
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);
    EXPECT_EQ(env.workspace->containerLeaves(env.workspace->activeLeaf()).size(), 2);
    ASSERT_TRUE(env.commands->execute("workspace:close"));
    flush();
    EXPECT_EQ(env.workspace->containerLeaves(math->leaf()).size(), 1);

    // Notices from commands show in the focused pop-out.
    env.window->notices()->show("Hello from the pop-out");
    EXPECT_TRUE(p->overlay()->findChild<Notice*>());
    EXPECT_FALSE(env.window->ownOverlay()->findChild<Notice*>());

    // Back in the main window: the palette opens there.
    activate(env.window);
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), nullptr);
    ASSERT_TRUE(env.commands->execute("command-palette:open"));
    auto* mainPalette = env.window->ownOverlay()->findChild<SuggestModal*>("ocCommandPalette");
    ASSERT_NE(mainPalette, nullptr);
    mainPalette->close();
}

TEST(PopoutSession, LinksOpenInThePopout)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    activate(p);
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_TRUE(mv);
    ASSERT_EQ(mv->window(), p);

    // A plain click follows the link in the same (pop-out) tab, with history.
    emit mv->editor()->linkActivated("Formatting", 0, false);
    flush();
    EXPECT_EQ(env.activePath(), "Formatting.md");
    EXPECT_EQ(env.activeMarkdown(), mv);
    EXPECT_TRUE(mv->leaf()->canGoBack());
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);
    EXPECT_EQ(env.rootLeafCount(), 1);
    // A new tab: in the pop-out's tab group.
    emit mv->editor()->linkActivated("Lists", 0, true);
    flush();
    EXPECT_EQ(env.activePath(), "Lists.md");
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), p);
    EXPECT_EQ(env.workspace->containerLeaves(mv->leaf()).size(), 2);
    // The recent files include what was opened in the pop-out.
    EXPECT_EQ(env.workspace->lastOpenFiles().value(0), "Lists.md");
    // editor:open-link-in-new-window: the link under the cursor in another window.
    env.workspace->setActiveLeaf(mv->leaf(), true);
    mv->leaf()->goBack();
    flush();
    ASSERT_EQ(env.activePath(), "Welcome.md");
    const qsizetype link = mv->document()->text().toString().indexOf("[[Lists]]");
    ASSERT_GE(link, 0);
    mv->editor()->setCursorPosition(link + 3);
    flush();
    ASSERT_TRUE(env.commands->execute("editor:open-link-in-new-window"));
    flush();
    EXPECT_EQ(env.workspace->popoutWindows().size(), 2);
    EXPECT_EQ(env.activePath(), "Lists.md");
    EXPECT_EQ(env.workspace->popoutOf(env.workspace->activeLeaf()), env.workspace->popoutWindows().last());
}

TEST(PopoutSession, PagePreviewShowsInTheHoveredWindow)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_TRUE(mv);
    activate(env.window); // hovering doesn't need the focus
    PagePreview* pp = PagePreview::of(env.window);
    ASSERT_TRUE(pp);
    pp->setDelays(0, 10000); // (the mouse isn't on the popover)
    QWidget* vp = mv->editor()->viewport();
    const QRect anchor(vp->mapToGlobal(QPoint(40, 40)), QSize(60, 16));
    pp->onLinkHover("preview", "Formatting", "Welcome.md", anchor, vp); // "preview": no Mod needed
    ASSERT_TRUE(QTest::qWaitFor([&] { return !pp->popovers().isEmpty(); }, 3000));
    HoverPopover* pop = pp->popovers().first();
    EXPECT_EQ(pop->window(), p);
    EXPECT_EQ(pop->parentWidget(), p->overlay());
    pp->closeAll();
}

TEST(PopoutSession, EditorSuggestFollowsTheViewIntoAPopout)
{
    NavigationEnv env;
    env.openNote("Lists.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    auto typeAtEnd = [e](const QString& text) {
        e->setCursorPosition(e->document()->text().length());
        for (const QChar c : text)
            e->insertText(QString(c));
    };
    e->setFocus();
    typeAtEnd("\n[[Form");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 3000));
    EXPECT_EQ(s->popup()->parentWidget(), env.window->ownOverlay());
    QTest::keyClick(e, Qt::Key_Escape);
    ASSERT_FALSE(s->isOpen());
    // The same view in a pop-out: its "[[" popup shows in the pop-out's overlay.
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    activate(p);
    e->setFocus();
    typeAtEnd(" [[Mat");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 3000));
    EXPECT_EQ(s->popup()->parentWidget(), p->overlay());
    EXPECT_TRUE(s->popup()->isVisible());
    EXPECT_TRUE(p->rect().contains(s->popup()->geometry()));
    QTest::keyClick(e, Qt::Key_Escape);
}

TEST(PopoutSession, SingleInstanceHandoffOpensInTheMainWindow)
{
    // Application::openPath() (files from another launch): the main window, even when a pop-out has the
    // active leaf.
    NavigationEnv env;
    env.openNote("Welcome.md");
    ASSERT_TRUE(env.commands->execute("workspace:move-to-new-window"));
    flush();
    ASSERT_TRUE(env.workspace->popoutOf(env.workspace->activeLeaf()));
    if (env.workspace->popoutOf(env.workspace->activeLeaf()))
        if (WorkspaceLeaf* mainLeaf = env.workspace->mostRecentLeafIn(nullptr))
            env.workspace->setActiveLeaf(mainLeaf, false);
    WorkspaceLeaf* leaf = env.workspace->getLeaf(NewLeaf::No);
    EXPECT_EQ(env.workspace->popoutOf(leaf), nullptr);
}

TEST(PopoutSession, ClosingTheMainWindowKeepsThePopoutsForTheNextStart)
{
    QString layout;
    auto env = std::make_unique<NavigationEnv>();
    env->openNote("Welcome.md");
    ASSERT_TRUE(env->commands->execute("workspace:move-to-new-window"));
    flush();
    QPointer<PopoutWindow> p = onlyPopout(*env);
    ASSERT_TRUE(p);
    layout = ClonePaths::vaultConfigDir(env->session->id()) + "/workspace.json";
    env->window->close(); // saves; the pop-out's leaves stay in the layout
    EXPECT_FALSE(p->isVisible());
    const QJsonObject saved = JsonFile::read(layout);
    ASSERT_EQ(saved["floating"].toObject()["children"].toArray().size(), 1);
    env.reset(); // the main window goes: everything else with it
    flush();
    EXPECT_FALSE(p);
}

// ---------------------------------------------------------------------------------------------
// Headless review: the app's ScenarioRunner (--script) grabs a pop-out with "screenshot-popout"
// ---------------------------------------------------------------------------------------------

namespace {
// Runs `lines` through the app's ScenarioRunner in-process until `done` exists. The script ends with a
// long wait: finishing would QApplication::exit(), which ends every later event loop of this process.
bool runScenario(NavigationEnv& env, const QString& lines, const QString& done)
{
    const QString path = env.tmp.path() + "/scenario.txt";
    if (!writeFile(path, (lines + "\nwait 600000\n").toUtf8()))
        return false;
    auto* runner = new ScenarioRunner(env.window, path);
    runner->start();
    const bool ok = QTest::qWaitFor([&] { return QFileInfo::exists(done); }, 30000);
    delete runner;
    return ok;
}

// A pop-out with two tabs (Welcome, Formatting), made like a user would.
const char* const kTwoTabPopout = "open Welcome.md live\n"
                                  "command workspace:move-to-new-window\n"
                                  "command workspace:new-tab\n"
                                  "open Formatting.md live\n";

QString popoutShotDir() { return QString::fromLocal8Bit(qgetenv("OC_SHOT_DIR")); }
} // namespace

TEST(PopoutSession, ScenarioScreenshotOfAPopout)
{
    NavigationEnv env;
    const QString out = env.tmp.path() + "/popout.png";
    ASSERT_TRUE(runScenario(env, QString::fromLatin1(kTwoTabPopout) + "screenshot-popout " + out, out));
    PopoutWindow* p = onlyPopout(env);
    ASSERT_TRUE(p);
    EXPECT_EQ(describe(WorkspaceTabs::impl(env.workspace)->windows[0]), "V[T(Welcome,Formatting*)]");
    const QImage shot(out);
    EXPECT_EQ(shot.size(), p->size() * p->devicePixelRatio());
}

class PopoutShots : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override
    {
        if (popoutShotDir().isEmpty())
            GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
        ThemeManager* tm = testThemes();
        tm->setMode(GetParam() ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        tm->apply();
    }
    void TearDown() override
    {
        testThemes()->setMode(ThemeManager::Mode::Dark);
        testThemes()->apply();
    }
};

// popout-{dark,light}.png: a frameless ("hidden" frame) pop-out with two tabs; popout-main-*.png: the main
// window it came from.
TEST_P(PopoutShots, TwoTabPopout)
{
    NavigationEnv env({}, true, true);
    env.window->resize(1400, 850);
    QCursor::setPos(-10000, -10000); // nothing hovered
    const QString suffix = GetParam() ? "dark" : "light";
    const QString out = popoutShotDir() + "/popout-" + suffix + ".png";
    QFile::remove(out);
    ASSERT_TRUE(runScenario(env, QString::fromLatin1(kTwoTabPopout) + "screenshot-popout " + out, out));
    env.window->repaint();
    EXPECT_TRUE(env.window->grab().save(popoutShotDir() + "/popout-main-" + suffix + ".png"));
}

INSTANTIATE_TEST_SUITE_P(Themes, PopoutShots, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& info) { return std::string(info.param ? "Dark" : "Light"); });
