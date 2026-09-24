// d3-force port (ForceSimulation), quadtree, and the threaded SimulationRunner.
#include "graph/force_simulation.h"
#include "graph/quadtree.h"
#include "graph/simulation_runner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <random>

using namespace oc::graph;

namespace {

// Deterministic random graph: n nodes, ~m links with a preferential (Zipf-like) target choice.
std::vector<ForceSimulation::Link> randomLinks(int n, int m, unsigned seed)
{
    std::mt19937 rng(seed);
    std::vector<ForceSimulation::Link> links;
    for (int k = 0; k < m; ++k) {
        const int s = int(rng() % unsigned(n));
        const double u = std::uniform_real_distribution<double>(0, 1)(rng);
        const int t = int(std::pow(u, 2.5) * n) % n;
        if (s != t)
            links.push_back({s, t});
    }
    return links;
}

std::shared_ptr<GraphData> randomGraph(int n, int m, unsigned seed, const QString& prefix = "n")
{
    auto g = std::make_shared<GraphData>();
    for (int i = 0; i < n; ++i)
        g->addNode(prefix + QString::number(i) + ".md");
    for (const auto& l : randomLinks(n, m, seed))
        g->addEdge(l.source, l.target);
    g->finalize();
    return g;
}

double minPairDistance(const ForceSimulation& sim)
{
    double best = INFINITY;
    for (int i = 0; i < sim.nodeCount(); ++i)
        for (int j = i + 1; j < sim.nodeCount(); ++j)
            best = std::min(best, std::hypot(sim.x()[size_t(i)] - sim.x()[size_t(j)], sim.y()[size_t(i)] - sim.y()[size_t(j)]));
    return best;
}

} // namespace

TEST(ForceParams, ObsidianMappings)
{
    ForceParams p;
    EXPECT_NEAR(p.d3CenterStrength(), 0.1, 1e-12); // 0.518713248970312 is unmap(0.1)
    EXPECT_NEAR(ForceParams::unmapSlider(0.1), 0.518713248970312, 1e-12);
    EXPECT_NEAR(ForceParams::mapSlider(0), 0, 1e-15);
    EXPECT_NEAR(ForceParams::mapSlider(1), 1, 1e-15);
    EXPECT_EQ(p.d3ChargeStrength(), -1000);
    p.repelStrength = 0.5;
    EXPECT_EQ(p.d3ChargeStrength(), -1);
    p.repelStrength = 20;
    EXPECT_EQ(p.d3ChargeStrength(), -8000);
}

TEST(ForceSimulation, PhyllotaxisAndLcgMatchD3)
{
    ForceSimulation sim;
    sim.setGraph(3, {});
    const double angle = std::numbers::pi * (3 - std::sqrt(5.0));
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(sim.x()[size_t(i)], 10 * std::sqrt(0.5 + i) * std::cos(i * angle));
        EXPECT_DOUBLE_EQ(sim.y()[size_t(i)], 10 * std::sqrt(0.5 + i) * std::sin(i * angle));
    }
    // d3's lcg(): s = (1664525 s + 1013904223) mod 2^32 starting at s = 1.
    EXPECT_DOUBLE_EQ(sim.jiggle(), (1015568748.0 / 4294967296.0 - 0.5) * 1e-6);
    EXPECT_DOUBLE_EQ(sim.jiggle(), (1586005467.0 / 4294967296.0 - 0.5) * 1e-6);
}

TEST(ForceSimulation, QuadtreeHoldsEveryPoint)
{
    std::mt19937 rng(7);
    std::vector<double> xs, ys;
    for (int i = 0; i < 500; ++i) {
        xs.push_back(std::uniform_real_distribution<double>(-1000, 1000)(rng));
        ys.push_back(std::uniform_real_distribution<double>(-300, 700)(rng));
    }
    xs.push_back(xs[3]); // coincident point
    ys.push_back(ys[3]);
    xs.push_back(NAN); // ignored
    ys.push_back(0);
    Quadtree t;
    t.build(xs.data(), ys.data(), int(xs.size()));
    std::vector<int> seen;
    std::vector<int> stack{t.root()};
    int chained = 0;
    while (!stack.empty()) {
        const int q = stack.back();
        stack.pop_back();
        const auto& n = t.node(q);
        if (n.isLeaf()) {
            for (int l = q; l >= 0; l = t.node(l).next)
                seen.push_back(t.node(l).data);
            chained += n.next >= 0;
        } else {
            for (int c : n.child)
                if (c >= 0)
                    stack.push_back(c);
        }
    }
    std::sort(seen.begin(), seen.end());
    ASSERT_EQ(seen.size(), 501u);
    for (int i = 0; i < 501; ++i)
        EXPECT_EQ(seen[size_t(i)], i);
    EXPECT_EQ(chained, 1);
    // Integer-aligned square extent covering all points.
    EXPECT_EQ(t.x1() - t.x0(), t.y1() - t.y0());
    EXPECT_EQ(t.x0(), std::floor(t.x0()));
}

TEST(ForceSimulation, DeterministicAndThreadIndependent)
{
    auto runOnce = [](int parallelThreshold) {
        SimulationConfig cfg;
        cfg.parallelThreshold = parallelThreshold;
        ForceSimulation sim(cfg);
        sim.setGraph(1200, randomLinks(1200, 3000, 42));
        for (int i = 0; i < 60; ++i)
            sim.tick();
        return std::make_pair(std::vector<double>(sim.x().begin(), sim.x().end()), std::vector<double>(sim.y().begin(), sim.y().end()));
    };
    const auto a = runOnce(0);    // serial
    const auto b = runOnce(0);    // serial again
    const auto c = runOnce(100);  // parallel many-body
    const auto d = runOnce(100);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c); // bitwise identical: per-node summation order does not depend on threads
    EXPECT_EQ(c, d);

    // The seed only drives jiggles (coincident nodes), never the initial placement.
    SimulationConfig other;
    other.seed = 99;
    ForceSimulation s1, s2(other);
    s1.setGraph(1200, randomLinks(1200, 3000, 42));
    s2.setGraph(1200, randomLinks(1200, 3000, 42));
    EXPECT_EQ(s1.x()[5], s2.x()[5]);
}

TEST(ForceSimulation, CoolsDownToAlphaMin)
{
    ForceSimulation sim;
    sim.setGraph(300, randomLinks(300, 700, 3));
    std::vector<double> energy;
    int ticks = 0;
    while (sim.isActive() && ticks < 1000) {
        sim.tick();
        energy.push_back(sim.kineticEnergy());
        ++ticks;
    }
    // alpha: 1 -> 0.001 with decay 1 - 0.001^(1/300) takes exactly 300 ticks.
    EXPECT_EQ(ticks, 300);
    EXPECT_LE(sim.alpha(), sim.config().alphaMin);
    const double peak = *std::max_element(energy.begin(), energy.end());
    double tail = 0;
    for (size_t i = energy.size() - 10; i < energy.size(); ++i)
        tail += energy[i];
    EXPECT_LT(tail / 10, peak * 1e-3);
    // Energy decreases overall once the initial explosion is over.
    const auto peakAt = size_t(std::max_element(energy.begin(), energy.end()) - energy.begin());
    EXPECT_LT(peakAt, 60u);
    EXPECT_LT(energy[150], energy[peakAt]);
    EXPECT_LT(energy[250], energy[150]);
    // Reheat keeps it running while alphaTarget holds it warm.
    sim.setAlpha(0.3);
    sim.setAlphaTarget(0.3);
    EXPECT_EQ(sim.run(400), 400);
    EXPECT_NEAR(sim.alpha(), 0.3, 1e-9);
}

TEST(ForceSimulation, BarnesHutMatchesBruteForce)
{
    ForceSimulation sim;
    std::mt19937 rng(11);
    const int n = 2000;
    std::vector<double> x(n), y(n);
    for (int i = 0; i < n; ++i) {
        // clustered layout: a few blobs, like a real graph
        const int c = i % 7;
        x[size_t(i)] = std::normal_distribution<double>(std::cos(c) * 3000, 600)(rng);
        y[size_t(i)] = std::normal_distribution<double>(std::sin(c) * 3000, 600)(rng);
    }
    sim.setGraph(n, {}, x, y);
    auto forces = [&](bool exact) {
        for (int i = 0; i < n; ++i)
            sim.setVelocity(i, 0, 0);
        if (exact)
            sim.applyManyBodyExact(1.0);
        else
            sim.applyManyBody(1.0);
        return std::make_pair(std::vector<double>(sim.vx().begin(), sim.vx().end()), std::vector<double>(sim.vy().begin(), sim.vy().end()));
    };
    const auto exact = forces(true);
    const auto approx = forces(false);
    double err = 0, norm = 0, worst = 0;
    for (int i = 0; i < n; ++i) {
        const double dx = approx.first[size_t(i)] - exact.first[size_t(i)], dy = approx.second[size_t(i)] - exact.second[size_t(i)];
        const double mag = std::hypot(exact.first[size_t(i)], exact.second[size_t(i)]);
        err += dx * dx + dy * dy;
        norm += mag * mag;
        worst = std::max(worst, std::hypot(dx, dy) / std::max(mag, 1e-9));
    }
    const double rel = std::sqrt(err / norm);
    std::printf("[graph] Barnes-Hut θ=0.9 vs exact: RMS relative error %.4f, worst node %.3f\n", rel, worst);
    EXPECT_LT(rel, 0.03);

    // Tighter theta converges to the exact sum.
    SimulationConfig tight;
    tight.theta = 0.1;
    ForceSimulation sim2(tight);
    sim2.setGraph(n, {}, x, y);
    sim2.applyManyBody(1.0);
    double err2 = 0;
    for (int i = 0; i < n; ++i)
        err2 += std::pow(sim2.vx()[size_t(i)] - exact.first[size_t(i)], 2) + std::pow(sim2.vy()[size_t(i)] - exact.second[size_t(i)], 2);
    EXPECT_LT(std::sqrt(err2 / norm), 1e-3);
}

TEST(ForceSimulation, CollideKeepsNodesApart)
{
    auto settle = [](double radius) {
        SimulationConfig cfg;
        cfg.collideRadius = radius;
        ForceSimulation sim(cfg);
        ForceParams p;
        p.repelStrength = 0;    // charge −1: almost nothing keeps nodes apart but collide
        p.centerStrength = 0.8; // strong pull to the origin
        sim.setForces(p);
        sim.setGraph(40, {});
        sim.run();
        return minPairDistance(sim);
    };
    const double without = settle(0);
    const double with = settle(60);
    std::printf("[graph] collide: min distance without %.1f, with r=60 %.1f\n", without, with);
    EXPECT_LT(without, 30);
    EXPECT_GT(with, 100); // 2r = 120 minus what the centre force squeezes in
}

TEST(ForceSimulation, PinAndUnpin)
{
    ForceSimulation sim;
    sim.setGraph(50, randomLinks(50, 80, 5));
    sim.fix(7, 1234.5, -99.25);
    for (int i = 0; i < 50; ++i) {
        sim.tick();
        EXPECT_EQ(sim.x()[7], 1234.5);
        EXPECT_EQ(sim.y()[7], -99.25);
        EXPECT_EQ(sim.vx()[7], 0);
    }
    EXPECT_TRUE(sim.isFixed(7));
    sim.unfix(7);
    EXPECT_FALSE(sim.isFixed(7));
    sim.setAlpha(0.3);
    for (int i = 0; i < 30; ++i)
        sim.tick();
    EXPECT_NE(sim.x()[7], 1234.5); // pulled back towards its neighbours / the centre
    EXPECT_LT(std::abs(sim.x()[7]), 1234.5);
}

TEST(SimulationRunner, ConvergesAndPublishes)
{
    SimulationRunner runner;
    runner.setFrameRate(0); // unthrottled
    std::atomic<int> frames{0};
    QObject::connect(&runner, &SimulationRunner::frameReady, &runner, [&] { ++frames; }, Qt::DirectConnection);
    auto g = randomGraph(200, 400, 9);
    const quint64 v = runner.setGraph(g);
    ASSERT_TRUE(runner.waitForIdle(20000));
    EXPECT_TRUE(runner.acquire());
    const PositionSnapshot& s = runner.snapshot();
    EXPECT_EQ(s.graphVersion, v);
    EXPECT_EQ(s.graph, g);
    EXPECT_EQ(s.x.size(), 200u);
    EXPECT_FALSE(s.active);
    EXPECT_GE(s.tick, 300u);
    EXPECT_GE(frames.load(), 1);
    EXPECT_FALSE(runner.acquire()); // nothing newer
}

TEST(SimulationRunner, DeterministicWithQueuedCommands)
{
    auto layout = [] {
        SimulationRunner runner;
        runner.setFrameRate(0);
        runner.setPaused(true);
        runner.setGraph(randomGraph(300, 600, 21));
        ForceParams p;
        p.linkDistance = 120;
        runner.setForces(p);
        runner.setPaused(false);
        EXPECT_TRUE(runner.waitForIdle(20000));
        runner.acquire();
        return runner.snapshot().x;
    };
    EXPECT_EQ(layout(), layout());
}

TEST(SimulationRunner, ReplacementKeepsPositions)
{
    SimulationRunner runner;
    runner.setFrameRate(0);
    auto a = randomGraph(150, 300, 4);
    runner.setGraph(a);
    ASSERT_TRUE(runner.waitForIdle(20000));
    runner.acquire();
    const std::vector<double> ax = runner.snapshot().x, ay = runner.snapshot().y;

    // b: drop n0..n9, keep the rest (in a different order), add 20 new nodes linked to kept ones.
    auto b = std::make_shared<GraphData>();
    for (int i = 149; i >= 10; --i)
        b->addNode("n" + QString::number(i) + ".md");
    for (int i = 0; i < 20; ++i) {
        const int j = b->addNode("new" + QString::number(i) + ".md");
        b->addEdge(j, b->indexOf("n" + QString::number(20 + i) + ".md"));
    }
    b->finalize();
    runner.setPaused(true);
    const quint64 vb = runner.setGraph(b);
    ASSERT_TRUE(runner.waitForIdle(20000));
    ASSERT_TRUE(runner.acquire());
    const PositionSnapshot& s = runner.snapshot();
    ASSERT_EQ(s.graphVersion, vb);
    for (int i = 0; i < b->nodeCount(); ++i) {
        const QString id = b->node(i).id;
        if (id.startsWith("new")) {
            // Placed near the neighbour (Obsidian: mean of placed neighbours ± 30·√20).
            const int nb = a->indexOf(b->node(b->outgoing(i)[0]).id);
            EXPECT_LT(std::abs(s.x[size_t(i)] - ax[size_t(nb)]), 30 * std::sqrt(20.0) + 1e-9);
            EXPECT_LT(std::abs(s.y[size_t(i)] - ay[size_t(nb)]), 30 * std::sqrt(20.0) + 1e-9);
            continue;
        }
        const int old = a->indexOf(id);
        EXPECT_EQ(s.x[size_t(i)], ax[size_t(old)]) << id.toStdString();
        EXPECT_EQ(s.y[size_t(i)], ay[size_t(old)]) << id.toStdString();
    }
    runner.setPaused(false);
    ASSERT_TRUE(runner.waitForIdle(20000));
}

TEST(SimulationRunner, PinThroughCommands)
{
    SimulationRunner runner;
    runner.setFrameRate(0);
    runner.setGraph(randomGraph(80, 120, 8));
    ASSERT_TRUE(runner.waitForIdle(20000));
    runner.pin("n5.md", 500, 600);
    // alphaTarget 0.3 keeps it running; wait for a few ticks, then check the pinned position.
    const quint64 t0 = runner.ticks();
    while (runner.ticks() < t0 + 20)
        std::this_thread::yield();
    runner.acquire();
    EXPECT_EQ(runner.snapshot().x[5], 500);
    EXPECT_EQ(runner.snapshot().y[5], 600);
    EXPECT_TRUE(runner.snapshot().active);
    runner.unpin("n5.md");
    ASSERT_TRUE(runner.waitForIdle(20000));
    runner.acquire();
    EXPECT_FALSE(runner.snapshot().active);
    EXPECT_NE(runner.snapshot().x[5], 500);
}
