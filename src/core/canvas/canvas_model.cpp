#include "core/canvas/canvas_model.h"

#include <QRandomGenerator>
#include <QSet>

#include <cmath>
#include <numbers>

namespace oc::canvas {

using namespace Qt::StringLiterals;

QString nodeTypeName(NodeType type)
{
    switch (type) {
    case NodeType::Text: return u"text"_s;
    case NodeType::File: return u"file"_s;
    case NodeType::Link: return u"link"_s;
    case NodeType::Group: return u"group"_s;
    case NodeType::Unknown: break;
    }
    return {};
}

NodeType nodeTypeFromName(QStringView name)
{
    if (name == u"text")
        return NodeType::Text;
    if (name == u"file")
        return NodeType::File;
    if (name == u"link")
        return NodeType::Link;
    if (name == u"group")
        return NodeType::Group;
    return NodeType::Unknown;
}

QString sideName(Side side)
{
    switch (side) {
    case Side::Top: return u"top"_s;
    case Side::Right: return u"right"_s;
    case Side::Bottom: return u"bottom"_s;
    case Side::Left: return u"left"_s;
    }
    return {};
}

std::optional<Side> sideFromName(QStringView name)
{
    if (name == u"top")
        return Side::Top;
    if (name == u"right")
        return Side::Right;
    if (name == u"bottom")
        return Side::Bottom;
    if (name == u"left")
        return Side::Left;
    return std::nullopt;
}

Side oppositeSide(Side side)
{
    switch (side) {
    case Side::Top: return Side::Bottom;
    case Side::Right: return Side::Left;
    case Side::Bottom: return Side::Top;
    case Side::Left: return Side::Right;
    }
    return Side::Top;
}

double jsRound(double v)
{
    if (!std::isfinite(v))
        return v;
    return std::floor(v + 0.5);
}

void Node::setRect(const QRectF& r)
{
    x = jsRound(r.x());
    y = jsRound(r.y());
    width = jsRound(r.width());
    height = jsRound(r.height());
}

bool Node::sameAs(const Node& o) const
{
    return id == o.id && type == o.type && x == o.x && y == o.y && width == o.width && height == o.height && color == o.color
        && text == o.text && file == o.file && subpath == o.subpath && url == o.url && label == o.label
        && background == o.background && backgroundStyle == o.backgroundStyle && (type != NodeType::Unknown || source == o.source);
}

bool Edge::sameAs(const Edge& o) const
{
    return id == o.id && fromNode == o.fromNode && toNode == o.toNode && fromSide == o.fromSide && toSide == o.toSide
        && fromSideSet == o.fromSideSet && toSideSet == o.toSideSet && fromEnd == o.fromEnd && toEnd == o.toEnd && color == o.color
        && label == o.label && opaque == o.opaque && (!opaque || source == o.source);
}

Side autoSide(const QRectF& from, const QRectF& to)
{
    const QPointF n = from.center();
    const QPointF i = to.center();
    const double r = std::atan2(i.y() - n.y(), i.x() - n.x());
    const double o = std::atan2(from.height(), from.width());
    constexpr double pi = std::numbers::pi;
    if (r > -o && r <= o)
        return Side::Right;
    if (r > o && r <= pi - o)
        return Side::Bottom;
    if (r > pi - o || r <= -(pi - o))
        return Side::Left;
    return Side::Top;
}

// ---------------------------------------------------------------------------------------------
// CanvasData
// ---------------------------------------------------------------------------------------------

int CanvasData::nodeIndex(QStringView id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id && nodes[i].isShown())
            return i;
    return -1;
}

int CanvasData::edgeIndex(QStringView id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < edges.size(); ++i)
        if (edges[i].id == id && !edges[i].opaque)
            return i;
    return -1;
}

const Node* CanvasData::node(QStringView id) const
{
    const int i = nodeIndex(id);
    return i < 0 ? nullptr : &nodes[i];
}

Node* CanvasData::node(QStringView id)
{
    const int i = nodeIndex(id);
    return i < 0 ? nullptr : &nodes[i];
}

const Edge* CanvasData::edge(QStringView id) const
{
    const int i = edgeIndex(id);
    return i < 0 ? nullptr : &edges[i];
}

Edge* CanvasData::edge(QStringView id)
{
    const int i = edgeIndex(id);
    return i < 0 ? nullptr : &edges[i];
}

bool CanvasData::isEdgeShown(const Edge& e) const { return !e.opaque && node(e.fromNode) && node(e.toNode); }

bool CanvasData::sameAs(const CanvasData& o) const
{
    if (nodes.size() != o.nodes.size() || edges.size() != o.edges.size())
        return false;
    for (qsizetype i = 0; i < nodes.size(); ++i)
        if (!nodes[i].sameAs(o.nodes[i]))
            return false;
    for (qsizetype i = 0; i < edges.size(); ++i)
        if (!edges[i].sameAs(o.edges[i]))
            return false;
    return true;
}

QString newId()
{
    static const char hex[] = "0123456789abcdef";
    QString id(16, Qt::Uninitialized);
    for (int i = 0; i < 16; ++i)
        id[i] = QLatin1Char(hex[QRandomGenerator::global()->bounded(16)]);
    return id;
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------

namespace {

// Ids and colors written as numbers are read as their JavaScript text.
QString scalarText(const Json* v)
{
    if (!v)
        return {};
    if (v->isString())
        return v->toString();
    if (v->isNumber())
        return QString::fromLatin1(jsNumber(v->toDouble()));
    return {};
}

// JavaScript's ToNumber for Math.round(value) (NaN for missing members, arrays and objects).
double jsToNumber(const Json* v)
{
    if (!v)
        return std::nan("");
    switch (v->type()) {
    case Json::Type::Number:
        return v->toDouble();
    case Json::Type::Null:
        return 0;
    case Json::Type::Bool:
        return v->toBool() ? 1 : 0;
    case Json::Type::String: {
        const QString s = v->toString().trimmed();
        if (s.isEmpty())
            return 0;
        bool ok = false;
        const double d = s.toDouble(&ok);
        return ok ? d : std::nan("");
    }
    default:
        return std::nan("");
    }
}

// Obsidian's setData: Math.round(value), keeping the default when that is NaN.
double geometry(const Json& o, QStringView key, double fallback)
{
    const double d = jsToNumber(o.find(key));
    return std::isnan(d) ? fallback : jsRound(d);
}

QString stringMember(const Json& o, QStringView key) { return o.value(key).toString(); }

// Interpretations of edge ends: fromEnd defaults to none, toEnd to arrow; only "arrow" draws one.
EdgeEnd fromEndOf(const Json& o)
{
    const Json* v = o.find(u"fromEnd");
    return v && v->toString() == u"arrow" ? EdgeEnd::Arrow : EdgeEnd::None;
}

EdgeEnd toEndOf(const Json& o)
{
    const Json* v = o.find(u"toEnd");
    if (!v || v->isNull() || (v->isString() && v->toString().isEmpty()) || (v->isBool() && !v->toBool()))
        return EdgeEnd::Arrow; // `toEnd || "arrow"`
    return v->toString() == u"arrow" ? EdgeEnd::Arrow : EdgeEnd::None;
}

} // namespace

Node nodeFromJson(const Json& object)
{
    Node n;
    n.source = std::make_shared<const Json>(object);
    if (!object.isObject()) {
        n.type = NodeType::Unknown;
        return n;
    }
    n.id = scalarText(object.find(u"id"));
    n.type = nodeTypeFromName(stringMember(object, u"type"));
    n.x = geometry(object, u"x", 0);
    n.y = geometry(object, u"y", 0);
    n.width = geometry(object, u"width", 100);
    n.height = geometry(object, u"height", 100);
    n.color = scalarText(object.find(u"color"));
    switch (n.type) {
    case NodeType::Text:
        n.text = stringMember(object, u"text");
        break;
    case NodeType::File:
        n.file = stringMember(object, u"file");
        n.subpath = stringMember(object, u"subpath");
        break;
    case NodeType::Link:
        n.url = stringMember(object, u"url");
        break;
    case NodeType::Group:
        n.label = stringMember(object, u"label");
        n.background = stringMember(object, u"background");
        n.backgroundStyle = stringMember(object, u"backgroundStyle");
        break;
    case NodeType::Unknown:
        break;
    }
    return n;
}

Edge edgeFromJson(const Json& object)
{
    Edge e;
    e.source = std::make_shared<const Json>(object);
    if (!object.isObject()) {
        e.opaque = true;
        return e;
    }
    e.id = scalarText(object.find(u"id"));
    e.fromNode = scalarText(object.find(u"fromNode"));
    e.toNode = scalarText(object.find(u"toNode"));
    if (const auto s = sideFromName(stringMember(object, u"fromSide")))
        e.fromSide = *s;
    else
        e.fromSideSet = false;
    if (const auto s = sideFromName(stringMember(object, u"toSide")))
        e.toSide = *s;
    else
        e.toSideSet = false;
    e.fromEnd = fromEndOf(object);
    e.toEnd = toEndOf(object);
    e.color = scalarText(object.find(u"color"));
    e.label = stringMember(object, u"label");
    return e;
}

void resolveEdgeSides(CanvasData& data)
{
    for (Edge& e : data.edges) {
        if (e.opaque || (e.fromSideSet && e.toSideSet))
            continue;
        const Node* from = data.node(e.fromNode);
        const Node* to = data.node(e.toNode);
        if (!from || !to)
            continue;
        if (!e.fromSideSet)
            e.fromSide = autoSide(from->rect(), to->rect());
        if (!e.toSideSet)
            e.toSide = autoSide(to->rect(), from->rect());
    }
}

std::optional<CanvasData> parseCanvas(QByteArrayView bytes, QString* error)
{
    CanvasData data;
    qsizetype start = 0;
    if (bytes.size() >= 3 && quint8(bytes[0]) == 0xEF && quint8(bytes[1]) == 0xBB && quint8(bytes[2]) == 0xBF)
        start = 3;
    bool blank = true;
    for (qsizetype i = start; i < bytes.size() && blank; ++i)
        blank = bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n' || bytes[i] == '\r';
    if (blank)
        return data;
    JsonError err;
    std::optional<Json> root = parseJson(bytes, &err);
    if (!root) {
        if (error)
            *error = QStringLiteral("%1 (at byte %2)").arg(err.message).arg(err.offset);
        return std::nullopt;
    }
    if (!root->isObject()) {
        if (error)
            *error = QStringLiteral("The canvas file is not a JSON object");
        return std::nullopt;
    }
    QSet<QString> ids;
    for (const Json& item : root->value(u"nodes").items()) {
        Node n = nodeFromJson(item);
        if (n.isShown()) {
            if (n.id.isEmpty())
                n.id = newId(); // Obsidian gives id-less nodes a new id
            if (ids.contains(n.id))
                n.type = NodeType::Unknown; // a duplicate: kept verbatim, not shown
            else
                ids.insert(n.id);
        }
        data.nodes.append(std::move(n));
    }
    QSet<QString> edgeIds;
    for (const Json& item : root->value(u"edges").items()) {
        Edge e = edgeFromJson(item);
        if (!e.opaque) {
            if (e.id.isEmpty())
                e.id = newId();
            if (edgeIds.contains(e.id))
                e.opaque = true;
            else
                edgeIds.insert(e.id);
        }
        data.edges.append(std::move(e));
    }
    resolveEdgeSides(data);
    data.root = std::make_shared<const Json>(std::move(*root));
    return data;
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------

namespace {

// Sets a string member unless the source already says the same; an empty value removes the member
// (when `optional`) — a member that was already empty/missing stays as it is.
void mergeString(Json& o, const QString& key, const QString& value, const QString& current, bool optional)
{
    if (current == value)
        return;
    if (optional && value.isEmpty())
        o.remove(key);
    else
        o.insert(key, Json(value));
}

void mergeGeometry(Json& o, const QString& key, double value, double fallback)
{
    if (geometry(o, key, fallback) == value && o.contains(key))
        return;
    if (!o.contains(key) && value == fallback)
        return; // missing means the default
    o.insert(key, Json(value));
}

Json freshNodeJson(const Node& n)
{
    // Obsidian's getData(): {id, x, y, width, height, color?} + type-specific members.
    Json o = Json::object();
    o.insert(u"id"_s, Json(n.id));
    o.insert(u"x"_s, Json(n.x));
    o.insert(u"y"_s, Json(n.y));
    o.insert(u"width"_s, Json(n.width));
    o.insert(u"height"_s, Json(n.height));
    if (!n.color.isEmpty())
        o.insert(u"color"_s, Json(n.color));
    o.insert(u"type"_s, Json(nodeTypeName(n.type)));
    switch (n.type) {
    case NodeType::Text:
        o.insert(u"text"_s, Json(n.text));
        break;
    case NodeType::File:
        o.insert(u"file"_s, Json(n.file));
        if (!n.subpath.isEmpty())
            o.insert(u"subpath"_s, Json(n.subpath));
        break;
    case NodeType::Link:
        o.insert(u"url"_s, Json(n.url));
        break;
    case NodeType::Group:
        if (!n.label.isEmpty())
            o.insert(u"label"_s, Json(n.label));
        if (!n.background.isEmpty()) {
            o.insert(u"background"_s, Json(n.background));
            if (!n.backgroundStyle.isEmpty() && n.backgroundStyle != u"cover")
                o.insert(u"backgroundStyle"_s, Json(n.backgroundStyle));
        }
        break;
    case NodeType::Unknown:
        break;
    }
    return o;
}

Json freshEdgeJson(const Edge& e)
{
    Json o = Json::object();
    o.insert(u"id"_s, Json(e.id));
    o.insert(u"fromNode"_s, Json(e.fromNode));
    o.insert(u"fromSide"_s, Json(sideName(e.fromSide)));
    o.insert(u"toNode"_s, Json(e.toNode));
    o.insert(u"toSide"_s, Json(sideName(e.toSide)));
    if (e.fromEnd != EdgeEnd::None)
        o.insert(u"fromEnd"_s, Json("arrow"));
    if (e.toEnd != EdgeEnd::Arrow)
        o.insert(u"toEnd"_s, Json("none"));
    if (!e.color.isEmpty())
        o.insert(u"color"_s, Json(e.color));
    if (!e.label.isEmpty())
        o.insert(u"label"_s, Json(e.label));
    return o;
}

void mergeId(Json& o, const QString& key, const QString& value)
{
    if (scalarText(o.find(key)) == value)
        return;
    if (o.contains(key))
        o.insert(key, Json(value));
    else
        o.insertAt(0, key, Json(value));
}

} // namespace

Json nodeToJson(const Node& n)
{
    if (n.type == NodeType::Unknown)
        return n.source ? *n.source : Json();
    if (!n.source || !n.source->isObject())
        return freshNodeJson(n);
    Json o = *n.source;
    mergeId(o, u"id"_s, n.id);
    if (nodeTypeFromName(stringMember(o, u"type")) != n.type)
        o.insert(u"type"_s, Json(nodeTypeName(n.type)));
    mergeGeometry(o, u"x"_s, n.x, 0);
    mergeGeometry(o, u"y"_s, n.y, 0);
    mergeGeometry(o, u"width"_s, n.width, 100);
    mergeGeometry(o, u"height"_s, n.height, 100);
    mergeString(o, u"color"_s, n.color, scalarText(o.find(u"color")), true);
    switch (n.type) {
    case NodeType::Text:
        mergeString(o, u"text"_s, n.text, stringMember(o, u"text"), false);
        break;
    case NodeType::File:
        mergeString(o, u"file"_s, n.file, stringMember(o, u"file"), false);
        mergeString(o, u"subpath"_s, n.subpath, stringMember(o, u"subpath"), true);
        break;
    case NodeType::Link:
        mergeString(o, u"url"_s, n.url, stringMember(o, u"url"), false);
        break;
    case NodeType::Group:
        mergeString(o, u"label"_s, n.label, stringMember(o, u"label"), true);
        mergeString(o, u"background"_s, n.background, stringMember(o, u"background"), true);
        mergeString(o, u"backgroundStyle"_s, n.background.isEmpty() ? QString() : n.backgroundStyle,
                    stringMember(o, u"backgroundStyle"), true);
        break;
    case NodeType::Unknown:
        break;
    }
    return o;
}

Json edgeToJson(const Edge& e)
{
    if (e.opaque)
        return e.source ? *e.source : Json();
    if (!e.source || !e.source->isObject())
        return freshEdgeJson(e);
    Json o = *e.source;
    mergeId(o, u"id"_s, e.id);
    mergeId(o, u"fromNode"_s, e.fromNode);
    mergeId(o, u"toNode"_s, e.toNode);
    if (e.fromSideSet && sideFromName(stringMember(o, u"fromSide")) != e.fromSide)
        o.insert(u"fromSide"_s, Json(sideName(e.fromSide)));
    if (e.toSideSet && sideFromName(stringMember(o, u"toSide")) != e.toSide)
        o.insert(u"toSide"_s, Json(sideName(e.toSide)));
    if (fromEndOf(o) != e.fromEnd) {
        if (e.fromEnd == EdgeEnd::None)
            o.remove(u"fromEnd");
        else
            o.insert(u"fromEnd"_s, Json("arrow"));
    }
    if (toEndOf(o) != e.toEnd) {
        if (e.toEnd == EdgeEnd::Arrow)
            o.remove(u"toEnd");
        else
            o.insert(u"toEnd"_s, Json("none"));
    }
    mergeString(o, u"color"_s, e.color, scalarText(o.find(u"color")), true);
    mergeString(o, u"label"_s, e.label, stringMember(o, u"label"), true);
    return o;
}

FileFormat FileFormat::detect(QByteArrayView bytes)
{
    FileFormat f;
    f.bom = bytes.size() >= 3 && quint8(bytes[0]) == 0xEF && quint8(bytes[1]) == 0xBB && quint8(bytes[2]) == 0xBF;
    const qsizetype nl = bytes.indexOf('\n');
    f.crlf = nl > 0 && bytes[nl - 1] == '\r';
    f.finalNewline = !bytes.isEmpty() && bytes.back() == '\n';
    return f;
}

QByteArray serializeCanvas(const CanvasData& data, const FileFormat& format)
{
    Json root = data.root && data.root->isObject() ? *data.root : Json::object();
    Json nodes = Json::array();
    for (const Node& n : data.nodes)
        nodes.append(nodeToJson(n));
    Json edges = Json::array();
    for (const Edge& e : data.edges)
        edges.append(edgeToJson(e));
    root.insert(u"nodes"_s, std::move(nodes));
    root.insert(u"edges"_s, std::move(edges));
    QByteArray out = stringifyCanvas(root);
    if (format.crlf)
        out.replace("\n", "\r\n"); // string values never contain raw newlines
    if (format.finalNewline)
        out += format.crlf ? "\r\n" : "\n";
    if (format.bom)
        out.prepend("\xEF\xBB\xBF");
    return out;
}

} // namespace oc::canvas
