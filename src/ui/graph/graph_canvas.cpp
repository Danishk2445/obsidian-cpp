#include "ui/graph/graph_canvas.h"

#include "graph/simulation_runner.h"
#include "render/theme/theme.h"
#include "ui/graph/graph_raster.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oc {

namespace {

// Obsidian's t$(e, t, n = 0.9): e·n + t·(1 − n).
double lerp(double from, double to, double keep = 0.9) { return from * keep + to * (1 - keep); }

// Obsidian's n$: per-channel lerp of packed 0xRRGGBB colours, truncating like `<<` / `0|` do.
QRgb lerpRgb(QRgb from, QRgb to)
{
    auto ch = [](QRgb a, QRgb b, int shift) { return quint32(int(lerp(double((a >> shift) & 255), double((b >> shift) & 255)))) << shift; };
    return ch(from, to, 16) | ch(from, to, 8) | ch(from, to, 0);
}

bool intersects(const QRectF& a, double left, double top, double right, double bottom)
{
    return !(right < a.left() || left > a.right() || bottom < a.top() || top > a.bottom());
}

QColor withAlpha(QRgb rgb, double a)
{
    QColor c = QColor::fromRgb(rgb);
    c.setAlphaF(float(std::clamp(a, 0.0, 1.0)));
    return c;
}

} // namespace

GraphCanvas::GraphCanvas(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(1, 1);
    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        step();
        update();
        if (isIdle())
            m_timer.stop();
    });
    updateColors();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateColors();
            m_sprites.clear();
            m_labels.clear();
            m_shadow.clear();
            changed();
            update();
        });
    m_panClock.start();
}

GraphCanvas::~GraphCanvas() = default;

void GraphCanvas::setRunner(graph::SimulationRunner* runner)
{
    if (m_frameConn)
        disconnect(m_frameConn);
    m_runner = runner;
    if (runner)
        m_frameConn = connect(runner, &graph::SimulationRunner::frameReady, this, &GraphCanvas::onFrameReady, Qt::QueuedConnection);
    changed();
}

void GraphCanvas::setRenderOptions(const RenderOptions& options)
{
    const bool sizes = options.nodeSizeMultiplier != m_options.nodeSizeMultiplier;
    m_options = options;
    if (sizes)
        updateSizes();
    setScaleInternal(m_scale); // text alpha depends on the fade threshold
    changed();
}

void GraphCanvas::setFocusedNode(const QString& id)
{
    m_focusedId = id;
    m_focusedIndex = indexOf(id);
    changed();
}

void GraphCanvas::setBackgroundToken(const QString& token)
{
    m_backgroundToken = token;
    update();
}

void GraphCanvas::setPanelShadow(const QRectF& rect, double radius)
{
    if (rect == m_panelRect && radius == m_panelRadius)
        return;
    m_panelRect = rect;
    m_panelRadius = radius;
    update();
}

double GraphCanvas::dpr() const { return devicePixelRatioF(); }

// ---------------------------------------------------------------------------------------------
// Colours and sizes
// ---------------------------------------------------------------------------------------------
void GraphCanvas::updateColors()
{
    // Obsidian's testCSS(): the computed colour of ".graph-view.color-*" (alpha × CSS opacity).
    const ThemePtr t = currentTheme();
    auto rgba = [&](const char* token, double opacity = 1.0) {
        const QColor c = t ? t->color(QLatin1String(token), QColor(0x88, 0x88, 0x88)) : QColor(0x88, 0x88, 0x88);
        return Rgba{c.rgb() & 0xFFFFFFu, c.alphaF() * opacity};
    };
    m_colors.fill = rgba("--graph-node");
    m_colors.fillFocused = rgba("--graph-node-focused");
    m_colors.fillTag = rgba("--graph-node-tag");
    m_colors.fillUnresolved = rgba("--graph-node-unresolved", 0.5);
    m_colors.fillAttachment = rgba("--graph-node-attachment");
    m_colors.arrow = rgba("--text-normal", 0.5);
    m_colors.circle = rgba("--graph-node-focused");
    m_colors.line = rgba("--graph-line");
    m_colors.text = rgba("--graph-text");
    m_colors.fillHighlight = rgba("--interactive-accent");
    m_colors.lineHighlight = rgba("--interactive-accent");
    m_labelFamily = t ? t->interfaceFamily() : QString();
    m_shadowLayers = t ? BoxShadowCache::parse(t->raw(QStringLiteral("--shadow-s"))) : std::vector<BoxShadowCache::Layer>{};
}

void GraphCanvas::updateSizes()
{
    const int n = m_graph ? m_graph->nodeCount() : 0;
    m_size.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        m_size[size_t(i)] = m_options.nodeSizeMultiplier * std::max(8.0, std::min(3 * std::sqrt(m_graph->node(i).weight + 1), 30.0));
    m_gridDirty = true;
}

GraphCanvas::Rgba GraphCanvas::fillColor(int i) const
{
    if (i == highlightIndex())
        return m_colors.fillHighlight;
    if (i == m_focusedIndex) {
        if (m_colors.fillFocused.a > 0)
            return m_colors.fillFocused;
        return m_colors.fill;
    }
    const graph::GraphNode& n = m_graph->node(i);
    if (n.color)
        return {n.color->rgb & 0xFFFFFFu, n.color->a};
    switch (n.kind) {
    case graph::NodeKind::Tag:
        return m_colors.fillTag;
    case graph::NodeKind::Unresolved:
        return m_colors.fillUnresolved;
    case graph::NodeKind::Attachment:
        return m_colors.fillAttachment;
    case graph::NodeKind::File:
        break;
    }
    return m_colors.fill;
}

double GraphCanvas::nodeRadius(int index) const
{
    if (index < 0 || size_t(index) >= m_size.size())
        return 0;
    return m_size[size_t(index)] * m_nodeScale;
}

QPointF GraphCanvas::nodePosition(int index) const
{
    if (index < 0 || size_t(index) >= m_x.size())
        return {};
    return {m_x[size_t(index)], m_y[size_t(index)]};
}

QRectF GraphCanvas::layoutBounds() const
{
    if (m_x.empty())
        return {};
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t i = 0; i < m_x.size(); ++i) {
        x0 = std::min(x0, m_x[i]);
        x1 = std::max(x1, m_x[i]);
        y0 = std::min(y0, m_y[i]);
        y1 = std::max(y1, m_y[i]);
    }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

QString GraphCanvas::hoveredNode() const
{
    const int h = highlightIndex();
    return h >= 0 && m_graph ? m_graph->node(h).id : QString();
}

void GraphCanvas::setHoveredNode(const QString& id)
{
    setHoverIndex(id.isEmpty() ? -1 : indexOf(id));
}

void GraphCanvas::setHoverIndex(int i)
{
    if (i == m_hoverIndex)
        return;
    m_hoverIndex = i;
    changed();
    updateCursor();
    emit hoverChanged(i >= 0 && m_graph ? m_graph->node(i).id : QString());
}

// ---------------------------------------------------------------------------------------------
// View transform
// ---------------------------------------------------------------------------------------------
QPointF GraphCanvas::worldToWidget(const QPointF& world) const { return worldToDevice(world) / dpr(); }
QPointF GraphCanvas::widgetToWorld(const QPointF& pos) const { return deviceToWorld(toDevice(pos)); }

QRectF GraphCanvas::viewportWorld() const
{
    const double left = -m_panX / m_scale, top = -m_panY / m_scale;
    return QRectF(left, top, m_deviceSize.width() / m_scale, m_deviceSize.height() / m_scale);
}

void GraphCanvas::setPan(const QPointF& pan)
{
    m_panX = pan.x();
    m_panY = pan.y();
    m_panInitialized = true;
    changed();
}

void GraphCanvas::resetPan()
{
    m_deviceSize = (QSizeF(size()) * dpr()).toSize();
    setPan(QPointF(m_deviceSize.width() / 2.0, m_deviceSize.height() / 2.0));
}

void GraphCanvas::centerOn(const QPointF& world)
{
    m_deviceSize = (QSizeF(size()) * dpr()).toSize();
    setPan(QPointF(m_deviceSize.width() / 2.0 - world.x() * m_scale, m_deviceSize.height() / 2.0 - world.y() * m_scale));
}

void GraphCanvas::zoomTo(double scale, std::optional<QPointF> center)
{
    m_targetScale = std::clamp(scale, 1.0 / 128, 8.0);
    if (center) {
        m_zoomCenterX = center->x();
        m_zoomCenterY = center->y();
    } else {
        m_zoomCenterX = m_zoomCenterY = 0;
    }
    emit targetScaleChanged(m_targetScale);
    changed();
}

void GraphCanvas::setScaleImmediate(double scale)
{
    scale = std::clamp(scale, 1.0 / 128, 8.0);
    const double cx = m_deviceSize.width() / 2.0, cy = m_deviceSize.height() / 2.0;
    const double wx = (cx - m_panX) / m_scale, wy = (cy - m_panY) / m_scale;
    m_targetScale = scale;
    setScaleInternal(scale);
    m_panX = cx - wx * scale;
    m_panY = cy - wy * scale;
    emit targetScaleChanged(m_targetScale);
    changed();
}

void GraphCanvas::setScaleInternal(double scale)
{
    if (scale != m_scale)
        m_gridDirty = true;
    m_scale = scale;
    m_nodeScale = std::sqrt(1 / scale);
    m_textAlpha = std::clamp(std::log2(scale) + 1 - m_options.textFadeMultiplier, 0.0, 1.0);
}

void GraphCanvas::updateZoom()
{
    double t = m_scale;
    const double n = m_targetScale = std::clamp(m_targetScale, 1.0 / 128, 8.0);
    if ((t > n ? t / n : n / t) - 1 >= 0.01) {
        double ox = m_zoomCenterX, oy = m_zoomCenterY;
        if (ox == 0 && oy == 0) {
            ox = m_deviceSize.width() / 2.0;
            oy = m_deviceSize.height() / 2.0;
        }
        const double lx = (ox - m_panX) / t, ly = (oy - m_panY) / t;
        t = lerp(t, n, 0.85);
        m_panX = ox - lx * t;
        m_panY = oy - ly * t;
        m_idleFrames = 0;
    }
    setScaleInternal(t);
}

// ---------------------------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------------------------
void GraphCanvas::changed()
{
    m_idleFrames = 0;
    if (isVisible() && !m_timer.isActive())
        m_timer.start();
}

void GraphCanvas::onFrameReady() { changed(); }

bool GraphCanvas::pullSnapshot()
{
    if (!m_runner || !m_runner->acquire())
        return false;
    const graph::PositionSnapshot& s = m_runner->snapshot();
    if (s.graph != m_graph || s.graphVersion != m_graphVersion) {
        adoptGraph(s.graph);
        m_graphVersion = s.graphVersion;
        emit graphChanged();
    }
    const size_t n = m_graph ? size_t(m_graph->nodeCount()) : 0;
    if (s.x.size() == n && s.y.size() == n) {
        m_x = s.x;
        m_y = s.y;
    }
    if (m_dragIndex >= 0 && m_dragPinned && size_t(m_dragIndex) < m_x.size()) {
        m_x[size_t(m_dragIndex)] = m_dragWorld.x();
        m_y[size_t(m_dragIndex)] = m_dragWorld.y();
    }
    m_gridDirty = true;
    m_idleFrames = 0;
    return true;
}

void GraphCanvas::adoptGraph(const std::shared_ptr<const graph::GraphData>& g)
{
    const std::shared_ptr<const graph::GraphData> old = m_graph;
    const std::vector<NodeState> oldNodes = std::move(m_nodes);
    const std::vector<EdgeState> oldEdges = std::move(m_edges);
    const QString hover = m_hoverIndex >= 0 && old ? old->node(m_hoverIndex).id : QString();
    const QString drag = m_dragIndex >= 0 && old ? old->node(m_dragIndex).id : QString();

    m_graph = g;
    const int n = g ? g->nodeCount() : 0;
    m_nodes.assign(size_t(n), NodeState{});
    std::vector<int> oldIndex(size_t(n), -1);
    m_renderedCount = 0;
    for (int i = 0; i < n; ++i) {
        const int j = old ? old->indexOf(g->node(i).id) : -1;
        oldIndex[size_t(i)] = j;
        if (j >= 0 && size_t(j) < oldNodes.size())
            m_nodes[size_t(i)] = oldNodes[size_t(j)];
        if (m_nodes[size_t(i)].rendered)
            ++m_renderedCount;
    }

    QHash<quint64, int> oldEdgeIndex;
    if (old) {
        oldEdgeIndex.reserve(old->edgeCount());
        for (int k = 0; k < old->edgeCount(); ++k) {
            const graph::GraphEdge& e = old->edges()[size_t(k)];
            oldEdgeIndex.insert((quint64(quint32(e.source)) << 32) | quint32(e.target), k);
        }
    }
    const int m = g ? g->edgeCount() : 0;
    m_edges.assign(size_t(m), EdgeState{});
    for (int k = 0; k < m; ++k) {
        const graph::GraphEdge& e = g->edges()[size_t(k)];
        EdgeState& st = m_edges[size_t(k)];
        const int os = oldIndex[size_t(e.source)], ot = oldIndex[size_t(e.target)];
        if (os >= 0 && ot >= 0) {
            const int ok = oldEdgeIndex.value((quint64(quint32(os)) << 32) | quint32(ot), -1);
            if (ok >= 0 && size_t(ok) < oldEdges.size())
                st = oldEdges[size_t(ok)];
        }
        // A mutual link is drawn once: Obsidian skips o→a when a→o exists and o.id < a.id.
        st.drawLine = e.source != e.target
                      && !(g->hasEdge(e.target, e.source) && g->node(e.source).id.localeAwareCompare(g->node(e.target).id) < 0);
    }

    m_hoverIndex = hover.isEmpty() ? -1 : indexOf(hover);
    m_dragIndex = drag.isEmpty() ? -1 : indexOf(drag);
    if (m_dragIndex < 0)
        m_dragPinned = false;
    m_focusedIndex = indexOf(m_focusedId);
    m_neighborMarkFor = -2;
    m_x.assign(size_t(n), 0);
    m_y.assign(size_t(n), 0);
    updateSizes();
}

void GraphCanvas::revealNearest()
{
    // Obsidian initialises the graphics of at most 50 not yet rendered nodes per frame, nearest to
    // the view centre first.
    if (!m_graph || m_renderedCount >= m_graph->nodeCount())
        return;
    const QRectF vp = viewportWorld();
    const double cx = vp.center().x(), cy = vp.center().y();
    std::vector<std::pair<double, int>> pending;
    pending.reserve(size_t(m_graph->nodeCount() - m_renderedCount));
    for (int i = 0; i < m_graph->nodeCount(); ++i) {
        if (m_nodes[size_t(i)].rendered)
            continue;
        const double dx = m_x[size_t(i)] - cx, dy = m_y[size_t(i)] - cy;
        pending.emplace_back(dx * dx + dy * dy, i);
    }
    const size_t take = std::min<size_t>(50, pending.size());
    std::partial_sort(pending.begin(), pending.begin() + std::ptrdiff_t(take), pending.end());
    for (size_t k = 0; k < take; ++k) {
        NodeState& s = m_nodes[size_t(pending[k].second)];
        s.rendered = true;
        s.fade = 0;
        s.tint = fillColor(pending[k].second).rgb;
        s.textShown = false;
        ++m_renderedCount;
    }
    if (take > 0) {
        m_idleFrames = 0;
        m_gridDirty = true;
    }
}

void GraphCanvas::animateNodes()
{
    const int hl = highlightIndex();
    if (hl != m_neighborMarkFor) {
        m_neighborOfHighlight.assign(m_nodes.size(), 0);
        if (hl >= 0)
            for (int j : m_graph->neighbors(hl))
                m_neighborOfHighlight[size_t(j)] = 1;
        m_neighborMarkFor = hl;
    }
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        NodeState& s = m_nodes[i];
        if (!s.rendered)
            continue;
        const bool d = int(i) == hl;
        const double target = (hl < 0 || d || m_neighborOfHighlight[i]) ? 1.0 : 0.2;
        s.fade = lerp(s.fade, target);
        const double b = d ? 15 : 0;
        s.moveText = s.textShown ? lerp(s.moveText, b) : b;
        s.tint = lerpRgb(s.tint, fillColor(int(i)).rgb);
    }
}

void GraphCanvas::animateEdges()
{
    const int hl = highlightIndex();
    const double arrowScale = std::clamp(2 * (m_scale - 0.3), 0.0, 1.0);
    for (size_t k = 0; k < m_edges.size(); ++k) {
        EdgeState& s = m_edges[k];
        const graph::GraphEdge& e = m_graph->edges()[k];
        if (!s.rendered) {
            // Links get their graphics once both ends are rendered.
            if (!m_nodes[size_t(e.source)].rendered || !m_nodes[size_t(e.target)].rendered)
                continue;
            s.rendered = true;
            s.lineAlpha = 0.2 * m_colors.line.a;
            s.arrowAlpha = 0.2 * m_colors.text.a;
            s.tint = m_colors.line.rgb;
        }
        const bool incident = e.source == hl || e.target == hl;
        const double c = (hl < 0 || incident) ? 1.0 : 0.2;
        const Rgba& h = incident ? m_colors.lineHighlight : m_colors.line;
        s.lineAlpha = lerp(s.lineAlpha, c * h.a);
        s.arrowAlpha = lerp(s.arrowAlpha, c * arrowScale * m_colors.arrow.a);
        s.tint = lerpRgb(s.tint, h.rgb);
    }
}

void GraphCanvas::checkHover()
{
    // The hovered node may move away from under a still mouse (Obsidian's render-loop check).
    if (!m_mouseDevice || m_hoverIndex < 0 || !m_graph)
        return;
    const QPointF w = deviceToWorld(*m_mouseDevice);
    const double dx = m_x[size_t(m_hoverIndex)] - w.x(), dy = m_y[size_t(m_hoverIndex)] - w.y();
    const double r = m_size[size_t(m_hoverIndex)] * m_nodeScale + 2;
    if (dx * dx + dy * dy > r * r)
        setHoverIndex(-1);
}

void GraphCanvas::step()
{
    m_deviceSize = (QSizeF(size()) * dpr()).toSize();
    pullSnapshot();
    if (!m_panning) {
        m_panX += 1000 * m_panvX / 60;
        m_panY += 1000 * m_panvY / 60;
        int w = 0, k = 0;
        if (m_keys.up)
            k += 1;
        if (m_keys.down)
            k -= 1;
        if (m_keys.left)
            w += 1;
        if (m_keys.right)
            w -= 1;
        if (w != 0 || k != 0)
            m_idleFrames = 0;
        const double mult = m_keys.shift ? 3 : 1;
        m_panvX = lerp(m_panvX, w * mult);
        m_panvY = lerp(m_panvY, k * mult);
        if (std::abs(m_panvX) < 1e-9)
            m_panvX = 0;
        if (std::abs(m_panvY) < 1e-9)
            m_panvY = 0;
    }
    const double factor = 1 + (m_keys.shift ? 0.1 : 0.03);
    bool keyZoom = false;
    if (m_keys.zoomIn) {
        m_targetScale *= factor;
        keyZoom = true;
    }
    if (m_keys.zoomOut) {
        m_targetScale /= factor;
        keyZoom = true;
    }
    if (keyZoom) {
        m_idleFrames = 0;
        m_zoomCenterX = m_deviceSize.width() / 2.0;
        m_zoomCenterY = m_deviceSize.height() / 2.0;
        emit targetScaleChanged(std::clamp(m_targetScale, 1.0 / 128, 8.0));
    }
    updateZoom();
    if (m_graph) {
        revealNearest();
        animateNodes();
        animateEdges();
    }
    ++m_idleFrames;
    checkHover();
}

void GraphCanvas::settle(int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    if (m_runner)
        m_runner->waitForIdle(timeoutMs);
    for (int frame = 0; frame < 1'000'000 && clock.elapsed() < timeoutMs; ++frame) {
        step();
        const bool revealed = !m_graph || m_renderedCount >= m_graph->nodeCount();
        if (isIdle() && revealed)
            break;
    }
    update();
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------
void GraphCanvas::paintEvent(QPaintEvent*)
{
    // The frame is composed in an offscreen image (drawn straight into, on the thread pool) and
    // blitted 1:1 in device pixels, like Obsidian's stage.
    const double d = dpr();
    m_deviceSize = (QSizeF(size()) * d).toSize();
    if (m_frame.size() != m_deviceSize)
        m_frame = QImage(m_deviceSize, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter fp(&m_frame);
        render(fp, m_deviceSize, true);
    }
    QPainter p(this);
    p.scale(1 / d, 1 / d);
    p.drawImage(QPoint(0, 0), m_frame);
}

QImage GraphCanvas::grabFrame(bool transparent)
{
    const double d = dpr();
    m_deviceSize = (QSizeF(size()) * d).toSize();
    QImage img(m_deviceSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    render(p, m_deviceSize, !transparent);
    p.end();
    return img;
}

void GraphCanvas::render(QPainter& p, const QSize& deviceSize, bool background)
{
    const QSize savedSize = m_deviceSize;
    m_deviceSize = deviceSize;
    m_stats = {};
    p.save();
    if (background) {
        const ThemePtr t = currentTheme();
        const QColor bg = m_backgroundToken.isEmpty() || !t ? QColor(Qt::transparent) : t->color(m_backgroundToken, Qt::black);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(0, 0), deviceSize), bg);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    if (!m_graph || m_x.size() != size_t(m_graph->nodeCount())) {
        p.restore();
        m_deviceSize = savedSize;
        return;
    }
    // Raw pixel access when painting 1:1 into a 32-bit image (the widget's frame, tests, grabs):
    // links are rasterised directly, the rest drawn in parallel bands.
    QImage* direct = nullptr;
    if (p.device() && p.device()->devType() == QInternal::Image && p.deviceTransform().isIdentity() && !p.hasClipping()) {
        auto* img = static_cast<QImage*>(p.device());
        if (graph_raster::supportsDirect(*img) && img->size() == deviceSize)
            direct = img;
    }
    const QRectF vp = viewportWorld();
    const double s = m_scale, f = m_nodeScale;
    const double g = m_options.lineSizeMultiplier / s; // line width in world units
    const int hl = highlightIndex();
    p.setRenderHint(QPainter::Antialiasing);
    QElapsedTimer layerClock;
    layerClock.start();

    // ---- links (batched by colour/alpha; mutual links drawn once) ----
    std::vector<graph_raster::LineBatch> batches;
    const double lineWidth = m_options.lineSizeMultiplier; // device px
    const double widthAlpha = lineWidth < 1 ? lineWidth : 1; // hairlines: coverage as alpha
    const auto& edges = m_graph->edges();
    for (size_t k = 0; k < edges.size(); ++k) {
        const EdgeState& st = m_edges[k];
        if (!st.rendered || !st.drawLine)
            continue;
        const int a = int(std::lround(st.lineAlpha * widthAlpha * 255));
        if (a <= 0)
            continue;
        const graph::GraphEdge& e = edges[k];
        const double x0 = m_x[size_t(e.source)], y0 = m_y[size_t(e.source)], x1 = m_x[size_t(e.target)], y1 = m_y[size_t(e.target)];
        if (!intersects(vp, std::min(x0, x1) - g, std::min(y0, y1) - g, std::max(x0, x1) + g, std::max(y0, y1) + g))
            continue;
        const double dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        const double rs = m_size[size_t(e.source)] * f, rt = m_size[size_t(e.target)] * f;
        const double visible = len - rs - rt;
        if (!(visible > 0))
            continue;
        const double ux = dx / len, uy = dy / len;
        const QPointF a0((x0 + ux * rs) * s + m_panX, (y0 + uy * rs) * s + m_panY);
        const QPointF a1((x0 + ux * (rs + visible)) * s + m_panX, (y0 + uy * (rs + visible)) * s + m_panY);
        graph_raster::LineBatch* b = batches.empty() ? nullptr : &batches.back(); // runs of equal style are common
        if (!b || b->rgb != st.tint || b->alpha != a) {
            b = nullptr;
            for (graph_raster::LineBatch& cand : batches)
                if (cand.rgb == st.tint && cand.alpha == a) {
                    b = &cand;
                    break;
                }
        }
        if (!b) {
            batches.push_back({st.tint, a, {}});
            b = &batches.back();
        }
        b->lines.emplace_back(a0, a1);
        ++m_stats.links;
    }
    if (direct && lineWidth <= 1.25) {
        graph_raster::drawHairlines(*direct, batches);
    } else {
        for (const graph_raster::LineBatch& b : batches) {
            QPen pen(withAlpha(b.rgb, b.alpha / 255.0), std::max(1.0, lineWidth), Qt::SolidLine, Qt::FlatCap);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.drawLines(b.lines.data(), int(b.lines.size()));
        }
    }
    m_stats.linksNs = layerClock.nsecsElapsed();

    // ---- nodes (sprites resolved here, blitted below) ----
    struct NodeDraw {
        CircleSpriteCache::Blit blit;
        QPointF center;
        double r;
        QRgb rgb;
        double alpha;
    };
    std::vector<NodeDraw> nodeDraws;
    nodeDraws.reserve(m_nodes.size());
    m_sprites.beginFrame();
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const NodeState& st = m_nodes[i];
        if (!st.rendered)
            continue;
        const double r = m_size[i] * f;
        const double x = m_x[i], y = m_y[i];
        if (int(i) != hl && !intersects(vp, x - r - 1, y - r - 1, x + r + 1, y + r + 1))
            continue;
        const double alpha = std::min(1.0, st.fade * fillColor(int(i)).a);
        if (alpha <= 0.001)
            continue;
        const QPointF c(x * s + m_panX, y * s + m_panY);
        nodeDraws.push_back({m_sprites.blit(c.x(), c.y(), r * s, st.tint), c, r * s, st.tint, alpha});
        ++m_stats.nodes;
    }

    // ---- arrows ----
    struct Arrow {
        QPointF poly[4];
        int alpha;
    };
    std::vector<Arrow> arrows;
    if (m_options.showArrow) {
        const double arrowDevice = 2 * std::sqrt(m_options.lineSizeMultiplier); // shape scale in device px
        for (size_t k = 0; k < edges.size(); ++k) {
            const EdgeState& st = m_edges[k];
            if (!st.rendered || st.arrowAlpha <= 0.001)
                continue;
            const graph::GraphEdge& e = edges[k];
            if (e.source == e.target)
                continue;
            const double x0 = m_x[size_t(e.source)], y0 = m_y[size_t(e.source)], x1 = m_x[size_t(e.target)], y1 = m_y[size_t(e.target)];
            if (!intersects(vp, std::min(x0, x1) - g, std::min(y0, y1) - g, std::max(x0, x1) + g, std::max(y0, y1) + g))
                continue;
            const double dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
            if (!(len > g))
                continue;
            const double back = m_size[size_t(e.target)] * f + 1;
            const QPointF tip((x1 - dx * back / len) * s + m_panX, (y1 - dy * back / len) * s + m_panY);
            if (tip.x() < -16 || tip.y() < -16 || tip.x() > deviceSize.width() + 16 || tip.y() > deviceSize.height() + 16)
                continue;
            const double ux = dx / len, uy = dy / len;
            auto pt = [&](double lx, double ly) {
                return QPointF(tip.x() + (lx * ux - ly * uy) * arrowDevice, tip.y() + (lx * uy + ly * ux) * arrowDevice);
            };
            arrows.push_back({{pt(0, 0), pt(-4, -2), pt(-3, 0), pt(-4, 2)}, int(std::lround(std::min(1.0, st.arrowAlpha) * 255))});
            ++m_stats.arrows;
        }
    }

    // ---- labels (images rendered here, on the thread pool when many are missing) ----
    const qint64 beforeLabels = layerClock.nsecsElapsed();
    const std::vector<LabelDraw> labels = collectLabels(vp);
    m_stats.labelsNs = layerClock.nsecsElapsed() - beforeLabels;

    // ---- draw: circles, arrows, hover ring (z-index 1), then labels (z-index 2) ----
    const QColor arrowBase = QColor::fromRgb(m_colors.arrow.rgb);
    const bool ring = hl >= 0 && m_nodes[size_t(hl)].rendered;
    const double ringWidth = std::max(1.0, 1 / s / f) * f * s;
    const double ringRadius = ring ? (m_size[size_t(hl)] + std::max(1.0, 1 / s / f) / 2) * f * s : 0;
    const QPointF ringCenter = ring ? QPointF(m_x[size_t(hl)] * s + m_panX, m_y[size_t(hl)] * s + m_panY) : QPointF();
    auto drawLayers = [&](QPainter& q, double y0, double y1) {
        q.setPen(Qt::NoPen);
        for (const NodeDraw& n : nodeDraws) {
            if (n.center.y() + n.r + 2 < y0 || n.center.y() - n.r - 2 > y1)
                continue;
            q.setOpacity(n.alpha);
            if (n.blit.image) {
                q.drawImage(n.blit.pos, *n.blit.image);
            } else {
                q.setBrush(QColor::fromRgb(n.rgb));
                q.drawEllipse(n.center, n.r, n.r);
            }
        }
        q.setOpacity(1);
        for (const Arrow& a : arrows) {
            const auto [lo, hi] = std::minmax({a.poly[0].y(), a.poly[1].y(), a.poly[3].y()});
            if (hi + 1 < y0 || lo - 1 > y1)
                continue;
            QColor c = arrowBase;
            c.setAlpha(a.alpha);
            q.setBrush(c);
            q.drawPolygon(a.poly, 4);
        }
        if (ring && ringCenter.y() + ringRadius + ringWidth >= y0 && ringCenter.y() - ringRadius - ringWidth <= y1) {
            q.setPen(QPen(withAlpha(m_colors.circle.rgb, m_colors.circle.a), ringWidth));
            q.setBrush(Qt::NoBrush);
            q.drawEllipse(ringCenter, ringRadius, ringRadius);
            q.setPen(Qt::NoPen);
        }
        for (const LabelDraw& l : labels) {
            if (l.pos.y() + l.image->height() < y0 || l.pos.y() > y1)
                continue;
            q.setOpacity(l.alpha);
            q.drawImage(l.pos, *l.image);
        }
        q.setOpacity(1);
    };
    if (direct)
        graph_raster::paintInBands(*direct, [&](QPainter& bp, int y0, int y1) { drawLayers(bp, y0, y1); });
    else
        drawLayers(p, -INFINITY, INFINITY);
    m_stats.layersNs = layerClock.nsecsElapsed() - m_stats.linksNs - m_stats.labelsNs;
    m_stats.labels = int(labels.size());

    // ---- controls panel shadow ----
    if (!m_panelRect.isEmpty() && !m_shadowLayers.empty()) {
        const double d = dpr();
        std::vector<BoxShadowCache::Layer> layers = m_shadowLayers;
        for (BoxShadowCache::Layer& l : layers) {
            l.dx *= d;
            l.dy *= d;
            l.blur *= d;
            l.spread *= d;
        }
        const QRectF r(m_panelRect.topLeft() * d, m_panelRect.size() * d);
        p.setClipRect(QRectF(QPointF(0, 0), QSizeF(deviceSize)));
        m_shadow.paint(p, r, m_panelRadius * d, layers);
    }
    p.restore();
    m_deviceSize = savedSize;
}

std::vector<GraphCanvas::LabelDraw> GraphCanvas::collectLabels(const QRectF& vp)
{
    const int hl = highlightIndex();
    const double s = m_scale, f = m_nodeScale;
    struct Pending {
        int node;
        int px, wrap;
        double x, top, alpha;
    };
    std::vector<Pending> pending;
    std::vector<LabelCache::Request> requests;
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        NodeState& st = m_nodes[i];
        if (!st.rendered)
            continue;
        const bool d = int(i) == hl;
        double alpha = m_textAlpha * st.fade;
        if (d)
            alpha = 1;
        alpha *= m_colors.text.a;
        const double x = m_x[i], y = m_y[i];
        bool shown = alpha > 0.001;
        if (shown)
            shown = d || intersects(vp, x - 300, y, x + 300, y + 200);
        st.textShown = shown;
        if (!shown)
            continue;
        const double c = m_size[i];
        const double labelScale = (d && s < 1) ? 1 / s : f; // the hovered label keeps its size when zoomed out
        const int px = int(std::lround((14 + c / 4) * labelScale * s));
        if (px < 1)
            continue;
        const int wrap = int(std::lround(300 * labelScale * s));
        pending.push_back({int(i), px, wrap, x * s + m_panX, y * s + m_panY + (c + 5) * f * s + st.moveText, std::min(1.0, alpha)});
        const graph::GraphNode& node = m_graph->node(int(i));
        requests.push_back({node.id, node.label, px, wrap});
    }
    m_labels.setStyle(m_labelFamily, m_colors.text.rgb);
    m_labels.prepare(requests);
    std::vector<LabelDraw> out;
    out.reserve(pending.size());
    for (const Pending& l : pending) {
        const QImage* img = m_labels.find(m_graph->node(l.node).id, l.px, l.wrap);
        if (!img)
            continue;
        out.push_back({img, QPoint(int(std::lround(l.x - img->width() / 2.0)), int(std::lround(l.top - 1))), l.alpha});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------------------------
void GraphCanvas::rebuildGrid() const
{
    m_gridDirty = false;
    m_gridW = m_gridH = 0;
    m_gridStart.clear();
    m_gridItems.clear();
    if (!m_graph || m_x.empty())
        return;
    double maxR = 1, x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    int count = 0;
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (!m_nodes[i].rendered)
            continue;
        maxR = std::max(maxR, m_size[i] * m_nodeScale);
        x0 = std::min(x0, m_x[i]);
        x1 = std::max(x1, m_x[i]);
        y0 = std::min(y0, m_y[i]);
        y1 = std::max(y1, m_y[i]);
        ++count;
    }
    if (count == 0)
        return;
    // A circle containing the point has its centre within maxR, i.e. in the point's cell or a neighbour.
    double cell = 2 * maxR;
    auto dims = [&](double c) { return std::pair<int, int>{int((x1 - x0) / c) + 1, int((y1 - y0) / c) + 1}; };
    while (double(dims(cell).first) * dims(cell).second > 4.0 * count + 64)
        cell *= 1.5;
    m_gridCell = cell;
    m_gridX0 = x0;
    m_gridY0 = y0;
    std::tie(m_gridW, m_gridH) = dims(cell);
    const size_t cells = size_t(m_gridW) * size_t(m_gridH);
    m_gridStart.assign(cells + 1, 0);
    auto cellOf = [&](size_t i) { return size_t(int((m_y[i] - y0) / cell)) * size_t(m_gridW) + size_t(int((m_x[i] - x0) / cell)); };
    for (size_t i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].rendered)
            ++m_gridStart[cellOf(i) + 1];
    for (size_t c = 0; c < cells; ++c)
        m_gridStart[c + 1] += m_gridStart[c];
    m_gridItems.assign(size_t(count), 0);
    std::vector<int> fill(m_gridStart.begin(), m_gridStart.end() - 1);
    for (size_t i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].rendered)
            m_gridItems[size_t(fill[cellOf(i)]++)] = int(i);
}

int GraphCanvas::nodeAt(const QPointF& widgetPos) const
{
    if (!m_graph)
        return -1;
    if (m_gridDirty)
        rebuildGrid();
    if (m_gridW == 0)
        return -1;
    const QPointF w = deviceToWorld(widgetPos * devicePixelRatioF());
    const int cx = int(std::floor((w.x() - m_gridX0) / m_gridCell)), cy = int(std::floor((w.y() - m_gridY0) / m_gridCell));
    int best = -1;
    for (int gy = cy - 1; gy <= cy + 1; ++gy) {
        if (gy < 0 || gy >= m_gridH)
            continue;
        for (int gx = cx - 1; gx <= cx + 1; ++gx) {
            if (gx < 0 || gx >= m_gridW)
                continue;
            const size_t c = size_t(gy) * size_t(m_gridW) + size_t(gx);
            for (int k = m_gridStart[c]; k < m_gridStart[c + 1]; ++k) {
                const int i = m_gridItems[size_t(k)];
                const double dx = m_x[size_t(i)] - w.x(), dy = m_y[size_t(i)] - w.y();
                const double r = m_size[size_t(i)] * m_nodeScale;
                if (dx * dx + dy * dy <= r * r && i > best)
                    best = i; // the last drawn circle is on top
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------
void GraphCanvas::updateCursor()
{
    if (m_panning)
        setCursor(Qt::ClosedHandCursor);
    else if (m_dragIndex >= 0 || m_hoverIndex >= 0)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
}

void GraphCanvas::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const QSize dev = (QSizeF(size()) * dpr()).toSize();
    if (!m_panInitialized) {
        m_deviceSize = dev;
        resetPan();
    } else {
        // Keep the view centre (Obsidian's onResize).
        m_panX += (dev.width() - m_deviceSize.width()) / 2.0;
        m_panY += (dev.height() - m_deviceSize.height()) / 2.0;
        m_deviceSize = dev;
    }
    changed();
}

void GraphCanvas::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    changed();
}

void GraphCanvas::mousePressEvent(QMouseEvent* e)
{
    const QPointF dev = toDevice(e->position());
    m_mouseDevice = dev;
    const int hit = nodeAt(e->position());
    if (hit >= 0 && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton || e->button() == Qt::RightButton)) {
        if (m_dragIndex < 0) {
            m_dragIndex = hit;
            m_dragButton = e->button();
            m_dragStartDevice = dev;
            m_dragClick = true;
            m_dragPinned = false;
            m_dragWorld = nodePosition(hit);
            changed();
            updateCursor();
        }
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_panning = true;
        m_panStartDevice = dev;
        m_panStartPan = QPointF(m_panX, m_panY);
        updateCursor();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void GraphCanvas::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF dev = toDevice(e->position());
    m_mouseDevice = dev;
    if (m_dragIndex >= 0) {
        const QPointF delta = dev - m_dragStartDevice;
        if (m_dragClick && QPointF::dotProduct(delta, delta) > 25)
            m_dragClick = false;
        m_dragWorld = deviceToWorld(dev);
        m_dragPinned = true;
        m_x[size_t(m_dragIndex)] = m_dragWorld.x();
        m_y[size_t(m_dragIndex)] = m_dragWorld.y();
        m_gridDirty = true;
        if (m_runner)
            m_runner->pin(m_graph->node(m_dragIndex).id, m_dragWorld.x(), m_dragWorld.y());
        changed();
        return;
    }
    if (m_panning) {
        const double nx = m_panStartPan.x() + dev.x() - m_panStartDevice.x();
        const double ny = m_panStartPan.y() + dev.y() - m_panStartDevice.y();
        const qint64 now = m_panClock.elapsed();
        m_panFrameMs = lerp(m_panFrameMs, double(now - m_lastPanMoveMs), 0.8);
        m_lastPanMoveMs = now;
        m_panvX = lerp(m_panvX, nx - m_panX, 0.8);
        m_panvY = lerp(m_panvY, ny - m_panY, 0.8);
        m_panX = nx;
        m_panY = ny;
        changed();
        return;
    }
    setHoverIndex(nodeAt(e->position()));
}

void GraphCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragIndex >= 0 && e->button() == m_dragButton) {
        const QString id = m_graph->node(m_dragIndex).id;
        const bool click = m_dragClick;
        const Qt::MouseButton button = m_dragButton;
        m_dragIndex = -1;
        m_dragPinned = false;
        m_dragButton = Qt::NoButton;
        if (m_runner)
            m_runner->unpin(id);
        changed();
        updateCursor();
        if (click) {
            // Emitted last: the receiver may replace this view.
            if (button == Qt::LeftButton || button == Qt::MiddleButton)
                emit openRequested(id, button == Qt::MiddleButton || (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)));
            else if (button == Qt::RightButton)
                emit contextMenuRequested(id, e->globalPosition().toPoint());
        }
        return;
    }
    if (m_panning && e->button() == Qt::LeftButton) {
        m_panning = false;
        const double idle = double(m_panClock.elapsed() - m_lastPanMoveMs);
        m_panFrameMs = lerp(m_panFrameMs, idle, 0.8);
        if (idle > 100 || m_panFrameMs <= 0) {
            m_panvX = m_panvY = 0;
        } else {
            m_panvX /= m_panFrameMs; // px per ms
            m_panvY /= m_panFrameMs;
        }
        updateCursor();
        changed();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void GraphCanvas::wheelEvent(QWheelEvent* e)
{
    e->accept();
    double dy = -e->angleDelta().y(); // DOM deltaY: positive when scrolling down (zoom out)
    if (dy == 0)
        dy = -e->pixelDelta().y();
    if (dy == 0)
        return;
    const double target = std::clamp(m_targetScale * std::pow(1.5, -dy / 120), 1.0 / 128, 8.0);
    m_targetScale = target;
    if (target < m_scale) {
        m_zoomCenterX = m_zoomCenterY = 0; // zooming out: around the view centre
    } else {
        const QPointF dev = toDevice(e->position());
        m_zoomCenterX = dev.x();
        m_zoomCenterY = dev.y();
    }
    emit targetScaleChanged(target);
    changed();
}

void GraphCanvas::keyPressEvent(QKeyEvent* e)
{
    m_keys.shift = e->modifiers() & Qt::ShiftModifier;
    bool handled = true;
    switch (e->key()) {
    case Qt::Key_Up:
        m_keys.up = true;
        break;
    case Qt::Key_Down:
        m_keys.down = true;
        break;
    case Qt::Key_Left:
        m_keys.left = true;
        break;
    case Qt::Key_Right:
        m_keys.right = true;
        break;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        m_keys.zoomIn = true;
        break;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        m_keys.zoomOut = true;
        break;
    default:
        handled = false;
    }
    if (!handled)
        return QWidget::keyPressEvent(e);
    e->accept();
    changed();
}

void GraphCanvas::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat())
        return;
    m_keys.shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Up:
        m_keys.up = false;
        break;
    case Qt::Key_Down:
        m_keys.down = false;
        break;
    case Qt::Key_Left:
        m_keys.left = false;
        break;
    case Qt::Key_Right:
        m_keys.right = false;
        break;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        m_keys.zoomIn = false;
        break;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        m_keys.zoomOut = false;
        break;
    default:
        return QWidget::keyReleaseEvent(e);
    }
    changed();
}

void GraphCanvas::leaveEvent(QEvent* e)
{
    m_mouseDevice.reset();
    if (!m_panning && m_dragIndex < 0)
        setHoverIndex(-1);
    QWidget::leaveEvent(e);
}

void GraphCanvas::focusOutEvent(QFocusEvent* e)
{
    m_keys = {};
    QWidget::focusOutEvent(e);
}

void GraphCanvas::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::DevicePixelRatioChange) {
        m_sprites.clear();
        m_labels.clear();
        m_shadow.clear();
        changed();
    }
    QWidget::changeEvent(e);
}

} // namespace oc
