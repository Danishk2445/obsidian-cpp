// GraphCanvas (renderer + interactions), GraphControls and GraphWidget (filter pipeline, local
// graph, timelapse, settings), plus the headless screenshots (set OC_GRAPH_SHOT_DIR to write PNGs).
#include "graph/graph_test_support.h"

#include "graph/simulation_runner.h"
#include "render/theme/theme.h"
#include "ui/graph/graph_canvas.h"
#include "ui/graph/graph_controls.h"
#include "ui/graph/graph_widget.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;
using namespace oc::graph;

namespace {

ThemeManager* themes()
{
    static ThemeManager* tm = [] {
        auto* t = ThemeManager::instance() ? ThemeManager::instance() : new ThemeManager(qApp);
        t->setMode(ThemeManager::Mode::Dark);
        t->apply();
        return t;
    }();
    return tm;
}

void setScheme(bool dark)
{
    themes()->setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
    themes()->apply();
}

// A small hand-made vault graph: a hub, a chain, an unresolved link, a tag and an attachment.
std::shared_ptr<GraphData> smallGraph()
{
    auto g = std::make_shared<GraphData>();
    g->addNode("Hub.md", NodeKind::File, {}, 10);
    g->addNode("A.md", NodeKind::File, {}, 20);
    g->addNode("B.md", NodeKind::File, {}, 30);
    g->addNode("C.md", NodeKind::File, {}, 40);
    g->addNode("Lonely.md", NodeKind::File, {}, 50);
    g->addNode("Missing", NodeKind::Unresolved);
    g->addNode("#topic", NodeKind::Tag);
    g->addNode("pic.png", NodeKind::Attachment, {}, 60);
    g->addEdge("A.md", "Hub.md");
    g->addEdge("B.md", "Hub.md");
    g->addEdge("C.md", "Hub.md");
    g->addEdge("Hub.md", "A.md");
    g->addEdge("C.md", "B.md");
    g->addEdge("B.md", "Missing");
    g->addEdge("A.md", "#topic");
    g->addEdge("C.md", "pic.png");
    g->finalize();
    return g;
}

QStringList ids(const GraphData& g)
{
    QStringList out;
    for (const GraphNode& n : g.nodes())
        out << n.id;
    out.sort();
    return out;
}

struct Fixture : ::testing::Test {
    void SetUp() override { setScheme(true); }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------------------------
TEST_F(Fixture, CanvasZoomMathMatchesObsidian)
{
    GraphWidget w;
    w.resize(800, 600);
    w.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&w));
    GraphCanvas* c = w.canvas();
    c->resetPan();
    EXPECT_EQ(c->pan(), QPointF(400, 300));
    c->setScaleImmediate(0.25);
    EXPECT_DOUBLE_EQ(c->scale(), 0.25);
    EXPECT_DOUBLE_EQ(c->nodeScale(), 2.0);  // sqrt(1/scale)
    EXPECT_DOUBLE_EQ(c->textAlpha(), 0.0);  // clamp(log2(.25) + 1 - 0, 0, 1)
    c->setScaleImmediate(1);
    EXPECT_DOUBLE_EQ(c->textAlpha(), 1.0);
    GraphCanvas::RenderOptions o;
    o.textFadeMultiplier = -1.5;
    c->setRenderOptions(o);
    c->setScaleImmediate(0.5);
    EXPECT_DOUBLE_EQ(c->textAlpha(), 1.0); // -1 + 1 + 1.5 -> clamped
    c->setScaleImmediate(0.25);
    EXPECT_DOUBLE_EQ(c->textAlpha(), 0.5);

    // Wheel: one notch in = ×1.5 target around the cursor, which stays fixed in world space.
    c->setScaleImmediate(1);
    c->resetPan();
    const QPointF cursor(600, 200);
    const QPointF worldBefore = c->widgetToWorld(cursor);
    QWheelEvent wheel(cursor, c->mapToGlobal(cursor), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(c, &wheel);
    EXPECT_NEAR(c->targetScale(), 1.5, 1e-12);
    for (int i = 0; i < 200; ++i)
        c->step();
    EXPECT_NEAR(c->scale(), 1.5, 0.02); // eased to within 1 %
    const QPointF worldAfter = c->widgetToWorld(cursor);
    EXPECT_NEAR(worldAfter.x(), worldBefore.x(), 1e-6);
    EXPECT_NEAR(worldAfter.y(), worldBefore.y(), 1e-6);
    // Zooming out is centred on the view.
    QWheelEvent out(cursor, c->mapToGlobal(cursor), QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(c, &out);
    EXPECT_NEAR(c->targetScale(), c->scale() > 0 ? 1.5 / 2.25 : 0, 0.05);
    const QPointF centerWorld = c->widgetToWorld(QPointF(400, 300));
    for (int i = 0; i < 200; ++i)
        c->step();
    EXPECT_NEAR(c->widgetToWorld(QPointF(400, 300)).x(), centerWorld.x(), 1e-6);
}

TEST_F(Fixture, CanvasSizesHitTestingAndClicks)
{
    GraphWidget w;
    w.resize(900, 700);
    w.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&w));
    GraphSettings s;
    s.scale = 1;
    w.setSettings(s, false);
    w.setGraph(smallGraph());
    w.settle();
    GraphCanvas* c = w.canvas();
    ASSERT_TRUE(c->graph());
    EXPECT_EQ(ids(*c->graph()), (QStringList{"A.md", "B.md", "C.md", "Hub.md", "Lonely.md", "Missing"}));

    // Radius: nodeSizeMultiplier · clamp(3·sqrt(weight + 1), 8, 30) · nodeScale.
    const int hub = c->indexOf("Hub.md");
    const double weight = c->graph()->node(hub).weight; // A, B, C in + A out
    EXPECT_EQ(weight, 4);
    EXPECT_NEAR(c->nodeRadius(hub), std::max(8.0, 3 * std::sqrt(5.0)), 1e-9);
    const QPointF hubPos = c->worldToWidget(c->nodePosition(hub));
    EXPECT_EQ(c->nodeAt(hubPos), hub);
    EXPECT_EQ(c->nodeAt(hubPos + QPointF(c->nodeRadius(hub) - 1, 0)), hub);
    EXPECT_EQ(c->nodeAt(QPointF(2, 2)), -1);

    // Hover highlights the node and its neighbours; everything else fades to 0.2.
    QTest::mouseMove(c, hubPos.toPoint());
    EXPECT_EQ(c->hoveredNode(), "Hub.md");

    // A click (press + release without moving 5 px) opens; Ctrl opens in a new tab.
    QSignalSpy open(&w, &GraphWidget::openRequested);
    QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, hubPos.toPoint());
    ASSERT_EQ(open.size(), 1);
    EXPECT_EQ(open[0][0].toString(), "Hub.md");
    EXPECT_FALSE(open[0][1].toBool());
    QTest::mouseClick(c, Qt::LeftButton, Qt::ControlModifier, hubPos.toPoint());
    ASSERT_EQ(open.size(), 2);
    EXPECT_TRUE(open[1][1].toBool());
    QTest::mouseClick(c, Qt::MiddleButton, Qt::NoModifier, hubPos.toPoint());
    ASSERT_EQ(open.size(), 3);
    EXPECT_TRUE(open[2][1].toBool());

    // Right click asks for the context menu.
    QSignalSpy menu(&w, &GraphWidget::contextMenuRequested);
    QTest::mouseClick(c, Qt::RightButton, Qt::NoModifier, hubPos.toPoint());
    ASSERT_EQ(menu.size(), 1);
    EXPECT_EQ(menu[0][0].toString(), "Hub.md");
}

TEST_F(Fixture, CanvasDragPinsReheatsAndDoesNotClick)
{
    GraphWidget w;
    w.resize(900, 700);
    w.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&w));
    GraphSettings s;
    s.scale = 1;
    w.setSettings(s, false);
    w.setGraph(smallGraph());
    w.settle();
    GraphCanvas* c = w.canvas();
    const int a = c->indexOf("A.md");
    const QPoint start = c->worldToWidget(c->nodePosition(a)).toPoint();
    QSignalSpy open(&w, &GraphWidget::openRequested);
    QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, start);
    const QPoint target = start + QPoint(120, 80);
    QMouseEvent move(QEvent::MouseMove, target, c->mapToGlobal(target), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(c, &move);
    // The dragged node follows the pointer immediately and the simulation is pinned + reheated.
    EXPECT_EQ(c->hoveredNode(), "A.md");
    const QPointF dragged = c->worldToWidget(c->nodePosition(a));
    EXPECT_NEAR(dragged.x(), target.x(), 1e-6);
    EXPECT_NEAR(dragged.y(), target.y(), 1e-6);
    const QPointF pinnedWorld = c->widgetToWorld(target);
    w.runner()->setFrameRate(0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return w.runner()->ticks() > 0 && w.runner()->acquire(); }, 5000));
    w.runner()->waitForIdle(1000); // pinned: stays hot (alphaTarget 0.3) — only drain the queue
    QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, target);
    EXPECT_TRUE(open.isEmpty()); // moved more than 5 px: not a click
    w.settle();
    // Released: the node is free again and drifts back towards its neighbours.
    const QPointF after = c->nodePosition(c->indexOf("A.md"));
    EXPECT_GT(std::hypot(after.x() - pinnedWorld.x(), after.y() - pinnedWorld.y()), 1.0);
}

TEST_F(Fixture, CanvasHoverFadesOthersAndPanInertia)
{
    GraphWidget w;
    w.resize(900, 700);
    w.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&w));
    GraphSettings s;
    s.scale = 1;
    w.setSettings(s, false);
    w.setGraph(smallGraph());
    w.settle();
    GraphCanvas* c = w.canvas();
    c->setHoveredNode("Lonely.md");
    for (int i = 0; i < 80; ++i)
        c->step();
    QImage img = c->grabFrame();
    EXPECT_FALSE(img.isNull());
    EXPECT_GT(c->lastFrameStats().labels, 0);
    c->setHoveredNode(QString());

    // Dragging the background pans 1:1.
    c->resetPan();
    const QPointF before = c->pan();
    QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
    QMouseEvent move(QEvent::MouseMove, QPointF(55, 25), c->mapToGlobal(QPointF(55, 25)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(c, &move);
    EXPECT_EQ(c->pan(), before + QPointF(50, 20));
    QTest::qWait(150); // released after a pause: no inertia
    QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, QPoint(55, 25));
    const QPointF released = c->pan();
    for (int i = 0; i < 10; ++i)
        c->step();
    EXPECT_EQ(c->pan(), released);
}

// ---------------------------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------------------------
TEST_F(Fixture, ControlsEditSettingsAndLayout)
{
    GraphControls controls(GraphControls::Variant::Global);
    QSignalSpy edited(&controls, &GraphControls::edited);
    GraphSettings s = GraphSettings::userDefaults();
    controls.setSettings(s);
    EXPECT_TRUE(edited.isEmpty()); // programmatic
    EXPECT_EQ(controls.sizeHint().width(), 240);
    EXPECT_FALSE(controls.toggle("localBacklinks"));
    EXPECT_FALSE(controls.slider("localJumps"));
    ASSERT_TRUE(controls.toggle("showOrphans"));
    EXPECT_TRUE(controls.toggle("showOrphans")->isChecked());

    controls.toggle("showTags")->click();
    ASSERT_EQ(edited.size(), 1);
    EXPECT_TRUE(controls.settings().showTags);
    EXPECT_EQ(edited[0][0].value<GraphControls::Changes>(), GraphControls::Changes(GraphControls::Change::Filters));

    controls.slider("repelStrength")->setFocus();
    QTest::keyClick(controls.slider("repelStrength"), Qt::Key_End);
    EXPECT_DOUBLE_EQ(controls.settings().repelStrength, 20);
    EXPECT_TRUE(edited.last()[0].value<GraphControls::Changes>() & GraphControls::Change::Forces);

    controls.searchInput()->setText("tag:#work");
    EXPECT_EQ(controls.settings().search, "tag:#work");
    EXPECT_TRUE(edited.last()[0].value<GraphControls::Changes>() & GraphControls::Change::Search);

    // Sections: collapsed by default; expanding one grows the panel.
    const int collapsedHeight = controls.sizeHint().height();
    controls.setSectionCollapsed(3, false);
    EXPECT_FALSE(controls.settings().collapseForces);
    EXPECT_GT(controls.sizeHint().height(), collapsedHeight + 100);

    // Groups: new group gets hsl(0, 70%, 60%) in dark mode; delete removes it.
    controls.newGroupButton()->click();
    ASSERT_EQ(controls.settings().colorGroups.size(), 1);
    EXPECT_EQ(QColor::fromRgb(controls.settings().colorGroups[0].color.rgb).name(), QColor::fromHslF(0, 0.7f, 0.6f).name());
    controls.setGroupQuery(0, "path:Projects");
    EXPECT_EQ(controls.settings().colorGroups[0].query, "path:Projects");
    controls.newGroupButton()->click();
    EXPECT_EQ(QColor::fromRgb(controls.settings().colorGroups[1].color.rgb).name(), QColor::fromHslF(40.0f / 360, 0.7f, 0.6f).name());
    controls.removeGroup(0);
    ASSERT_EQ(controls.settings().colorGroups.size(), 1);
    EXPECT_EQ(controls.groupRowCount(), 1);

    // Restore defaults keeps the search text and panel state.
    controls.resetButton()->click();
    EXPECT_EQ(controls.settings().search, "tag:#work");
    EXPECT_FALSE(controls.settings().showTags);
    EXPECT_DOUBLE_EQ(controls.settings().repelStrength, 10);
    EXPECT_TRUE(controls.settings().colorGroups.isEmpty());
    EXPECT_FALSE(controls.settings().collapseForces);

    // Close: shrinks to the settings + animate buttons.
    controls.closeButton()->click();
    EXPECT_TRUE(controls.isClosed());
    EXPECT_EQ(controls.sizeHint(), QSize(44, 74));
    controls.openButton()->click();
    EXPECT_FALSE(controls.isClosed());

    GraphControls local(GraphControls::Variant::Local);
    EXPECT_TRUE(local.toggle("localBacklinks"));
    EXPECT_TRUE(local.slider("localJumps"));
    EXPECT_FALSE(local.toggle("showOrphans"));
    EXPECT_FALSE(local.animateButton());
    local.setClosed(true);
    EXPECT_EQ(local.sizeHint(), QSize(44, 40));
}

// ---------------------------------------------------------------------------------------------
// Widget pipeline
// ---------------------------------------------------------------------------------------------
TEST_F(Fixture, WidgetFiltersGroupsAndSettings)
{
    GraphWidget w;
    w.resize(800, 600);
    w.setGraph(smallGraph());
    EXPECT_EQ(ids(*w.shownGraph()), (QStringList{"A.md", "B.md", "C.md", "Hub.md", "Lonely.md", "Missing"}));

    QSignalSpy changed(&w, &GraphWidget::settingsChanged);
    w.controls()->toggle("showTags")->click();
    w.controls()->toggle("showAttachments")->click();
    w.controls()->toggle("hideUnresolved")->click();
    w.controls()->toggle("showOrphans")->click();
    EXPECT_EQ(ids(*w.shownGraph()), (QStringList{"#topic", "A.md", "B.md", "C.md", "Hub.md", "pic.png"}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !changed.isEmpty(); }, 2000));
    EXPECT_TRUE(w.settings().showTags);
    EXPECT_FALSE(w.settings().showOrphans);

    // Search (debounced) with the stand-in query compiler; Enter applies immediately.
    w.controls()->toggle("showOrphans")->click();
    w.controls()->searchInput()->setText("hub");
    QTest::keyClick(w.controls()->searchInput(), Qt::Key_Return);
    EXPECT_EQ(ids(*w.shownGraph()), QStringList{"Hub.md"}); // tags must match the search too
    w.controls()->searchInput()->setText(QString());
    QTest::keyClick(w.controls()->searchInput(), Qt::Key_Return);

    // Colour groups colour matching files.
    w.controls()->newGroupButton()->click();
    w.controls()->setGroupQuery(0, "file:A.md");
    ASSERT_TRUE(QTest::qWaitFor([&] {
        const int a = w.shownGraph()->indexOf("A.md");
        return a >= 0 && w.shownGraph()->node(a).color.has_value();
    }, 2000));
    EXPECT_FALSE(w.shownGraph()->node(w.shownGraph()->indexOf("B.md")).color.has_value());

    // Settings round trip through JSON with exactly Obsidian's keys.
    const GraphSettings back = GraphSettings::fromJson(w.settings().toJson());
    EXPECT_EQ(back, w.settings());
}

TEST_F(Fixture, WidgetRebuildsOnlyReheatOnTopologyChanges)
{
    GraphWidget w;
    w.resize(600, 400);
    w.setGraph(smallGraph());
    w.settle();
    w.runner()->setFrameRate(0);
    const quint64 ticks = w.runner()->ticks();
    ASSERT_GT(ticks, 0u);
    // Same content in a new object (e.g. a metadata change elsewhere): the layout stays at rest.
    w.setGraph(smallGraph());
    ASSERT_TRUE(w.runner()->waitForIdle(5000));
    EXPECT_EQ(w.runner()->ticks(), ticks);
    // Colours only (a colour group): still at rest, but the new graph is shown.
    w.controls()->newGroupButton()->click();
    w.controls()->setGroupQuery(0, "file:A.md");
    ASSERT_TRUE(QTest::qWaitFor([&] { return w.shownGraph()->node(w.shownGraph()->indexOf("A.md")).color.has_value(); }, 2000));
    ASSERT_TRUE(w.runner()->waitForIdle(5000));
    EXPECT_EQ(w.runner()->ticks(), ticks);
    // A new link wakes the simulation.
    auto g = std::make_shared<GraphData>(*smallGraph());
    g->addEdge("Lonely.md", "Hub.md");
    g->finalize();
    w.setGraph(g);
    ASSERT_TRUE(w.runner()->waitForIdle(20000));
    EXPECT_GT(w.runner()->ticks(), ticks);
}

TEST_F(Fixture, WidgetLocalGraphAndTimelapse)
{
    GraphWidget local(GraphWidget::Mode::Local);
    local.resize(600, 400);
    local.setGraph(smallGraph());
    EXPECT_EQ(local.shownGraph()->nodeCount(), 0); // no file yet
    local.setLocalCenter("C.md");
    EXPECT_EQ(ids(*local.shownGraph()), (QStringList{"B.md", "C.md", "Hub.md"}));
    EXPECT_EQ(local.shownGraph()->node(local.shownGraph()->indexOf("C.md")).weight, 30);
    local.controls()->slider("localJumps")->setFocus();
    QTest::keyClick(local.controls()->slider("localJumps"), Qt::Key_Right);
    EXPECT_EQ(local.settings().localJumps, 2);
    EXPECT_EQ(ids(*local.shownGraph()), (QStringList{"A.md", "B.md", "C.md", "Hub.md", "Missing"}));
    local.controls()->toggle("localBacklinks")->click(); // outgoing only from C: B, Hub, then Missing, A
    EXPECT_FALSE(local.settings().localBacklinks);
    EXPECT_EQ(ids(*local.shownGraph()), (QStringList{"A.md", "B.md", "C.md", "Hub.md", "Missing"}));
    const QJsonObject json = local.settings().toJson(GraphSettings::Variant::Local);
    EXPECT_TRUE(json.contains("localJumps"));
    EXPECT_FALSE(json.contains("showOrphans"));

    GraphWidget global;
    global.resize(600, 400);
    global.setGraph(smallGraph());
    global.startTimelapse();
    EXPECT_TRUE(global.isTimelapseRunning());
    EXPECT_LT(global.shownGraph()->nodeCount(), 6);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !global.isTimelapseRunning(); }, 10000));
    EXPECT_EQ(global.shownGraph()->nodeCount(), 6);
}

// ---------------------------------------------------------------------------------------------
// Screenshots
// ---------------------------------------------------------------------------------------------
TEST_F(Fixture, HeadlessScreenshots)
{
    const QString dir = qEnvironmentVariable("OC_GRAPH_SHOT_DIR");
    if (!dir.isEmpty())
        QDir().mkpath(dir);
    const auto graph = test::makeSyntheticGraph();
    for (const bool dark : {true, false}) {
        setScheme(dark);
        GraphWidget w;
        w.resize(1200, 800);
        GraphSettings s = GraphSettings::userDefaults();
        s.collapseFilter = false;
        w.setSettings(s, false);
        w.setGraph(graph);
        w.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&w));
        w.settle();
        QImage shot = w.grab().toImage();
        ASSERT_FALSE(shot.isNull());
        // The graph draws something besides the background.
        const QRgb bg = shot.pixel(5, shot.height() - 5);
        int differing = 0;
        for (int y = 0; y < shot.height(); y += 4)
            for (int x = 0; x < shot.width(); x += 4)
                differing += shot.pixel(x, y) != bg;
        EXPECT_GT(differing, 500);
        EXPECT_GT(w.canvas()->lastFrameStats().nodes, 100);
        if (!dir.isEmpty()) {
            const QString scheme = dark ? QStringLiteral("dark") : QStringLiteral("light");
            shot.save(dir + QStringLiteral("/graph-%1.png").arg(scheme));
            // Zoomed in on a hub with a hover highlight (labels, arrows).
            GraphSettings z = s;
            z.scale = 1.4;
            z.showArrow = true;
            z.collapseFilter = true;
            z.collapseDisplay = false;
            w.setSettings(z, false);
            const auto shown = w.shownGraph();
            int hub = 0;
            for (int i = 0; i < shown->nodeCount(); ++i)
                if (shown->node(i).weight > shown->node(hub).weight)
                    hub = i;
            w.canvas()->centerOn(w.canvas()->nodePosition(w.canvas()->indexOf(shown->node(hub).id)));
            w.canvas()->setHoveredNode(shown->node(hub).id);
            w.settle();
            w.grab().toImage().save(dir + QStringLiteral("/graph-%1-hover.png").arg(scheme));
            // Closed panel, zoomed out to the whole graph.
            w.canvas()->setHoveredNode(QString());
            GraphSettings o = s;
            o.close = true;
            o.scale = 0.25;
            w.setSettings(o, false);
            w.canvas()->resetPan();
            w.settle();
            w.grab().toImage().save(dir + QStringLiteral("/graph-%1-overview.png").arg(scheme));
            // Every section open, two colour groups (colours from the "New group" sequence).
            GraphSettings c = s;
            c.collapseFilter = c.collapseColorGroups = c.collapseDisplay = c.collapseForces = false;
            c.search = QStringLiteral("path:Projects");
            c.showTags = true;
            c.colorGroups = {{QStringLiteral("path:Journal"), {QColor::fromHslF(0, dark ? 0.7f : 0.6f, 0.6f).rgb() & 0xFFFFFFu, 1}},
                             {QStringLiteral("tag:#work"), {QColor::fromHslF(40.0f / 360, dark ? 0.7f : 0.6f, 0.6f).rgb() & 0xFFFFFFu, 1}}};
            w.resize(1200, 1300);
            w.setSettings(c, false);
            w.canvas()->resetPan();
            w.settle();
            w.grab().toImage().save(dir + QStringLiteral("/graph-%1-controls.png").arg(scheme));
        }
        if (!dir.isEmpty()) {
            // The local graph of a note (depth 1) in a narrow, sidebar-like pane.
            GraphWidget local(GraphWidget::Mode::Local);
            local.resize(420, 640);
            local.setInSidebar(true);
            GraphSettings ls;
            ls.localJumps = 1;
            ls.close = true;
            ls.scale = 0.9;
            local.setSettings(ls, false);
            local.setGraph(graph);
            int hub = 0; // a mid-sized note: its neighbourhood, not the whole vault
            for (int i = 0; i < graph->nodeCount(); ++i)
                if (graph->node(i).kind == NodeKind::File && graph->node(i).weight < 9
                    && (graph->node(hub).weight >= 9 || graph->node(i).weight > graph->node(hub).weight))
                    hub = i;
            local.setLocalCenter(graph->node(hub).id);
            local.setActiveNode(graph->node(hub).id);
            local.show();
            ASSERT_TRUE(QTest::qWaitForWindowExposed(&local));
            local.settle();
            local.grab().toImage().save(dir + QStringLiteral("/localgraph-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
        }
    }
    setScheme(true);
}
