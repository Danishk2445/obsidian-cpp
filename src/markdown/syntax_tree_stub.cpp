// TEMPORARY STUB (lead engineer's tree only) so the editor can be developed before the real
// incremental parser lands. Recognizes blank lines, ATX headings, fenced code, quotes and bullet
// list markers with a simple full re-parse. Replaced by the real implementation at merge time.
#include "markdown/syntax_tree.h"

#include <QRegularExpression>

namespace oc::md {

bool isContainer(NodeType t)
{
    switch (t) {
    case NodeType::Document:
    case NodeType::BlockQuote:
    case NodeType::Callout:
    case NodeType::BulletList:
    case NodeType::OrderedList:
    case NodeType::ListItem:
    case NodeType::FootnoteDef:
        return true;
    default:
        return false;
    }
}
bool isLeaf(NodeType t) { return !isContainer(t); }
const char* nodeTypeName(NodeType) { return "node"; }
const char* lineRoleName(LineRole) { return "role"; }

struct SyntaxTree::Impl {
    QList<LineRole> roles;
    QList<BlockNode> nodes;
    QList<int> leafOfLine;
    QList<HeadingInfo> headingInfos;
    QList<FenceInfo> fenceInfos;
    QList<QList<ContainerMarker>> markers;
    QList<LinkRefDefinition> refs;
};

SyntaxTree::SyntaxTree() : d(std::make_unique<Impl>()) { parse(Text()); }
SyntaxTree::~SyntaxTree() = default;
SyntaxTree::SyntaxTree(const SyntaxTree& o) : d(std::make_unique<Impl>(*o.d)) {}
SyntaxTree& SyntaxTree::operator=(const SyntaxTree& o)
{
    *d = *o.d;
    return *this;
}

void SyntaxTree::parse(const Text& text)
{
    Impl& s = *d;
    s = Impl();
    const int n = text.lineCount();
    s.roles.resize(n);
    s.leafOfLine.fill(-1, n);
    s.markers.resize(n);
    s.nodes.push_back({NodeType::Document, 0, 0, 0, n, -1});
    static const QRegularExpression atx(QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+|$)"));
    static const QRegularExpression fenceRe(QStringLiteral("^ {0,3}(`{3,}|~{3,})\\s*(\\S*)"));
    static const QRegularExpression quote(QStringLiteral("^ {0,3}>[ ]?"));
    static const QRegularExpression bullet(QStringLiteral("^([ \\t]*)([-*+]|\\d{1,9}[.)])([ \\t]+|$)"));
    int i = 0;
    while (i < n) {
        const QString line = text.line(i);
        if (line.trimmed().isEmpty()) {
            s.roles[i] = LineRole::Blank;
            ++i;
            continue;
        }
        if (auto m = fenceRe.match(line); m.hasMatch()) {
            FenceInfo fi;
            fi.ch = m.captured(1)[0];
            fi.length = int(m.capturedLength(1));
            fi.language = m.captured(2).toLower();
            fi.info = m.captured(2);
            fi.infoFrom = int(m.capturedStart(2));
            fi.infoTo = int(m.capturedEnd(2));
            const int start = i;
            s.roles[i++] = LineRole::FenceOpen;
            while (i < n) {
                const QString l = text.line(i);
                if (l.trimmed().startsWith(QString(fi.length, fi.ch))) {
                    s.roles[i++] = LineRole::FenceClose;
                    fi.closed = true;
                    break;
                }
                s.roles[i++] = LineRole::FenceBody;
            }
            const int id = int(s.nodes.size());
            s.fenceInfos.push_back(fi);
            s.nodes.push_back({NodeType::FencedCode, 1, quint16(fi.closed ? 0 : FlagUnclosed), start, i, int(s.fenceInfos.size() - 1)});
            for (int k = start; k < i; ++k)
                s.leafOfLine[k] = id;
            continue;
        }
        if (auto m = atx.match(line); m.hasMatch()) {
            HeadingInfo hi;
            hi.level = int(m.capturedLength(1));
            hi.markerFrom = int(m.capturedStart(1));
            hi.markerTo = int(m.capturedEnd(0));
            hi.textFrom = hi.markerTo;
            hi.textTo = int(line.size());
            s.headingInfos.push_back(hi);
            const int id = int(s.nodes.size());
            s.nodes.push_back({NodeType::AtxHeading, 1, 0, i, i + 1, int(s.headingInfos.size() - 1)});
            s.roles[i] = LineRole::AtxHeading;
            s.leafOfLine[i] = id;
            ++i;
            continue;
        }
        if (auto m = quote.match(line); m.hasMatch())
            s.markers[i].push_back({ContainerMarker::Kind::Quote, int(m.capturedStart(0)), int(m.capturedEnd(0)), -1});
        else if (auto b = bullet.match(line); b.hasMatch())
            s.markers[i].push_back({ContainerMarker::Kind::ListMarker, int(b.capturedStart(2)), int(b.capturedEnd(0)), -1});
        const int id = int(s.nodes.size());
        s.nodes.push_back({NodeType::Paragraph, 1, 0, i, i + 1, -1});
        s.roles[i] = LineRole::Paragraph;
        s.leafOfLine[i] = id;
        ++i;
    }
}

TreeUpdate SyntaxTree::update(const Text& newText, const ChangeDesc&, const ParseBudget&)
{
    const int old = lineCount();
    parse(newText);
    TreeUpdate u;
    u.fromLine = 0;
    u.toLine = lineCount();
    u.oldToLine = old;
    u.lineDelta = lineCount() - old;
    u.structural = true;
    u.headingsChanged = true;
    return u;
}

bool SyntaxTree::resume(const Text&, qint64, TreeUpdate*) { return false; }
int SyntaxTree::validUpTo() const { return lineCount(); }
int SyntaxTree::lineCount() const { return int(d->roles.size()); }
LineRole SyntaxTree::role(int line) const { return d->roles.value(line, LineRole::Blank); }
QList<ContainerMarker> SyntaxTree::containerMarkers(int line) const { return d->markers.value(line); }
qint32 SyntaxTree::contentColumn(int line) const
{
    const auto m = d->markers.value(line);
    return m.isEmpty() ? 0 : m.last().to;
}
int SyntaxTree::leafAt(int line) const { return d->leafOfLine.value(line, -1); }
int SyntaxTree::nodeCount() const { return int(d->nodes.size()); }
const BlockNode& SyntaxTree::node(int id) const { return d->nodes[id]; }
int SyntaxTree::parent(int id) const { return id == 0 ? -1 : 0; }
int SyntaxTree::firstChild(int id) const { return id == 0 && d->nodes.size() > 1 ? 1 : -1; }
int SyntaxTree::nextSibling(int id) const { return id > 0 && id + 1 < d->nodes.size() ? id + 1 : -1; }
int SyntaxTree::deepestAt(int line) const
{
    const int l = leafAt(line);
    return l < 0 ? 0 : l;
}
QList<int> SyntaxTree::ancestorsAt(int line) const
{
    const int l = leafAt(line);
    return l < 0 ? QList<int>{0} : QList<int>{0, l};
}
int SyntaxTree::firstNodeAtOrAfter(int line) const
{
    for (int i = 1; i < d->nodes.size(); ++i)
        if (d->nodes[i].fromLine >= line)
            return i;
    return nodeCount();
}
QList<int> SyntaxTree::headings() const
{
    QList<int> out;
    for (int i = 0; i < d->nodes.size(); ++i)
        if (d->nodes[i].type == NodeType::AtxHeading)
            out.push_back(i);
    return out;
}
HeadingInfo SyntaxTree::heading(int id) const { return d->headingInfos.value(d->nodes[id].data); }
FenceInfo SyntaxTree::fence(int id) const { return d->fenceInfos.value(d->nodes[id].data); }
ListInfo SyntaxTree::list(int) const { return {}; }
ListItemInfo SyntaxTree::listItem(int) const { return {}; }
CalloutInfo SyntaxTree::callout(int) const { return {}; }
TableInfo SyntaxTree::table(int) const { return {}; }
QList<TableCell> SyntaxTree::tableCells(int, const Text&) const { return {}; }
FootnoteDefInfo SyntaxTree::footnoteDef(int) const { return {}; }
int SyntaxTree::htmlBlockKind(int) const { return 0; }
const QList<LinkRefDefinition>& SyntaxTree::linkRefDefs() const { return d->refs; }
int SyntaxTree::frontmatter() const { return -1; }
QString SyntaxTree::dump(const Text&) const { return QStringLiteral("(stub)"); }
bool SyntaxTree::equivalent(const SyntaxTree&, const SyntaxTree&, QString*) { return true; }

} // namespace oc::md
