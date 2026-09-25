#include "core/canvas/canvas_document.h"

#include <QHash>

#include <cmath>

namespace oc::canvas {

using namespace Qt::StringLiterals;

CanvasDocument::CanvasDocument() { m_steps << m_data; }

CanvasDocument::CanvasDocument(CanvasData data) { reset(std::move(data)); }

void CanvasDocument::reset(CanvasData data)
{
    m_data = std::move(data);
    m_steps = {m_data};
    m_current = 0;
    ++m_revision;
}

// ---------------------------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------------------------

bool CanvasDocument::hasUncommittedChanges() const { return !m_data.sameAs(m_steps[m_current]); }

bool CanvasDocument::commit()
{
    if (!hasUncommittedChanges())
        return false;
    m_steps.erase(m_steps.begin() + m_current + 1, m_steps.end());
    m_steps << m_data;
    if (m_steps.size() >= kMaxSteps)
        m_steps.removeFirst();
    m_current = m_steps.size() - 1;
    return true;
}

void CanvasDocument::replaceCurrentStep() { m_steps[m_current] = m_data; }

bool CanvasDocument::undo()
{
    commit(); // an edit that was not recorded yet is the one to undo
    if (!canUndo())
        return false;
    --m_current;
    m_data = m_steps[m_current];
    ++m_revision;
    return true;
}

bool CanvasDocument::redo()
{
    if (hasUncommittedChanges() || !canRedo())
        return false;
    ++m_current;
    m_data = m_steps[m_current];
    ++m_revision;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------------------------

QString CanvasDocument::addNode(Node node)
{
    while (node.id.isEmpty() || m_data.nodeIndex(node.id) >= 0)
        node.id = newId();
    node.setRect(node.rect());
    qsizetype pos = m_data.nodes.size();
    if (node.isGroup()) {
        const double area = node.width * node.height;
        for (qsizetype i = 0; i < m_data.nodes.size(); ++i) {
            const Node& o = m_data.nodes[i];
            if (o.isShown() && (!o.isGroup() || o.width * o.height < area)) {
                pos = i;
                break;
            }
        }
    }
    const QString id = node.id;
    m_data.nodes.insert(pos, std::move(node));
    ++m_revision;
    return id;
}

QString CanvasDocument::addEdge(Edge edge)
{
    while (edge.id.isEmpty() || m_data.edgeIndex(edge.id) >= 0)
        edge.id = newId();
    const QString id = edge.id;
    m_data.edges.append(std::move(edge));
    ++m_revision;
    return id;
}

void CanvasDocument::removeItems(const QSet<QString>& ids)
{
    if (ids.isEmpty())
        return;
    QSet<QString> removed;
    for (qsizetype i = m_data.nodes.size() - 1; i >= 0; --i) {
        const Node& n = m_data.nodes[i];
        if (n.isShown() && ids.contains(n.id)) {
            removed.insert(n.id);
            m_data.nodes.removeAt(i);
        }
    }
    for (qsizetype i = m_data.edges.size() - 1; i >= 0; --i) {
        const Edge& e = m_data.edges[i];
        if (e.opaque)
            continue;
        if (ids.contains(e.id) || removed.contains(e.fromNode) || removed.contains(e.toNode))
            m_data.edges.removeAt(i);
    }
    ++m_revision;
}

bool CanvasDocument::setNodeRect(QStringView id, const QRectF& rect)
{
    Node* n = m_data.node(id);
    if (!n)
        return false;
    const QRectF before = n->rect();
    n->setRect(rect);
    if (n->rect() == before)
        return false;
    ++m_revision;
    return true;
}

void CanvasDocument::moveNodes(const QSet<QString>& ids, const QPointF& delta)
{
    for (Node& n : m_data.nodes)
        if (n.isShown() && ids.contains(n.id))
            n.setRect(n.rect().translated(delta));
    ++m_revision;
}

void CanvasDocument::setColor(const QSet<QString>& ids, const QString& color)
{
    for (Node& n : m_data.nodes)
        if (n.isShown() && ids.contains(n.id))
            n.color = color;
    for (Edge& e : m_data.edges)
        if (!e.opaque && ids.contains(e.id))
            e.color = color;
    ++m_revision;
}

bool CanvasDocument::setText(QStringView id, const QString& text)
{
    Node* n = m_data.node(id);
    if (!n || n->text == text)
        return false;
    n->text = text;
    ++m_revision;
    return true;
}

bool CanvasDocument::setFile(QStringView id, const QString& path, const QString& subpath)
{
    Node* n = m_data.node(id);
    if (!n || (n->file == path && n->subpath == subpath))
        return false;
    n->file = path;
    n->subpath = subpath;
    ++m_revision;
    return true;
}

bool CanvasDocument::setUrl(QStringView id, const QString& url)
{
    Node* n = m_data.node(id);
    if (!n || n->url == url)
        return false;
    n->url = url;
    ++m_revision;
    return true;
}

bool CanvasDocument::setGroupLabel(QStringView id, const QString& label)
{
    Node* n = m_data.node(id);
    if (!n || n->label == label)
        return false;
    n->label = label;
    ++m_revision;
    return true;
}

bool CanvasDocument::setGroupBackground(QStringView id, const QString& path, const QString& style)
{
    Node* n = m_data.node(id);
    if (!n)
        return false;
    const QString newStyle = path.isEmpty() ? QString() : style;
    if (n->background == path && n->backgroundStyle == newStyle)
        return false;
    n->background = path;
    n->backgroundStyle = newStyle;
    ++m_revision;
    return true;
}

bool CanvasDocument::setEdgeLabel(QStringView id, const QString& label)
{
    Edge* e = m_data.edge(id);
    if (!e || e->label == label)
        return false;
    e->label = label;
    ++m_revision;
    return true;
}

void CanvasDocument::setEdgeEnds(const QSet<QString>& ids, EdgeEnd fromEnd, EdgeEnd toEnd)
{
    for (Edge& e : m_data.edges) {
        if (e.opaque || !ids.contains(e.id))
            continue;
        e.fromEnd = fromEnd;
        e.toEnd = toEnd;
    }
    ++m_revision;
}

bool CanvasDocument::reconnectEdge(QStringView id, bool fromEnd, const QString& nodeId, Side side)
{
    Edge* e = m_data.edge(id);
    if (!e)
        return false;
    if (fromEnd) {
        e->fromNode = nodeId;
        e->fromSide = side;
        e->fromSideSet = true;
    } else {
        e->toNode = nodeId;
        e->toSide = side;
        e->toSideSet = true;
    }
    ++m_revision;
    return true;
}

void CanvasDocument::raise(const QSet<QString>& ids)
{
    QList<Node> keep, top;
    for (Node& n : m_data.nodes)
        (n.isShown() && !n.isGroup() && ids.contains(n.id) ? top : keep) << std::move(n);
    if (top.isEmpty()) {
        m_data.nodes = keep;
        return;
    }
    m_data.nodes = keep + top;
    ++m_revision;
}

void CanvasDocument::retargetEdges(const QString& oldId, const QString& newId)
{
    for (Edge& e : m_data.edges) {
        if (e.opaque)
            continue;
        if (e.fromNode == oldId)
            e.fromNode = newId;
        if (e.toNode == oldId)
            e.toNode = newId;
    }
    ++m_revision;
}

void CanvasDocument::nudge(const QSet<QString>& ids, Side direction, bool large, double grid, bool snapToGrid)
{
    QSet<QString> nodes;
    for (const Node& n : m_data.nodes)
        if (n.isShown() && ids.contains(n.id))
            nodes.insert(n.id);
    if (nodes.isEmpty())
        return;
    const double step = large ? 5 * grid : grid;
    QPointF r = sideDirection(direction) * step;
    if (snapToGrid) {
        const QRectF box = itemsBounds(m_data, ids);
        const double sx = jsRound(std::fmod(box.left(), grid));
        const double sy = jsRound(std::fmod(box.top(), grid));
        const bool horizontal = direction == Side::Left || direction == Side::Right;
        if (horizontal && sx > 0) {
            r.setX(-sx + (direction == Side::Right ? grid : 0));
        } else if (!horizontal && sy > 0) {
            r.setY(-sy + (direction == Side::Bottom ? grid : 0));
        }
    }
    moveNodes(nodes, r);
}

namespace {
QList<Node*> nodesById(CanvasData& data, const QStringList& ids)
{
    QList<Node*> out;
    for (const QString& id : ids)
        if (Node* n = data.node(id))
            out << n;
    return out;
}

void moveTo(Node* n, double x, double y) { n->setRect(QRectF(x, y, n->width, n->height)); }
} // namespace

void CanvasDocument::align(const QStringList& ids, Align how, const QRectF& b)
{
    for (Node* s : nodesById(m_data, ids)) {
        switch (how) {
        case Align::Left: moveTo(s, b.left(), s->y); break;
        case Align::Right: moveTo(s, b.right() - s->width, s->y); break;
        case Align::Top: moveTo(s, s->x, b.top()); break;
        case Align::Bottom: moveTo(s, s->x, b.bottom() - s->height); break;
        case Align::Center: moveTo(s, (b.left() + b.right() - s->width) / 2, s->y); break;
        case Align::Middle: moveTo(s, s->x, (b.top() + b.bottom() - s->height) / 2); break;
        }
    }
    ++m_revision;
}

void CanvasDocument::distribute(const QStringList& ids, Qt::Orientation orientation, const QRectF& b)
{
    QList<Node*> n = nodesById(m_data, ids);
    if (n.size() <= 1)
        return;
    if (orientation == Qt::Horizontal) {
        double total = 0;
        for (const Node* s : n)
            total += s->width;
        std::stable_sort(n.begin(), n.end(), [](const Node* a, const Node* c) { return a->x != c->x ? a->x < c->x : a->y < c->y; });
        const double gap = (b.width() - total) / double(n.size() - 1);
        for (qsizetype i = 1; i < n.size() - 1; ++i)
            moveTo(n[i], n[i - 1]->x + n[i - 1]->width + gap, n[i]->y);
    } else {
        double total = 0;
        for (const Node* s : n)
            total += s->height;
        std::stable_sort(n.begin(), n.end(), [](const Node* a, const Node* c) { return a->y != c->y ? a->y < c->y : a->x < c->x; });
        const double gap = (b.height() - total) / double(n.size() - 1);
        for (qsizetype i = 1; i < n.size() - 1; ++i)
            moveTo(n[i], n[i]->x, n[i - 1]->y + n[i - 1]->height + gap);
    }
    ++m_revision;
}

void CanvasDocument::justify(const QStringList& ids, Qt::Orientation orientation, const QRectF& b)
{
    const QList<Node*> n = nodesById(m_data, ids);
    if (n.size() <= 1)
        return;
    for (Node* s : n) {
        if (orientation == Qt::Horizontal)
            s->setRect(QRectF(b.left(), s->y, b.width(), s->height));
        else
            s->setRect(QRectF(s->x, b.top(), s->width, b.height()));
    }
    ++m_revision;
}

void CanvasDocument::arrange(const QStringList& ids, Arrange how, const QRectF& b, double f)
{
    QList<Node*> n = nodesById(m_data, ids);
    if (n.size() <= 1)
        return;
    std::stable_sort(n.begin(), n.end(), [](const Node* a, const Node* c) { return a->y < c->y; });
    // Rows: a card joins the current row when its top is above the row's (smallest) bottom.
    QList<QList<Node*>> rows;
    double rowBottom = -std::numeric_limits<double>::infinity();
    for (Node* m : n) {
        if (m->y < rowBottom) {
            rows.last() << m;
            rowBottom = std::min(rowBottom, m->y + m->height);
        } else {
            rows << QList<Node*>{m};
            rowBottom = m->y + m->height;
        }
    }
    for (QList<Node*>& row : rows)
        std::stable_sort(row.begin(), row.end(), [](const Node* a, const Node* c) { return a->x < c->x; });
    QList<Node*> flat;
    for (const QList<Node*>& row : std::as_const(rows))
        flat << row;
    Node* d = flat.first();
    switch (how) {
    case Arrange::Row:
        for (qsizetype v = 1; v < flat.size(); ++v) {
            moveTo(flat[v], d->x + d->width + f, d->y);
            d = flat[v];
        }
        break;
    case Arrange::Column:
        for (qsizetype v = 1; v < flat.size(); ++v) {
            moveTo(flat[v], d->x, d->y + d->height + f);
            d = flat[v];
        }
        break;
    case Arrange::Grid: {
        double g = b.top();
        for (const QList<Node*>& row : std::as_const(rows)) {
            moveTo(row[0], b.left(), g);
            double k = row[0]->height;
            for (qsizetype v = 1; v < row.size(); ++v) {
                moveTo(row[v], row[v - 1]->x + row[v - 1]->width + f, g);
                k = std::max(k, row[v]->height);
            }
            g += k + f;
        }
        break;
    }
    }
    ++m_revision;
}

// ---------------------------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------------------------

Json CanvasDocument::selectionData(const QSet<QString>& ids) const
{
    Json nodes = Json::array();
    QSet<QString> selectedNodes;
    for (int i : paintOrder(m_data)) {
        const Node& n = m_data.nodes[i];
        if (ids.contains(n.id)) {
            nodes.append(nodeToJson(n));
            selectedNodes.insert(n.id);
        }
    }
    Json edges = Json::array();
    for (const Edge& e : m_data.edges)
        if (m_data.isEdgeShown(e) && selectedNodes.contains(e.fromNode) && selectedNodes.contains(e.toNode))
            edges.append(edgeToJson(e));
    Json clip = Json::object();
    clip.insert(u"nodes"_s, std::move(nodes));
    clip.insert(u"edges"_s, std::move(edges));
    const QPointF c = itemsBounds(m_data, ids).center();
    Json center = Json::object();
    center.insert(u"x"_s, Json(c.x()));
    center.insert(u"y"_s, Json(c.y()));
    clip.insert(u"center"_s, std::move(center));
    return clip;
}

QStringList CanvasDocument::importClone(const Json& clip, const QPointF& offset)
{
    QHash<QString, QString> idMap;
    QStringList added;
    for (const Json& item : clip.value(u"nodes").items()) {
        Node n = nodeFromJson(item);
        if (!n.isShown())
            continue;
        const QString oldId = n.id;
        n.id.clear();
        n.x += offset.x();
        n.y += offset.y();
        const QString id = addNode(std::move(n));
        if (!oldId.isEmpty())
            idMap.insert(oldId, id);
        added << id;
    }
    for (const Json& item : clip.value(u"edges").items()) {
        Edge e = edgeFromJson(item);
        if (e.opaque)
            continue;
        e.fromNode = idMap.value(e.fromNode);
        e.toNode = idMap.value(e.toNode);
        const Node* from = m_data.node(e.fromNode);
        const Node* to = m_data.node(e.toNode);
        if (!from || !to)
            continue;
        if (!e.fromSideSet) {
            e.fromSide = autoSide(from->rect(), to->rect());
            e.fromSideSet = true; // Obsidian writes the computed side of an imported edge
        }
        if (!e.toSideSet) {
            e.toSide = autoSide(to->rect(), from->rect());
            e.toSideSet = true;
        }
        e.id.clear();
        addEdge(std::move(e));
    }
    return added;
}

} // namespace oc::canvas
