#include "ui/graph/graph_widget.h"

#include "graph/simple_query.h"
#include "graph/simulation_runner.h"
#include "ui/graph/graph_canvas.h"

#include <QResizeEvent>

#include <cmath>

namespace oc {

GraphWidget::GraphWidget(Mode mode, QWidget* parent, const graph::SimulationConfig& config)
    : QWidget(parent)
    , m_mode(mode)
    , m_runner(std::make_unique<graph::SimulationRunner>(config))
    , m_canvas(new GraphCanvas(this))
    , m_controls(new GraphControls(mode == Mode::Local ? GraphControls::Variant::Local : GraphControls::Variant::Global, this))
    , m_compiler(graph::compileSimpleQuery)
{
    m_canvas->setRunner(m_runner.get());
    m_controls->raise();
    if (m_mode == Mode::Local)
        m_settings.showOrphans = true;
    m_controls->setSettings(m_settings);

    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(250);
    connect(&m_searchTimer, &QTimer::timeout, this, [this] {
        compileQueries();
        rebuild();
    });
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(100);
    connect(&m_saveTimer, &QTimer::timeout, this, &GraphWidget::settingsChanged);
    m_scaleTimer.setSingleShot(true);
    m_scaleTimer.setInterval(2000);
    connect(&m_scaleTimer, &QTimer::timeout, this, [this] {
        if (m_canvas->targetScale() != m_settings.scale) {
            m_settings.scale = m_canvas->targetScale();
            emit settingsChanged();
        }
    });
    m_timelapseTimer.setInterval(16);
    connect(&m_timelapseTimer, &QTimer::timeout, this, &GraphWidget::timelapseTick);

    connect(m_controls, &GraphControls::edited, this, &GraphWidget::onControlsEdited);
    connect(m_controls, &GraphControls::sizeChanged, this, &GraphWidget::layoutControls);
    connect(m_controls, &GraphControls::animateRequested, this, &GraphWidget::startTimelapse);
    connect(m_canvas, &GraphCanvas::openRequested, this, &GraphWidget::openRequested);
    connect(m_canvas, &GraphCanvas::contextMenuRequested, this, &GraphWidget::contextMenuRequested);
    connect(m_canvas, &GraphCanvas::targetScaleChanged, this, [this] { m_scaleTimer.start(); });

    applyDisplay();
    m_runner->setForces(m_settings.forces());
    compileQueries();
    rebuild();
}

GraphWidget::~GraphWidget()
{
    // The canvas holds a QPointer to the runner; stop the simulation thread first.
    m_canvas->setRunner(nullptr);
    m_runner.reset();
}

void GraphWidget::setGraph(std::shared_ptr<const graph::GraphData> full)
{
    m_full = std::move(full);
    rebuild();
}

void GraphWidget::setActiveNode(const QString& id)
{
    m_activeId = id;
    m_canvas->setFocusedNode(id);
}

void GraphWidget::setLocalCenter(const QString& id)
{
    if (id == m_localCenter)
        return;
    m_localCenter = id;
    // Obsidian's local update(): a different file recentres the view and drops the hover.
    m_canvas->resetPan();
    m_canvas->setHoveredNode(QString());
    rebuild();
}

graph::GraphSettings GraphWidget::settings() const
{
    graph::GraphSettings s = m_settings;
    s.scale = m_canvas->targetScale();
    s.close = m_controls->isClosed();
    return s;
}

void GraphWidget::setSettings(const graph::GraphSettings& settings, bool animateZoom)
{
    m_settings = settings;
    if (m_mode == Mode::Local)
        m_settings.showOrphans = true; // the local graph has no Orphans filter
    m_controls->setSettings(m_settings);
    applyDisplay();
    m_runner->setForces(m_settings.forces());
    if (m_settings.scale > 0) {
        const QSignalBlocker block(m_canvas); // restoring is not an edit
        if (animateZoom)
            m_canvas->zoomTo(m_settings.scale);
        else
            m_canvas->setScaleImmediate(m_settings.scale);
    }
    compileQueries();
    rebuild();
    layoutControls();
}

void GraphWidget::setQueryCompiler(QueryCompiler compiler)
{
    m_compiler = std::move(compiler);
    refreshQueries();
}

void GraphWidget::refreshQueries()
{
    compileQueries();
    rebuild();
}

void GraphWidget::setInSidebar(bool inSidebar)
{
    m_canvas->setBackgroundToken(inSidebar ? QStringLiteral("--background-secondary") : QStringLiteral("--background-primary"));
    m_controls->setInSidebar(inSidebar);
}

void GraphWidget::onControlsEdited(GraphControls::Changes changes)
{
    const double scale = m_settings.scale;
    m_settings = m_controls->settings();
    m_settings.scale = scale;
    using C = GraphControls::Change;
    if (changes & (C::Search | C::GroupQuery)) {
        emit queriesChanged();
        m_searchTimer.start(); // requestUpdateSearch: debounced
    }
    if (changes & (C::SearchSubmitted | C::Groups)) {
        m_searchTimer.stop();
        emit queriesChanged();
        compileQueries();
        rebuild();
    } else if (changes & C::Filters) {
        rebuild();
    }
    if (changes & C::Display)
        applyDisplay();
    if (changes & C::Forces)
        m_runner->setForces(m_settings.forces());
    if (changes & C::Layout)
        layoutControls();
    scheduleSave();
}

void GraphWidget::scheduleSave() { m_saveTimer.start(); }

void GraphWidget::applyDisplay()
{
    GraphCanvas::RenderOptions o;
    o.showArrow = m_settings.showArrow;
    o.textFadeMultiplier = m_settings.textFadeMultiplier;
    o.nodeSizeMultiplier = m_settings.nodeSizeMultiplier;
    o.lineSizeMultiplier = m_settings.lineSizeMultiplier;
    m_canvas->setRenderOptions(o);
}

void GraphWidget::compileQueries()
{
    // Obsidian: an empty query has no matcher and is ignored (no filter, no colour).
    m_search = {};
    m_groups.clear();
    if (!m_compiler)
        return;
    if (!m_settings.search.trimmed().isEmpty())
        m_search = m_compiler(m_settings.search);
    for (const graph::ColorGroupSetting& g : std::as_const(m_settings.colorGroups)) {
        if (g.query.trimmed().isEmpty())
            continue;
        if (graph::NodePredicate p = m_compiler(g.query))
            m_groups.push_back({std::move(p), g.color});
    }
}

void GraphWidget::rebuild()
{
    auto shown = std::make_shared<graph::GraphData>();
    const bool local = m_mode == Mode::Local;
    if (m_full && m_full->isFinalized() && (!local || !m_localCenter.isEmpty())) {
        graph::GraphFilter f;
        f.search = m_search;
        f.showTags = m_settings.showTags;
        f.showAttachments = m_settings.showAttachments;
        f.hideUnresolved = m_settings.hideUnresolved;
        f.showOrphans = local || m_settings.showOrphans;
        f.colorGroups = m_groups;
        if (local) {
            f.alwaysInclude = m_localCenter;
            graph::LocalGraphOptions o;
            o.depth = std::clamp(m_settings.localJumps, 1, 5);
            o.incoming = m_settings.localBacklinks;
            o.outgoing = m_settings.localForelinks;
            o.neighborLinks = m_settings.localInterlinks;
            *shown = graph::extractLocalGraph(f.apply(*m_full), m_localCenter, o);
        } else if (m_progression > 0) {
            *shown = f.applyProgression(*m_full, m_progression, &m_progressionTotal);
        } else {
            *shown = f.apply(*m_full);
        }
    } else {
        shown->finalize();
    }
    m_shown = std::move(shown);
    m_runner->setGraph(m_shown);
    m_canvas->setFocusedNode(m_activeId);
    emit rebuilt();
}

void GraphWidget::startTimelapse()
{
    if (m_mode != Mode::Global || !m_full)
        return;
    m_progression = 1;
    rebuild();
    m_progressionSpeed = std::clamp(0.5 * std::sqrt(double(m_progressionTotal)), 5.0, 100.0);
    m_timelapseClock.start();
    m_timelapseTimer.start();
}

void GraphWidget::timelapseTick()
{
    const int next = 1 + int(std::floor(m_progressionSpeed * double(m_timelapseClock.elapsed()) / 1000.0));
    if (next == m_progression)
        return;
    m_progression = next;
    if (m_progression > m_progressionTotal) {
        m_progression = 0;
        m_timelapseTimer.stop();
    }
    rebuild();
}

void GraphWidget::settle(int timeoutMs)
{
    m_runner->setFrameRate(0);
    m_canvas->settle(timeoutMs);
    m_runner->setFrameRate(60);
}

void GraphWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutControls();
}

void GraphWidget::layoutControls()
{
    m_canvas->setGeometry(rect());
    const QSize hint = m_controls->sizeHint();
    int h = hint.height();
    if (!m_controls->isClosed())
        h = std::max(0, std::min(h, height() - 16)); // max-height: calc(100% - var(--size-4-4))
    // inset-inline-end / top: var(--size-4-3)
    m_controls->setGeometry(width() - 12 - hint.width(), 12, hint.width(), h);
    m_canvas->setPanelShadow(m_controls->isClosed() || m_controls->isHidden() ? QRectF() : QRectF(m_controls->geometry()),
                             m_controls->panelRadius());
}

} // namespace oc
