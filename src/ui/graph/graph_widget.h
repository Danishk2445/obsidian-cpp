#pragma once

// A complete graph view body (Obsidian's GraphView / LocalGraphView content): the renderer
// (GraphCanvas) driven by a SimulationRunner, with the controls panel floating in the top-right
// corner. It owns Obsidian's "data engine" pipeline: full vault graph → GraphFilter (search,
// toggles, colour groups) → [local graph extraction] → simulation → renderer, plus the timelapse
// ("Animate", graph:animate).
//
// Queries (search box, colour groups) are compiled by a QueryCompiler; the default is the
// stand-in compileSimpleQuery(). GraphView plugs in the vault search (GraphVaultSearch).

#include "graph/force_simulation.h"
#include "graph/graph_data.h"
#include "graph/graph_settings.h"
#include "ui/graph/graph_controls.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

namespace oc::graph {
class SimulationRunner;
}

namespace oc {

class GraphCanvas;

class GraphWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Global, Local };
    using QueryCompiler = std::function<graph::NodePredicate(const QString& query)>;

    explicit GraphWidget(Mode mode = Mode::Global, QWidget* parent = nullptr, const graph::SimulationConfig& config = {});
    ~GraphWidget() override;

    Mode mode() const { return m_mode; }

    // The whole vault graph (finalized; weights are recomputed per view). Rebuilds the shown graph;
    // nodes that stay keep their positions.
    void setGraph(std::shared_ptr<const graph::GraphData> full);
    std::shared_ptr<const graph::GraphData> fullGraph() const { return m_full; }
    // What is on screen: the filtered (and, locally, extracted) graph handed to the simulation.
    std::shared_ptr<const graph::GraphData> shownGraph() const { return m_shown; }

    // The active file (drawn with --graph-node-focused).
    void setActiveNode(const QString& id);
    QString activeNode() const { return m_activeId; }
    // Local mode: the file the graph is centred on. Changing it recentres the view.
    void setLocalCenter(const QString& id);
    QString localCenter() const { return m_localCenter; }

    // Settings as graph.json / local view state: includes the current zoom target and panel state.
    graph::GraphSettings settings() const;
    // Applies everything (filters, groups, display, forces, panel). The zoom animates towards
    // settings.scale like Obsidian's setOptions(), unless animateZoom is false.
    void setSettings(const graph::GraphSettings& settings, bool animateZoom = true);

    void setQueryCompiler(QueryCompiler compiler);
    // Recompiles the queries and rebuilds (e.g. asynchronous search results changed).
    void refreshQueries();

    // Obsidian's renderProgression(): replays the graph in file creation order (global mode only).
    void startTimelapse();
    bool isTimelapseRunning() const { return m_progression > 0; }

    // Sidebar placement: --background-secondary behind the graph and the closed panel.
    void setInSidebar(bool inSidebar);

    GraphCanvas* canvas() const { return m_canvas; }
    GraphControls* controls() const { return m_controls; }
    graph::SimulationRunner* runner() const { return m_runner.get(); }

    // Headless: runs the simulation unthrottled and waits until the layout and all animations are at rest.
    void settle(int timeoutMs = 60000);

signals:
    void settingsChanged(); // debounced like Obsidian's onOptionsChange (100 ms; zoom after 2 s)
    void openRequested(const QString& id, bool newTab);
    void contextMenuRequested(const QString& id, const QPoint& globalPos);
    void rebuilt(); // the shown graph was recomputed
    void queriesChanged(); // search/group queries edited (before they are recompiled)

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void onControlsEdited(GraphControls::Changes changes);
    void compileQueries();
    void rebuild();
    void layoutControls();
    void applyDisplay();
    void scheduleSave();
    void timelapseTick();

    Mode m_mode;
    std::unique_ptr<graph::SimulationRunner> m_runner;
    GraphCanvas* m_canvas;
    GraphControls* m_controls;
    std::shared_ptr<const graph::GraphData> m_full;
    std::shared_ptr<const graph::GraphData> m_shown;
    QString m_activeId;
    QString m_localCenter;
    graph::GraphSettings m_settings;
    QueryCompiler m_compiler;
    graph::NodePredicate m_search;
    std::vector<graph::ColorGroup> m_groups;
    QTimer m_searchTimer; // requestUpdateSearch: 250 ms
    QTimer m_saveTimer;   // onOptionsChange: 100 ms
    QTimer m_scaleTimer;  // zoom persisted after it rests for 2 s
    QTimer m_timelapseTimer;
    QElapsedTimer m_timelapseClock;
    int m_progression = 0;      // > 0 while the timelapse runs
    int m_progressionTotal = 0; // steps of the complete graph
    double m_progressionSpeed = 20;
};

} // namespace oc
