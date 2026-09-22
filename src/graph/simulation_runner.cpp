#include "graph/simulation_runner.h"

#include <QHash>

#include <chrono>
#include <cmath>
#include <numbers>

namespace oc::graph {

namespace {

// Small deterministic generator for new-node placement (Obsidian uses Math.random()).
class PlacementRng {
public:
    explicit PlacementRng(quint64 seed)
        : m_state(seed)
    {
    }
    double next()
    {
        quint64 z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return double(z >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    quint64 m_state;
};

// Obsidian's GraphRenderer.setData placement for nodes that are new to the layout: the mean of
// already placed neighbours plus uniform jitter of ±30·√I, or a uniformly random point of the
// annulus of area 3600·I just outside the current layout (I = number of new nodes).
void placeNewNodes(const GraphData& g, std::vector<double>& x, std::vector<double>& y, std::vector<char>& placed, quint64 seed)
{
    const int n = g.nodeCount();
    double maxR2 = 0;
    int fresh = 0;
    for (int i = 0; i < n; ++i) {
        if (placed[size_t(i)])
            maxR2 = std::max(maxR2, x[size_t(i)] * x[size_t(i)] + y[size_t(i)] * y[size_t(i)]);
        else
            ++fresh;
    }
    if (fresh == 0)
        return;
    const double v = std::sqrt(maxR2);
    const double area = 60.0 * fresh * 60.0;
    const double ring = std::sqrt(area / std::numbers::pi + v * v) - v;
    const double jitter = std::sqrt(area);
    PlacementRng rng(seed);
    for (int i = 0; i < n; ++i) {
        if (placed[size_t(i)])
            continue;
        double sx = 0, sy = 0;
        int count = 0;
        auto acc = [&](std::span<const int> list) {
            for (int j : list) {
                if (!placed[size_t(j)])
                    continue;
                sx += x[size_t(j)];
                sy += y[size_t(j)];
                ++count;
            }
        };
        acc(g.outgoing(i)); // Obsidian's getRelated(): forward then reverse links
        acc(g.incoming(i));
        if (count > 0) {
            x[size_t(i)] = sx / count + (rng.next() - 0.5) * jitter;
            y[size_t(i)] = sy / count + (rng.next() - 0.5) * jitter;
        } else {
            const double angle = 2 * rng.next() * std::numbers::pi;
            const double r = v + std::sqrt(rng.next()) * ring;
            x[size_t(i)] = r * std::cos(angle);
            y[size_t(i)] = r * std::sin(angle);
        }
        placed[size_t(i)] = 1;
    }
}

} // namespace

SimulationRunner::SimulationRunner(const SimulationConfig& config, QObject* parent)
    : QObject(parent)
    , m_cfg(config)
{
    m_thread = std::thread([this] { threadMain(); });
}

SimulationRunner::~SimulationRunner()
{
    {
        std::lock_guard lk(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void SimulationRunner::setFrameRate(double hz)
{
    {
        std::lock_guard lk(m_mutex);
        m_frameRate = hz;
    }
    m_cv.notify_all();
}

void SimulationRunner::setPaused(bool paused)
{
    {
        std::lock_guard lk(m_mutex);
        m_paused = paused;
    }
    m_cv.notify_all();
}

void SimulationRunner::post(Command cmd)
{
    {
        std::lock_guard lk(m_mutex);
        m_queue.push_back(std::move(cmd));
        m_idle = false;
    }
    m_cv.notify_all();
}

quint64 SimulationRunner::setGraph(std::shared_ptr<const GraphData> graph, Placement placement)
{
    quint64 version;
    {
        std::lock_guard lk(m_mutex);
        version = ++m_nextVersion;
    }
    post(SetGraphCmd{std::move(graph), version, placement});
    return version;
}

void SimulationRunner::setForces(const ForceParams& params) { post(SetForcesCmd{params}); }
void SimulationRunner::pin(const QString& id, double x, double y) { post(PinCmd{id, x, y}); }
void SimulationRunner::unpin(const QString& id) { post(UnpinCmd{id}); }
void SimulationRunner::reheat(double alpha) { post(ReheatCmd{alpha}); }
void SimulationRunner::setAlphaTarget(double target) { post(AlphaTargetCmd{target}); }

bool SimulationRunner::acquire()
{
    // Clear the pending flag first: a publish racing with us re-notifies instead of being lost.
    m_notifyPending.store(false, std::memory_order_release);
    if (!(m_middle.load(std::memory_order_acquire) & kFresh))
        return false;
    const int prev = m_middle.exchange(m_front, std::memory_order_acq_rel);
    m_front = prev & ~kFresh;
    return true;
}

bool SimulationRunner::waitForIdle(int timeoutMs)
{
    std::unique_lock lk(m_mutex);
    return m_idleCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return (m_idle && m_queue.empty()) || m_stop; });
}

void SimulationRunner::publish(const ForceSimulation& sim, const std::shared_ptr<const GraphData>& graph, quint64 version)
{
    PositionSnapshot& s = m_buffers[size_t(m_back)];
    s.graph = graph;
    s.graphVersion = version;
    s.tick = sim.tickCount();
    s.alpha = sim.alpha();
    s.active = sim.isActive();
    s.x.assign(sim.x().begin(), sim.x().end());
    s.y.assign(sim.y().begin(), sim.y().end());
    const int prev = m_middle.exchange(m_back | kFresh, std::memory_order_acq_rel);
    m_back = prev & ~kFresh;
    if (!m_notifyPending.exchange(true, std::memory_order_acq_rel))
        emit frameReady();
}

void SimulationRunner::threadMain()
{
    using Clock = std::chrono::steady_clock;
    ForceSimulation sim(m_cfg);
    std::shared_ptr<const GraphData> graph;
    quint64 version = 0;
    QHash<QString, int> index; // id -> node index of the current graph
    std::vector<Command> commands;
    auto next = Clock::now();

    auto applyGraph = [&](SetGraphCmd& c) {
        const GraphData empty;
        const GraphData& g = c.graph ? *c.graph : empty;
        const int n = g.nodeCount();
        std::vector<double> x(size_t(n), NAN), y(size_t(n), NAN), vx(size_t(n), NAN), vy(size_t(n), NAN);
        std::vector<double> fx(size_t(n), NAN), fy(size_t(n), NAN);
        std::vector<char> placed(size_t(n), 0);
        for (int i = 0; i < n; ++i) {
            const int old = index.value(g.node(i).id, -1);
            if (old < 0)
                continue;
            x[size_t(i)] = sim.x()[size_t(old)];
            y[size_t(i)] = sim.y()[size_t(old)];
            vx[size_t(i)] = sim.vx()[size_t(old)];
            vy[size_t(i)] = sim.vy()[size_t(old)];
            fx[size_t(i)] = sim.fx()[size_t(old)];
            fy[size_t(i)] = sim.fy()[size_t(old)];
            placed[size_t(i)] = 1;
        }
        if (c.placement == Placement::Obsidian && g.isFinalized())
            placeNewNodes(g, x, y, placed, (quint64(m_cfg.seed) << 32) ^ c.version);
        std::vector<ForceSimulation::Link> links;
        links.reserve(size_t(g.edgeCount()));
        for (const GraphEdge& e : g.edges())
            links.push_back({e.source, e.target});
        sim.setGraph(n, std::move(links), std::move(x), std::move(y), std::move(vx), std::move(vy), std::move(fx), std::move(fy));
        index.clear();
        index.reserve(n);
        for (int i = 0; i < n; ++i)
            index.insert(g.node(i).id, i);
        graph = std::move(c.graph);
        version = c.version;
        sim.setAlpha(std::max(sim.alpha(), 0.3));
    };

    // Like the worker, an empty graph never ticks (and keeps its alpha for the first real graph).
    auto hot = [&](bool paused) { return !paused && sim.nodeCount() > 0 && sim.isActive(); };

    for (;;) {
        double frameRate;
        bool paused;
        {
            std::unique_lock lk(m_mutex);
            if (m_queue.empty() && !m_stop && !hot(m_paused)) {
                m_idle = true;
                m_idleCv.notify_all();
                m_cv.wait(lk, [&] { return m_stop || !m_queue.empty() || hot(m_paused); });
                m_idle = false;
                next = Clock::now();
            }
            if (m_stop)
                break;
            commands.swap(m_queue);
            frameRate = m_frameRate;
            paused = m_paused;
        }

        bool changed = false;
        for (Command& cmd : commands) {
            if (auto* c = std::get_if<SetGraphCmd>(&cmd)) {
                applyGraph(*c);
                changed = true;
            } else if (auto* c = std::get_if<SetForcesCmd>(&cmd)) {
                sim.setForces(c->params);
                sim.setAlpha(std::max(sim.alpha(), 0.3));
            } else if (auto* c = std::get_if<PinCmd>(&cmd)) {
                const int i = index.value(c->id, -1);
                if (i >= 0) {
                    sim.fix(i, c->x, c->y);
                    sim.setAlpha(std::max(sim.alpha(), 0.3));
                    sim.setAlphaTarget(0.3);
                }
            } else if (auto* c = std::get_if<UnpinCmd>(&cmd)) {
                const int i = index.value(c->id, -1);
                if (i >= 0)
                    sim.unfix(i);
                sim.setAlphaTarget(0);
            } else if (auto* c = std::get_if<ReheatCmd>(&cmd)) {
                sim.setAlpha(std::max(sim.alpha(), c->alpha));
            } else if (auto* c = std::get_if<AlphaTargetCmd>(&cmd)) {
                sim.setAlphaTarget(c->target);
            }
        }
        commands.clear();

        if (changed)
            publish(sim, graph, version); // new topology visible before the first tick
        if (hot(paused)) {
            sim.tick();
            m_ticks.store(sim.tickCount(), std::memory_order_relaxed);
            publish(sim, graph, version);
        }

        if (frameRate > 0) {
            const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / frameRate));
            next += interval;
            const auto now = Clock::now();
            if (next < now - interval)
                next = now; // fell behind (slow tick): don't burst
            std::unique_lock lk(m_mutex);
            m_cv.wait_until(lk, next, [&] { return m_stop; });
        }
    }
}

} // namespace oc::graph
