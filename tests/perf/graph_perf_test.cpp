// Perf gates for the graph view at 5,000 nodes / 15,000 links (label "perf"): one simulation tick
// (Barnes–Hut on the thread pool) and one rendered frame into a QImage (≤ 8 ms). Assertions only
// with OC_PERF_ASSERT=1 (optimized builds); other runs just report numbers.
#include "graph/graph_test_support.h"

#include "graph/force_simulation.h"
#include "graph/simulation_runner.h"
#include "render/theme/theme.h"
#include "ui/graph/graph_canvas.h"
#include "ui/graph/graph_raster.h"
#include "ui/graph/graph_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <random>

using namespace oc;
using namespace oc::graph;

namespace {

bool assertPerf() { return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1; }

std::shared_ptr<GraphData> bigGraph()
{
    test::SyntheticGraphOptions o;
    o.notes = 5000;
    o.linksPerNote = 3.4;
    o.tags = 0;
    o.attachments = 0;
    o.unresolved = 150;
    o.seed = 11;
    return test::makeSyntheticGraph(o);
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

TEST(GraphPerf, SimulationTick5k)
{
    const auto g = bigGraph();
    std::vector<ForceSimulation::Link> links;
    for (const GraphEdge& e : g->edges())
        links.push_back({e.source, e.target});
    std::printf("graph: %d nodes, %d links\n", g->nodeCount(), g->edgeCount());
    EXPECT_GE(g->nodeCount(), 5000);
    EXPECT_GE(g->edgeCount(), 15000);
    ForceSimulation sim;
    sim.setGraph(g->nodeCount(), links);
    sim.setForces(GraphSettings::userDefaults().forces());
    sim.tick(); // warm-up (allocations)
    std::vector<double> ms;
    QElapsedTimer t;
    for (int i = 0; i < 40; ++i) {
        t.restart();
        sim.tick();
        ms.push_back(t.nsecsElapsed() / 1e6);
    }
    const double med = median(ms);
    std::printf("tick: median %.2f ms, max %.2f ms\n", med, *std::max_element(ms.begin(), ms.end()));
    if (assertPerf()) {
        EXPECT_LT(med, 16.0); // keeps the 60 Hz worker rate
    }
}

TEST(GraphPerf, HairlineRaster)
{
    // 15k lines of ~60 px in a 1600×1000 frame: the link layer of a zoomed-out 5k graph.
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> ux(0, 1600), uy(0, 1000), ua(0, 6.2831853);
    graph_raster::LineBatch batch{0x3f3f3f, 255, {}};
    for (int i = 0; i < 15000; ++i) {
        const QPointF a(ux(rng), uy(rng));
        const double t = ua(rng);
        batch.lines.emplace_back(a, a + QPointF(std::cos(t), std::sin(t)) * 60);
    }
    QImage img(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    std::vector<double> ms;
    QElapsedTimer t;
    for (int i = 0; i < 20; ++i) {
        img.fill(0xff1e1e1e);
        t.restart();
        graph_raster::drawHairlines(img, {batch});
        ms.push_back(t.nsecsElapsed() / 1e6);
    }
    std::printf("hairlines: median %.2f ms (%d bands)\n", median(ms), graph_raster::bandCount(1000));
    if (assertPerf()) {
        EXPECT_LT(median(ms), 4.0);
    }
}

TEST(GraphPerf, RenderFrame5k)
{
    auto* tm = ThemeManager::instance() ? ThemeManager::instance() : new ThemeManager(qApp);
    tm->setMode(ThemeManager::Mode::Dark);
    tm->apply();

    GraphWidget w;
    w.resize(1600, 1000);
    w.show();
    GraphSettings s = GraphSettings::userDefaults();
    s.close = true;
    w.setSettings(s, false);
    w.setGraph(bigGraph());
    QElapsedTimer layout;
    layout.start();
    w.settle(120000);
    std::printf("layout converged in %.1f s\n", layout.elapsed() / 1000.0);
    GraphCanvas* c = w.canvas();
    ASSERT_TRUE(c->graph());

    QImage img(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    auto measure = [&](const char* what) {
        std::vector<double> ms;
        QElapsedTimer t;
        for (int i = 0; i < 30; ++i) {
            t.restart();
            c->step();
            QPainter p(&img);
            c->render(p, img.size());
            p.end();
            ms.push_back(t.nsecsElapsed() / 1e6);
        }
        const GraphCanvas::FrameStats st = c->lastFrameStats();
        const double med = median(ms);
        std::printf("%s: median %.2f ms, max %.2f ms (nodes %d, links %d, labels %d, arrows %d; last frame: links %.2f ms, "
                    "label images %.2f ms, layers %.2f ms)\n",
                    what, med, *std::max_element(ms.begin(), ms.end()), st.nodes, st.links, st.labels, st.arrows, st.linksNs / 1e6,
                    st.labelsNs / 1e6, st.layersNs / 1e6);
        return med;
    };

    // Everything in view (Obsidian's zoomed-out overview: no labels).
    const QRectF bounds = c->layoutBounds();
    const double fit = std::min(1600 / bounds.width(), 1000 / bounds.height()) * 0.95;
    c->setScaleImmediate(fit);
    c->centerOn(bounds.center());
    const double overview = measure("frame, whole graph");
    EXPECT_GT(c->lastFrameStats().nodes, 4500);
    // The user's zoom with a hovered hub (fading, highlight, labels fading in).
    c->setScaleImmediate(s.scale);
    int hub = 0;
    for (int i = 0; i < c->graph()->nodeCount(); ++i)
        if (c->graph()->node(i).weight > c->graph()->node(hub).weight)
            hub = i;
    c->centerOn(c->nodePosition(hub));
    c->setHoveredNode(c->graph()->node(hub).id);
    const double hover = measure("frame, user zoom + hover");
    // Zoomed in with arrows and labels.
    GraphCanvas::RenderOptions o = c->renderOptions();
    o.showArrow = true;
    c->setRenderOptions(o);
    c->setScaleImmediate(1.5);
    const double zoomed = measure("frame, zoom 1.5 + arrows");
    if (assertPerf()) {
        EXPECT_LT(overview, 8.0);
        EXPECT_LT(hover, 8.0);
        EXPECT_LT(zoomed, 8.0);
    }
}
