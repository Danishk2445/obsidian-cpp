#pragma once

// A canvas being edited: its data, Obsidian's snapshot undo history, and the edit operations of the
// canvas view (pure data changes, testable without a view).
//
// History (Obsidian's canvas history): the loaded data is the first step; commit() records the
// current data as a new step when it differs from the current one (Obsidian pushes on every
// requestSave, debounced). Undo/redo swap whole snapshots; 100 steps at most. Selection is view state
// and not part of it.

#include "core/canvas/canvas_geometry.h"
#include "core/canvas/canvas_model.h"

#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QStringList>

namespace oc::canvas {

enum class Align { Left, Center, Right, Top, Middle, Bottom };
enum class Arrange { Row, Column, Grid };

class CanvasDocument {
public:
    CanvasDocument();
    explicit CanvasDocument(CanvasData data);

    const CanvasData& data() const { return m_data; }
    // New content (load / reload): the history restarts with it.
    void reset(CanvasData data);
    quint64 revision() const { return m_revision; } // bumps on every change of data()

    // ---- history ----
    bool commit(); // false when nothing changed since the current step
    void replaceCurrentStep(); // Obsidian's overrideHistory(): the current step becomes the current data
    bool hasUncommittedChanges() const;
    bool canUndo() const { return m_current > 0; }
    bool canRedo() const { return m_current + 1 < m_steps.size(); }
    bool undo(); // back to the previous step (uncommitted changes are dropped)
    bool redo();
    int stepCount() const { return int(m_steps.size()); }
    static constexpr int kMaxSteps = 100;

    // ---- edits (not recorded until commit()) ----
    // Direct access for small field changes; call touch() afterwards.
    Node* node(QStringView id) { return m_data.node(id); }
    Edge* edge(QStringView id) { return m_data.edge(id); }
    void touch() { ++m_revision; }

    // Appends a node (on top); a new id when empty; geometry rounded. Groups go below the other
    // cards, larger groups first (where Obsidian's z-index sorting writes them). Returns the id.
    QString addNode(Node node);
    QString addEdge(Edge edge);
    // Nodes (with their edges) and edges.
    void removeItems(const QSet<QString>& ids);
    bool setNodeRect(QStringView id, const QRectF& rect);
    void moveNodes(const QSet<QString>& ids, const QPointF& delta);
    void setColor(const QSet<QString>& ids, const QString& color); // nodes and edges
    bool setText(QStringView id, const QString& text);
    bool setFile(QStringView id, const QString& path, const QString& subpath);
    bool setUrl(QStringView id, const QString& url);
    bool setGroupLabel(QStringView id, const QString& label);
    bool setGroupBackground(QStringView id, const QString& path, const QString& style);
    bool setEdgeLabel(QStringView id, const QString& label);
    void setEdgeEnds(const QSet<QString>& ids, EdgeEnd fromEnd, EdgeEnd toEnd);
    // Moves one end of an edge to another node / side (an explicit side is then written).
    bool reconnectEdge(QStringView id, bool fromEnd, const QString& nodeId, Side side);
    // To the top of the z-order (the end of the list), keeping their order. Groups stay where they are
    // (they are always drawn below the other cards).
    void raise(const QSet<QString>& ids);
    // Moves node ids from `oldId` edges to `newId` (a text card converted to a file card).
    void retargetEdges(const QString& oldId, const QString& newId);

    // Obsidian's nudgeSelection(): by one grid step (5 with Shift), aligned to the grid first when
    // snapping to the grid.
    void nudge(const QSet<QString>& ids, Side direction, bool large, double gridSpacing, bool snapToGrid);

    // Selection menu "Align", "Distribute", "Justify", "Arrange": on `ids` within `bounds` (the
    // selection's bbox, or a group's inner box). `spacing` = grid spacing for Arrange.
    void align(const QStringList& ids, Align how, const QRectF& bounds);
    void distribute(const QStringList& ids, Qt::Orientation orientation, const QRectF& bounds);
    void justify(const QStringList& ids, Qt::Orientation orientation, const QRectF& bounds);
    void arrange(const QStringList& ids, Arrange how, const QRectF& bounds, double spacing);

    // ---- clipboard (Obsidian's "obsidian/canvas" data) ----
    // {"nodes": [...], "edges": [...], "center": {x, y}}: the selected nodes (in z-order), the edges
    // between selected nodes, and the center of the selection's bbox.
    Json selectionData(const QSet<QString>& ids) const;
    // Obsidian's cloneData() + importData(): new ids, moved by `offset`; edges whose nodes are not in
    // the data are dropped. Returns the ids of the new nodes and edges.
    QStringList importClone(const Json& clip, const QPointF& offset);

private:
    CanvasData m_data;
    QList<CanvasData> m_steps;
    qsizetype m_current = 0;
    quint64 m_revision = 0;
};

} // namespace oc::canvas
