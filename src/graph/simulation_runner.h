#pragma once

// Runs a ForceSimulation on its own thread, like Obsidian's graph worker: ticks at 60 Hz while
// alpha > alphaMin and sleeps otherwise. The UI talks to it through a command queue and reads
// positions from a lock-free triple buffer (never blocks the GUI thread).

#include "graph/force_simulation.h"
#include "graph/graph_data.h"

#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace oc::graph {

// Positions published by the simulation thread for one graph version.
struct PositionSnapshot {
    std::shared_ptr<const GraphData> graph; // the graph these positions index into
    quint64 graphVersion = 0;               // setGraph() call that produced `graph`
    quint64 tick = 0;                       // simulation ticks so far
    double alpha = 0;
    bool active = false; // still cooling down (alpha > alphaMin)
    std::vector<double> x, y;
};

class SimulationRunner : public QObject {
    Q_OBJECT
public:
    // How nodes without a previous position are placed when a graph is (re)set.
    enum class Placement {
        Obsidian,    // next to already placed neighbours, else on a ring outside the layout (seeded)
        Phyllotaxis, // d3's default spiral by index
    };

    explicit SimulationRunner(const SimulationConfig& config = {}, QObject* parent = nullptr);
    ~SimulationRunner() override;

    // Tick rate while active (60 Hz like the worker). <= 0: unthrottled (tests, headless layout).
    void setFrameRate(double hz);
    // Paused: commands still apply and publish snapshots, but no ticks run (hidden views).
    void setPaused(bool paused);

    // ---- commands (thread-safe, non-blocking, applied in order before the next tick) ----
    // Replaces the graph; nodes whose id persists keep position, velocity and pin. Reheats alpha
    // to >= 0.3 (the first graph starts at alpha 1). Returns the new graph version.
    quint64 setGraph(std::shared_ptr<const GraphData> graph, Placement placement = Placement::Obsidian);
    void setForces(const ForceParams& params); // + reheat 0.3
    void pin(const QString& id, double x, double y); // drag: fx/fy, alpha >= 0.3, alphaTarget 0.3
    void unpin(const QString& id);                   // drag end: free again, alphaTarget 0
    void reheat(double alpha = 0.3);                  // alpha = max(alpha, value)
    void setAlphaTarget(double target);

    // ---- positions (single consumer, normally the GUI thread) ----
    // Swaps in the newest published snapshot; false when nothing new was published.
    bool acquire();
    const PositionSnapshot& snapshot() const { return m_buffers[size_t(m_front)]; }

    // Blocks until the queue is drained and the simulation is cold (or paused). Tests/headless only.
    bool waitForIdle(int timeoutMs);
    quint64 ticks() const { return m_ticks.load(std::memory_order_relaxed); }

signals:
    // Emitted from the simulation thread when a snapshot is published (at most one pending).
    void frameReady();

private:
    struct SetGraphCmd {
        std::shared_ptr<const GraphData> graph;
        quint64 version;
        Placement placement;
    };
    struct SetForcesCmd {
        ForceParams params;
    };
    struct PinCmd {
        QString id;
        double x, y;
    };
    struct UnpinCmd {
        QString id;
    };
    struct ReheatCmd {
        double alpha;
    };
    struct AlphaTargetCmd {
        double target;
    };
    using Command = std::variant<SetGraphCmd, SetForcesCmd, PinCmd, UnpinCmd, ReheatCmd, AlphaTargetCmd>;

    void post(Command cmd);
    void threadMain();
    void publish(const ForceSimulation& sim, const std::shared_ptr<const GraphData>& graph, quint64 version);

    const SimulationConfig m_cfg;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;     // wakes the simulation thread
    std::condition_variable m_idleCv; // signals waitForIdle()
    std::vector<Command> m_queue;
    bool m_stop = false;
    bool m_idle = false;
    bool m_paused = false;
    double m_frameRate = 60;
    quint64 m_nextVersion = 0;

    // Triple buffer: the writer owns m_back, the reader m_front, m_middle holds the latest
    // published buffer (kFresh set until the reader takes it).
    static constexpr int kFresh = 4;
    std::array<PositionSnapshot, 3> m_buffers;
    std::atomic<int> m_middle{1};
    int m_back = 0;  // simulation thread
    int m_front = 2; // consumer thread
    std::atomic<bool> m_notifyPending{false};
    std::atomic<quint64> m_ticks{0};
};

} // namespace oc::graph
