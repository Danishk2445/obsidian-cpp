#pragma once

// Port of d3-force (simulation, forceX/forceY, forceLink, forceManyBody, forceCollide) configured
// like Obsidian's graph worker: forces applied in the order x, y, link, charge, collide; velocity
// ×0.6 per tick; the worker keeps ticking while alpha > alphaMin.

#include "graph/quadtree.h"

#include <QtGlobal>

#include <cmath>
#include <span>
#include <vector>

namespace oc::graph {

// The four Forces sliders of the graph view, in graph.json units, and their d3 equivalents.
struct ForceParams {
    double centerStrength = 0.518713248970312; // slider 0..1 (default maps to d3 strength 0.1)
    double repelStrength = 10;                 // slider 0..20, charge = -(v^3)
    double linkStrength = 1;                   // slider 0..1
    double linkDistance = 250;                 // slider 30..500

    // Obsidian's slider mapping (0.01^(1-v) - 0.01) / 0.99 and its inverse.
    static double mapSlider(double v) { return (std::pow(0.01, 1 - v) - 0.01) / 0.99; }
    static double unmapSlider(double s) { return 1 - std::log(s * 0.99 + 0.01) / std::log(0.01); }

    double d3CenterStrength() const { return mapSlider(centerStrength); }
    // The worker uses -(repel^3), but never weaker than -1.
    double d3ChargeStrength() const
    {
        const double b = repelStrength * repelStrength * repelStrength;
        return std::abs(b) < 1 ? -1.0 : -b;
    }
    double d3LinkStrengthScale() const { return mapSlider(linkStrength); } // × 1/min(count)
    double d3LinkDistance() const { return linkDistance; }

    friend bool operator==(const ForceParams&, const ForceParams&) = default;
};

struct SimulationConfig {
    double alphaMin = 0.001;
    double alphaDecay = 1 - std::pow(0.001, 1.0 / 300); // d3 default
    double velocityDecay = 0.4;                          // v *= 0.6
    double theta = 0.9;                                  // Barnes-Hut
    double distanceMin = 30;                             // many-body (d3 squares it: 900)
    double distanceMax = INFINITY;
    double collideRadius = 60;
    double collideStrength = 0.5;
    int collideIterations = 1;
    int linkIterations = 1;
    quint32 seed = 1;           // d3's LCG starts at 1
    int parallelThreshold = 3000; // many-body runs on the thread pool above this node count (<=0: never)
};

class ForceSimulation {
public:
    struct Link {
        int source = -1;
        int target = -1;
    };

    explicit ForceSimulation(const SimulationConfig& config = {});

    const SimulationConfig& config() const { return m_cfg; }
    void setParallelThreshold(int n) { m_cfg.parallelThreshold = n; }

    // Replaces nodes and links. Positions (optional) may contain NaN: such nodes get d3's
    // phyllotaxis placement (radius 10·sqrt(0.5 + i), angle i·π(3 − √5)); missing or NaN
    // velocities start at 0; fixed coordinates (fx/fy, NaN = free) override positions.
    void setGraph(int nodeCount, std::vector<Link> links, std::vector<double> x = {}, std::vector<double> y = {},
                  std::vector<double> vx = {}, std::vector<double> vy = {}, std::vector<double> fx = {},
                  std::vector<double> fy = {});
    void setForces(const ForceParams& params);
    const ForceParams& forces() const { return m_params; }

    double alpha() const { return m_alpha; }
    void setAlpha(double a) { m_alpha = a; }
    double alphaTarget() const { return m_alphaTarget; }
    void setAlphaTarget(double t) { m_alphaTarget = t; }
    // Obsidian's worker loop condition.
    bool isActive() const { return m_alpha > m_cfg.alphaMin; }

    // d3 tick: alpha += (target − alpha)·decay; x, y, link, charge, collide; integrate.
    void tick(int iterations = 1);
    // Ticks while active, at most maxTicks; returns the number of ticks run.
    int run(int maxTicks = 1'000'000);
    quint64 tickCount() const { return m_ticks; }

    int nodeCount() const { return int(m_x.size()); }
    int linkCount() const { return int(m_links.size()); }
    const std::vector<Link>& links() const { return m_links; }
    std::span<const double> x() const { return m_x; }
    std::span<const double> y() const { return m_y; }
    std::span<const double> vx() const { return m_vx; }
    std::span<const double> vy() const { return m_vy; }
    std::span<const double> fx() const { return m_fx; }
    std::span<const double> fy() const { return m_fy; }
    void setPosition(int i, double x, double y);
    void setVelocity(int i, double vx, double vy);
    // Pin (drag): the node stays at (fx, fy) with zero velocity until unfixed.
    void fix(int i, double fx, double fy);
    void unfix(int i);
    bool isFixed(int i) const { return !std::isnan(m_fx[size_t(i)]) || !std::isnan(m_fy[size_t(i)]); }

    // Σ(vx² + vy²) after the last tick.
    double kineticEnergy() const;

    // ---- individual forces (add to vx/vy exactly like the d3 force functions) ----
    void applyCenter(double alpha);
    void applyLinks(double alpha);
    void applyManyBody(double alpha);      // Barnes-Hut (θ from config), parallel above the threshold
    void applyManyBodyExact(double alpha); // O(n²) reference with the same distance limits
    void applyCollide();
    void integrate();

    // d3's jiggle: (lcg() − 0.5)·1e-6 from the simulation's LCG (sequential forces only).
    double jiggle();

private:
    void initializeNodes();
    void initializeLinks();
    void manyBodyRange(int begin, int end, double alpha);
    double hashJiggle(int node, quint32 counter) const;

    SimulationConfig m_cfg;
    ForceParams m_params;
    double m_alpha = 1;
    double m_alphaTarget = 0;
    quint64 m_ticks = 0;
    quint32 m_lcg = 1;

    std::vector<double> m_x, m_y, m_vx, m_vy, m_fx, m_fy;
    std::vector<Link> m_links;
    std::vector<int> m_count;          // link count per node (d3 "count")
    std::vector<double> m_bias, m_linkStrength, m_distance;
    std::vector<double> m_charge;      // many-body strength per node
    std::vector<double> m_radius;      // collide radius per node

    // Many-body quadtree + per-quad aggregates.
    Quadtree m_tree;
    std::vector<double> m_qx, m_qy, m_qvalue;
    // Collide quadtree (built on x + vx) + per-quad max radius.
    Quadtree m_ctree;
    std::vector<double> m_cr;
    std::vector<double> m_px, m_py; // predicted positions for the collide tree
};

} // namespace oc::graph
