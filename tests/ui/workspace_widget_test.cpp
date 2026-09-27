// Workspace widget tests with synthetic input: tab clicks, middle click, close/pin buttons, the "+"
// button, tab drag & drop (drop regions and resulting model changes), view header navigation,
// resize handles and theme changes; stacked tab groups (spine / pane geometry, scrolling, activation,
// closing, drops, the tab list's Stack / Unstack).

#include "ui/workspace_test_support.h"
#include <QJsonArray>
#include <QSignalSpy>

#include "core/util/busy.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/icon_button.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

using namespace oc;
using namespace oc::test;

namespace {
using NewLeaf = Workspace::NewLeaf;
using Side = Workspace::Side;
using Target = WorkspaceImpl::DropTarget;

void sendMouse(QWidget* w, QEvent::Type type, QPoint local, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(local), QPointF(w->mapToGlobal(local)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void click(QWidget* w, QPoint local, Qt::MouseButton button = Qt::LeftButton)
{
    sendMouse(w, QEvent::MouseButtonPress, local, button, button);
    sendMouse(w, QEvent::MouseButtonRelease, local, button, Qt::NoButton);
}
} // namespace

class WorkspaceWidgetTest : public WorkspaceFixture {
protected:
    void SetUp() override
    {
        WorkspaceFixture::SetUp();
        m_ws = makeWorkspace();
        m_host = std::make_unique<QWidget>();
        auto* l = new QVBoxLayout(m_host.get());
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(m_ws->widget());
        m_host->resize(1200, 800);
        m_host->show();
        QCoreApplication::processEvents();
    }
    void TearDown() override
    {
        m_ws.reset();
        m_host.reset();
        WorkspaceFixture::TearDown();
    }
    WorkspaceImpl* d() const { return impl(m_ws.get()); }
    TabHeaderBar* header(WorkspaceTabs* t) const { return t->w->header(); }
    QWidget* ws() const { return m_ws->widget(); }
    // Center of `rel` (fractions) inside a tab group, in workspace-widget coordinates.
    QPoint inGroup(WorkspaceTabs* t, double fx, double fy) const
    {
        const QRect r(t->w->mapTo(ws(), QPoint(0, 0)), t->w->size());
        return QPoint(r.x() + int(r.width() * fx), r.y() + int(r.height() * fy));
    }
    Target targetAt(WorkspaceLeaf* dragged, QPoint wsPos) const { return d()->dropTargetAt(dragged, ws()->mapToGlobal(wsPos)); }
    void dragTab(WorkspaceTabs* from, int index, QPoint wsTarget)
    {
        TabHeaderBar* h = header(from);
        const QPoint start = h->tabRect(index).center();
        sendMouse(h, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(h, QEvent::MouseMove, start + QPoint(4, 0), Qt::NoButton, Qt::LeftButton); // under the threshold
        EXPECT_FALSE(d()->drag.active);
        sendMouse(h, QEvent::MouseMove, start + QPoint(12, 0), Qt::NoButton, Qt::LeftButton);
        EXPECT_TRUE(d()->drag.active);
        const QPoint target = h->mapFromGlobal(ws()->mapToGlobal(wsTarget));
        sendMouse(h, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        sendMouse(h, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(d()->drag.active);
        QCoreApplication::processEvents();
    }
    // A (A, B, C) | X layout.
    void buildTwoGroups()
    {
        WorkspaceLeaf* a = m_ws->activeRootLeaf();
        a->setViewState(graph("A"));
        m_ws->getLeaf(NewLeaf::Tab)->setViewState(graph("B"));
        m_ws->getLeaf(NewLeaf::Tab)->setViewState(graph("C"));
        m_ws->getLeaf(NewLeaf::SplitRight)->setViewState(graph("X"));
        QCoreApplication::processEvents();
        ASSERT_EQ(describe(d()->root), "V[T(A,B,C*),T(X*)]");
    }

    std::unique_ptr<Workspace> m_ws;
    std::unique_ptr<QWidget> m_host;
};

TEST_F(WorkspaceWidgetTest, RootTabGeometryFollowsObsidianCss)
{
    buildTwoGroups();
    // X: a single tab gets --tab-width.
    TabHeaderBar* hx = header(rootTabs(m_ws.get(), 1));
    EXPECT_EQ(hx->height(), 40);
    const QRect x0 = hx->tabRect(0);
    EXPECT_EQ(x0.left(), 18); // container padding 8 + inner margin -5 + padding 15
    EXPECT_EQ(x0.top(), 7);
    EXPECT_EQ(x0.bottom(), 39);
    EXPECT_EQ(x0.width(), 200);
    EXPECT_EQ(hx->newTabButton()->x(), x0.right() + 1 + 6);
    // A (450px): three tabs shrink to fit next to "+" and the tab list button.
    TabHeaderBar* h = header(rootTabs(m_ws.get(), 0));
    ASSERT_EQ(h->width(), 450);
    const double expected = (450 - 16 - 84) / 3.0;
    EXPECT_NEAR(h->tabRect(0).width(), expected, 1);
    EXPECT_NEAR(h->tabRect(1).left(), 18 + expected, 1);
    EXPECT_NEAR(h->newTabButton()->x(), 18 + 3 * expected + 6, 1);
    EXPECT_EQ(h->tabListButton()->x(), 450 - 8 - 4 - 26);
    // Narrow: the tabs keep sharing the strip (min-width 0); below 3rem the inactive ones lose their
    // close button, even when hovered.
    m_host->resize(700, 600);
    QCoreApplication::processEvents();
    const double narrow = (h->width() - 16 - 84) / 3.0;
    ASSERT_LT(narrow, 48);
    EXPECT_NEAR(h->tabRect(0).width(), narrow, 1);
    EXPECT_NEAR(h->tabRect(2).right(), 18 + 3 * narrow, 2);
    sendMouse(h, QEvent::MouseMove, h->tabRect(0).center(), Qt::NoButton, Qt::NoButton);
    const QPoint closeSpot(h->tabRect(0).right() - 8, h->tabRect(0).center().y());
    click(h, closeSpot);
    EXPECT_EQ(describe(d()->root), "V[T(A*,B,C),T(X*)]"); // activated, not closed
    // Even a very narrow active tab keeps its close button inside the tab.
    EXPECT_TRUE(h->tabRect(0).contains(h->closeRect(0)));
}

TEST_F(WorkspaceWidgetTest, ClickingATabActivatesIt)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    WorkspaceLeaf* a = A->leaves[0];
    click(header(A), header(A)->tabRect(0).center());
    EXPECT_EQ(m_ws->activeLeaf(), a);
    EXPECT_EQ(A->currentTab, 0);
    EXPECT_TRUE(d()->leafWidget(a)->isVisible());
    EXPECT_FALSE(d()->leafWidget(A->leaves[2])->isVisible());
}

TEST_F(WorkspaceWidgetTest, MiddleClickClosesATab)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    click(header(A), header(A)->tabRect(1).center(), Qt::MiddleButton);
    EXPECT_EQ(describe(d()->root), "V[T(A,C*),T(X*)]");
}

TEST_F(WorkspaceWidgetTest, CloseButtonAndPinIcon)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    TabHeaderBar* h = header(A);
    click(h, h->closeRect(2).center()); // the active tab shows its close button
    EXPECT_EQ(describe(d()->root), "V[T(A,B*),T(X*)]");
    // An inactive tab's close button only works while hovered.
    WorkspaceLeaf* a = A->leaves[0];
    a->setPinned(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(h->closeRect(0).isEmpty());
    click(h, h->closeRect(0).center()); // pinned: the pin icon unpins
    EXPECT_FALSE(a->isPinned());
    EXPECT_EQ(describe(d()->root), "V[T(A,B*),T(X*)]");
}

TEST_F(WorkspaceWidgetTest, NewTabButtonAppendsAnEmptyTab)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    header(A)->newTabButton()->click();
    EXPECT_EQ(describe(d()->root), "V[T(A,B,C,_*),T(X*)]");
    EXPECT_EQ(m_ws->activeLeaf(), A->leaves.last());
}

TEST_F(WorkspaceWidgetTest, FocusNewTabOffOpensNewTabsInTheBackground)
{
    WorkspaceLeaf* a = m_ws->activeRootLeaf();
    a->setViewState(graph("A"));
    auto note = makeFile("Note.md");
    m_ws->setFocusNewTab(false);
    m_ws->openFile(note.get(), NewLeaf::Tab); // Mod+click on a link
    WorkspaceTabs* tabs = rootTabs(m_ws.get(), 0);
    ASSERT_EQ(tabs->leaves.size(), 2);
    EXPECT_EQ(m_ws->activeLeaf(), a);
    EXPECT_EQ(tabs->currentTab, 0);
    auto fileOf = [](WorkspaceLeaf* l) { return l->viewState().value("state").toObject().value("file").toString(); };
    EXPECT_EQ(fileOf(tabs->leaves[1]), "Note.md"); // loaded in the background
    // "+" / workspace:new-tab and splits still focus their new leaf.
    WorkspaceLeaf* t = m_ws->newTab();
    EXPECT_EQ(m_ws->activeLeaf(), t);
    t->setViewState(graph("T"));
    EXPECT_EQ(m_ws->activeLeaf(), t);
    // On (the default): the new tab becomes the active one.
    m_ws->setFocusNewTab(true);
    auto other = makeFile("Other.md");
    m_ws->openFile(other.get(), NewLeaf::Tab);
    EXPECT_EQ(fileOf(m_ws->activeLeaf()), "Other.md");
}

TEST_F(WorkspaceWidgetTest, ShowViewHeaderOffHidesEveryViewHeader)
{
    buildTwoGroups();
    WorkspaceLeaf* x = m_ws->activeLeaf();
    ASSERT_TRUE(d()->leafWidget(x)->header()->isVisible());
    m_ws->setShowViewHeader(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(d()->leafWidget(x)->header()->isVisible());
    EXPECT_EQ(d()->leafWidget(x)->header()->isHidden(), true);
    EXPECT_EQ(x->view()->geometry().top(), 0); // the view takes the header's space
    // Leaves created while it's off get no header either.
    WorkspaceLeaf* n = m_ws->getLeaf(NewLeaf::Tab);
    n->setViewState(graph("N"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(!d()->leafWidget(n)->header() || d()->leafWidget(n)->header()->isHidden());
    m_ws->setShowViewHeader(true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(d()->leafWidget(n)->header()->isVisible());
    EXPECT_GT(n->view()->geometry().top(), 0);
}

TEST_F(WorkspaceWidgetTest, DropRegionsFollowObsidian)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    WorkspaceLeaf* b = A->leaves[1];
    const QRect xr(X->w->mapTo(ws(), QPoint(0, 0)), X->w->size());

    Target t = targetAt(b, inGroup(X, 0.5, 0.6));
    EXPECT_EQ(t.kind, Target::Kind::Tabs);
    EXPECT_EQ(t.tabs, X);
    EXPECT_EQ(t.edge, DropEdge::Center);
    EXPECT_EQ(t.rect.width(), 10); // tab insertion marker in the header
    EXPECT_EQ(t.rect.top(), xr.top() + 7);

    t = targetAt(b, inGroup(X, 0.05, 0.6));
    EXPECT_EQ(t.edge, DropEdge::Left);
    EXPECT_EQ(t.rect, QRect(xr.x(), xr.y(), xr.width() / 3, xr.height()));
    t = targetAt(b, inGroup(X, 0.95, 0.6));
    EXPECT_EQ(t.edge, DropEdge::Right);
    EXPECT_EQ(t.rect.right(), xr.right());
    t = targetAt(b, inGroup(X, 0.5, 0.95));
    EXPECT_EQ(t.edge, DropEdge::Bottom);
    EXPECT_EQ(t.rect.height(), xr.height() / 3);
    // Top: only the upper third of the tab header; the rest of the header inserts a tab.
    t = targetAt(b, QPoint(xr.x() + xr.width() / 2, xr.y() + 5));
    EXPECT_EQ(t.edge, DropEdge::Top);
    t = targetAt(b, QPoint(xr.x() + 30, xr.y() + 25));
    EXPECT_EQ(t.edge, DropEdge::Center);
    EXPECT_EQ(t.index, 0);
    t = targetAt(b, QPoint(xr.x() + 150, xr.y() + 25));
    EXPECT_EQ(t.index, 1);
    // Just below the header, near the left edge: the left band wins over "center".
    t = targetAt(b, QPoint(xr.x() + 10, xr.y() + 60));
    EXPECT_EQ(t.edge, DropEdge::Left);

    // A group's only tab can't be dropped onto its own group.
    WorkspaceLeaf* x = X->leaves[0];
    EXPECT_FALSE(targetAt(x, inGroup(X, 0.9, 0.6)).isValid());
    EXPECT_TRUE(targetAt(x, inGroup(A, 0.9, 0.6)).isValid());

    // Sidedock groups can't be split left/right.
    WorkspaceTabs* side = d()->left->children.first()->asTabs();
    t = targetAt(b, inGroup(side, 0.03, 0.5));
    EXPECT_EQ(t.tabs, side);
    EXPECT_EQ(t.edge, DropEdge::Center);
    t = targetAt(b, inGroup(side, 0.5, 0.97));
    EXPECT_EQ(t.edge, DropEdge::Bottom);
    EXPECT_GE(t.rect.height(), 40);
}

TEST_F(WorkspaceWidgetTest, DraggingATabOntoAnEdgeSplitsTheGroup)
{
    buildTwoGroups();
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    dragTab(rootTabs(m_ws.get(), 0), 1, inGroup(X, 0.9, 0.6));
    EXPECT_EQ(describe(d()->root), "V[T(A,C*),T(X*),T(B*)]");
    EXPECT_EQ(m_ws->activeLeaf()->displayText(), "B");
    // Bottom edge of X: a horizontal split around X.
    dragTab(rootTabs(m_ws.get(), 0), 0, inGroup(X, 0.5, 0.95));
    EXPECT_EQ(describe(d()->root), "V[T(C*),H[T(X*),T(A*)],T(B*)]");
}

TEST_F(WorkspaceWidgetTest, DraggingATabOntoAHeaderMovesOrReordersIt)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    const QRect xr(X->w->mapTo(ws(), QPoint(0, 0)), X->w->size());
    dragTab(A, 0, QPoint(xr.x() + 30, xr.y() + 25)); // before X's first tab
    EXPECT_EQ(describe(d()->root), "V[T(B,C*),T(A*,X)]");
    // Reorder inside a group: drop C after... the end of its own strip.
    const QRect lastTab = header(A)->tabRect(1);
    dragTab(A, 0, header(A)->mapTo(ws(), QPoint(lastTab.right() - 10, 25)));
    EXPECT_EQ(describe(A), "T(C,B*)");
}

TEST_F(WorkspaceWidgetTest, EscapeCancelsATabDrag)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    TabHeaderBar* h = header(A);
    const QPoint start = h->tabRect(0).center();
    sendMouse(h, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(h, QEvent::MouseMove, start + QPoint(20, 0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(d()->drag.active);
    const QPoint target = h->mapFromGlobal(ws()->mapToGlobal(inGroup(X, 0.9, 0.6)));
    sendMouse(h, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(d()->drag.target.isValid());
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(h, &esc);
    EXPECT_FALSE(d()->drag.active);
    EXPECT_FALSE(d()->overlay && d()->overlay->isVisible());
    sendMouse(h, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(describe(d()->root), "V[T(A,B,C*),T(X*)]"); // nothing moved
    EXPECT_EQ(QApplication::overrideCursor(), nullptr);
}

TEST_F(WorkspaceWidgetTest, DraggingIntoAndOutOfSidedocks)
{
    buildTwoGroups();
    m_ws->setSideCollapsed(Side::Right, false);
    QCoreApplication::processEvents();
    WorkspaceTabs* rightTabs = d()->right->children.first()->asTabs();
    dragTab(rootTabs(m_ws.get(), 0), 2, inGroup(rightTabs, 0.5, 0.6));
    EXPECT_EQ(describe(d()->right), "H[T(Backlinks,Outgoing links,Tags,Outline,C*)]");
    EXPECT_EQ(describe(d()->root), "V[T(A,B*),T(X*)]");
    // Back out, onto the right edge of the root's first group.
    dragTab(rightTabs, 4, inGroup(rootTabs(m_ws.get(), 0), 0.95, 0.6));
    EXPECT_EQ(describe(d()->root), "V[T(A,B*),T(C*),T(X*)]");
    EXPECT_FALSE(m_ws->activeLeaf()->isInSidedock());
    EXPECT_TRUE(d()->leafWidget(m_ws->activeLeaf())->header()->isVisible()); // root leaves get a view header
}

TEST_F(WorkspaceWidgetTest, SidedockHeaderShowsIconTabs)
{
    WorkspaceTabs* side = d()->left->children.first()->asTabs();
    TabHeaderBar* h = header(side);
    EXPECT_EQ(h->tabCount(), 3);
    EXPECT_EQ(h->tabRect(0), QRect(8, 7, 34, 25));
    EXPECT_EQ(h->tabRect(1).left(), 8 + 34 + 3);
    EXPECT_FALSE(h->newTabButton()->isVisible());
    click(h, h->tabRect(1).center());
    EXPECT_EQ(side->currentTab, 1);
    EXPECT_EQ(m_ws->activeLeaf()->view()->viewType(), "search");
    EXPECT_EQ(d()->leafWidget(m_ws->activeLeaf())->header(), nullptr); // sidebar views have no view header
}

TEST_F(WorkspaceWidgetTest, ViewHeaderNavigatesHistory)
{
    WorkspaceLeaf* leaf = m_ws->activeRootLeaf();
    leaf->setViewState(note("Projects/Alpha.md"));
    leaf->setViewState(note("Beta.md"));
    ViewHeader* vh = d()->leafWidget(leaf)->header();
    ASSERT_TRUE(vh);
    EXPECT_TRUE(vh->isVisible());
    EXPECT_EQ(vh->title()->text(), "Beta");
    EXPECT_TRUE(vh->backButton()->isEnabled());
    EXPECT_FALSE(vh->forwardButton()->isEnabled());
    vh->backButton()->click();
    EXPECT_EQ(leaf->displayText(), "Alpha");
    EXPECT_EQ(vh->title()->text(), "Projects / Alpha");
    EXPECT_FALSE(vh->backButton()->isEnabled());
    EXPECT_TRUE(vh->forwardButton()->isEnabled());
}

TEST_F(WorkspaceWidgetTest, ClickingInsideAViewActivatesItsLeaf)
{
    buildTwoGroups();
    WorkspaceLeaf* x = rootTabs(m_ws.get(), 1)->leaves[0];
    WorkspaceLeaf* c = rootTabs(m_ws.get(), 0)->leaves[2];
    ASSERT_EQ(m_ws->activeLeaf(), x);
    QWidget* view = c->view();
    sendMouse(view, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton, Qt::LeftButton);
    sendMouse(view, QEvent::MouseButtonRelease, QPoint(20, 20), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(m_ws->activeLeaf(), c);
}

TEST_F(WorkspaceWidgetTest, TopHeadersAreWindowDragRegions)
{
    buildTwoGroups();
    TabHeaderBar* h = header(rootTabs(m_ws.get(), 0));
    ASSERT_TRUE(h->isTop());
    sendMouse(h, QEvent::MouseMove, QPoint(h->newTabButton()->x() + 60, 20), Qt::NoButton, Qt::NoButton);
    EXPECT_TRUE(h->property("ocDragRegion").toBool());
    sendMouse(h, QEvent::MouseMove, h->tabRect(0).center(), Qt::NoButton, Qt::NoButton);
    EXPECT_FALSE(h->property("ocDragRegion").toBool());
    // A group below a horizontal split isn't at the top.
    WorkspaceLeaf* down = m_ws->getLeaf(NewLeaf::SplitDown);
    QCoreApplication::processEvents();
    EXPECT_FALSE(header(down->tabs())->isTop());
}

TEST_F(WorkspaceWidgetTest, ResizeHandleUpdatesDimensions)
{
    buildTwoGroups();
    SplitWidget* rootW = d()->root->w;
    ResizeHandle* handle = rootW->findChild<ResizeHandle*>(QString(), Qt::FindDirectChildrenOnly);
    ASSERT_TRUE(handle);
    const int before = d()->root->children[0]->widget()->width();
    const QPoint start = handle->rect().center();
    sendMouse(handle, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(handle, QEvent::MouseMove, start + QPoint(-100, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(handle, QEvent::MouseButtonRelease, start + QPoint(-100, 0), Qt::LeftButton, Qt::NoButton);
    const int after = d()->root->children[0]->widget()->width();
    EXPECT_NEAR(after, before - 100, 2);
    const double sum = d()->root->children[0]->dimension + d()->root->children[1]->dimension;
    EXPECT_NEAR(sum, 100.0, 0.01);
    EXPECT_GT(d()->root->children[1]->dimension, d()->root->children[0]->dimension);
}

TEST_F(WorkspaceWidgetTest, SidedockHandleResizesAndCollapses)
{
    ResizeHandle* handle = nullptr;
    for (ResizeHandle* h : ws()->findChildren<ResizeHandle*>(QString(), Qt::FindDirectChildrenOnly))
        if (h->isVisible() && h->x() < 400)
            handle = h;
    ASSERT_TRUE(handle);
    const QPoint start = handle->rect().center();
    sendMouse(handle, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(handle, QEvent::MouseMove, start + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
    EXPECT_NEAR(m_ws->sideWidth(Side::Left), 340, 3);
    const int width = m_ws->sideWidth(Side::Left);
    // Dragging below 50px collapses (Obsidian); the width is kept for re-expanding.
    sendMouse(handle, QEvent::MouseMove, handle->mapFromGlobal(ws()->mapToGlobal(QPoint(20, 100))), Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(m_ws->isSideCollapsed(Side::Left));
    sendMouse(handle, QEvent::MouseButtonRelease, QPoint(0, 0), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(m_ws->sideWidth(Side::Left), width);
    // Clamped to [200, 80% of the workspace].
    m_ws->setSideCollapsed(Side::Left, false);
    QCoreApplication::processEvents();
    const QPoint s2 = handle->rect().center();
    sendMouse(handle, QEvent::MouseButtonPress, s2, Qt::LeftButton, Qt::LeftButton);
    sendMouse(handle, QEvent::MouseMove, s2 + QPoint(-200, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(handle, QEvent::MouseButtonRelease, s2 + QPoint(-200, 0), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(m_ws->sideWidth(Side::Left), 200);
}

TEST_F(WorkspaceWidgetTest, RestoredLayoutIsShown)
{
    // Regression: restore() builds the tree directly; its widgets must still be synced and shown.
    const QJsonObject json = parseJson(R"({
      "main": {"id": "m1", "type": "split", "direction": "vertical", "children": [
        {"id": "t1", "type": "tabs", "currentTab": 1, "children": [
          {"id": "l1", "type": "leaf", "state": {"type": "graph", "state": {"name": "One"}}},
          {"id": "l2", "type": "leaf", "state": {"type": "graph", "state": {"name": "Two"}}},
          {"id": "l3", "type": "leaf", "state": {"type": "kanban", "state": {}}}
        ]},
        {"id": "t2", "type": "tabs", "dimension": 40, "children": [
          {"id": "l4", "type": "leaf", "state": {"type": "empty", "state": {}}}
        ]}
      ]},
      "left": {"id": "s1", "type": "split", "direction": "horizontal", "width": 260, "children": [
        {"id": "t3", "type": "tabs", "children": [
          {"id": "l5", "type": "leaf", "state": {"type": "file-explorer", "state": {}}},
          {"id": "l6", "type": "leaf", "state": {"type": "search", "state": {}}}
        ]}
      ]},
      "right": {"id": "s2", "type": "split", "direction": "horizontal", "width": 240, "children": [
        {"id": "t4", "type": "tabs", "children": [
          {"id": "l7", "type": "leaf", "state": {"type": "outline", "state": {}}}
        ]}
      ]},
      "active": "l2"
    })");
    ASSERT_TRUE(m_ws->restore(json));
    QCoreApplication::processEvents();
    EXPECT_EQ(describe(d()->root), "V[T(One,Two*,kanban),T(_*)]");
    for (WorkspaceSplit* s : {d()->root, d()->left, d()->right}) {
        EXPECT_TRUE(s->w->isVisible());
        d()->forEachTabs(s, [&](WorkspaceTabs* t) {
            EXPECT_TRUE(t->w->isVisible()) << qPrintable(describe(t));
            EXPECT_EQ(t->w->parentWidget(), s == t->parent ? static_cast<QWidget*>(s->w) : t->parent->w);
            EXPECT_GT(t->w->width(), 0);
            EXPECT_EQ(header(t)->tabCount(), t->leaves.size());
            for (WorkspaceLeaf* l : std::as_const(t->leaves))
                EXPECT_EQ(d()->leafWidget(l)->isVisible(), l == t->current()) << qPrintable(l->id());
        });
    }
    EXPECT_EQ(d()->left->w->width(), 260);
    EXPECT_EQ(d()->right->w->width(), 240);
    WorkspaceLeaf* two = d()->leafById("l2");
    ASSERT_TRUE(two);
    EXPECT_EQ(m_ws->activeLeaf(), two);
    EXPECT_TRUE(two->view()->isVisible());
    EXPECT_TRUE(d()->leafWidget(two)->header()->isVisible());
    // Only one group has a dimension: like Obsidian, the groups then share the root equally.
    EXPECT_NEAR(rootTabs(m_ws.get(), 0)->w->width(), rootTabs(m_ws.get(), 1)->w->width(), 1);
    // Restoring again (e.g. a workspace layout switch) replaces everything and stays visible.
    ASSERT_TRUE(m_ws->restore(m_ws->serialize()));
    QCoreApplication::processEvents();
    EXPECT_TRUE(rootTabs(m_ws.get(), 0)->w->isVisible());
    EXPECT_TRUE(d()->leafWidget(d()->leafById("l2"))->isVisible());
}

TEST_F(WorkspaceWidgetTest, RemovedGroupWidgetsOutliveTheirModelSafely)
{
    // Removing a group deletes its model item now and its widgets later (deleteLater): events reaching
    // the widgets in between (theme change, resize) must not touch the freed item (ASan checks this).
    buildTwoGroups();
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    QPointer<TabGroupWidget> xw = X->w;
    QPointer<TabHeaderBar> xh = X->w->header();
    X->leaves[0]->detach();
    EXPECT_EQ(describe(d()->root), "V[T(A,B,C*)]");
    ASSERT_TRUE(xw);
    ThemeManager::instance()->setMode(ThemeManager::Mode::Light);
    ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
    xw->resize(320, 240);
    xh->relayout();
    xh->grab();
    sendMouse(xh, QEvent::MouseButtonPress, QPoint(30, 20), Qt::LeftButton, Qt::LeftButton);
    sendMouse(xh, QEvent::MouseButtonRelease, QPoint(30, 20), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_FALSE(xw);
    EXPECT_FALSE(xh);
}

TEST_F(WorkspaceWidgetTest, ThemeChangesRestyleTheHeaders)
{
    TabHeaderBar* h = header(rootTabs(m_ws.get(), 0));
    const QColor dark = h->grab().toImage().pixelColor(h->width() - 20, 5);
    ThemeManager::instance()->setMode(ThemeManager::Mode::Light);
    QCoreApplication::processEvents();
    const QColor light = h->grab().toImage().pixelColor(h->width() - 20, 5);
    ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
    EXPECT_NE(dark, light);
    EXPECT_GT(light.lightness(), dark.lightness());
}

// With a real main window: the ribbon and the right sidebar toggle are drop targets for the
// sidedocks (Obsidian's getDropLocation()); a collapsed sidedock highlights its toggle button.
class WorkspaceWindowTest : public WorkspaceFixture {};

TEST_F(WorkspaceWindowTest, RibbonAndRightToggleDropIntoTheSidedocks)
{
    auto window = std::make_unique<MainWindow>(m_session.get(), false);
    auto* ws = new Workspace(m_session.get(), &m_registry, window.get());
    window->setWorkspace(ws);
    ws->loadLayout();
    window->resize(1200, 800);
    window->show();
    QCoreApplication::processEvents();
    WorkspaceImpl* d = impl(ws);
    WorkspaceLeaf* a = ws->activeRootLeaf();
    a->setViewState(graph("A"));
    WorkspaceLeaf* b = ws->getLeaf(NewLeaf::Tab);
    b->setViewState(graph("B"));
    ASSERT_TRUE(ws->isSideCollapsed(Side::Right));

    IconButton* toggle = window->rightSidebarToggle();
    Target t = d->dropTargetAt(a, toggle->mapToGlobal(toggle->rect().center()));
    EXPECT_EQ(t.kind, Target::Kind::Side);
    EXPECT_EQ(t.side, d->right);
    EXPECT_EQ(t.rect, QRect(ws->widget()->mapFromGlobal(toggle->mapToGlobal(QPoint(0, 0))), toggle->size()));
    ASSERT_TRUE(d->applyDrop(a, t));
    EXPECT_EQ(describe(d->right), "H[T(Backlinks,Outgoing links,Tags,Outline,A*)]");
    EXPECT_TRUE(ws->isSideCollapsed(Side::Right)); // dropped without expanding it
    EXPECT_EQ(describe(d->root), "V[T(B*)]");

    Ribbon* ribbon = window->ribbon();
    t = d->dropTargetAt(b, ribbon->mapToGlobal(QPoint(ribbon->width() / 2, 300)));
    EXPECT_EQ(t.kind, Target::Kind::Side);
    EXPECT_EQ(t.side, d->left);
    EXPECT_EQ(t.rect, d->left->w->geometry()); // expanded: the whole sidedock
    ASSERT_TRUE(d->applyDrop(b, t));
    EXPECT_EQ(describe(d->left), "H[T(Files,Search,Bookmarks,B*)]");
    EXPECT_EQ(ws->activeLeaf(), b);
    EXPECT_EQ(describe(d->root), "V[T(_*)]");

    // Collapsed left sidedock: the ribbon's toggle is highlighted.
    ws->setSideCollapsed(Side::Left, true);
    QCoreApplication::processEvents();
    WorkspaceLeaf* e = ws->activeRootLeaf();
    e->setViewState(graph("E"));
    ws->getLeaf(NewLeaf::Tab);
    t = d->dropTargetAt(e, ribbon->mapToGlobal(QPoint(ribbon->width() / 2, 300)));
    QWidget* leftToggle = ribbon->sidebarToggle();
    EXPECT_EQ(t.rect, QRect(ws->widget()->mapFromGlobal(leftToggle->mapToGlobal(QPoint(0, 0))), leftToggle->size()));
    window.reset(); // the window deletes the workspace (before the session goes away)
}

// Obsidian's ribbon items can be dragged into another order (5px threshold); the order is saved with the layout
// (Obsidian: hiddenItems' key order; here "left-ribbon".ocOrder) and new items the saved order doesn't know go first.
TEST_F(WorkspaceWindowTest, RibbonItemsReorderByDragAndPersist)
{
    auto build = [this](std::unique_ptr<MainWindow>& window, bool withNew) {
        window = std::make_unique<MainWindow>(m_session.get(), false);
        Ribbon* r = window->ribbon();
        if (withNew)
            r->addAction(QStringLiteral("zk-prefixer"), QStringLiteral("file-signature"), QStringLiteral("Create new unique note"));
        r->addAction(QStringLiteral("switcher:open"), QStringLiteral("file-search"), QStringLiteral("Open quick switcher"));
        r->addAction(QStringLiteral("graph:open"), QStringLiteral("git-fork"), QStringLiteral("Open graph view"));
        r->addAction(QStringLiteral("canvas:new-file"), QStringLiteral("layout-dashboard"), QStringLiteral("Create new canvas"));
        auto* ws = new Workspace(m_session.get(), &m_registry, window.get());
        window->setWorkspace(ws);
        ws->loadLayout();
        window->resize(1200, 800);
        window->show();
        QCoreApplication::processEvents();
        return ws;
    };
    std::unique_ptr<MainWindow> window;
    Workspace* ws = build(window, false);
    Ribbon* r = window->ribbon();
    ASSERT_EQ(r->order(), (QStringList{"switcher:open", "graph:open", "canvas:new-file"}));
    auto button = [r](const QString& id) {
        for (const auto& [aid, b] : r->actions())
            if (aid == id)
                return b;
        return static_cast<IconButton*>(nullptr);
    };
    QSignalSpy triggered(r, &Ribbon::actionTriggered);
    QSignalSpy changed(r, &Ribbon::orderChanged);
    // drag "Open graph view" (the middle item) below "Create new canvas"
    IconButton* g = button("graph:open");
    IconButton* c = button("canvas:new-file");
    const QPoint start = g->rect().center();
    sendMouse(g, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(g, QEvent::MouseMove, start + QPoint(0, 3), Qt::NoButton, Qt::LeftButton); // under the threshold
    EXPECT_EQ(r->order().value(1), "graph:open");
    const QPoint below = g->mapFromGlobal(c->mapToGlobal(QPoint(c->width() / 2, c->height() - 2)));
    sendMouse(g, QEvent::MouseMove, below, Qt::NoButton, Qt::LeftButton);
    sendMouse(g, QEvent::MouseButtonRelease, g->mapFromGlobal(c->mapToGlobal(QPoint(c->width() / 2, c->height() - 2))), Qt::LeftButton,
              Qt::NoButton);
    QCoreApplication::processEvents();
    EXPECT_EQ(r->order(), (QStringList{"switcher:open", "canvas:new-file", "graph:open"}));
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(triggered.count(), 0); // a drag isn't a click
    EXPECT_EQ(r->actions().value(1).first, "canvas:new-file"); // actions() follows
    // a plain click still triggers
    QTest::mouseClick(button("switcher:open"), Qt::LeftButton);
    EXPECT_EQ(triggered.count(), 1);
    // saved with the layout
    const QJsonObject json = ws->serialize();
    EXPECT_EQ(json.value("left-ribbon").toObject().value("ocOrder").toArray(),
              QJsonArray({"switcher:Open quick switcher", "canvas:Create new canvas", "graph:Open graph view"}));
    window.reset(); // the drag requested a save: flushed with the workspace
    // restored at the next start; an item the saved order doesn't know comes first (Obsidian's indexOf() == -1)
    ws = build(window, true);
    EXPECT_EQ(window->ribbon()->order(), (QStringList{"zk-prefixer", "switcher:open", "canvas:new-file", "graph:open"}));
    window.reset();
}

// ---------------------------------------------------------------------------------------------
// Stacked tabs (Obsidian's .workspace-tabs.mod-stacked)
// ---------------------------------------------------------------------------------------------

class StackedTabsTest : public WorkspaceWidgetTest {
protected:
    void SetUp() override
    {
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false); // scrolls jump to their end
        WorkspaceWidgetTest::SetUp();
    }
    // A root group of `n` graph tabs (A, B, ...), the last one current, stacked. The root is 900 x 800
    // (the 1200px host less the 300px left sidedock).
    WorkspaceTabs* buildStacked(int n = 3)
    {
        m_ws->activeRootLeaf()->setViewState(graph("A"));
        for (int i = 1; i < n; ++i)
            m_ws->getLeaf(NewLeaf::Tab)->setViewState(graph(QString(QChar('A' + i))));
        WorkspaceTabs* t = rootTabs(m_ws.get());
        EXPECT_TRUE(m_ws->setStacked(t->current(), true));
        QCoreApplication::processEvents();
        return t;
    }
    QRect spineRect(WorkspaceTabs* t, int i) const { return t->w->spine(i) ? t->w->spine(i)->geometry() : QRect(); }
    QRect paneRect(WorkspaceTabs* t, int i) const { return d()->leafWidget(t->leaves[i])->geometry(); }
    bool paneShown(WorkspaceTabs* t, int i) const { return d()->leafWidget(t->leaves[i])->isVisible(); }
    static int zIndex(QWidget* w) { return int(w->parentWidget()->children().indexOf(w)); }
    static void flush()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
    static void wheel(QWidget* w, QPoint angle, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        const QPointF pos = QRectF(w->rect()).center();
        QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(), angle, Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &ev);
    }
    // Drags the spine of tab `index` to a workspace-widget point.
    void dragSpine(WorkspaceTabs* from, int index, QPoint wsTarget)
    {
        StackedTabHeader* s = from->w->spine(index);
        ASSERT_TRUE(s);
        const QPoint start = s->titleRect().center();
        sendMouse(s, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(s, QEvent::MouseMove, start + QPoint(0, 4), Qt::NoButton, Qt::LeftButton); // under the threshold
        EXPECT_FALSE(d()->drag.active);
        sendMouse(s, QEvent::MouseMove, start + QPoint(0, 12), Qt::NoButton, Qt::LeftButton);
        EXPECT_TRUE(d()->drag.active);
        const QPoint target = s->mapFromGlobal(ws()->mapToGlobal(wsTarget));
        sendMouse(s, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        sendMouse(s, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(d()->drag.active);
        flush();
    }
    // Opens the group's tab list and triggers the item titled `text`.
    bool pickFromTabList(WorkspaceTabs* t, const QString& text)
    {
        bool found = false;
        QTimer::singleShot(50, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction* act : menu->actions())
                if (act->text() == text && !found) {
                    found = true;
                    act->trigger();
                }
            menu->close();
        });
        header(t)->tabListButton()->click();
        flush();
        return found;
    }
};

TEST_F(StackedTabsTest, SpinesAndPanesStickLikeObsidian)
{
    WorkspaceTabs* t = buildStacked();
    TabGroupWidget* g = t->w;
    ASSERT_EQ(g->size(), QSize(900, 800));
    ASSERT_TRUE(g->isStacked());
    // The header row keeps "+" (after the empty 12px strip) and the tab list; the tabs are spines.
    EXPECT_EQ(header(t)->tabCount(), 0);
    EXPECT_EQ(header(t)->newTabButton()->x(), 8 + 12 - 4);
    EXPECT_TRUE(header(t)->tabListButton()->isVisible());
    // Spines 40px (--tab-stacked-header-width); panes 700px (--tab-stacked-pane-width, within
    // [max(300, 900 - 120) / 3, max(300, 900 - 120)]); the 2220px strip overflows: a 12px scroll bar.
    EXPECT_EQ(g->stackHeaderWidth(), 40);
    EXPECT_DOUBLE_EQ(g->stackPaneWidth(), 700);
    EXPECT_EQ(g->stackMaxScroll(), 3 * 740 - 900);
    ASSERT_TRUE(g->stackScrollBar()->isVisible());
    EXPECT_EQ(g->stackScrollBar()->geometry(), QRect(0, 788, 900, 12));
    EXPECT_EQ(g->stackScrollBar()->maximum(), 1320);
    // C (current) was scrolled into view: the strip's end. Spines 0 and 1 stick at the left, B shows
    // 80px under them before C's spine, A is entirely covered (hidden, like Obsidian's .is-hidden).
    EXPECT_EQ(g->stackScroll(), 1320);
    EXPECT_EQ(spineRect(t, 0), QRect(0, 40, 40, 748));
    EXPECT_EQ(spineRect(t, 1), QRect(40, 40, 40, 748));
    EXPECT_EQ(spineRect(t, 2), QRect(160, 40, 40, 748));
    EXPECT_EQ(paneRect(t, 1), QRect(80, 40, 700, 748));
    EXPECT_EQ(paneRect(t, 2), QRect(200, 40, 700, 748));
    EXPECT_FALSE(paneShown(t, 0));
    EXPECT_TRUE(paneShown(t, 1));
    EXPECT_TRUE(paneShown(t, 2));
    // Later spines and panes paint over earlier ones.
    EXPECT_LT(zIndex(d()->leafWidget(t->leaves[1])), zIndex(g->spine(2)));
    EXPECT_LT(zIndex(g->spine(2)), zIndex(d()->leafWidget(t->leaves[2])));
    EXPECT_LT(zIndex(d()->leafWidget(t->leaves[2])), zIndex(g->stackScrollBar()));
    // Every pane has its view header.
    EXPECT_TRUE(d()->leafWidget(t->leaves[1])->header()->isVisible());

    // At the start: A shows; B's spine follows it, C's sticks at the right edge; C (current) stays shown.
    g->setStackScroll(0);
    EXPECT_EQ(spineRect(t, 0).x(), 0);
    EXPECT_EQ(paneRect(t, 0), QRect(40, 40, 700, 748));
    EXPECT_EQ(spineRect(t, 1).x(), 740);
    EXPECT_EQ(paneRect(t, 1).x(), 780);
    EXPECT_EQ(spineRect(t, 2).x(), 860);
    EXPECT_TRUE(paneShown(t, 0));
    EXPECT_TRUE(paneShown(t, 1));
    EXPECT_TRUE(paneShown(t, 2));
    EXPECT_EQ(g->stackScrollBar()->value(), 0);
    // Halfway: A sticks after its spine, B slides over it.
    g->setStackScroll(400);
    EXPECT_EQ(paneRect(t, 0).x(), 40);
    EXPECT_EQ(spineRect(t, 1).x(), 340);
    EXPECT_EQ(paneRect(t, 1).x(), 380);
    EXPECT_EQ(spineRect(t, 2).x(), 860);
    EXPECT_EQ(g->stackScrollBar()->value(), 400);
    // Out of range: clamped.
    g->setStackScroll(5000);
    EXPECT_EQ(g->stackScroll(), 1320);

    // The scroll bar, horizontal wheels and Shift + wheel scroll; plain vertical wheels don't (they
    // belong to the panes). (Real wheel events over a spine or a pane that doesn't scroll sideways reach the
    // group; synthesized ones don't propagate, so they go to the group here.)
    g->stackScrollBar()->setValue(500);
    EXPECT_EQ(g->stackScroll(), 500);
    wheel(g, QPoint(-120, 0));
    EXPECT_EQ(g->stackScroll(), 560);
    wheel(g, QPoint(0, -120));
    EXPECT_EQ(g->stackScroll(), 560);
    wheel(g, QPoint(0, -120), Qt::ShiftModifier);
    EXPECT_EQ(g->stackScroll(), 620);

    // A single tab fills the strip: no scrolling.
    m_ws->closeLeaves({t->leaves[0], t->leaves[1]});
    flush();
    ASSERT_EQ(describe(t), "T(C*)");
    EXPECT_DOUBLE_EQ(g->stackPaneWidth(), 860);
    EXPECT_EQ(g->stackMaxScroll(), 0);
    EXPECT_FALSE(g->stackScrollBar()->isVisible());
    EXPECT_EQ(spineRect(t, 0), QRect(0, 40, 40, 760));
    EXPECT_EQ(paneRect(t, 0), QRect(40, 40, 860, 760));
}

TEST_F(StackedTabsTest, SpinePaintsItsParts)
{
    WorkspaceTabs* t = buildStacked();
    StackedTabHeader* s = t->w->spine(0);
    ASSERT_TRUE(s);
    // Obsidian's spine column: close (20px) at the top after 8px, the icon (22px) 2px below, then the title
    // down to the 16px bottom padding.
    EXPECT_EQ(s->closeRect(), QRect(10, 8, 20, 20));
    EXPECT_EQ(s->iconRect(), QRect(9, 30, 22, 22));
    EXPECT_EQ(s->titleRect(), QRect(8, 54, 24, 748 - 16 - 54));
    const QImage img = s->grab().toImage();
    const QRgb bg = currentTheme()->color("--background-primary").rgba();
    EXPECT_EQ(img.pixel(2, 400), bg);
    // Ink in the close button, the icon and the title's first letter.
    auto inked = [&](const QRect& r) {
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                if (img.pixel(x, y) != bg)
                    return true;
        return false;
    };
    EXPECT_TRUE(inked(s->closeRect()));
    EXPECT_TRUE(inked(s->iconRect()));
    EXPECT_TRUE(inked(QRect(12, 58, 16, 10)));
    EXPECT_FALSE(inked(QRect(0, 200, 40, 300))); // "A" is short
}

TEST_F(StackedTabsTest, ClickingASpineActivatesAndRevealsItsPane)
{
    WorkspaceTabs* t = buildStacked();
    TabGroupWidget* g = t->w;
    click(g->spine(0), g->spine(0)->titleRect().center());
    EXPECT_EQ(m_ws->activeLeaf(), t->leaves[0]);
    EXPECT_EQ(t->currentTab, 0);
    EXPECT_EQ(g->stackScroll(), 0);
    EXPECT_TRUE(paneShown(t, 0));
    // Commands move the current tab and reveal it: B needs its pane and C's spine in view.
    m_ws->nextTab();
    EXPECT_EQ(g->stackScroll(), 3 * 40 + 2 * 700 - 900);
    EXPECT_EQ(paneRect(t, 1), QRect(160, 40, 700, 748));
    EXPECT_EQ(spineRect(t, 2).x(), 860);
    m_ws->goToTab(2);
    EXPECT_EQ(g->stackScroll(), 1320);
    m_ws->previousTab();
    EXPECT_EQ(g->stackScroll(), 700); // B as far left as it goes (A covered)
    // The current tab's spine reveals its pane after a manual scroll.
    g->setStackScroll(0);
    click(g->spine(1), g->spine(1)->titleRect().center());
    EXPECT_EQ(g->stackScroll(), 620);
    // Clicking into a partly covered pane activates (and reveals) it.
    m_ws->goToTab(2);
    ASSERT_EQ(g->stackScroll(), 1320);
    ASSERT_TRUE(paneShown(t, 1));
    click(t->leaves[1]->view(), QPoint(20, 200));
    EXPECT_EQ(m_ws->activeLeaf(), t->leaves[1]);
    EXPECT_EQ(g->stackScroll(), 700);
    // "+" appends an empty tab and scrolls to it.
    header(t)->newTabButton()->click();
    flush();
    ASSERT_EQ(t->leaves.size(), 4);
    EXPECT_EQ(g->stackScroll(), g->stackMaxScroll());
    EXPECT_EQ(m_ws->activeLeaf(), t->leaves[3]);
    EXPECT_TRUE(g->spine(3)->isVisible());
}

TEST_F(StackedTabsTest, SpinesCloseUnpinAndUndo)
{
    WorkspaceTabs* t = buildStacked(4);
    TabGroupWidget* g = t->w;
    StackedTabHeader* sb = g->spine(1);
    click(sb, sb->closeRect().center()); // every spine has its close button
    flush();
    EXPECT_EQ(describe(t), "T(A,C,D*)");
    EXPECT_EQ(g->spine(1)->leaf()->displayText(), "C");
    click(g->spine(0), g->spine(0)->titleRect().center(), Qt::MiddleButton);
    flush();
    EXPECT_EQ(describe(t), "T(C,D*)");
    ASSERT_EQ(g->spine(2), nullptr);
    // A pinned tab's pin replaces the close button: it unpins.
    WorkspaceLeaf* c = t->leaves[0];
    c->setPinned(true);
    click(g->spine(0), g->spine(0)->closeRect().center());
    EXPECT_FALSE(c->isPinned());
    EXPECT_EQ(describe(t), "T(C,D*)");
    // Undo close: A comes back where it was, current and in view.
    m_ws->undoCloseLeaf();
    flush();
    EXPECT_EQ(describe(t), "T(A*,C,D)");
    EXPECT_EQ(g->spine(0)->leaf()->displayText(), "A");
    EXPECT_EQ(g->stackScroll(), 0);
    EXPECT_TRUE(paneShown(t, 0));
}

TEST_F(StackedTabsTest, DropsIntoAndOutOfAStackedGroup)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    WorkspaceTabs* X = rootTabs(m_ws.get(), 1);
    ASSERT_TRUE(m_ws->setStacked(A->leaves[0], true));
    flush();
    TabGroupWidget* g = A->w;
    ASSERT_EQ(g->width(), 450);
    // 450px: panes max(300, 450 - 120) = 330; C current at the end: spines at 0, 40, 80.
    EXPECT_DOUBLE_EQ(g->stackPaneWidth(), 330);
    EXPECT_EQ(g->stackScroll(), 120 + 990 - 450);
    EXPECT_EQ(spineRect(A, 2).x(), 80);
    WorkspaceLeaf* x = X->leaves[0];
    const QRect ar(g->mapTo(ws(), QPoint(0, 0)), g->size());
    // Obsidian's getDropDirection() for a stacked group: its strip takes tabs between the spines (no left /
    // right split there), its lower third splits down...
    Target t = targetAt(x, QPoint(ar.x() + 10, ar.y() + 300));
    EXPECT_EQ(t.tabs, A);
    EXPECT_EQ(t.edge, DropEdge::Center);
    EXPECT_EQ(t.index, 0);
    EXPECT_EQ(t.rect, QRect(ar.x() - 5, ar.y() + 40, 10, spineRect(A, 0).height()));
    t = targetAt(x, QPoint(ar.x() + 30, ar.y() + 300));
    EXPECT_EQ(t.index, 1);
    EXPECT_EQ(t.rect.x(), ar.x() + 40 - 5);
    t = targetAt(x, QPoint(ar.right() - 10, ar.y() + 300));
    EXPECT_EQ(t.edge, DropEdge::Center);
    EXPECT_EQ(t.index, 3);
    t = targetAt(x, QPoint(ar.x() + 225, ar.bottom() - 20));
    EXPECT_EQ(t.edge, DropEdge::Bottom);
    // ...and its header row still splits left / right, or up from its upper third.
    t = targetAt(x, QPoint(ar.x() + 10, ar.y() + 25));
    EXPECT_EQ(t.edge, DropEdge::Left);
    t = targetAt(x, QPoint(ar.right() - 10, ar.y() + 25));
    EXPECT_EQ(t.edge, DropEdge::Right);
    t = targetAt(x, QPoint(ar.x() + 225, ar.y() + 5));
    EXPECT_EQ(t.edge, DropEdge::Top);
    t = targetAt(x, QPoint(ar.x() + 225, ar.y() + 25));
    EXPECT_EQ(t.edge, DropEdge::Center);

    // X's tab dropped after A's spine joins the stack there, current and in view.
    dragTab(X, 0, QPoint(ar.x() + 30, ar.y() + 300));
    EXPECT_EQ(describe(d()->root), "V[T(A,X*,B,C)]");
    ASSERT_TRUE(A->w->isStacked());
    EXPECT_EQ(A->w->spine(1)->leaf(), x);
    EXPECT_EQ(m_ws->activeLeaf(), x);
    EXPECT_TRUE(paneShown(A, 1));
    // A spine dragged onto the header row's right edge splits it off (a new, unstacked group).
    const QRect ar2(A->w->mapTo(ws(), QPoint(0, 0)), A->w->size());
    dragSpine(A, 2, QPoint(ar2.right() - 10, ar2.y() + 25));
    EXPECT_EQ(describe(d()->root), "V[T(A,X*,C),T(B*)]");
    EXPECT_TRUE(rootTabs(m_ws.get(), 0)->w->isStacked());
    EXPECT_FALSE(rootTabs(m_ws.get(), 1)->stacked);
    // Reordering within the stack: C's spine dropped before A's.
    const QRect ar3(A->w->mapTo(ws(), QPoint(0, 0)), A->w->size());
    const QRect s0 = spineRect(A, 0);
    dragSpine(A, 2, QPoint(ar3.x() + s0.x() + 5, ar3.y() + 300));
    EXPECT_EQ(describe(A), "T(C*,A,X)");
    EXPECT_EQ(A->w->spine(0)->leaf()->displayText(), "C");
}

TEST_F(StackedTabsTest, TabListStacksAndUnstacks)
{
    buildTwoGroups();
    WorkspaceTabs* A = rootTabs(m_ws.get(), 0);
    ASSERT_FALSE(A->stacked);
    EXPECT_TRUE(pickFromTabList(A, "Stack tabs"));
    EXPECT_TRUE(A->stacked);
    EXPECT_TRUE(A->w->isStacked());
    EXPECT_FALSE(rootTabs(m_ws.get(), 1)->stacked); // just that group
    ASSERT_TRUE(A->w->spine(2));
    EXPECT_EQ(A->w->stackScroll(), A->w->stackMaxScroll()); // the current tab (C) in view
    EXPECT_TRUE(pickFromTabList(A, "Unstack tabs"));
    EXPECT_FALSE(A->stacked);
    flush();
    // Back to ordinary tabs: the header has them again and only the current pane shows, full size.
    EXPECT_FALSE(A->w->isStacked());
    EXPECT_EQ(header(A)->tabCount(), 3);
    EXPECT_EQ(A->w->spine(0), nullptr);
    EXPECT_FALSE(A->w->stackScrollBar()->isVisible());
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(paneShown(A, i), i == A->currentTab);
    EXPECT_EQ(paneRect(A, A->currentTab), QRect(0, 40, A->w->width(), A->w->height() - 40));
    EXPECT_EQ(header(A)->newTabButton()->x(), header(A)->tabRect(2).right() + 1 + 6);

    // Sidebar groups keep ordinary tabs (their tab list isn't shown; the flag only round-trips).
    WorkspaceTabs* side = d()->left->children.first()->asTabs();
    EXPECT_FALSE(m_ws->setStacked(side->leaves[0], true));
    d()->setStacked(side, true);
    flush();
    EXPECT_TRUE(side->stacked);
    EXPECT_FALSE(side->w->isStacked());
    EXPECT_EQ(header(side)->tabCount(), 3);
}

TEST_F(StackedTabsTest, SmoothScrollingEndsAtTheTarget)
{
    WorkspaceTabs* t = buildStacked();
    TabGroupWidget* g = t->w;
    const bool general = QApplication::isEffectEnabled(Qt::UI_General);
    QApplication::setEffectEnabled(Qt::UI_General, true);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
    m_ws->goToTab(0);
    EXPECT_GT(g->stackScroll(), 0); // on its way
    EXPECT_FALSE(Busy::idle());     // headless runs wait for it
    EXPECT_TRUE(QTest::qWaitFor([&] { return g->stackScroll() == 0 && Busy::idle(); }, 3000));
    // A manual scroll stops it.
    m_ws->goToTab(2);
    g->setStackScroll(100);
    EXPECT_TRUE(Busy::idle());
    QTest::qWait(50);
    EXPECT_EQ(g->stackScroll(), 100);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QApplication::setEffectEnabled(Qt::UI_General, general);
}
