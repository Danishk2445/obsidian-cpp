#pragma once

// Obsidian's graph renderer (GraphRenderer, Pixi) on QPainter.
//
// Coordinates follow Obsidian exactly: the stage is in device pixels, the node layer ("hanger") is
// translated by pan (device px) and scaled by `scale`, so screen = world·scale + pan. Node radius
// in world units is size·nodeScale with size = nodeSizeMultiplier·clamp(3√(weight+1), 8, 30) and
// nodeScale = √(1/scale); labels are (14 + size/4)·nodeScale world px, lines lineSizeMultiplier
// device px. Every frame (60 Hz while anything moves, stopping 60 frames after the last change)
// fades, colours, zoom and pan inertia are eased with Obsidian's lerps (×0.9 / ×0.85 per frame).
// Nodes are revealed progressively (the 50 nearest to the view centre per frame), links once both
// ends are shown. Positions come from a SimulationRunner's snapshots.

#include "ui/graph/graph_render_cache.h"

#include "graph/graph_data.h"
#include "graph/simulation_runner.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

namespace oc {

class GraphCanvas : public QWidget {
    Q_OBJECT
public:
    // The Display section of the controls (graph.json showArrow/textFadeMultiplier/...).
    struct RenderOptions {
        bool showArrow = false;
        double textFadeMultiplier = 0; // -3..3
        double nodeSizeMultiplier = 1; // 0.1..5
        double lineSizeMultiplier = 1; // 0.1..5
        friend bool operator==(const RenderOptions&, const RenderOptions&) = default;
    };

    // Per-frame counters of the last paint (tests, perf).
    struct FrameStats {
        int nodes = 0;
        int links = 0;
        int arrows = 0;
        int labels = 0;
    };

    explicit GraphCanvas(QWidget* parent = nullptr);
    ~GraphCanvas() override;

    // Positions and graph come from the runner's snapshots (the canvas is its only consumer).
    void setRunner(graph::SimulationRunner* runner);
    graph::SimulationRunner* runner() const { return m_runner; }

    void setRenderOptions(const RenderOptions& options);
    const RenderOptions& renderOptions() const { return m_options; }

    // The active file: drawn with --graph-node-focused (Obsidian's "focused" node type).
    void setFocusedNode(const QString& id);
    QString focusedNode() const { return m_focusedId; }

    // Background token painted behind the graph ("" = transparent). Default --background-primary.
    void setBackgroundToken(const QString& token);

    // ---- view transform (device pixels) ----
    double scale() const { return m_scale; }
    double targetScale() const { return m_targetScale; }
    double nodeScale() const { return m_nodeScale; }
    double textAlpha() const { return m_textAlpha; }
    // Animated zoom (Obsidian zoomTo): towards `scale`, around `center` (device px) or the view centre.
    void zoomTo(double scale, std::optional<QPointF> center = std::nullopt);
    // Immediate zoom around the view centre (restoring a view, headless rendering).
    void setScaleImmediate(double scale);
    QPointF pan() const { return {m_panX, m_panY}; }
    void setPan(const QPointF& pan);
    void resetPan(); // world origin at the view centre
    QPointF worldToDevice(const QPointF& world) const { return {world.x() * m_scale + m_panX, world.y() * m_scale + m_panY}; }
    QPointF deviceToWorld(const QPointF& device) const { return {(device.x() - m_panX) / m_scale, (device.y() - m_panY) / m_scale}; }
    QPointF worldToWidget(const QPointF& world) const;
    QPointF widgetToWorld(const QPointF& pos) const;
    // Pans so that `world` is at the view centre.
    void centerOn(const QPointF& world);

    // ---- graph state ----
    const graph::GraphData* graph() const { return m_graph.get(); }
    std::shared_ptr<const graph::GraphData> graphPtr() const { return m_graph; }
    QPointF nodePosition(int index) const;
    int indexOf(const QString& id) const { return m_graph ? m_graph->indexOf(id) : -1; }
    double nodeRadius(int index) const;       // world units: size·nodeScale
    int nodeAt(const QPointF& widgetPos) const; // topmost node under the point (grid hit test), -1
    QString hoveredNode() const;              // hovered or dragged node id, "" if none
    void setHoveredNode(const QString& id);   // simulate hover (tests)
    // Bounding box of the laid-out nodes (world units).
    QRectF layoutBounds() const;

    // Advances one animation frame (what the 60 Hz timer does).
    void step();
    // Headless: waits for the simulation to cool down and runs frames until everything is at rest.
    void settle(int timeoutMs = 60000);
    bool isIdle() const { return m_idleFrames > kIdleFrames; }
    // Renders the current state into `p` (device pixels, `size` = device size), e.g. a QImage.
    void render(QPainter& p, const QSize& deviceSize, bool background = true);
    // The current frame as an image (device pixels); transparent background like "Copy screenshot".
    QImage grabFrame(bool transparent = false);
    FrameStats lastFrameStats() const { return m_stats; }

    // The graph controls panel floating above the canvas: its --shadow-s is drawn here (it extends
    // beyond the panel's own widget). Widget coordinates; an empty rect hides it.
    void setPanelShadow(const QRectF& rect, double radius);

signals:
    // Left/middle click on a node (not a drag). newTab: middle button or Mod held.
    void openRequested(const QString& id, bool newTab);
    void contextMenuRequested(const QString& id, const QPoint& globalPos);
    void hoverChanged(const QString& id); // "" when no node is hovered
    void targetScaleChanged(double scale);
    void graphChanged(); // a snapshot with a new graph version was taken

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void changeEvent(QEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    static constexpr int kIdleFrames = 60;

    struct Rgba {
        QRgb rgb = 0x888888;
        double a = 1;
    };
    struct Colors {
        Rgba fill, fillFocused, fillTag, fillUnresolved, fillAttachment, arrow, circle, line, text, fillHighlight, lineHighlight;
    };
    struct NodeState {
        double fade = 0;     // fadeAlpha: towards 1, or 0.2 when another node is highlighted
        double moveText = 0; // the hovered label slides down 15 px
        QRgb tint = 0;       // eased fill colour
        bool rendered = false;
        bool textShown = false;
    };
    struct EdgeState {
        double lineAlpha = 0;
        double arrowAlpha = 0;
        QRgb tint = 0;
        bool rendered = false;
        bool drawLine = true; // false for the second direction of a mutual link
    };

    void changed(); // Obsidian's changed(): restart the frame loop
    void onFrameReady();
    bool pullSnapshot();
    void adoptGraph(const std::shared_ptr<const graph::GraphData>& graph);
    void updateZoom();
    void setScaleInternal(double scale);
    void revealNearest();
    void animateNodes();
    void animateEdges();
    void checkHover();
    void updateColors();
    void updateSizes();
    Rgba fillColor(int i) const;
    int highlightIndex() const { return m_dragIndex >= 0 ? m_dragIndex : m_hoverIndex; }
    void setHoverIndex(int i);
    QRectF viewportWorld() const;
    double dpr() const;
    QPointF toDevice(const QPointF& widgetPos) const { return widgetPos * dpr(); }
    void rebuildGrid() const;
    void updateCursor();
    void paintLabels(QPainter& p, const QRectF& vp);

    QPointer<graph::SimulationRunner> m_runner;
    QMetaObject::Connection m_frameConn;
    RenderOptions m_options;
    QString m_focusedId;
    QString m_backgroundToken = QStringLiteral("--background-primary");

    // Current graph and per-node/link render state (indexed like the graph).
    std::shared_ptr<const graph::GraphData> m_graph;
    quint64 m_graphVersion = 0;
    std::vector<double> m_x, m_y;
    std::vector<double> m_size; // Obsidian getSize()
    std::vector<NodeState> m_nodes;
    std::vector<EdgeState> m_edges;
    std::vector<char> m_neighborOfHighlight;
    int m_neighborMarkFor = -2;
    int m_focusedIndex = -1;
    int m_hoverIndex = -1;
    int m_dragIndex = -1;
    int m_renderedCount = 0;

    // View state.
    double m_scale = 1, m_targetScale = 1, m_nodeScale = 1, m_textAlpha = 1;
    double m_panX = 0, m_panY = 0, m_panvX = 0, m_panvY = 0;
    double m_zoomCenterX = 0, m_zoomCenterY = 0;
    QSize m_deviceSize;
    bool m_panInitialized = false;
    int m_idleFrames = 0;
    QTimer m_timer;

    // Interaction.
    struct Keys {
        bool up = false, down = false, left = false, right = false, zoomIn = false, zoomOut = false, shift = false;
    } m_keys;
    bool m_panning = false;
    QPointF m_panStartDevice, m_panStartPan;
    double m_panFrameMs = 0; // smoothed time between pan moves
    QElapsedTimer m_panClock;
    qint64 m_lastPanMoveMs = 0;
    QPointF m_dragStartDevice;
    bool m_dragClick = false; // still a click (moved < 5 px)
    bool m_dragPinned = false;
    Qt::MouseButton m_dragButton = Qt::NoButton;
    QPointF m_dragWorld;
    std::optional<QPointF> m_mouseDevice;

    // Hit-test grid (world units), rebuilt lazily when positions change.
    mutable bool m_gridDirty = true;
    mutable double m_gridCell = 1, m_gridX0 = 0, m_gridY0 = 0;
    mutable int m_gridW = 0, m_gridH = 0;
    mutable std::vector<int> m_gridStart, m_gridItems;

    Colors m_colors;
    CircleSpriteCache m_sprites;
    LabelCache m_labels;
    BoxShadowCache m_shadow;
    std::vector<BoxShadowCache::Layer> m_shadowLayers;
    QRectF m_panelRect;
    double m_panelRadius = 8;
    QString m_labelFamily;
    FrameStats m_stats;
};

} // namespace oc
