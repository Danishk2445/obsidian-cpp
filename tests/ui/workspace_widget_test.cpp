// Workspace widget tests with synthetic input: tab clicks, middle click, close/pin buttons, the "+"
// button, tab drag & drop (drop regions and resulting model changes), view header navigation,
// resize handles and theme changes.

#include "ui/workspace_test_support.h"

#include "ui/widgets/icon_button.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <QVBoxLayout>

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
    // Very narrow: tabs stop shrinking at 48px and the strip scrolls to the current tab.
    m_host->resize(700, 600);
    QCoreApplication::processEvents();
    EXPECT_GE(h->tabRect(0).width(), 48);
    EXPECT_LT(h->tabRect(0).width(), expected);
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
