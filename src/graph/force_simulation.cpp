#include "graph/force_simulation.h"

#include "graph/parallel.h"

#include <algorithm>
#include <limits>
#include <numbers>
#include <utility>

namespace oc::graph {

namespace {
constexpr double kInitialRadius = 10;
const double kInitialAngle = std::numbers::pi * (3 - std::sqrt(5.0));

struct Frame {
    int q;
    double x0, y0, x1, y1;
};

inline quint64 splitmix64(quint64 z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Post-order over the tree (children before parents): fills `out` with quad indices.
void postOrder(const Quadtree& tree, std::vector<int>& out, std::vector<int>& stack)
{
    out.clear();
    stack.clear();
    if (tree.root() < 0)
        return;
    stack.push_back(tree.root());
    while (!stack.empty()) {
        const int q = stack.back();
        stack.pop_back();
        out.push_back(q);
        const Quadtree::Node& n = tree.node(q);
        if (!n.isLeaf())
            for (int c : n.child)
                if (c >= 0)
                    stack.push_back(c);
    }
    std::reverse(out.begin(), out.end());
}

inline void pushChildren(std::vector<Frame>& stack, const Quadtree::Node& n, const Frame& f)
{
    const double xm = (f.x0 + f.x1) / 2, ym = (f.y0 + f.y1) / 2;
    // Pushed 3..0 so that 0 is visited first (d3's quadtree.visit order).
    if (n.child[3] >= 0)
        stack.push_back({n.child[3], xm, ym, f.x1, f.y1});
    if (n.child[2] >= 0)
        stack.push_back({n.child[2], f.x0, ym, xm, f.y1});
    if (n.child[1] >= 0)
        stack.push_back({n.child[1], xm, f.y0, f.x1, ym});
    if (n.child[0] >= 0)
        stack.push_back({n.child[0], f.x0, f.y0, xm, ym});
}
} // namespace

ForceSimulation::ForceSimulation(const SimulationConfig& config)
    : m_cfg(config)
    , m_lcg(config.seed)
{
}

double ForceSimulation::jiggle()
{
    // d3's LCG: s = (1664525·s + 1013904223) mod 2^32; value s / 2^32.
    m_lcg = m_lcg * 1664525u + 1013904223u;
    return (double(m_lcg) / 4294967296.0 - 0.5) * 1e-6;
}

double ForceSimulation::hashJiggle(int node, quint32 counter) const
{
    // Counter-based, so parallel many-body passes stay deterministic regardless of scheduling.
    const quint64 key = (quint64(m_cfg.seed) << 32) ^ (m_ticks * 0xD1B54A32D192ED03ull) ^ (quint64(quint32(node)) << 8) ^ counter;
    const double u = double(splitmix64(key) >> 11) * (1.0 / 9007199254740992.0);
    return (u - 0.5) * 1e-6;
}

void ForceSimulation::setGraph(int nodeCount, std::vector<Link> links, std::vector<double> x, std::vector<double> y,
                               std::vector<double> vx, std::vector<double> vy, std::vector<double> fx, std::vector<double> fy)
{
    const size_t n = size_t(std::max(0, nodeCount));
    auto sized = [n](std::vector<double>& v) {
        if (v.size() != n)
            v.assign(n, NAN);
    };
    sized(x);
    sized(y);
    sized(vx);
    sized(vy);
    sized(fx);
    sized(fy);
    m_x = std::move(x);
    m_y = std::move(y);
    m_vx = std::move(vx);
    m_vy = std::move(vy);
    m_fx = std::move(fx);
    m_fy = std::move(fy);
    m_links.clear();
    m_links.reserve(links.size());
    for (const Link& l : links)
        if (l.source >= 0 && l.target >= 0 && size_t(l.source) < n && size_t(l.target) < n)
            m_links.push_back(l);
    initializeNodes();
    initializeLinks();
}

void ForceSimulation::initializeNodes()
{
    const int n = nodeCount();
    for (int i = 0; i < n; ++i) {
        if (!std::isnan(m_fx[size_t(i)]))
            m_x[size_t(i)] = m_fx[size_t(i)];
        if (!std::isnan(m_fy[size_t(i)]))
            m_y[size_t(i)] = m_fy[size_t(i)];
        if (std::isnan(m_x[size_t(i)]) || std::isnan(m_y[size_t(i)])) {
            const double radius = kInitialRadius * std::sqrt(0.5 + i), angle = i * kInitialAngle;
            m_x[size_t(i)] = radius * std::cos(angle);
            m_y[size_t(i)] = radius * std::sin(angle);
        }
        if (std::isnan(m_vx[size_t(i)]) || std::isnan(m_vy[size_t(i)]))
            m_vx[size_t(i)] = m_vy[size_t(i)] = 0;
    }
    m_charge.assign(size_t(n), m_params.d3ChargeStrength());
    m_radius.assign(size_t(n), m_cfg.collideRadius);
}

void ForceSimulation::initializeLinks()
{
    const size_t n = m_x.size(), m = m_links.size();
    m_count.assign(n, 0);
    for (const Link& l : m_links) {
        ++m_count[size_t(l.source)];
        ++m_count[size_t(l.target)];
    }
    m_bias.resize(m);
    m_linkStrength.resize(m);
    m_distance.resize(m);
    const double scale = m_params.d3LinkStrengthScale(), distance = m_params.d3LinkDistance();
    for (size_t i = 0; i < m; ++i) {
        const int cs = m_count[size_t(m_links[i].source)], ct = m_count[size_t(m_links[i].target)];
        m_bias[i] = double(cs) / double(cs + ct);
        m_linkStrength[i] = scale * (1.0 / double(std::min(cs, ct)));
        m_distance[i] = distance;
    }
}

void ForceSimulation::setForces(const ForceParams& params)
{
    m_params = params;
    m_charge.assign(m_x.size(), m_params.d3ChargeStrength());
    initializeLinks();
}

void ForceSimulation::setPosition(int i, double x, double y)
{
    m_x[size_t(i)] = x;
    m_y[size_t(i)] = y;
}

void ForceSimulation::setVelocity(int i, double vx, double vy)
{
    m_vx[size_t(i)] = vx;
    m_vy[size_t(i)] = vy;
}

void ForceSimulation::fix(int i, double fx, double fy)
{
    m_fx[size_t(i)] = fx;
    m_fy[size_t(i)] = fy;
}

void ForceSimulation::unfix(int i)
{
    m_fx[size_t(i)] = NAN;
    m_fy[size_t(i)] = NAN;
}

double ForceSimulation::kineticEnergy() const
{
    double e = 0;
    for (size_t i = 0; i < m_vx.size(); ++i)
        e += m_vx[i] * m_vx[i] + m_vy[i] * m_vy[i];
    return e;
}

void ForceSimulation::tick(int iterations)
{
    for (int k = 0; k < iterations; ++k) {
        m_alpha += (m_alphaTarget - m_alpha) * m_cfg.alphaDecay;
        applyCenter(m_alpha);
        applyLinks(m_alpha);
        applyManyBody(m_alpha);
        applyCollide();
        integrate();
        ++m_ticks;
    }
}

int ForceSimulation::run(int maxTicks)
{
    int t = 0;
    while (isActive() && t < maxTicks) {
        tick();
        ++t;
    }
    return t;
}

// forceX(0) and forceY(0) with Obsidian's mapped centre strength.
void ForceSimulation::applyCenter(double alpha)
{
    const double s = m_params.d3CenterStrength();
    const size_t n = m_x.size();
    for (size_t i = 0; i < n; ++i)
        m_vx[i] += (0 - m_x[i]) * s * alpha;
    for (size_t i = 0; i < n; ++i)
        m_vy[i] += (0 - m_y[i]) * s * alpha;
}

void ForceSimulation::applyLinks(double alpha)
{
    const size_t m = m_links.size();
    for (int k = 0; k < m_cfg.linkIterations; ++k) {
        for (size_t i = 0; i < m; ++i) {
            const size_t s = size_t(m_links[i].source), t = size_t(m_links[i].target);
            double x = m_x[t] + m_vx[t] - m_x[s] - m_vx[s];
            if (x == 0 || std::isnan(x))
                x = jiggle();
            double y = m_y[t] + m_vy[t] - m_y[s] - m_vy[s];
            if (y == 0 || std::isnan(y))
                y = jiggle();
            double l = std::sqrt(x * x + y * y);
            l = (l - m_distance[i]) / l * alpha * m_linkStrength[i];
            x *= l;
            y *= l;
            double b = m_bias[i];
            m_vx[t] -= x * b;
            m_vy[t] -= y * b;
            b = 1 - b;
            m_vx[s] += x * b;
            m_vy[s] += y * b;
        }
    }
}

void ForceSimulation::applyManyBody(double alpha)
{
    const int n = nodeCount();
    if (n == 0)
        return;
    m_tree.build(m_x.data(), m_y.data(), n);
    const size_t qn = size_t(m_tree.size());
    m_qx.resize(qn);
    m_qy.resize(qn);
    m_qvalue.resize(qn);

    // visitAfter(accumulate)
    std::vector<int> order, stack;
    postOrder(m_tree, order, stack);
    for (int q : order) {
        const Quadtree::Node& node = m_tree.node(q);
        double strength = 0;
        if (!node.isLeaf()) {
            double weight = 0, x = 0, y = 0;
            for (int c : node.child) {
                if (c < 0)
                    continue;
                const double cv = std::abs(m_qvalue[size_t(c)]);
                if (cv == 0 || std::isnan(cv))
                    continue;
                strength += m_qvalue[size_t(c)];
                weight += cv;
                x += cv * m_qx[size_t(c)];
                y += cv * m_qy[size_t(c)];
            }
            m_qx[size_t(q)] = x / weight;
            m_qy[size_t(q)] = y / weight;
        } else {
            m_qx[size_t(q)] = m_x[size_t(node.data)];
            m_qy[size_t(q)] = m_y[size_t(node.data)];
            for (int l = q; l >= 0; l = m_tree.node(l).next)
                strength += m_charge[size_t(m_tree.node(l).data)];
        }
        m_qvalue[size_t(q)] = strength;
    }

    if (m_cfg.parallelThreshold > 0 && n > m_cfg.parallelThreshold) {
        constexpr int kChunk = 128;
        std::vector<std::pair<int, int>> ranges;
        ranges.reserve(size_t(n / kChunk + 1));
        for (int b = 0; b < n; b += kChunk)
            ranges.emplace_back(b, std::min(n, b + kChunk));
        parallelFor(int(ranges.size()), [&](int k) { manyBodyRange(ranges[size_t(k)].first, ranges[size_t(k)].second, alpha); });
    } else {
        manyBodyRange(0, n, alpha);
    }
}

void ForceSimulation::manyBodyRange(int begin, int end, double alpha)
{
    const double theta2 = m_cfg.theta * m_cfg.theta;
    const double distMin2 = m_cfg.distanceMin * m_cfg.distanceMin;
    const double distMax2 = m_cfg.distanceMax * m_cfg.distanceMax;
    const int root = m_tree.root();
    if (root < 0)
        return;
    std::vector<Frame> stack;
    stack.reserve(256);
    for (int i = begin; i < end; ++i) {
        const double xi = m_x[size_t(i)], yi = m_y[size_t(i)];
        double vxi = m_vx[size_t(i)], vyi = m_vy[size_t(i)];
        quint32 jig = 0;
        stack.clear();
        stack.push_back({root, m_tree.x0(), m_tree.y0(), m_tree.x1(), m_tree.y1()});
        while (!stack.empty()) {
            const Frame f = stack.back();
            stack.pop_back();
            const double value = m_qvalue[size_t(f.q)];
            if (value == 0 || std::isnan(value))
                continue;
            double x = m_qx[size_t(f.q)] - xi, y = m_qy[size_t(f.q)] - yi;
            const double w = f.x1 - f.x0;
            double l = x * x + y * y;
            // Barnes-Hut approximation if the quad is far enough away.
            if (w * w / theta2 < l) {
                if (l < distMax2) {
                    if (x == 0) {
                        x = hashJiggle(i, jig++);
                        l += x * x;
                    }
                    if (y == 0) {
                        y = hashJiggle(i, jig++);
                        l += y * y;
                    }
                    if (l < distMin2)
                        l = std::sqrt(distMin2 * l);
                    vxi += x * value * alpha / l;
                    vyi += y * value * alpha / l;
                }
                continue;
            }
            const Quadtree::Node& node = m_tree.node(f.q);
            if (!node.isLeaf()) {
                pushChildren(stack, node, f);
                continue;
            }
            if (l >= distMax2)
                continue;
            // Individual points (limit forces for very close nodes, randomize coincident ones).
            if (node.data != i || node.next >= 0) {
                if (x == 0) {
                    x = hashJiggle(i, jig++);
                    l += x * x;
                }
                if (y == 0) {
                    y = hashJiggle(i, jig++);
                    l += y * y;
                }
                if (l < distMin2)
                    l = std::sqrt(distMin2 * l);
            }
            for (int lq = f.q; lq >= 0; lq = m_tree.node(lq).next) {
                const int d = m_tree.node(lq).data;
                if (d != i) {
                    const double wv = m_charge[size_t(d)] * alpha / l;
                    vxi += x * wv;
                    vyi += y * wv;
                }
            }
        }
        m_vx[size_t(i)] = vxi;
        m_vy[size_t(i)] = vyi;
    }
}

void ForceSimulation::applyManyBodyExact(double alpha)
{
    const int n = nodeCount();
    const double distMin2 = m_cfg.distanceMin * m_cfg.distanceMin;
    const double distMax2 = m_cfg.distanceMax * m_cfg.distanceMax;
    for (int i = 0; i < n; ++i) {
        const double xi = m_x[size_t(i)], yi = m_y[size_t(i)];
        double vxi = m_vx[size_t(i)], vyi = m_vy[size_t(i)];
        quint32 jig = 0;
        for (int j = 0; j < n; ++j) {
            if (j == i)
                continue;
            double x = m_x[size_t(j)] - xi, y = m_y[size_t(j)] - yi;
            double l = x * x + y * y;
            if (l >= distMax2)
                continue;
            if (x == 0) {
                x = hashJiggle(i, jig++);
                l += x * x;
            }
            if (y == 0) {
                y = hashJiggle(i, jig++);
                l += y * y;
            }
            if (l < distMin2)
                l = std::sqrt(distMin2 * l);
            const double w = m_charge[size_t(j)] * alpha / l;
            vxi += x * w;
            vyi += y * w;
        }
        m_vx[size_t(i)] = vxi;
        m_vy[size_t(i)] = vyi;
    }
}

void ForceSimulation::applyCollide()
{
    const int n = nodeCount();
    if (n == 0)
        return;
    const double strength = m_cfg.collideStrength;
    std::vector<int> order, istack;
    std::vector<Frame> stack;
    stack.reserve(256);
    for (int k = 0; k < m_cfg.collideIterations; ++k) {
        m_px.resize(size_t(n));
        m_py.resize(size_t(n));
        for (size_t i = 0; i < size_t(n); ++i) {
            m_px[i] = m_x[i] + m_vx[i];
            m_py[i] = m_y[i] + m_vy[i];
        }
        m_ctree.build(m_px.data(), m_py.data(), n);
        const int root = m_ctree.root();
        if (root < 0)
            return;
        // visitAfter(prepare): leaf radius = head point's radius; internal = max over children.
        m_cr.resize(size_t(m_ctree.size()));
        postOrder(m_ctree, order, istack);
        for (int q : order) {
            const Quadtree::Node& node = m_ctree.node(q);
            if (node.isLeaf()) {
                m_cr[size_t(q)] = m_radius[size_t(node.data)];
                continue;
            }
            double r = 0;
            for (int c : node.child)
                if (c >= 0 && m_cr[size_t(c)] > r)
                    r = m_cr[size_t(c)];
            m_cr[size_t(q)] = r;
        }

        for (int i = 0; i < n; ++i) {
            const double ri = m_radius[size_t(i)], ri2 = ri * ri;
            const double xi = m_x[size_t(i)] + m_vx[size_t(i)], yi = m_y[size_t(i)] + m_vy[size_t(i)];
            stack.clear();
            stack.push_back({root, m_ctree.x0(), m_ctree.y0(), m_ctree.x1(), m_ctree.y1()});
            while (!stack.empty()) {
                const Frame f = stack.back();
                stack.pop_back();
                const Quadtree::Node& node = m_ctree.node(f.q);
                double rj = m_cr[size_t(f.q)];
                double r = ri + rj;
                if (node.isLeaf()) {
                    const int d = node.data;
                    if (d > i) {
                        double x = xi - m_x[size_t(d)] - m_vx[size_t(d)], y = yi - m_y[size_t(d)] - m_vy[size_t(d)];
                        double l = x * x + y * y;
                        if (l < r * r) {
                            if (x == 0) {
                                x = jiggle();
                                l += x * x;
                            }
                            if (y == 0) {
                                y = jiggle();
                                l += y * y;
                            }
                            l = std::sqrt(l);
                            l = (r - l) / l * strength;
                            x *= l;
                            y *= l;
                            rj *= rj;
                            r = rj / (ri2 + rj);
                            m_vx[size_t(i)] += x * r;
                            m_vy[size_t(i)] += y * r;
                            r = 1 - r;
                            m_vx[size_t(d)] -= x * r;
                            m_vy[size_t(d)] -= y * r;
                        }
                    }
                    continue;
                }
                if (f.x0 > xi + r || f.x1 < xi - r || f.y0 > yi + r || f.y1 < yi - r)
                    continue;
                pushChildren(stack, node, f);
            }
        }
    }
}

void ForceSimulation::integrate()
{
    const double decay = 1 - m_cfg.velocityDecay;
    const size_t n = m_x.size();
    for (size_t i = 0; i < n; ++i) {
        if (std::isnan(m_fx[i])) {
            m_vx[i] *= decay;
            m_x[i] += m_vx[i];
        } else {
            m_x[i] = m_fx[i];
            m_vx[i] = 0;
        }
        if (std::isnan(m_fy[i])) {
            m_vy[i] *= decay;
            m_y[i] += m_vy[i];
        } else {
            m_y[i] = m_fy[i];
            m_vy[i] = 0;
        }
    }
}

} // namespace oc::graph
