#pragma once

// Graph model for the graph view / local graph (Obsidian's "graph" core plugin).
//
// GraphData is the vault-wide link graph the metadata layer builds: one node per file (vault path),
// attachment, tag ("#tag") and unresolved link target (link text), and one directed edge per link
// (file -> file/attachment/unresolved, file -> tag). GraphFilter turns it into what the view shows
// (Obsidian's filter/colour-group semantics); extractLocalGraph() does the local graph BFS.
// Search is not part of this library: queries arrive as predicates (NodePredicate).

#include <QHash>
#include <QString>

#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace oc::graph {

enum class NodeKind : quint8 { File, Tag, Attachment, Unresolved };

// graph.json colour: {"a": 1, "rgb": 0xRRGGBB}.
struct GraphColor {
    quint32 rgb = 0;
    double a = 1.0;
    friend bool operator==(const GraphColor&, const GraphColor&) = default;
};

struct GraphNode {
    QString id;        // vault path, "#tag" or unresolved link text
    QString label;     // display text (Obsidian: basename, ".md" stripped)
    NodeKind kind = NodeKind::File;
    double weight = 0; // node size input: link count (forward + reverse), local graph: depth based
    qint64 timestamp = 0; // Animate order: min(ctime, mtime) in ms; 0 = unknown (sorted last)
    std::optional<GraphColor> color; // colour-group colour (set by GraphFilter)
};

struct GraphEdge {
    int source = -1;
    int target = -1;
    friend bool operator==(const GraphEdge&, const GraphEdge&) = default;
};

class GraphData {
public:
    GraphData() = default;

    // ---- building ----
    // Returns the index of the node with this id, adding it if absent (existing nodes are untouched).
    // An empty label means defaultLabel(id).
    int addNode(const QString& id, NodeKind kind = NodeKind::File, const QString& label = {}, qint64 timestamp = 0);
    // Directed link; exact duplicates are ignored (Obsidian keys links by target). Self links are kept
    // (they count towards the weight, like Obsidian) but never drawn. Returns true when added.
    bool addEdge(int source, int target);
    bool addEdge(const QString& sourceId, const QString& targetId); // false if either id is unknown
    // Builds the adjacency (CSR) and, if requested, sets every weight to the link count. Mutating
    // after finalize() un-finalizes; adjacency accessors require a finalized graph.
    void finalize(bool computeWeights = true);
    bool isFinalized() const { return m_finalized; }

    // ---- access ----
    int nodeCount() const { return int(m_nodes.size()); }
    int edgeCount() const { return int(m_edges.size()); }
    bool isEmpty() const { return m_nodes.empty(); }
    const GraphNode& node(int i) const { return m_nodes[size_t(i)]; }
    GraphNode& node(int i) { return m_nodes[size_t(i)]; } // label/weight/colour/timestamp edits only
    const std::vector<GraphNode>& nodes() const { return m_nodes; }
    const std::vector<GraphEdge>& edges() const { return m_edges; }
    int indexOf(const QString& id) const { return m_index.value(id, -1); }
    bool contains(const QString& id) const { return m_index.contains(id); }
    bool hasEdge(int source, int target) const;

    std::span<const int> outgoing(int i) const;  // targets of i's links, in insertion order
    std::span<const int> incoming(int i) const;  // sources linking to i, in insertion order
    std::span<const int> neighbors(int i) const; // union of both, sorted, unique, without i
    int linkCount(int i) const { return int(outgoing(i).size() + incoming(i).size()); }

    // Obsidian's display text: basename after the last '/', ".md" extension stripped.
    static QString defaultLabel(const QString& id);

private:
    static quint64 edgeKey(int s, int t) { return (quint64(quint32(s)) << 32) | quint32(t); }

    std::vector<GraphNode> m_nodes;
    std::vector<GraphEdge> m_edges;
    QHash<QString, int> m_index;
    std::unordered_set<quint64> m_edgeKeys;
    std::vector<int> m_outOffsets, m_out, m_inOffsets, m_in, m_nbOffsets, m_nb;
    bool m_finalized = false;
};

// Search/colour-group queries compiled by the search module: true when `node` of `graph` matches.
using NodePredicate = std::function<bool(const GraphData& graph, int node)>;

struct ColorGroup {
    NodePredicate matches;
    GraphColor color;
};

// Obsidian's graph filter (Filters section + colour groups):
//  - files pass when there is no search, when the search matches, or when id == alwaysInclude
//    (the local graph's file); attachments/tags additionally need showAttachments/showTags;
//  - tags, attachments and unresolved targets only appear when linked from a visible node;
//  - hideUnresolved drops unresolved targets; !showOrphans drops nodes without links;
//  - colour groups colour file nodes only, first matching group wins.
struct GraphFilter {
    NodePredicate search; // empty = no search filter
    bool showTags = false;
    bool showAttachments = false;
    bool hideUnresolved = false;
    bool showOrphans = true;
    std::vector<ColorGroup> colorGroups;
    QString alwaysInclude;

    // Visible subgraph (finalized, weights = link counts in the result). Nodes keep their ids.
    GraphData apply(const GraphData& full) const;

    // Timelapse ("Animate", Obsidian's render progression): files are revealed in timestamp order,
    // one step per file (when showOrphans) and per outgoing link. `limit` steps are included;
    // *totalSteps receives the number of steps of the complete graph. limit < 0 = everything.
    GraphData applyProgression(const GraphData& full, int limit, int* totalSteps = nullptr) const;
};

// Local graph options (graph view state keys localJumps/localBacklinks/localForelinks/localInterlinks).
struct LocalGraphOptions {
    int depth = 1;              // localJumps, 1..5
    bool incoming = true;       // localBacklinks
    bool outgoing = true;       // localForelinks
    bool neighborLinks = false; // localInterlinks
};

// Obsidian's local graph: BFS from `centerId` over `graph` (normally GraphFilter output) up to
// options.depth hops following outgoing and/or incoming links. Tag nodes are dead ends. Only the
// discovering links are kept unless neighborLinks, which keeps every link among the found nodes.
// Weights follow depth: the centre gets 30, hop d gets 30 - 30*d/depth (so size shrinks with depth).
GraphData extractLocalGraph(const GraphData& graph, const QString& centerId, const LocalGraphOptions& options);

} // namespace oc::graph
