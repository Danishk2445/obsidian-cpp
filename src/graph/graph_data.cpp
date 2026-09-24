#include "graph/graph_data.h"

#include <algorithm>
#include <limits>

namespace oc::graph {

// ---------------------------------------------------------------------------------------------
// GraphData
// ---------------------------------------------------------------------------------------------
int GraphData::addNode(const QString& id, NodeKind kind, const QString& label, qint64 timestamp)
{
    auto it = m_index.constFind(id);
    if (it != m_index.cend())
        return *it;
    const int index = int(m_nodes.size());
    GraphNode n;
    n.id = id;
    n.label = label.isEmpty() ? defaultLabel(id) : label;
    n.kind = kind;
    n.timestamp = timestamp;
    m_nodes.push_back(std::move(n));
    m_index.insert(id, index);
    m_finalized = false;
    return index;
}

bool GraphData::addEdge(int source, int target)
{
    if (source < 0 || target < 0 || source >= nodeCount() || target >= nodeCount())
        return false;
    if (!m_edgeKeys.insert(edgeKey(source, target)).second)
        return false;
    m_edges.push_back({source, target});
    m_finalized = false;
    return true;
}

bool GraphData::addEdge(const QString& sourceId, const QString& targetId)
{
    return addEdge(indexOf(sourceId), indexOf(targetId));
}

bool GraphData::hasEdge(int source, int target) const
{
    return m_edgeKeys.contains(edgeKey(source, target));
}

void GraphData::finalize(bool computeWeights)
{
    const size_t n = m_nodes.size();
    auto buildCsr = [&](std::vector<int>& offsets, std::vector<int>& list, bool outgoingDir) {
        offsets.assign(n + 1, 0);
        for (const GraphEdge& e : m_edges)
            ++offsets[size_t(outgoingDir ? e.source : e.target) + 1];
        for (size_t i = 0; i < n; ++i)
            offsets[i + 1] += offsets[i];
        list.assign(m_edges.size(), 0);
        std::vector<int> fill(offsets.begin(), offsets.end() - 1);
        for (const GraphEdge& e : m_edges) {
            const int from = outgoingDir ? e.source : e.target;
            list[size_t(fill[size_t(from)]++)] = outgoingDir ? e.target : e.source;
        }
    };
    buildCsr(m_outOffsets, m_out, true);
    buildCsr(m_inOffsets, m_in, false);

    m_nbOffsets.assign(n + 1, 0);
    m_nb.clear();
    m_nb.reserve(m_edges.size() * 2);
    std::vector<int> scratch;
    for (size_t i = 0; i < n; ++i) {
        scratch.clear();
        for (int k = m_outOffsets[i]; k < m_outOffsets[i + 1]; ++k)
            scratch.push_back(m_out[size_t(k)]);
        for (int k = m_inOffsets[i]; k < m_inOffsets[i + 1]; ++k)
            scratch.push_back(m_in[size_t(k)]);
        std::sort(scratch.begin(), scratch.end());
        scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
        for (int v : scratch)
            if (v != int(i))
                m_nb.push_back(v);
        m_nbOffsets[i + 1] = int(m_nb.size());
    }
    m_finalized = true;
    if (computeWeights) {
        for (size_t i = 0; i < n; ++i)
            m_nodes[i].weight = double((m_outOffsets[i + 1] - m_outOffsets[i]) + (m_inOffsets[i + 1] - m_inOffsets[i]));
    }
}

std::span<const int> GraphData::outgoing(int i) const
{
    Q_ASSERT(m_finalized);
    return {m_out.data() + m_outOffsets[size_t(i)], size_t(m_outOffsets[size_t(i) + 1] - m_outOffsets[size_t(i)])};
}

std::span<const int> GraphData::incoming(int i) const
{
    Q_ASSERT(m_finalized);
    return {m_in.data() + m_inOffsets[size_t(i)], size_t(m_inOffsets[size_t(i) + 1] - m_inOffsets[size_t(i)])};
}

std::span<const int> GraphData::neighbors(int i) const
{
    Q_ASSERT(m_finalized);
    return {m_nb.data() + m_nbOffsets[size_t(i)], size_t(m_nbOffsets[size_t(i) + 1] - m_nbOffsets[size_t(i)])};
}

QString GraphData::defaultLabel(const QString& id)
{
    const qsizetype slash = id.lastIndexOf(QLatin1Char('/'));
    const QString base = slash < 0 ? id : id.mid(slash + 1);
    const qsizetype dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == base.size() - 1)
        return base;
    if (QStringView(base).mid(dot + 1).compare(u"md", Qt::CaseInsensitive) == 0)
        return base.left(dot);
    return base;
}

// ---------------------------------------------------------------------------------------------
// GraphFilter
// ---------------------------------------------------------------------------------------------
namespace {

// Rebuilds `g` without the nodes flagged in `drop` (order preserved).
GraphData withoutNodes(const GraphData& g, const std::vector<char>& drop, bool computeWeights)
{
    GraphData out;
    std::vector<int> map(size_t(g.nodeCount()), -1);
    for (int i = 0; i < g.nodeCount(); ++i) {
        if (drop[size_t(i)])
            continue;
        const GraphNode& n = g.node(i);
        map[size_t(i)] = out.addNode(n.id, n.kind, n.label, n.timestamp);
        out.node(map[size_t(i)]).color = n.color;
        out.node(map[size_t(i)]).weight = n.weight;
    }
    for (const GraphEdge& e : g.edges())
        if (map[size_t(e.source)] >= 0 && map[size_t(e.target)] >= 0)
            out.addEdge(map[size_t(e.source)], map[size_t(e.target)]);
    out.finalize(computeWeights);
    return out;
}

} // namespace

GraphData GraphFilter::apply(const GraphData& full) const
{
    return applyProgression(full, -1, nullptr);
}

GraphData GraphFilter::applyProgression(const GraphData& full, int limit, int* totalSteps) const
{
    Q_ASSERT(full.isFinalized());
    const int n = full.nodeCount();
    const bool progression = limit >= 0;

    std::vector<signed char> passCache(size_t(n), -1);
    auto passes = [&](int i) -> bool {
        signed char& c = passCache[size_t(i)];
        if (c >= 0)
            return c != 0;
        const GraphNode& node = full.node(i);
        bool ok = false;
        switch (node.kind) {
        case NodeKind::File:
            ok = (!alwaysInclude.isEmpty() && node.id == alwaysInclude) || !search || search(full, i);
            break;
        case NodeKind::Attachment:
            ok = showAttachments && (!search || search(full, i));
            break;
        case NodeKind::Tag:
            ok = showTags && (!search || search(full, i));
            break;
        case NodeKind::Unresolved:
            ok = !hideUnresolved;
            break;
        }
        c = ok ? 1 : 0;
        return ok;
    };

    // Link sources in Obsidian's file order (creation order when animating).
    std::vector<int> order;
    order.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const NodeKind k = full.node(i).kind;
        if (k == NodeKind::File || (k == NodeKind::Attachment && showAttachments))
            order.push_back(i);
    }
    if (progression) {
        auto key = [&](int i) {
            const qint64 t = full.node(i).timestamp;
            return t > 0 ? t : std::numeric_limits<qint64>::max();
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    }

    std::vector<int> nodeOrder;
    nodeOrder.reserve(size_t(n));
    std::vector<char> added(size_t(n), 0);
    std::vector<GraphEdge> links;
    links.reserve(size_t(full.edgeCount()));
    int steps = 0;
    int cutPos = -1;
    auto step = [&](int pos) {
        const bool within = !progression || steps < limit;
        if (!within && steps == limit)
            cutPos = pos;
        ++steps;
        return within;
    };
    auto linkPhase = [](NodeKind k) {
        switch (k) {
        case NodeKind::File:
        case NodeKind::Attachment:
            return 0; // resolved links
        case NodeKind::Unresolved:
            return 1;
        case NodeKind::Tag:
            return 2;
        }
        return 0;
    };

    for (int pos = 0; pos < int(order.size()); ++pos) {
        const int i = order[size_t(pos)];
        if (!passes(i))
            continue;
        added[size_t(i)] = 1;
        nodeOrder.push_back(i);
        if (showOrphans)
            step(pos);
        const std::span<const int> out = full.outgoing(i);
        for (int phase = 0; phase < 3; ++phase) {
            for (int t : out) {
                const NodeKind tk = full.node(t).kind;
                if (linkPhase(tk) != phase || !passes(t))
                    continue;
                if (!step(pos))
                    continue;
                links.push_back({i, t});
                if ((tk == NodeKind::Unresolved || tk == NodeKind::Tag) && !added[size_t(t)]) {
                    added[size_t(t)] = 1;
                    nodeOrder.push_back(t);
                }
            }
        }
    }
    if (totalSteps)
        *totalSteps = steps;

    std::vector<char> keep(size_t(n), 0);
    for (int i : nodeOrder)
        keep[size_t(i)] = 1;
    if (cutPos >= 0) {
        for (size_t pos = size_t(cutPos) + 1; pos < order.size(); ++pos)
            keep[size_t(order[pos])] = 0;
    }

    GraphData result;
    std::vector<int> map(size_t(n), -1);
    for (int i : nodeOrder) {
        if (!keep[size_t(i)])
            continue;
        const GraphNode& src = full.node(i);
        const int j = result.addNode(src.id, src.kind, src.label, src.timestamp);
        map[size_t(i)] = j;
        GraphNode& dst = result.node(j);
        dst.color.reset();
        if (src.kind == NodeKind::File && !(src.id == alwaysInclude && !alwaysInclude.isEmpty())) {
            for (const ColorGroup& g : colorGroups) {
                if (g.matches && g.matches(full, i)) {
                    dst.color = g.color;
                    break;
                }
            }
        }
    }
    for (const GraphEdge& e : links)
        if (map[size_t(e.source)] >= 0 && map[size_t(e.target)] >= 0)
            result.addEdge(map[size_t(e.source)], map[size_t(e.target)]);
    result.finalize(true);

    if (!showOrphans) {
        std::vector<char> drop(size_t(result.nodeCount()), 0);
        bool any = false;
        for (int i = 0; i < result.nodeCount(); ++i) {
            bool linked = false;
            for (int t : result.outgoing(i))
                linked |= t != i;
            for (int s : result.incoming(i))
                linked |= s != i;
            if (!linked) {
                drop[size_t(i)] = 1;
                any = true;
            }
        }
        if (any)
            return withoutNodes(result, drop, true);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Local graph
// ---------------------------------------------------------------------------------------------
GraphData extractLocalGraph(const GraphData& g, const QString& centerId, const LocalGraphOptions& options)
{
    Q_ASSERT(g.isFinalized());
    constexpr double kCenterWeight = 30;
    const int jumps = std::max(1, options.depth);
    const int center = g.indexOf(centerId);
    GraphData out;
    if (center < 0) {
        const int i = out.addNode(centerId, NodeKind::File);
        out.finalize(false);
        out.node(i).weight = kCenterWeight;
        return out;
    }

    const int n = g.nodeCount();
    std::vector<int> depth(size_t(n), -1);
    depth[size_t(center)] = 0;
    std::vector<int> order{center};
    struct Found {
        int source, target;
    };
    std::vector<Found> found;
    std::vector<char> fresh(size_t(n), 0);
    for (int hop = 0; hop < jumps; ++hop) {
        std::vector<int> discovered;
        for (int o = 0; o < n; ++o) {
            if (g.node(o).kind == NodeKind::Tag)
                continue;
            const bool oIn = depth[size_t(o)] >= 0;
            for (int u : g.outgoing(o)) {
                const bool uIn = depth[size_t(u)] >= 0;
                if (options.outgoing && oIn && !uIn) {
                    if (!fresh[size_t(u)]) {
                        fresh[size_t(u)] = 1;
                        discovered.push_back(u);
                    }
                    found.push_back({o, u});
                } else if (options.incoming && uIn && !oIn && g.node(u).kind != NodeKind::Tag) {
                    if (!fresh[size_t(o)]) {
                        fresh[size_t(o)] = 1;
                        discovered.push_back(o);
                    }
                    found.push_back({o, u});
                }
            }
        }
        for (int v : discovered) {
            depth[size_t(v)] = hop + 1;
            order.push_back(v);
        }
        if (discovered.empty())
            break;
    }

    std::vector<int> map(size_t(n), -1);
    for (int i : order) {
        const GraphNode& src = g.node(i);
        map[size_t(i)] = out.addNode(src.id, src.kind, src.label, src.timestamp);
        out.node(map[size_t(i)]).color = src.color;
    }
    if (options.neighborLinks) {
        for (int i : order)
            for (int t : g.outgoing(i))
                if (map[size_t(t)] >= 0)
                    out.addEdge(map[size_t(i)], map[size_t(t)]);
    } else {
        // Obsidian stores the discovering links per source node; emit them grouped by source.
        std::stable_sort(found.begin(), found.end(), [&](const Found& a, const Found& b) { return map[size_t(a.source)] < map[size_t(b.source)]; });
        for (const Found& f : found)
            out.addEdge(map[size_t(f.source)], map[size_t(f.target)]);
    }
    out.finalize(false);
    for (int i : order)
        out.node(map[size_t(i)]).weight = kCenterWeight - kCenterWeight / jumps * depth[size_t(i)];
    return out;
}

} // namespace oc::graph
