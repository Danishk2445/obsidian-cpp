#pragma once

// JSON Canvas 1.0 (.canvas) documents: the nodes and edges of a canvas file, read from and written in
// Obsidian's file format.
//
// Byte stability: every node, edge and the top-level object remember the JSON object they were read
// from (`source`). Writing merges the current values into it: a member keeps its position and its
// source text while its value is unchanged, a member whose value became the default (no color, no
// label, toEnd "arrow", ...) is removed, and a new member is appended — for new objects in the order
// Obsidian writes them. Unknown members, nodes of unknown types and edges between missing nodes are
// kept as they are (Obsidian drops the latter two). A canvas that was not edited is never written
// (the view's job); one that was is written in Obsidian's layout (stringifyCanvas: one line per node
// and edge, tab indentation).

#include "core/canvas/canvas_json.h"

#include <QList>
#include <QRectF>
#include <QString>

#include <memory>
#include <optional>

namespace oc::canvas {

enum class NodeType { Text, File, Link, Group, Unknown };
enum class Side { Top, Right, Bottom, Left };
enum class EdgeEnd { None, Arrow };

QString nodeTypeName(NodeType type);        // "text", "file", "link", "group" ("" for Unknown)
NodeType nodeTypeFromName(QStringView name); // Unknown for anything else
QString sideName(Side side);                // "top", "right", "bottom", "left"
std::optional<Side> sideFromName(QStringView name);
Side oppositeSide(Side side);

// JavaScript's Math.round (halves round up: -2.5 -> -2).
double jsRound(double v);

// Obsidian's defaults (canvas config).
inline constexpr double kDefaultTextWidth = 250, kDefaultTextHeight = 60;
inline constexpr double kDefaultFileWidth = 400, kDefaultFileHeight = 400;
inline constexpr double kMinNodeDimension = 50;

struct Node {
    QString id;
    NodeType type = NodeType::Text;
    double x = 0, y = 0, width = kDefaultTextWidth, height = kDefaultTextHeight;
    QString color;           // "" (none), "1".."6" (preset) or a CSS color ("#ff0000")
    QString text;            // text
    QString file;            // file: vault path
    QString subpath;         // file: "#Heading", "#^block" or ""
    QString url;             // link
    QString label;           // group
    QString background;      // group: vault path of a background image
    QString backgroundStyle; // group: "" / "cover", "ratio", "repeat"

    std::shared_ptr<const Json> source; // the object this node was read from (null for new nodes)

    QRectF rect() const { return QRectF(x, y, width, height); }
    void setRect(const QRectF& r); // rounded like Obsidian (whole canvas units)
    bool isGroup() const { return type == NodeType::Group; }
    bool isShown() const { return type != NodeType::Unknown; } // unknown types, non-objects and duplicate ids are kept verbatim
    // Same content (ignoring `source`).
    bool sameAs(const Node& o) const;
};

struct Edge {
    QString id;
    QString fromNode, toNode;
    Side fromSide = Side::Right, toSide = Side::Left;
    // A side missing in the file is computed when the canvas is read (like Obsidian) but only written
    // back once it changes.
    bool fromSideSet = true, toSideSet = true;
    EdgeEnd fromEnd = EdgeEnd::None, toEnd = EdgeEnd::Arrow;
    QString color;
    QString label;
    // Kept verbatim and not shown: an item that is not an object, or a duplicate id.
    bool opaque = false;

    std::shared_ptr<const Json> source;

    bool sameAs(const Edge& o) const;
};

// Obsidian's side choice for an edge whose side is not given: the side of `from` facing `to`'s center.
Side autoSide(const QRectF& from, const QRectF& to);

struct CanvasData {
    QList<Node> nodes; // z-order: first = bottom (groups are drawn below all other cards)
    QList<Edge> edges;
    std::shared_ptr<const Json> root; // the top-level object as read (null for an empty file)

    int nodeIndex(QStringView id) const;
    int edgeIndex(QStringView id) const;
    const Node* node(QStringView id) const;
    Node* node(QStringView id);
    const Edge* edge(QStringView id) const;
    Edge* edge(QStringView id);
    // Both ends exist and are shown.
    bool isEdgeShown(const Edge& e) const;
    bool isEmpty() const { return nodes.isEmpty() && edges.isEmpty(); }
    bool sameAs(const CanvasData& o) const; // nodes and edges (in order) have the same content
};

// Reads a canvas file. Empty or whitespace-only content is an empty canvas (Obsidian creates new
// canvas files empty). Nodes without an id get a new one. nullopt + *error for invalid JSON or a top
// level that is not an object.
std::optional<CanvasData> parseCanvas(QByteArrayView bytes, QString* error = nullptr);

// Line-ending conventions of an existing file, kept when it is rewritten.
struct FileFormat {
    bool bom = false;
    bool crlf = false;
    bool finalNewline = false; // Obsidian writes none
    static FileFormat detect(QByteArrayView bytes);
};

// The file content for `data`: the merged top-level object in Obsidian's layout.
QByteArray serializeCanvas(const CanvasData& data, const FileFormat& format = {});

// The object written for one node / edge (merged into its source).
Json nodeToJson(const Node& node);
Json edgeToJson(const Edge& edge);
// Reading one node / edge object (clipboard data, parseCanvas). An edge's missing sides are not
// resolved here (fromSideSet / toSideSet false; see resolveEdgeSides).
Node nodeFromJson(const Json& object);
Edge edgeFromJson(const Json& object);
// Computes missing sides of edges whose nodes exist.
void resolveEdgeSides(CanvasData& data);

// A new random id: 16 lowercase hex digits, like Obsidian's.
QString newId();

} // namespace oc::canvas
