// CanvasWidget input: hit testing, mouse/keyboard/wheel handling, drags, context menus, drops.
#include "ui/views/canvas/canvas_widget_p.h"

#include "core/files/file_manager.h"
#include "core/util/owned_menu.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "render/theme/icons.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/canvas/canvas_controls.h"
#include "ui/views/canvas/canvas_items.h"
#include "ui/views/canvas/canvas_modals.h"
#include "ui/views/canvas/canvas_render.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QGraphicsProxyWidget>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <cmath>

namespace oc {

using namespace Qt::StringLiterals;
using namespace oc::canvas;
using canvasui::Palette;

namespace {
bool isMod(Qt::KeyboardModifiers m) { return m & Qt::ControlModifier; }
// Obsidian: Ctrl-drag clones a card on Linux/Windows (Alt on macOS), Alt disables snapping (Ctrl on macOS).
bool cloneModifier(Qt::KeyboardModifiers m) { return m & Qt::ControlModifier; }

QList<MenuEntry> sectioned(const QList<MenuEntry>& items, const QStringList& order)
{
    QList<MenuEntry> out;
    for (const QString& s : order)
        for (const MenuEntry& e : items)
            if (e.section == s)
                out << e;
    for (const MenuEntry& e : items)
        if (!order.contains(e.section))
            out << e;
    return out;
}

// Adds `items` in `order`, one separator between sections (Obsidian's menu sections).
void addSections(QMenu* menu, const QList<MenuEntry>& items, const QStringList& order)
{
    const QList<MenuEntry> sorted = sectioned(items, order);
    QString current;
    QList<MenuEntry> group;
    auto flush = [&] {
        if (group.isEmpty())
            return;
        // buildSectionedMenu adds the separator between non-empty groups.
        for (MenuEntry& e : group)
            e.section = QString();
        buildSectionedMenu(menu, group);
        group.clear();
    };
    for (const MenuEntry& e : sorted) {
        if (e.section != current && !group.isEmpty())
            flush();
        current = e.section;
        group << e;
    }
    flush();
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------------------------

QString CanvasWidget::edgeAt(const QPointF& cp) const
{
    const CanvasData& d = m_doc.data();
    const double tol = 12 * zoomMultiplier(); // half the 24px interaction stroke
    for (qsizetype i = d.edges.size() - 1; i >= 0; --i) {
        const Edge& e = d.edges[i];
        const QRectF lr = edgeLabelRect(e);
        if (!lr.isNull() && lr.contains(cp))
            return e.id;
        const auto g = edgeGeometry(e);
        if (g && distanceToEdge(*g, cp) <= tol)
            return e.id;
    }
    return {};
}

QString CanvasWidget::groupLabelAt(const QPointF& cp) const
{
    const CanvasData& d = m_doc.data();
    for (const Node& n : d.nodes)
        if (n.isShown() && n.isGroup()) {
            const QRectF r = groupLabelRect(n);
            if (!r.isNull() && r.contains(cp))
                return n.id;
        }
    return {};
}

QString CanvasWidget::fileLabelAt(const QPointF& cp) const
{
    if (m_vp.zoom <= kZoomedOutZoom || m_options.cardLabelVisibility == u"never") // .mod-zoomed-out hides the names
        return {};
    const CanvasData& d = m_doc.data();
    for (qsizetype i = d.nodes.size() - 1; i >= 0; --i) {
        const Node& n = d.nodes[i];
        if (n.type != NodeType::File && n.type != NodeType::Link)
            continue;
        if (nodeLabelRect(n).contains(cp))
            return n.id;
    }
    return {};
}

bool CanvasWidget::overSelectionBox(const QPointF& cp) const
{
    if (m_selection.size() <= 1)
        return false;
    return selectionBounds().adjusted(-10, -10, 10, 10).contains(cp);
}

CanvasWidget::Hover CanvasWidget::hitTest(const QPointF& cp) const
{
    Hover h;
    const CanvasData& d = m_doc.data();
    const double s = m_vp.scale();
    // The interaction layer's target: the topmost card (groups included) within 8px.
    const int ti = nodeAt(d, cp, 8 / s, true);
    if (ti >= 0) {
        const Node& t = d.nodes[ti];
        if (!(m_selection.size() > 1 && m_selection.contains(t.id))) {
            h.node = t.id;
            if (!m_readOnly && t.id != m_editingId) {
                h.handle = handleAt(t.rect(), cp, zoomMultiplier());
                switch (h.handle) {
                case Handle::Top: h.connectSide = Side::Top; break;
                case Handle::Right: h.connectSide = Side::Right; break;
                case Handle::Bottom: h.connectSide = Side::Bottom; break;
                case Handle::Left: h.connectSide = Side::Left; break;
                default: break;
                }
            }
        }
    }
    h.label = groupLabelAt(cp);
    if (h.label.isEmpty())
        h.label = fileLabelAt(cp);
    // Edges are under the cards (positive z) but above the groups.
    bool cardAbove = false;
    const QList<int> order = paintOrder(d);
    for (int i : order)
        if (!d.nodes[i].isGroup() && d.nodes[i].rect().contains(cp))
            cardAbove = true;
    if (!cardAbove && h.label.isEmpty())
        h.edge = edgeAt(cp);
    h.selectionBox = overSelectionBox(cp);
    return h;
}

void CanvasWidget::updateHover(const QPointF& cp)
{
    const Hover h = hitTest(cp);
    const bool changed = h.node != m_hover.node || h.handle != m_hover.handle || h.connectSide != m_hover.connectSide || h.edge != m_hover.edge
        || h.label != m_hover.label || h.selectionBox != m_hover.selectionBox;
    m_hover = h;
    if (changed) {
        if (m_options.cardLabelVisibility == u"hover")
            m_syncedRevision = ~0ull, syncItems();
        for (const QString& id : {h.node, h.edge, h.label})
            if (!id.isEmpty()) {
                if (auto* it = m_nodeItems.value(id))
                    it->update();
                if (auto* it = m_edgeItems.value(id))
                    it->update();
            }
        viewport()->update();
    }
    updateCursor();
}

void CanvasWidget::updateCursor()
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (m_drag && m_drag->started) {
        switch (m_drag->kind) {
        case Drag::Kind::Resize:
        case Drag::Kind::ResizeSelection:
            switch (m_drag->handle) {
            case Handle::Top:
            case Handle::Bottom: shape = Qt::SizeVerCursor; break;
            case Handle::Left:
            case Handle::Right: shape = Qt::SizeHorCursor; break;
            case Handle::TopLeft:
            case Handle::BottomRight: shape = Qt::SizeFDiagCursor; break;
            default: shape = Qt::SizeBDiagCursor; break;
            }
            break;
        case Drag::Kind::BoxSelect: shape = Qt::ArrowCursor; break;
        case Drag::Kind::Connect:
        case Drag::Kind::EdgeEnd: shape = Qt::PointingHandCursor; break;
        default: shape = Qt::ClosedHandCursor; break;
        }
    } else if (m_spaceHeld) {
        shape = Qt::OpenHandCursor;
    } else if (m_hover.handle != Handle::None) {
        const Node* n = m_doc.data().node(m_hover.node);
        const QPointF cp = mapToCanvas(m_lastMouse);
        if (n && m_hover.connectSide && connectionPoint(n->rect(), *m_hover.connectSide, zoomMultiplier()).contains(cp))
            shape = Qt::PointingHandCursor;
        else
            switch (m_hover.handle) {
            case Handle::Top:
            case Handle::Bottom: shape = Qt::SizeVerCursor; break;
            case Handle::Left:
            case Handle::Right: shape = Qt::SizeHorCursor; break;
            case Handle::TopLeft:
            case Handle::BottomRight: shape = Qt::SizeFDiagCursor; break;
            default: shape = Qt::SizeBDiagCursor; break;
            }
    } else if (!m_readOnly && (!m_hover.label.isEmpty() || !m_hover.edge.isEmpty() || m_hover.selectionBox)) {
        shape = Qt::OpenHandCursor;
    } else if (!m_readOnly && !m_hover.node.isEmpty() && m_hover.node != m_editingId) {
        const Node* n = m_doc.data().node(m_hover.node);
        if (n && (!n->isGroup() || m_selection.contains(n->id)))
            shape = Qt::OpenHandCursor;
    }
    viewport()->setCursor(shape);
}

// ---------------------------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------------------------

namespace {
bool editorContains(QGraphicsProxyWidget* proxy, const QPointF& scenePos)
{
    return proxy && proxy->isVisible() && proxy->sceneBoundingRect().contains(scenePos);
}
} // namespace

void CanvasWidget::mousePressEvent(QMouseEvent* e)
{
    m_lastMouse = e->pos();
    const QPointF cp = mapToCanvas(e->pos());
    if (editorContains(m_editorProxy, mapToScene(e->pos()))) {
        setProperty("ocForwardMouse", true);
        QGraphicsView::mousePressEvent(e);
        return;
    }
    // The focused media card's viewer / player (its resizers still resize).
    if (e->button() != Qt::MiddleButton && !m_spaceHeld && overFocusedMedia(mapToScene(e->pos()))
        && hitTest(mapToCanvas(e->pos())).handle == Handle::None) {
        setProperty("ocForwardMouse", true);
        QGraphicsView::mousePressEvent(e);
        return;
    }
    if (m_labelEdit && m_labelEdit->isVisible())
        finishLabelEdit(true);
    setFocus(Qt::MouseFocusReason);
    if (m_drag)
        return;
    const Qt::KeyboardModifiers mods = e->modifiers();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_spaceHeld)) {
        beginPan(e->pos());
        m_drag->started = true;
        m_drag->button = e->button();
        updateCursor();
        return;
    }
    if (e->button() == Qt::RightButton) {
        beginPan(e->pos());
        m_drag->button = Qt::RightButton; // a right-drag pans; a right-click opens the menu
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (!m_editingId.isEmpty())
        stopEditing();
    const Hover h = hitTest(cp);
    m_hover = h;
    const CanvasData& d = m_doc.data();
    // Resizers and connection points of the hovered card (".canvas-node-interaction-layer").
    if (!m_readOnly && !h.node.isEmpty() && h.handle != Handle::None) {
        const Node* n = d.node(h.node);
        if (n && h.connectSide && connectionPoint(n->rect(), *h.connectSide, zoomMultiplier()).contains(cp))
            beginConnect(h.node, *h.connectSide, cp);
        else
            beginResize(h.node, h.handle, cp);
        m_drag->pressView = e->pos();
        m_drag->mods = mods;
        return;
    }
    // Resizers of the multi-selection box.
    if (!m_readOnly && m_selection.size() > 1) {
        const Handle sh = handleAt(selectionBounds().adjusted(-10, -10, 10, 10), cp, zoomMultiplier());
        if (sh != Handle::None) {
            beginResizeSelection(sh, cp);
            m_drag->pressView = e->pos();
            return;
        }
    }
    // Cards (and labels, which belong to their card), then edges, then the selection box.
    QString node;
    const QList<int> order = paintOrder(d);
    for (auto it = order.crbegin(); it != order.crend() && node.isEmpty(); ++it) {
        const Node& n = d.nodes[*it];
        if (!n.rect().contains(cp))
            continue;
        if (!n.isGroup() || m_selection.contains(n.id))
            node = n.id;
    }
    if (!h.label.isEmpty())
        node = h.label;
    if (!node.isEmpty()) {
        beginMoveSelection(cp, node);
        m_drag->pressView = e->pos();
        m_drag->mods = mods;
        return;
    }
    if (!h.edge.isEmpty()) {
        beginEdgeDrag(h.edge, cp);
        m_drag->pressView = e->pos();
        m_drag->mods = mods;
        return;
    }
    if (h.selectionBox) {
        beginMoveSelection(cp, QString());
        m_drag->pressView = e->pos();
        m_drag->mods = mods;
        return;
    }
    // Empty canvas: deselect (unless Shift) and start a box selection.
    if (!(mods & Qt::ShiftModifier))
        deselectAll();
    beginBoxSelect(cp, mods & Qt::ShiftModifier, isMod(mods));
    m_drag->pressView = e->pos();
    m_drag->mods = mods;
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e)
{
    m_lastMouse = e->pos();
    if (property("ocForwardMouse").toBool()
        || (!m_drag && (editorContains(m_editorProxy, mapToScene(e->pos())) || overFocusedMedia(mapToScene(e->pos()))))) {
        QGraphicsView::mouseMoveEvent(e);
        if (!property("ocForwardMouse").toBool())
            viewport()->unsetCursor();
        return;
    }
    const QPointF cp = mapToCanvas(e->pos());
    if (m_drag) {
        if (!m_drag->started) {
            if ((e->pos() - m_drag->pressView).manhattanLength() <= canvasui::kDragThreshold)
                return;
            m_drag->started = true;
            // Deferred starts (Obsidian's drag begins after the pointer moved).
            switch (m_drag->kind) {
            case Drag::Kind::Move: {
                if (m_readOnly) {
                    m_drag.reset();
                    return;
                }
                if (!m_drag->pressedNode.isEmpty() && !m_selection.contains(m_drag->pressedNode))
                    selectOnly(m_drag->pressedNode);
                QSet<QString> moving;
                const CanvasData& d = m_doc.data();
                for (const QString& id : std::as_const(m_selection)) {
                    const Node* n = d.node(id);
                    if (!n)
                        continue;
                    moving.insert(id);
                    if (n->isGroup())
                        for (int i : nodesInside(d, n->rect(), d.nodeIndex(id)))
                            moving.insert(d.nodes[i].id);
                }
                m_drag->moving = moving;
                for (const QString& id : std::as_const(moving))
                    m_drag->startRects.insert(id, d.node(id)->rect());
                m_doc.raise(moving); // dragged cards come to the top (Obsidian's updateZIndex)
                break;
            }
            case Drag::Kind::Pan:
                if (m_drag->button == Qt::RightButton)
                    m_suppressContextMenu = true;
                break;
            case Drag::Kind::Resize:
                selectOnly(m_drag->nodeId);
                break;
            case Drag::Kind::Connect:
            case Drag::Kind::EdgeEnd:
                deselectAll();
                break;
            default:
                break;
            }
            updateOverlays();
        }
        dragMove(e->pos(), e->modifiers());
        return;
    }
    updateHover(cp);
    // Links in the focused card (its content is interactive): hover previews.
    if (m_selection.size() == 1 && m_hover.handle == Handle::None) {
        const QString id = *m_selection.begin();
        const Node* n = m_doc.data().node(id);
        if (n && n->rect().contains(cp) && (n->type == NodeType::Text || n->type == NodeType::File)) {
            if (canvasui::CardContent* c = contentFor(id)) {
                const QRectF inner = n->rect().adjusted(1, 1, -1, -1);
                if (const auto link = c->linkAt(cp - inner.topLeft(), inner.size(), m_scroll.value(id))) {
                    viewport()->setCursor(Qt::PointingHandCursor);
                    const QPoint g = viewport()->mapToGlobal(e->pos());
                    emit linkHovered(link->target, QRect(g - QPoint(8, 8), QSize(16, 16)));
                }
            }
        }
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e)
{
    m_lastMouse = e->pos();
    if (property("ocForwardMouse").toBool()) {
        setProperty("ocForwardMouse", false);
        QGraphicsView::mouseReleaseEvent(e);
        return;
    }
    if (!m_drag)
        return;
    if (m_drag->button != e->button())
        return;
    if (m_drag->started) {
        dragEnd(e->pos(), e->modifiers(), false);
        return;
    }
    // A click (no drag).
    const std::unique_ptr<Drag> d = std::move(m_drag);
    const QPointF cp = mapToCanvas(e->pos());
    const Qt::KeyboardModifiers mods = e->modifiers();
    switch (d->kind) {
    case Drag::Kind::Pan:
        if (d->button == Qt::RightButton && !m_suppressContextMenu)
            showContextMenu(e->pos());
        m_suppressContextMenu = false;
        break;
    case Drag::Kind::Move: {
        const QString id = d->pressedNode;
        if (id.isEmpty())
            break;
        const Node* n = m_doc.data().node(id);
        if (!n)
            break;
        // The label of a file card: Mod+click opens the file in a new tab.
        if (fileLabelAt(cp) == id && isMod(mods) && n->type == NodeType::File) {
            emit linkActivated(n->file + n->subpath, true);
            break;
        }
        // Links inside the focused card.
        if (m_selection == QSet<QString>{id} && (n->type == NodeType::Text || n->type == NodeType::File)) {
            if (canvasui::CardContent* c = contentFor(id)) {
                const QRectF inner = n->rect().adjusted(1, 1, -1, -1);
                if (inner.contains(cp))
                    if (const auto link = c->linkAt(cp - inner.topLeft(), inner.size(), m_scroll.value(id))) {
                        openLinkFromCard(link->target, isMod(mods));
                        break;
                    }
            }
        }
        if (mods & Qt::ShiftModifier) {
            QSet<QString> sel = m_selection;
            if (sel.contains(id))
                sel.remove(id);
            else
                sel.insert(id);
            setSelection(sel);
        } else {
            selectOnly(id);
        }
        break;
    }
    case Drag::Kind::Resize:
    case Drag::Kind::Connect:
        // A click on a card's resizer selects the card.
        if (mods & Qt::ShiftModifier) {
            QSet<QString> sel = m_selection;
            sel.insert(d->kind == Drag::Kind::Resize ? d->nodeId : d->fixedNode);
            setSelection(sel);
        } else {
            selectOnly(d->kind == Drag::Kind::Resize ? d->nodeId : d->fixedNode);
        }
        break;
    case Drag::Kind::EdgeEnd:
        if (m_readOnly)
            break;
        if (mods & Qt::ShiftModifier) {
            QSet<QString> sel = m_selection;
            if (sel.contains(d->edgeId))
                sel.remove(d->edgeId);
            else
                sel.insert(d->edgeId);
            setSelection(sel);
        } else if (m_selection == QSet<QString>{d->edgeId}) {
            editEdgeLabel(d->edgeId);
        } else {
            selectOnly(d->edgeId);
        }
        break;
    default:
        break;
    }
    updateOverlays();
    viewport()->update();
    updateCursor();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPointF cp = mapToCanvas(e->pos());
    if (editorContains(m_editorProxy, mapToScene(e->pos())) || overFocusedMedia(mapToScene(e->pos()))) {
        QGraphicsView::mouseDoubleClickEvent(e);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    m_drag.reset();
    const Hover h = hitTest(cp);
    const CanvasData& d = m_doc.data();
    // Labels: a group's label edits it; a file card's label opens the file.
    if (!h.label.isEmpty()) {
        const Node* n = d.node(h.label);
        if (n && n->isGroup()) {
            editGroupLabel(n->id);
            return;
        }
        if (n && n->type == NodeType::File) {
            emit linkActivated(n->file + n->subpath, false);
            return;
        }
        if (n && n->type == NodeType::Link) {
            QDesktopServices::openUrl(QUrl(n->url));
            return;
        }
    }
    // The bottom/right resizers of a text card fit it to its content (Obsidian's onResizeDblclick).
    if (!m_readOnly && !h.node.isEmpty() && (h.handle == Handle::Bottom || h.handle == Handle::Top)) {
        const Node* n = d.node(h.node);
        if (n && (n->type == NodeType::Text || n->type == NodeType::File)) {
            if (canvasui::CardContent* c = contentFor(n->id)) {
                const double inner = c->contentHeight(n->width - 2);
                if (inner > 0) {
                    const double h2 = std::max(kMinNodeDimension, std::ceil(inner + 2 + 32));
                    m_doc.setNodeRect(n->id, QRectF(n->x, n->y, n->width, h2));
                    commit();
                    return;
                }
            }
        }
    }
    // Cards: text and note cards edit in place.
    const QList<int> order = paintOrder(d);
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        const Node& n = d.nodes[*it];
        if (!n.rect().contains(cp))
            continue;
        if (n.isGroup() && !m_selection.contains(n.id))
            continue; // groups are click-through unless selected
        if (n.type == NodeType::Text || n.type == NodeType::File)
            startEditing(n.id);
        return;
    }
    if (!h.edge.isEmpty()) {
        editEdgeLabel(h.edge);
        return;
    }
    // Empty space: a new text card, centered.
    if (!m_readOnly)
        createTextNode(cp, Anchor::Center);
}

void CanvasWidget::leaveEvent(QEvent* e)
{
    QGraphicsView::leaveEvent(e);
    if (!m_drag && (!m_hover.node.isEmpty() || !m_hover.edge.isEmpty())) {
        m_hover = Hover{};
        viewport()->update();
    }
}

void CanvasWidget::focusOutEvent(QFocusEvent* e)
{
    QGraphicsView::focusOutEvent(e);
    if (m_spaceHeld) {
        m_spaceHeld = false;
        updateCursor();
    }
}

bool CanvasWidget::focusNextPrevChild(bool next)
{
    Q_UNUSED(next);
    return false; // Tab belongs to the card editor
}

// ---------------------------------------------------------------------------------------------
// Drags
// ---------------------------------------------------------------------------------------------

void CanvasWidget::beginPan(const QPoint& viewPos)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::Pan;
    m_drag->pressView = viewPos;
    m_drag->pressCanvas = mapToCanvas(viewPos);
    m_drag->panAnchor = m_drag->pressCanvas;
}

void CanvasWidget::beginBoxSelect(const QPointF& cp, bool additive, bool mod)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::BoxSelect;
    m_drag->pressCanvas = cp;
    m_drag->additive = additive;
    m_drag->modCreate = mod;
    m_drag->initialSelection = m_selection;
    m_drag->box = QRectF(cp, QSizeF(0, 0));
}

void CanvasWidget::beginMoveSelection(const QPointF& cp, const QString& pressedNode)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::Move;
    m_drag->pressCanvas = cp;
    m_drag->pressedNode = pressedNode;
}

void CanvasWidget::beginResize(const QString& nodeId, Handle handle, const QPointF& cp)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::Resize;
    m_drag->pressCanvas = cp;
    m_drag->nodeId = nodeId;
    m_drag->handle = handle;
    const Node* n = m_doc.data().node(nodeId);
    m_drag->startRect = n ? n->rect() : QRectF();
    // Media keep their aspect ratio (Shift toggles).
    if (n && n->type == NodeType::File)
        if (canvasui::CardContent* c = contentFor(nodeId))
            m_drag->aspect = c->aspectRatio();
    if (n && n->isGroup() && n->backgroundStyle == u"ratio" && n->height > 0)
        m_drag->aspect = n->width / n->height;
}

void CanvasWidget::beginResizeSelection(Handle handle, const QPointF& cp)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::ResizeSelection;
    m_drag->pressCanvas = cp;
    m_drag->handle = handle;
    const QRectF box = selectionBounds();
    m_drag->startRect = box;
    for (const QString& id : std::as_const(m_selection))
        if (const Node* n = m_doc.data().node(id))
            m_drag->startRects.insert(id, n->rect());
}

void CanvasWidget::beginConnect(const QString& nodeId, Side side, const QPointF& cp)
{
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::Connect;
    m_drag->pressCanvas = cp;
    m_drag->pointer = cp;
    m_drag->fixedNode = nodeId;
    m_drag->fixedSide = side;
    m_drag->fixedEnd = EdgeEnd::None;
    m_drag->freeEnd = EdgeEnd::Arrow;
    m_drag->freeSide = oppositeSide(side);
    m_drag->movingFrom = false;
}

void CanvasWidget::beginEdgeDrag(const QString& edgeId, const QPointF& cp)
{
    const Edge* e = m_doc.data().edge(edgeId);
    const auto g = e ? edgeGeometry(*e) : std::nullopt;
    m_drag = std::make_unique<Drag>();
    m_drag->kind = Drag::Kind::EdgeEnd;
    m_drag->pressCanvas = cp;
    m_drag->pointer = cp;
    m_drag->edgeId = edgeId;
    if (!e || !g)
        return;
    // The end nearer to the pointer moves (Obsidian's onConnectionPointerdown).
    const bool moveTo = QLineF(cp, g->fromAnchor).length() > QLineF(cp, g->toAnchor).length();
    m_drag->movingFrom = !moveTo;
    if (moveTo) {
        m_drag->fixedNode = e->fromNode;
        m_drag->fixedSide = e->fromSide;
        m_drag->fixedEnd = e->fromEnd;
        m_drag->freeEnd = e->toEnd;
        m_drag->freeSide = e->toSide;
    } else {
        m_drag->fixedNode = e->toNode;
        m_drag->fixedSide = e->toSide;
        m_drag->fixedEnd = e->toEnd;
        m_drag->freeEnd = e->fromEnd;
        m_drag->freeSide = e->fromSide;
    }
}

bool CanvasWidget::canSnap(Qt::KeyboardModifiers mods) const { return !(mods & Qt::AltModifier); }

SnapOptions CanvasWidget::snapOptions() const
{
    SnapOptions o;
    o.grid = m_options.snapToGrid;
    o.objects = m_options.snapToObjects;
    o.gridSpacing = gridSpacing(m_vp.zoom);
    o.distance = snapDistance(m_vp.scale());
    return o;
}

QList<QRectF> CanvasWidget::snapTargets(const QSet<QString>& exclude, bool groups) const
{
    QList<QRectF> out;
    const QRectF vis = m_vp.visibleRect();
    for (const Node& n : m_doc.data().nodes)
        if (n.isShown() && !exclude.contains(n.id) && n.isGroup() == groups && n.rect().intersects(vis))
            out << n.rect();
    return out;
}

void CanvasWidget::dragMove(const QPoint& viewPos, Qt::KeyboardModifiers mods)
{
    if (!m_drag)
        return;
    Drag& d = *m_drag;
    const QPointF cp = mapToCanvas(viewPos);
    d.pointer = cp;
    d.mods = mods;
    d.guides = SnapGuides{};
    // Auto-pan near the edges (Obsidian's handleDragWithPan: 30px inset, up to 40px per frame).
    if (d.kind != Drag::Kind::Pan) {
        const QRect inner = viewport()->rect().adjusted(30, 30, -30, -30);
        QPointF delta;
        if (viewPos.x() < inner.left())
            delta.setX(viewPos.x() - inner.left());
        if (viewPos.x() > inner.right())
            delta.setX(viewPos.x() - inner.right());
        if (viewPos.y() < inner.top())
            delta.setY(viewPos.y() - inner.top());
        if (viewPos.y() > inner.bottom())
            delta.setY(viewPos.y() - inner.bottom());
        m_autoPanDelta = QPointF(std::clamp(delta.x(), -40.0, 40.0), std::clamp(delta.y(), -40.0, 40.0));
        if (!m_autoPanDelta.isNull() && viewport()->rect().contains(viewPos)) {
            if (!m_autoPan.isActive())
                m_autoPan.start(16, this);
        } else {
            m_autoPan.stop();
        }
    }
    const CanvasData& data = m_doc.data();
    switch (d.kind) {
    case Drag::Kind::Pan:
        panBy(d.panAnchor - cp);
        break;
    case Drag::Kind::BoxSelect: {
        d.box = QRectF(QPointF(std::min(d.pressCanvas.x(), cp.x()), std::min(d.pressCanvas.y(), cp.y())),
                       QPointF(std::max(d.pressCanvas.x(), cp.x()), std::max(d.pressCanvas.y(), cp.y())));
        QSet<QString> hit;
        for (int i : nodesInBox(data, d.box))
            hit.insert(data.nodes[i].id);
        if (d.additive) {
            QSet<QString> sel = d.initialSelection;
            for (const QString& id : std::as_const(hit)) {
                if (sel.contains(id))
                    sel.remove(id);
                else
                    sel.insert(id);
            }
            hit = sel;
        }
        setSelectionInternal(hit);
        break;
    }
    case Drag::Kind::Move: {
        QPointF delta = cp - d.pressCanvas;
        if (mods & Qt::ShiftModifier) {
            if (std::abs(delta.x()) > std::abs(delta.y()))
                delta.setY(0);
            else
                delta.setX(0);
        }
        if (canSnap(mods)) {
            // Snap what leads the move: the dragged groups (outermost), else every dragged card.
            QList<QString> groups, all;
            for (auto it = d.startRects.cbegin(); it != d.startRects.cend(); ++it) {
                all << it.key();
                if (const Node* n = data.node(it.key()); n && n->isGroup())
                    groups << it.key();
            }
            QList<QString> leading = groups.isEmpty() ? all : groups;
            if (groups.size() > 1) {
                leading.clear();
                for (const QString& g : groups) {
                    const QRectF gr = d.startRects.value(g);
                    bool inside = false;
                    for (const QString& o : groups) {
                        const QRectF orr = d.startRects.value(o);
                        if (o != g && orr.contains(gr) && orr != gr)
                            inside = true;
                    }
                    if (!inside)
                        leading << g;
                }
            }
            QList<QRectF> moving;
            for (const QString& id : leading)
                moving << d.startRects.value(id).translated(delta);
            const QList<QRectF> others = snapTargets(QSet<QString>(all.begin(), all.end()), !groups.isEmpty());
            delta += snapMove(moving, others, delta.x() != 0, delta.y() != 0, snapOptions(), &d.guides);
        }
        const bool clone = cloneModifier(mods);
        if (clone != d.clone) {
            d.clone = clone;
            if (clone) // the originals go back while their ghosts move
                for (auto it = d.startRects.cbegin(); it != d.startRects.cend(); ++it)
                    m_doc.setNodeRect(it.key(), it.value());
        }
        d.offset = delta;
        if (!d.clone)
            for (auto it = d.startRects.cbegin(); it != d.startRects.cend(); ++it)
                m_doc.setNodeRect(it.key(), it.value().translated(delta));
        markChanged();
        break;
    }
    case Drag::Kind::Resize: {
        double aspect = d.aspect;
        if (mods & Qt::ShiftModifier)
            aspect = aspect != 0 ? 0 : (d.startRect.height() > 0 ? d.startRect.width() / d.startRect.height() : 0);
        const QList<QRectF> others = snapTargets(m_selection + QSet<QString>{d.nodeId}, false) + snapTargets(m_selection + QSet<QString>{d.nodeId}, true);
        const QRectF r = resizeRect(d.startRect, d.handle, cp, aspect, others, snapOptions(), canSnap(mods), &d.guides);
        m_doc.setNodeRect(d.nodeId, r);
        markChanged();
        positionEditor();
        break;
    }
    case Drag::Kind::ResizeSelection: {
        const QRectF box = d.startRect;
        if (box.width() <= 0 || box.height() <= 0)
            break;
        double aspect = 0;
        if (mods & Qt::ShiftModifier)
            aspect = box.width() / box.height();
        const QList<QRectF> others = snapTargets(m_selection, false);
        QRectF nb = resizeRect(box, d.handle, cp, aspect, others, snapOptions(), canSnap(mods), &d.guides);
        // Keep every card at least 50 wide/high (Obsidian scales the minimum by the smallest card).
        double minW = 1e9, minH = 1e9;
        for (const QRectF& r : std::as_const(d.startRects)) {
            minW = std::min(minW, r.width() / box.width());
            minH = std::min(minH, r.height() / box.height());
        }
        if (minW > 0 && nb.width() * minW < kMinNodeDimension) {
            const double w = kMinNodeDimension / minW;
            if (handleHas(d.handle, Side::Left))
                nb.setLeft(nb.right() - w);
            else
                nb.setWidth(w);
        }
        if (minH > 0 && nb.height() * minH < kMinNodeDimension) {
            const double h = kMinNodeDimension / minH;
            if (handleHas(d.handle, Side::Top))
                nb.setTop(nb.bottom() - h);
            else
                nb.setHeight(h);
        }
        for (auto it = d.startRects.cbegin(); it != d.startRects.cend(); ++it) {
            const QRectF r = it.value();
            const double l = nb.left() + (r.left() - box.left()) / box.width() * nb.width();
            const double t = nb.top() + (r.top() - box.top()) / box.height() * nb.height();
            const double rr = nb.right() + (r.right() - box.right()) / box.width() * nb.width();
            const double bb = nb.bottom() + (r.bottom() - box.bottom()) / box.height() * nb.height();
            m_doc.setNodeRect(it.key(), QRectF(QPointF(l, t), QPointF(rr, bb)));
        }
        markChanged();
        break;
    }
    case Drag::Kind::Connect:
    case Drag::Kind::EdgeEnd: {
        const QRectF probe(cp.x() - kObjectSnapDistance, cp.y() - kObjectSnapDistance, 2 * kObjectSnapDistance, 2 * kObjectSnapDistance);
        QList<int> candidates;
        const Node* fixed = data.node(d.fixedNode);
        for (int i = 0; i < data.nodes.size(); ++i) {
            const Node& n = data.nodes[i];
            if (!n.isShown() || n.id == d.fixedNode || !n.rect().intersects(probe))
                continue;
            if (d.kind == Drag::Kind::Connect && fixed) {
                // Not into a group containing the card, nor from a group into its contents.
                if (n.isGroup() && n.rect().contains(fixed->rect()))
                    continue;
                if (fixed->isGroup() && fixed->rect().contains(n.rect()))
                    continue;
            }
            candidates << i;
        }
        d.target = nearestSide(data, candidates, cp, kObjectSnapDistance);
        viewport()->update();
        break;
    }
    case Drag::Kind::NewCard: {
        QRectF r(cp - QPointF(d.cardSize.width() / 2, d.cardSize.height() / 2), d.cardSize);
        if (canSnap(mods))
            r.translate(snapMove({r}, snapTargets({}, false), true, true, snapOptions(), &d.guides));
        d.cardRect = r;
        viewport()->update();
        break;
    }
    }
    m_overlay->update();
    viewport()->update();
    updateCursor();
}

void CanvasWidget::autoPanTick()
{
    if (!m_drag || !m_drag->started || m_autoPanDelta.isNull()) {
        m_autoPan.stop();
        return;
    }
    const double k = 0.5 / std::sqrt(m_vp.scale());
    panBy(m_autoPanDelta * k);
    dragMove(m_lastMouse, m_drag->mods);
}

void CanvasWidget::dragEnd(const QPoint& viewPos, Qt::KeyboardModifiers mods, bool cancel)
{
    if (!m_drag)
        return;
    m_autoPan.stop();
    std::unique_ptr<Drag> d = std::move(m_drag);
    const QPointF cp = mapToCanvas(viewPos);
    switch (d->kind) {
    case Drag::Kind::Pan:
        break;
    case Drag::Kind::BoxSelect:
        if (cancel) {
            setSelectionInternal(d->initialSelection);
        } else if (isMod(mods) && d->box.width() > 0 && d->box.height() > 0 && !m_readOnly) {
            createFromBox(d->box, viewport()->mapToGlobal(viewPos));
        }
        break;
    case Drag::Kind::Move:
        if (cancel) {
            for (auto it = d->startRects.cbegin(); it != d->startRects.cend(); ++it)
                m_doc.setNodeRect(it.key(), it.value());
            markChanged();
        } else if (d->clone) {
            const Json clip = m_doc.selectionData(d->moving);
            const QStringList added = m_doc.importClone(clip, d->offset);
            commit();
            setSelection(QSet<QString>(added.begin(), added.end()));
        } else {
            commit();
        }
        break;
    case Drag::Kind::Resize:
    case Drag::Kind::ResizeSelection:
        if (cancel) {
            if (d->kind == Drag::Kind::Resize)
                m_doc.setNodeRect(d->nodeId, d->startRect);
            else
                for (auto it = d->startRects.cbegin(); it != d->startRects.cend(); ++it)
                    m_doc.setNodeRect(it.key(), it.value());
            markChanged();
        } else {
            commit();
        }
        break;
    case Drag::Kind::Connect:
        if (cancel)
            break;
        if (d->target) {
            Edge e;
            e.fromNode = d->fixedNode;
            e.fromSide = d->fixedSide;
            e.toNode = m_doc.data().nodes[d->target->node].id;
            e.toSide = d->target->side;
            e.fromEnd = EdgeEnd::None;
            e.toEnd = EdgeEnd::Arrow;
            m_doc.addEdge(std::move(e));
            commit();
        } else {
            // Dropped on the empty canvas: a menu to create the card at the other end.
            Edge e;
            e.fromNode = d->fixedNode;
            e.fromSide = d->fixedSide;
            e.toNode = QString();
            e.toSide = d->freeSide;
            showConnectionDropMenu(d->fixedNode + u'|' + sideName(d->fixedSide), cp, d->freeSide);
        }
        break;
    case Drag::Kind::EdgeEnd:
        if (cancel)
            break;
        if (d->target) {
            m_doc.reconnectEdge(d->edgeId, d->movingFrom, m_doc.data().nodes[d->target->node].id, d->target->side);
        } else {
            m_doc.removeItems({d->edgeId}); // Obsidian removes an edge dropped on nothing
        }
        commit();
        break;
    case Drag::Kind::NewCard: {
        if (cancel || !viewport()->rect().contains(viewPos) || d->cardRect.isNull())
            break;
        const QPointF pos = d->cardRect.topLeft();
        const QSizeF size = d->cardSize;
        deselectAll();
        if (d->cardKind == u"card") {
            createTextNode(pos, Anchor::TopLeft, size);
        } else {
            pickFileFor(d->cardKind == u"media", [this, pos, size](const QString& path) { createFileNode(path, pos, Anchor::TopLeft, size); });
        }
        break;
    }
    }
    m_syncedRevision = ~0ull;
    syncItems();
    updateHover(cp);
    updateOverlays();
    viewport()->update();
    updateCursor();
}

// ---------------------------------------------------------------------------------------------
// Keyboard and wheel
// ---------------------------------------------------------------------------------------------

void CanvasWidget::keyPressEvent(QKeyEvent* e)
{
    if (m_editorProxy && m_editorProxy->hasFocus()) {
        QGraphicsView::keyPressEvent(e);
        return;
    }
    // Keys belong to a focused media card's viewer / player (Escape leaves it).
    if (QGraphicsItem* fi = scene()->focusItem(); fi && fi != m_editorProxy) {
        if (e->key() != Qt::Key_Escape) {
            QGraphicsView::keyPressEvent(e);
            return;
        }
        scene()->clearFocus();
    }
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (m_drag) {
        if (e->key() == Qt::Key_Escape) {
            dragEnd(m_lastMouse, mods, true);
            return;
        }
        if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt) {
            if (m_drag->started)
                dragMove(m_lastMouse, mods);
            return;
        }
    }
    if (e->key() == Qt::Key_Space) {
        if (!e->isAutoRepeat()) {
            m_spaceHeld = true;
            updateCursor();
        }
        return;
    }
    switch (e->key()) {
    case Qt::Key_Escape:
        deselectAll();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (!e->isAutoRepeat())
            deleteSelection();
        return;
    case Qt::Key_Z:
        if (ctrl) {
            shift ? redo() : undo();
            return;
        }
        break;
    case Qt::Key_Y:
        if (ctrl) {
            redo();
            return;
        }
        break;
    case Qt::Key_A:
        if (ctrl) {
            selectAll();
            return;
        }
        break;
    case Qt::Key_C:
        if (ctrl) {
            copySelection();
            return;
        }
        break;
    case Qt::Key_X:
        if (ctrl) {
            cutSelection();
            return;
        }
        break;
    case Qt::Key_V:
        if (ctrl) {
            paste();
            return;
        }
        break;
    case Qt::Key_Exclam:
    case Qt::Key_1:
        if (shift && !ctrl) {
            zoomToFit();
            return;
        }
        break;
    case Qt::Key_At:
    case Qt::Key_2:
        if (shift && !ctrl) {
            zoomToSelection();
            return;
        }
        break;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (m_readOnly || m_selection.isEmpty() || ctrl)
            break;
        const Side dir = e->key() == Qt::Key_Left ? Side::Left : e->key() == Qt::Key_Right ? Side::Right : e->key() == Qt::Key_Up ? Side::Top : Side::Bottom;
        m_doc.nudge(m_selection, dir, shift, gridSpacing(m_vp.zoom), m_options.snapToGrid);
        commit();
        return;
    }
    case Qt::Key_Menu:
        showContextMenu(m_lastMouse);
        return;
    default:
        break;
    }
    QGraphicsView::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e)
{
    if ((m_editorProxy && m_editorProxy->hasFocus()) || (scene()->focusItem() && scene()->focusItem() != m_editorProxy)) {
        QGraphicsView::keyReleaseEvent(e);
        return;
    }
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceHeld = false;
        updateCursor();
        return;
    }
    if (m_drag && m_drag->started && (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt)) {
        dragMove(m_lastMouse, e->modifiers());
        return;
    }
    QGraphicsView::keyReleaseEvent(e);
}

void CanvasWidget::wheelEvent(QWheelEvent* e)
{
    if (m_drag)
        return;
    const QPoint pos = e->position().toPoint();
    const bool ctrl = e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
    if (!ctrl && (editorContains(m_editorProxy, mapToScene(pos)) || overFocusedMedia(mapToScene(pos)))) {
        QGraphicsView::wheelEvent(e);
        return;
    }
    // DOM-style deltas: trackpads report pixels, wheels 100px per notch.
    double dx, dy;
    const bool pixels = !e->pixelDelta().isNull();
    if (pixels) {
        dx = -e->pixelDelta().x();
        dy = -e->pixelDelta().y();
    } else {
        dx = -e->angleDelta().x() / 120.0 * 100;
        dy = -e->angleDelta().y() / 120.0 * 100;
    }
    bool zoom = ctrl || m_spaceHeld;
    if (m_options.defaultWheelBehavior == u"zoom")
        zoom = !zoom;
    const QPointF cp = mapToCanvas(pos);
    // A focused card whose content overflows scrolls first.
    if (!zoom && m_selection.size() == 1) {
        const QString id = *m_selection.begin();
        const Node* n = m_doc.data().node(id);
        if (n && n->rect().contains(cp) && (n->type == NodeType::Text || n->type == NodeType::File) && !isZoomedOut()) {
            if (canvasui::CardContent* c = contentFor(id)) {
                const double inner = n->height - 2;
                const double total = c->contentHeight(n->width - 2);
                const double pad = canvasui::MarkdownContent::topPadding(inner, total);
                const double maxScroll = std::max(0.0, total + 2 * pad - inner);
                if (maxScroll > 0) {
                    const double before = m_scroll.value(id);
                    const double after = std::clamp(before + dy / m_vp.scale(), 0.0, maxScroll);
                    if (after != before) {
                        m_scroll.insert(id, after);
                        if (auto* it = m_nodeItems.value(id))
                            it->update();
                        e->accept();
                        return;
                    }
                }
            }
        }
    }
    e->accept();
    if (zoom) {
        const double c = -dy / 300;
        zoomBy(c, dy < 0 ? std::optional<QPointF>(QPointF(pos)) : std::nullopt);
        if (pixels)
            finishViewport();
        return;
    }
    const double k = 0.93 / m_vp.scale();
    if ((e->modifiers() & Qt::ShiftModifier) && dx == 0)
        panBy(QPointF(dy * k, 0));
    else
        panBy(QPointF(dx * k, dy * k));
}

bool CanvasWidget::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_labelEdit && obj == m_labelEdit && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) {
            finishLabelEdit(false);
            return true;
        }
    }
    if (m_editor && (obj == m_editor || obj == m_editor->viewport()) && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier) {
            const QString id = m_editingId;
            stopEditing();
            if (!id.isEmpty())
                selectOnly(id);
            return true;
        }
    }
    return QGraphicsView::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------------------------
// Context menus
// ---------------------------------------------------------------------------------------------

QMenu* CanvasWidget::newMenu()
{
    return ownedMenu(this); // an item may close the canvas while the menu is shown
}

void CanvasWidget::popupMenu(QMenu* menu, const QPoint& globalPos)
{
    m_lastMenu = menu;
    menu->popup(globalPos);
}

void CanvasWidget::contextMenuEvent(QContextMenuEvent* e)
{
    if (editorContains(m_editorProxy, mapToScene(e->pos())) || overFocusedMedia(mapToScene(e->pos()))) {
        QGraphicsView::contextMenuEvent(e);
        return;
    }
    e->accept();
    if (e->reason() == QContextMenuEvent::Keyboard)
        showContextMenu(m_lastMouse);
    // Mouse: shown on release unless the right button panned.
}

QMenu* CanvasWidget::showContextMenu(const QPoint& viewPos)
{
    const QPointF cp = mapToCanvas(viewPos);
    const QPoint global = viewport()->mapToGlobal(viewPos);
    const Hover h = hitTest(cp);
    const CanvasData& d = m_doc.data();
    QString node;
    const QList<int> order = paintOrder(d);
    for (auto it = order.crbegin(); it != order.crend() && node.isEmpty(); ++it) {
        const Node& n = d.nodes[*it];
        if (n.rect().contains(cp) && (!n.isGroup() || m_selection.contains(n.id)))
            node = n.id;
    }
    if (!h.label.isEmpty())
        node = h.label;
    QMenu* menu = newMenu();
    if (!node.isEmpty()) {
        if (m_selection.size() > 1 && m_selection.contains(node)) {
            fillSelectionMenu(menu);
        } else {
            selectOnly(node);
            fillNodeMenu(menu, node);
        }
    } else if (!h.edge.isEmpty()) {
        if (m_selection.size() > 1 && m_selection.contains(h.edge)) {
            fillSelectionMenu(menu);
        } else {
            if (!m_readOnly)
                selectOnly(h.edge);
            fillEdgeMenu(menu, h.edge, cp);
        }
    } else if (h.selectionBox) {
        fillSelectionMenu(menu);
    } else if (!m_readOnly) {
        deselectAll();
        showCreationMenu(menu, cp, std::nullopt);
        QList<MenuEntry> more;
        if (canUndo())
            more << MenuEntry{u"history"_s, tr("Undo"), u"undo-2"_s, [this] { undo(); }};
        if (canRedo())
            more << MenuEntry{u"history"_s, tr("Redo"), u"redo-2"_s, [this] { redo(); }};
        more << MenuEntry{u"clipboard"_s, tr("Paste"), u"clipboard-check"_s, [this] { paste(); }};
        addSections(menu, more, {u"history"_s, u"clipboard"_s});
        menu->addSeparator();
        fillSettingsMenu(menu);
    } else {
        fillSettingsMenu(menu);
    }
    if (menu->isEmpty()) {
        menu->deleteLater();
        return nullptr;
    }
    popupMenu(menu, global);
    return menu;
}

void CanvasWidget::showCreationMenu(QMenu* menu, const QPointF& pos, std::optional<QSizeF> size)
{
    QList<MenuEntry> items;
    items << MenuEntry{u"create"_s, tr("Add card"), u"sticky-note"_s, [this, pos, size] { createTextNode(pos, Anchor::TopLeft, size); }};
    items << MenuEntry{u"create"_s, tr("Add note from vault"), u"file-text"_s, [this, pos, size] {
                           pickFileFor(false, [this, pos, size](const QString& path) { createFileNode(path, pos, Anchor::TopLeft, size); });
                       }};
    items << MenuEntry{u"create"_s, tr("Add media from vault"), u"file-image"_s, [this, pos, size] {
                           pickFileFor(true, [this, pos, size](const QString& path) { createFileNode(path, pos, Anchor::TopLeft, size); });
                       }};
    items << MenuEntry{u"create"_s, tr("Add web page"), u"external-link"_s, [this, pos, size] {
                           promptUrl(QString(), [this, pos, size](const QString& url) { createLinkNode(url, pos, Anchor::TopLeft, size); });
                       }};
    items << MenuEntry{u"create"_s, tr("Create group"), u"group"_s, [this, pos, size] {
                           createGroupNode(QRectF(pos, size.value_or(QSizeF(kDefaultFileWidth, kDefaultFileHeight))));
                       }};
    addSections(menu, items, {u"create"_s});
}

void CanvasWidget::fillSettingsMenu(QMenu* menu)
{
    auto toggle = [&](const QString& title, const QString& icon, bool checked, std::function<void()> f) {
        QAction* a = menu->addAction(IconProvider::instance().icon(icon, 16, 2.0), title);
        a->setCheckable(true);
        a->setChecked(checked);
        connect(a, &QAction::triggered, this, std::move(f));
    };
    toggle(tr("Snap to grid"), u"grid-3x3"_s, m_options.snapToGrid, [this] {
        m_options.snapToGrid = !m_options.snapToGrid;
        emit optionsEdited();
    });
    toggle(tr("Snap to objects"), u"magnet"_s, m_options.snapToObjects, [this] {
        m_options.snapToObjects = !m_options.snapToObjects;
        emit optionsEdited();
    });
    toggle(tr("Read-only"), u"lock"_s, m_readOnly, [this] { setReadOnly(!m_readOnly); });
}

void CanvasWidget::fillNodeMenu(QMenu* menu, const QString& nodeId)
{
    const Node* n = m_doc.data().node(nodeId);
    if (!n)
        return;
    const QString id = nodeId;
    QList<MenuEntry> items;
    if (!m_readOnly) {
        items << MenuEntry{u"danger"_s, tr("Remove"), u"trash-2"_s, [this, id] {
                               stopEditing();
                               m_doc.removeItems({id});
                               m_selection.remove(id);
                               commit();
                           },
                           true};
        items << MenuEntry{u"action"_s, tr("Duplicate"), u"files"_s, [this] { duplicateSelection(); }};
    }
    items << MenuEntry{u"canvas"_s, tr("Zoom to selection"), u"scan-search"_s, [this] { zoomToSelection(); }};
    TFile* file = nullptr;
    switch (n->type) {
    case NodeType::Text:
        if (!m_readOnly) {
            items << MenuEntry{u"canvas"_s, tr("Edit"), u"square-pen"_s, [this, id] { startEditing(id); }};
            items << MenuEntry{u"canvas"_s, tr("Convert to file..."), u"file-input"_s, [this, id] { convertToFile(id); }};
        }
        break;
    case NodeType::File: {
        file = m_session && m_session->vault() ? m_session->vault()->file(n->file) : nullptr;
        if (!m_readOnly)
            items << MenuEntry{u"canvas"_s, tr("Swap file..."), file ? u"arrow-left-right"_s : u"wrench"_s, [this, id] { swapFile(id); }};
        if (file) {
            const QString path = file->path();
            if (file->isMarkdown()) {
                items << MenuEntry{u"open"_s, tr("Open in new tab"), u"file-plus"_s, [this, path] { emit linkActivated(path, true); }};
                if (!m_readOnly) {
                    items << MenuEntry{u"canvas"_s, tr("Edit"), u"square-pen"_s, [this, id] { startEditing(id); }};
                    items << MenuEntry{u"canvas"_s, tr("Narrow to heading..."), u"minimize-2"_s, [this, id] { narrowToHeading(id); }};
                }
            }
            if (n->subpath.isEmpty()) {
                QList<MenuEntry> hooks;
                FileMenuHooks::collect(hooks, file, u"canvas-menu"_s, nullptr, this);
                items << hooks;
            }
        }
        break;
    }
    case NodeType::Link: {
        const QString url = n->url;
        if (!m_readOnly)
            items << MenuEntry{u"canvas"_s, tr("Change URL..."), u"link"_s, [this, id] { changeUrl(id); }};
        items << MenuEntry{u"canvas"_s, tr("Open in browser"), u"globe"_s, [url] { QDesktopServices::openUrl(QUrl(url)); }};
        items << MenuEntry{u"canvas"_s, tr("Copy URL"), u"copy"_s, [url] { QGuiApplication::clipboard()->setText(url); }};
        break;
    }
    case NodeType::Group:
        if (!m_readOnly) {
            items << MenuEntry{u"canvas"_s, tr("Edit label"), u"square-pen"_s, [this, id] { editGroupLabel(id); }};
            items << MenuEntry{u"canvas"_s, n->background.isEmpty() ? tr("Set background") : tr("Replace background"), u"image"_s,
                               [this, id] { pickGroupBackground(id); }};
        }
        break;
    default:
        break;
    }
    addSections(menu, items,
                {u"title"_s, u"canvas"_s, u"open"_s, u"action-primary"_s, u"action"_s, u"info"_s, u"view"_s, u"system"_s, QString(), u"danger"_s});
}

void CanvasWidget::fillEdgeMenu(QMenu* menu, const QString& edgeId, const QPointF& cp)
{
    const QString id = edgeId;
    QList<MenuEntry> items;
    if (!m_readOnly) {
        items << MenuEntry{u"danger"_s, tr("Remove"), u"trash-2"_s, [this, id] {
                               m_doc.removeItems({id});
                               m_selection.remove(id);
                               commit();
                           },
                           true};
        items << MenuEntry{u"canvas"_s, tr("Edit label"), u"square-pen"_s, [this, id] { editEdgeLabel(id); }};
    }
    items << MenuEntry{u"canvas"_s, tr("Follow connection"), u"corner-down-right"_s, [this, id, cp] {
                           const Edge* e = m_doc.data().edge(id);
                           const auto g = e ? edgeGeometry(*e) : std::nullopt;
                           if (!e || !g)
                               return;
                           // To the other end than the one the menu was opened near.
                           const bool nearTo = QLineF(cp, g->fromAnchor).length() > QLineF(cp, g->toAnchor).length();
                           const Node* target = m_doc.data().node(nearTo ? e->fromNode : e->toNode);
                           if (target)
                               zoomToRect(target->rect());
                       }};
    addSections(menu, items, {u"canvas"_s, u"danger"_s});
}

void CanvasWidget::fillSelectionMenu(QMenu* menu)
{
    QList<MenuEntry> items;
    if (!m_readOnly)
        items << MenuEntry{u"danger"_s, tr("Delete"), u"trash-2"_s, [this] { deleteSelection(); }};
    items << MenuEntry{u"selection"_s, tr("Zoom to selection"), u"scan-search"_s, [this] { zoomToSelection(); }};
    if (!m_readOnly) {
        items << MenuEntry{u"selection"_s, tr("Create group"), u"group"_s, [this] { createGroupFromSelection(); }};
        items << MenuEntry{u"action"_s, tr("Duplicate"), u"files"_s, [this] { duplicateSelection(); }};
        items << MenuEntry{u"clipboard"_s, tr("Cut"), u"scissors"_s, [this] { cutSelection(); }};
    }
    items << MenuEntry{u"clipboard"_s, tr("Copy"), u"copy"_s, [this] { copySelection(); }};
    if (!m_readOnly)
        items << MenuEntry{u"clipboard"_s, tr("Paste"), u"clipboard-check"_s, [this] { paste(); }};
    addSections(menu, items,
                {u"title"_s, u"open"_s, u"selection"_s, u"clipboard"_s, u"action"_s, u"view"_s, u"info"_s, QString(), u"danger"_s});
}

void CanvasWidget::showConnectionDropMenu(const QString& from, const QPointF& cp, Side newNodeSide)
{
    // `from` = "<node id>|<side>" of the fixed end.
    const QString fromId = from.section(u'|', 0, 0);
    const Side fromSide = sideFromName(from.section(u'|', 1)).value_or(Side::Right);
    auto connect = [this, fromId, fromSide, newNodeSide](const QString& newId) {
        if (newId.isEmpty() || !m_doc.data().node(fromId))
            return;
        Edge e;
        e.fromNode = fromId;
        e.fromSide = fromSide;
        e.toNode = newId;
        e.toSide = newNodeSide;
        m_doc.addEdge(std::move(e));
        commit();
    };
    const Anchor anchor = newNodeSide == Side::Top ? Anchor::Top
        : newNodeSide == Side::Bottom              ? Anchor::Bottom
        : newNodeSide == Side::Left                ? Anchor::Left
                                                   : Anchor::Right;
    QMenu* menu = newMenu();
    QList<MenuEntry> items;
    items << MenuEntry{u"action"_s, tr("Add card"), QString(), [this, cp, anchor, connect] { connect(createTextNode(cp, anchor)); }};
    items << MenuEntry{u"action"_s, tr("Add note from vault"), QString(), [this, cp, anchor, connect] {
                           pickFileFor(false, [this, cp, anchor, connect](const QString& path) { connect(createFileNode(path, cp, anchor)); });
                       }};
    // Files linked from a note card (and not yet connected), like Obsidian's connection drop menu.
    const Node* src = m_doc.data().node(fromId);
    if (src && src->type == NodeType::File && m_session && m_session->metadata() && m_session->vault()) {
        QSet<QString> connected;
        for (const Edge& e : m_doc.data().edges)
            if (e.fromNode == fromId)
                if (const Node* t = m_doc.data().node(e.toNode); t && t->type == NodeType::File)
                    connected.insert(t->file + t->subpath);
        int count = 0;
        for (const md::LinkCache& l : m_session->metadata()->fileLinks(src->file)) {
            const auto [path, sub] = LinkResolver::splitSubpath(l.link);
            const QString dest = m_session->metadata()->resolver().resolve(path, src->file, l.markdown);
            if (dest.isEmpty() || connected.contains(dest + sub))
                continue;
            connected.insert(dest + sub);
            const QString target = dest, subpath = sub;
            items << MenuEntry{u"file"_s, dest + sub, QString(),
                               [this, cp, anchor, connect, target, subpath] { connect(createFileNode(target, cp, anchor, std::nullopt, subpath)); }};
            if (++count >= 20)
                break;
        }
    }
    addSections(menu, items, {u"action"_s, u"file"_s});
    popupMenu(menu, viewport()->mapToGlobal(mapFromCanvas(cp).toPoint()));
}

// ---------------------------------------------------------------------------------------------
// Pickers and prompts
// ---------------------------------------------------------------------------------------------

void CanvasWidget::pickFileFor(bool media, const std::function<void(const QString&)>& done)
{
    MainWindow* w = mainWindow();
    if (!w)
        return;
    QPointer<CanvasWidget> self(this);
    canvasui::openFilePicker(m_session, w->overlay(), media ? canvasui::FileFilter::Media : canvasui::FileFilter::Notes, m_filePath,
                             [self, done](const QString& path) {
                                 if (self && done)
                                     done(path);
                             });
}

void CanvasWidget::promptUrl(const QString& initial, const std::function<void(const QString&)>& done)
{
    MainWindow* w = mainWindow();
    if (!w)
        return;
    QPointer<CanvasWidget> self(this);
    canvasui::promptText(
        w->overlay(), tr("Enter URL"), initial, u"https://"_s,
        [](const QString& text) {
            const QUrl u(text.trimmed(), QUrl::StrictMode);
            return u.isValid() && !u.scheme().isEmpty() ? QString() : tr("Invalid URL");
        },
        [self, done](const QString& text) {
            if (self && done)
                done(text.trimmed());
        });
}

void CanvasWidget::createFromBox(const QRectF& box, const QPoint& globalPos)
{
    const QString behavior = m_options.defaultModDragBehavior;
    const QSizeF size = box.size();
    const QPointF pos = box.topLeft();
    if (behavior == u"card") {
        createTextNode(pos, Anchor::TopLeft, size);
    } else if (behavior == u"note" || behavior == u"media") {
        pickFileFor(behavior == u"media", [this, pos, size](const QString& path) { createFileNode(path, pos, Anchor::TopLeft, size); });
    } else if (behavior == u"webpage") {
        promptUrl(QString(), [this, pos, size](const QString& url) { createLinkNode(url, pos, Anchor::TopLeft, size); });
    } else if (behavior == u"group") {
        createGroupNode(box);
    } else {
        QMenu* menu = newMenu();
        showCreationMenu(menu, pos, size);
        popupMenu(menu, globalPos);
    }
}

void CanvasWidget::convertToFile(const QString& nodeId)
{
    MainWindow* w = mainWindow();
    const Node* n = m_doc.data().node(nodeId);
    if (!w || !n || n->type != NodeType::Text || !m_session || !m_session->fileManager())
        return;
    QPointer<CanvasWidget> self(this);
    const QString text = n->text;
    canvasui::promptText(w->overlay(), tr("Convert to file..."), tr("Untitled"), QString(), {}, [self, nodeId, text](const QString& name) {
        if (!self || !self->m_session || !self->m_session->fileManager())
            return;
        FileManager* fm = self->m_session->fileManager();
        QString err;
        TFile* f = fm->createNewMarkdownFile(fm->newFileParent(self->m_filePath), name.trimmed(), text.toUtf8(), &err);
        const Node* cur = self->m_doc.data().node(nodeId);
        if (!f || !cur) {
            if (!err.isEmpty() && self->mainWindow())
                self->mainWindow()->notices()->show(err);
            return;
        }
        Node file;
        file.type = NodeType::File;
        file.file = f->path();
        file.color = cur->color;
        file.setRect(cur->rect());
        const QString newId = self->m_doc.addNode(std::move(file));
        self->m_doc.retargetEdges(nodeId, newId);
        self->m_doc.removeItems({nodeId});
        self->commit();
        self->selectOnly(newId);
    });
}

void CanvasWidget::swapFile(const QString& nodeId)
{
    const Node* n = m_doc.data().node(nodeId);
    MainWindow* w = mainWindow();
    if (!n || !w)
        return;
    TFile* f = m_session && m_session->vault() ? m_session->vault()->file(n->file) : nullptr;
    const bool notes = !f || f->isMarkdown() || f->extension().compare(u"canvas", Qt::CaseInsensitive) == 0;
    const bool media = !f || !f->isMarkdown();
    QPointer<CanvasWidget> self(this);
    canvasui::openFilePicker(m_session, w->overlay(), notes && media ? canvasui::FileFilter::Any : notes ? canvasui::FileFilter::Notes : canvasui::FileFilter::Media,
                             m_filePath, [self, nodeId](const QString& path) {
                                 if (self && self->m_doc.setFile(nodeId, path, QString()))
                                     self->commit();
                             });
}

void CanvasWidget::narrowToHeading(const QString& nodeId)
{
    const Node* n = m_doc.data().node(nodeId);
    MainWindow* w = mainWindow();
    if (!n || !w)
        return;
    QPointer<CanvasWidget> self(this);
    const QString path = n->file;
    canvasui::openHeadingPicker(m_session, w->overlay(), path, [self, nodeId, path](const QString& subpath) {
        if (self && self->m_doc.setFile(nodeId, path, subpath))
            self->commit();
    });
}

void CanvasWidget::changeUrl(const QString& nodeId)
{
    const Node* n = m_doc.data().node(nodeId);
    if (!n)
        return;
    QPointer<CanvasWidget> self(this);
    promptUrl(n->url, [self, nodeId](const QString& url) {
        if (self && self->m_doc.setUrl(nodeId, url))
            self->commit();
    });
}

void CanvasWidget::pickGroupBackground(const QString& nodeId)
{
    MainWindow* w = mainWindow();
    if (!w)
        return;
    QPointer<CanvasWidget> self(this);
    canvasui::openFilePicker(m_session, w->overlay(), canvasui::FileFilter::Images, m_filePath, [self, nodeId](const QString& path) {
        if (!self)
            return;
        const Node* g = self->m_doc.data().node(nodeId);
        if (g && self->m_doc.setGroupBackground(nodeId, path, g->backgroundStyle))
            self->commit();
    });
}

// ---------------------------------------------------------------------------------------------
// Drops
// ---------------------------------------------------------------------------------------------

bool CanvasWidget::ocFileDrag(const QStringList& paths, const QPoint& globalPos, bool drop)
{
    if (m_readOnly || paths.isEmpty() || !m_session || !m_session->vault())
        return false;
    const QPoint vp = viewport()->mapFromGlobal(globalPos);
    if (!viewport()->rect().contains(vp))
        return false;
    if (!drop)
        return true;
    QStringList files;
    Vault* vault = m_session->vault();
    for (const QString& p : paths) {
        if (TFile* f = vault->file(p)) {
            files << f->path();
        } else if (TFolder* folder = vault->folder(p)) {
            // Obsidian adds every file of a dropped folder, sorted by name.
            QStringList inside;
            std::function<void(TFolder*)> walk = [&](TFolder* fo) {
                for (TAbstractFile* c : fo->children()) {
                    if (c->isFolder())
                        walk(static_cast<TFolder*>(c));
                    else
                        inside << c->path();
                }
            };
            walk(folder);
            std::sort(inside.begin(), inside.end(), [vault](const QString& a, const QString& b) {
                return vault->file(a)->basename().localeAwareCompare(vault->file(b)->basename()) < 0;
            });
            files << inside;
        }
    }
    if (files.isEmpty())
        return false;
    const QPointF cp = mapToCanvas(vp);
    if (files.size() == 1)
        createFileNode(files.first(), cp, Anchor::TopLeft);
    else
        createFileNodes(files, cp);
    setFocus();
    return true;
}

void CanvasWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (m_readOnly) {
        e->ignore();
        return;
    }
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
    else
        e->ignore();
}

void CanvasWidget::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_readOnly && (e->mimeData()->hasUrls() || e->mimeData()->hasText()))
        e->acceptProposedAction();
    else
        e->ignore();
}

void CanvasWidget::dropEvent(QDropEvent* e)
{
    if (m_readOnly)
        return;
    const QPointF cp = mapToCanvas(e->position().toPoint());
    const QMimeData* md = e->mimeData();
    if (md->hasUrls() && m_session && m_session->vault()) {
        QStringList paths;
        for (const QUrl& u : md->urls())
            if (u.isLocalFile())
                if (const auto vp = m_session->vault()->vaultPath(u.toLocalFile()))
                    if (m_session->vault()->item(*vp))
                        paths << *vp;
        if (!paths.isEmpty()) {
            e->acceptProposedAction();
            ocFileDrag(paths, viewport()->mapToGlobal(e->position().toPoint()), true);
            return;
        }
        // A web address: a link card.
        const QUrl u = md->urls().value(0);
        if (!u.isLocalFile() && !u.scheme().isEmpty()) {
            e->acceptProposedAction();
            createLinkNode(u.toString(), cp, Anchor::Center);
            return;
        }
    }
    const QString text = md->text();
    if (text.isEmpty())
        return;
    e->acceptProposedAction();
    static const QRegularExpression url(uR"(^https?://\S+$)"_s);
    if (url.match(text.trimmed()).hasMatch())
        createLinkNode(text.trimmed(), cp, Anchor::Center);
    else
        createTextNode(cp, Anchor::Center, std::nullopt, text, false);
}

} // namespace oc
