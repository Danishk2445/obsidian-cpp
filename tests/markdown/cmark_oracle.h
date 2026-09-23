#pragma once

// Differential-testing helpers: run cmark / cmark-gfm (--to xml --sourcepos) and compare their block
// structure with oc::md::SyntaxTree after normalising both trees the same way:
//  * line ranges are 0-based [from, to);
//  * trailing whitespace-only lines are trimmed from code blocks and containers (cmark includes the
//    blank lines that preceded the line closing a container; our containers exclude them), but a
//    container never ends before its last child;
//  * cmark's fenced code closed by its container ends on the closing line (off by one): clamped to
//    the parent's range first;
//  * cmark-gfm's table sourcepos starts at the paragraph the header came from: fixed from the rows;
//  * our LinkRefDef nodes are merged into the paragraph that follows them (cmark keeps one
//    paragraph node) or dropped (cmark removes refdef-only paragraphs).

#include "markdown/syntax_tree.h"
#include "text/text.h"

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include <functional>
#include <memory>
#include <vector>

namespace octest {

struct NB {
    QString type;
    int from = -1, to = -1; // 0-based, exclusive end; -1 when cmark gave no sourcepos
    int fromCol = 0;        // 0-based start column (cmark)
    bool mergeable = false; // ours: a paragraph / setext heading that can follow its refdefs
    QString attrs;
    std::vector<NB> kids;
};

inline bool cmarkAvailable(const QString& tool) { return QFile::exists(QStringLiteral("/usr/bin/") + tool); }

inline QStringList cmarkArgs(const QString& tool)
{
    QStringList a{QStringLiteral("--to"), QStringLiteral("xml"), QStringLiteral("--sourcepos")};
    if (tool == QLatin1String("cmark-gfm"))
        a << QStringLiteral("-e") << QStringLiteral("table");
    return a;
}

// Run the tool on many documents concurrently (one process each, `parallel` at a time).
inline std::vector<QByteArray> runCmarkBatch(const QString& tool, const std::vector<QString>& docs, int parallel = 16)
{
    std::vector<QByteArray> out(docs.size());
    size_t next = 0;
    while (next < docs.size()) {
        std::vector<std::unique_ptr<QProcess>> procs;
        const size_t first = next;
        for (; next < docs.size() && int(procs.size()) < parallel; ++next) {
            auto p = std::make_unique<QProcess>();
            p->start(QStringLiteral("/usr/bin/") + tool, cmarkArgs(tool));
            p->write(docs[next].toUtf8());
            p->closeWriteChannel();
            procs.push_back(std::move(p));
        }
        for (size_t i = 0; i < procs.size(); ++i) {
            procs[i]->waitForFinished(20000);
            out[first + i] = procs[i]->readAllStandardOutput();
        }
    }
    return out;
}

inline bool isXmlBlock(QStringView n)
{
    static const QStringList blocks = {QStringLiteral("document"),     QStringLiteral("block_quote"),  QStringLiteral("list"),
                                       QStringLiteral("item"),         QStringLiteral("paragraph"),    QStringLiteral("heading"),
                                       QStringLiteral("thematic_break"), QStringLiteral("code_block"), QStringLiteral("html_block"),
                                       QStringLiteral("custom_block"), QStringLiteral("table"),        QStringLiteral("table_header"),
                                       QStringLiteral("table_row")};
    return blocks.contains(n.toString());
}

inline NB readXmlBlock(QXmlStreamReader& r)
{
    NB b;
    b.type = r.name().toString();
    const QXmlStreamAttributes at = r.attributes();
    const QString sp = at.value(QStringLiteral("sourcepos")).toString();
    if (!sp.isEmpty()) {
        // "l1:c1-l2:c2"
        const QStringList parts = sp.split(QLatin1Char('-'));
        if (parts.size() == 2) {
            b.from = parts[0].section(QLatin1Char(':'), 0, 0).toInt() - 1;
            b.fromCol = std::max(0, parts[0].section(QLatin1Char(':'), 1, 1).toInt() - 1);
            b.to = parts[1].section(QLatin1Char(':'), 0, 0).toInt();
        }
    }
    if (b.type == QLatin1String("list")) {
        const bool ordered = at.value(QStringLiteral("type")) == QLatin1String("ordered");
        b.attrs = (ordered ? QStringLiteral("ordered") : QStringLiteral("bullet"))
                  + (at.value(QStringLiteral("tight")) == QLatin1String("true") ? QStringLiteral(" tight") : QStringLiteral(" loose"));
        if (ordered)
            b.attrs += QStringLiteral(" start=") + (at.hasAttribute(QStringLiteral("start")) ? at.value(QStringLiteral("start")).toString() : QStringLiteral("1"));
    } else if (b.type == QLatin1String("heading")) {
        b.attrs = QStringLiteral("level=") + at.value(QStringLiteral("level")).toString();
    }
    while (r.readNextStartElement()) {
        if (isXmlBlock(r.name()))
            b.kids.push_back(readXmlBlock(r));
        else
            r.skipCurrentElement();
    }
    return b;
}

inline NB parseCmarkXml(const QByteArray& xml)
{
    QXmlStreamReader r(xml);
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("document"))
            return readXmlBlock(r);
    }
    return NB{QStringLiteral("error")};
}

// Blank for a block nested in `quotes` block quotes: only whitespace and at most that many '>'.
inline bool blankLine(const oc::Text& t, int line, int quotes = 0)
{
    if (line < 0 || line >= t.lineCount())
        return true;
    int gt = 0;
    for (QChar c : t.lineView(line)) {
        if (c == u'>')
            ++gt;
        else if (c != u' ' && c != u'\t')
            return false;
    }
    return gt <= quotes;
}

// Shared normalisation (both trees): code blocks and containers lose trailing blank lines; the
// empty line after a final newline (our Text has it, cmark doesn't count it) is never part of a
// block.
inline void normalizeRanges(NB& b, const oc::Text& t, int parentTo, int quotes = 0)
{
    const int realLines = t.lineCount() - (t.lineCount() > 1 && t.lineLength(t.lineCount() - 1) == 0 ? 1 : 0);
    if (b.type != QLatin1String("document"))
        b.to = std::min(b.to, std::max(realLines, b.from + 1));
    const bool container = b.type == QLatin1String("block_quote") || b.type == QLatin1String("list") || b.type == QLatin1String("item");
    if (b.type == QLatin1String("code_block") && parentTo >= 0)
        b.to = std::min(b.to, parentTo);
    const int inner = quotes + (b.type == QLatin1String("block_quote") ? 1 : 0);
    for (NB& k : b.kids)
        normalizeRanges(k, t, b.to, inner);
    if (b.type == QLatin1String("code_block") || container) {
        // code: trailing lines blank inside all enclosing quotes; containers: blank for them
        while (b.to > b.from + 1 && blankLine(t, b.to - 1, quotes))
            --b.to;
        if (container)
            for (const NB& k : b.kids)
                b.to = std::max(b.to, k.to);
    }
}

// A setext underline line (possibly behind container markers).
inline bool underlineLike(QStringView line)
{
    int e = int(line.size());
    while (e > 0 && (line[e - 1] == u' ' || line[e - 1] == u'\t'))
        --e;
    if (e == 0 || (line[e - 1] != u'=' && line[e - 1] != u'-'))
        return false;
    const QChar c = line[e - 1];
    int b = e;
    while (b > 0 && line[b - 1] == c)
        --b;
    for (int i = 0; i < b; ++i) {
        const QChar x = line[i];
        if (x != u' ' && x != u'\t' && x != u'>' && x != u'-' && x != u'*' && x != u'+' && x != u'.' && x != u')' && !x.isDigit())
            return false;
    }
    return true;
}

inline int htmlKindAt(QStringView s)
{
    auto ci = [&](const char* lit) { return s.startsWith(QLatin1StringView(lit), Qt::CaseInsensitive); };
    for (const char* t : {"<script", "<pre", "<style", "<textarea"})
        if (ci(t))
            return 1;
    if (ci("<!--"))
        return 2;
    if (ci("<?"))
        return 3;
    if (ci("<![CDATA["))
        return 5;
    if (s.size() > 2 && s[0] == u'<' && s[1] == u'!' && s[2].isLetter())
        return 4;
    return 0;
}

inline bool htmlEndOn(int kind, QStringView s)
{
    switch (kind) {
    case 1:
        for (const char* t : {"</script>", "</pre>", "</style>", "</textarea>"})
            if (s.contains(QLatin1StringView(t), Qt::CaseInsensitive))
                return true;
        return false;
    case 2:
        return s.contains(QLatin1StringView("-->"));
    case 3:
        return s.contains(QLatin1StringView("?>"));
    case 4:
        return s.contains(u'>');
    case 5:
        return s.contains(QLatin1StringView("]]>"));
    }
    return false;
}

// cmark sourcepos quirks (not structural): thematic breaks absorb the blank lines before the next
// block, setext headings end on the line after the underline, and HTML blocks closed by their end
// condition end one line early.
inline void fixCmarkQuirks(NB& b, const oc::Text& t, int parentTo)
{
    if (b.type == QLatin1String("thematic_break") && b.from >= 0) {
        b.to = b.from + 1;
    } else if (b.type == QLatin1String("heading") && b.to - b.from >= 3 && b.to - 2 < t.lineCount()) {
        // setext heading followed by another line: cmark ends it on that line
        if (underlineLike(t.lineView(b.to - 2)))
            b.to -= 1;
    } else if (b.type == QLatin1String("html_block") && b.from >= 0 && b.from < t.lineCount()) {
        const int kind = htmlKindAt(t.lineView(b.from).mid(std::min<qsizetype>(b.fromCol, t.lineView(b.from).size())));
        if (kind >= 1 && kind <= 5 && b.to < t.lineCount()) {
            QStringView endLine = t.lineView(b.to);
            if (b.to == b.from)
                endLine = endLine.mid(std::min<qsizetype>(b.fromCol, endLine.size()));
            if (htmlEndOn(kind, endLine))
                b.to = std::min(b.to + 1, parentTo < 0 ? b.to + 1 : parentTo);
        }
    }
    for (NB& k : b.kids)
        fixCmarkQuirks(k, t, b.to);
}

// cmark-gfm table quirks: sourcepos of a table (and of the paragraph inserted before it).
inline void fixGfmTables(NB& b)
{
    for (size_t i = 0; i < b.kids.size(); ++i) {
        NB& k = b.kids[i];
        fixGfmTables(k);
        if (k.type != QLatin1String("table"))
            continue;
        int firstRow = -1;
        for (const NB& row : k.kids)
            if (row.type == QLatin1String("table_row")) {
                firstRow = row.from;
                break;
            }
        const int delimiter = firstRow >= 0 ? firstRow - 1 : k.to - 1; // 0-based
        const int header = delimiter - 1;
        if (i > 0 && b.kids[i - 1].type == QLatin1String("paragraph") && b.kids[i - 1].from < 0) {
            b.kids[i - 1].from = k.from;
            b.kids[i - 1].to = header;
        }
        k.from = header;
        k.kids.clear();
    }
}

inline NB ourTree(const oc::md::SyntaxTree& tree, int id, bool* obsidian)
{
    using oc::md::NodeType;
    const oc::md::BlockNode& n = tree.node(id);
    NB b;
    b.from = n.fromLine;
    b.to = n.toLine;
    switch (n.type) {
    case NodeType::Document:
        b.type = QStringLiteral("document");
        break;
    case NodeType::Callout:
        *obsidian = true;
        [[fallthrough]];
    case NodeType::BlockQuote:
        b.type = QStringLiteral("block_quote");
        break;
    case NodeType::BulletList:
    case NodeType::OrderedList: {
        const oc::md::ListInfo li = tree.list(id);
        b.type = QStringLiteral("list");
        b.attrs = (li.ordered ? QStringLiteral("ordered") : QStringLiteral("bullet")) + (li.tight ? QStringLiteral(" tight") : QStringLiteral(" loose"));
        if (li.ordered)
            b.attrs += QStringLiteral(" start=%1").arg(li.start);
        break;
    }
    case NodeType::ListItem:
        b.type = QStringLiteral("item");
        break;
    case NodeType::Paragraph:
    case NodeType::LinkRefDef:
        b.type = n.type == NodeType::Paragraph ? QStringLiteral("paragraph") : QStringLiteral("refdef");
        b.mergeable = true;
        break;
    case NodeType::AtxHeading:
    case NodeType::SetextHeading:
        b.type = QStringLiteral("heading");
        b.attrs = QStringLiteral("level=%1").arg(tree.heading(id).level);
        b.mergeable = n.type == NodeType::SetextHeading;
        break;
    case NodeType::ThematicBreak:
        b.type = QStringLiteral("thematic_break");
        break;
    case NodeType::FencedCode:
    case NodeType::IndentedCode:
        b.type = QStringLiteral("code_block");
        break;
    case NodeType::HtmlBlock:
        b.type = QStringLiteral("html_block");
        break;
    case NodeType::Table:
        b.type = QStringLiteral("table");
        break;
    default:
        *obsidian = true;
        b.type = QStringLiteral("obsidian:") + QLatin1StringView(oc::md::nodeTypeName(n.type));
        break;
    }
    for (int c = tree.firstChild(id); c >= 0; c = tree.nextSibling(c))
        b.kids.push_back(ourTree(tree, c, obsidian));
    // LinkRefDef: merge into the following paragraph / heading, keep before a table (cmark-gfm
    // leaves the paragraph before a table header unresolved), else drop.
    std::vector<NB> kids;
    for (size_t i = 0; i < b.kids.size(); ++i) {
        NB& k = b.kids[i];
        if (k.type != QLatin1String("refdef")) {
            kids.push_back(std::move(k));
            continue;
        }
        if (i + 1 < b.kids.size() && b.kids[i + 1].from == k.to) {
            NB& nx = b.kids[i + 1];
            if (nx.mergeable && (nx.type == QLatin1String("paragraph") || nx.type == QLatin1String("heading"))) {
                nx.from = k.from;
                continue;
            }
            if (nx.type == QLatin1String("table")) {
                k.type = QStringLiteral("paragraph");
                kids.push_back(std::move(k));
                continue;
            }
        }
    }
    b.kids = std::move(kids);
    return b;
}

inline QString dumpNB(const NB& b, int depth = 0)
{
    QString s = QString(depth * 2, QLatin1Char(' ')) + b.type + QStringLiteral(" [%1,%2)").arg(b.from).arg(b.to);
    if (!b.attrs.isEmpty())
        s += QLatin1Char(' ') + b.attrs;
    s += QLatin1Char('\n');
    for (const NB& k : b.kids)
        s += dumpNB(k, depth + 1);
    return s;
}

// Deepest last descendant of `x` reached through lists/items only, when it ends where x ends.
inline const NB* lastLeafThroughLists(const NB& x)
{
    const NB* c = &x;
    while ((c->type == QLatin1String("list") || c->type == QLatin1String("item")) && !c->kids.empty() && c->kids.back().to == c->to)
        c = &c->kids.back();
    return c;
}

// Looseness of one of our lists, optionally with cmark's quirk that blank lines directly after a
// thematic break are swallowed by it (cmark keeps the break open; the spec and remark count them).
inline bool looseWithHrQuirk(const NB& list)
{
    auto gap = [](const NB& a, const NB& b) {
        if (a.to >= b.from)
            return false;
        return lastLeafThroughLists(a)->type != QLatin1String("thematic_break");
    };
    for (size_t i = 0; i < list.kids.size(); ++i) {
        const NB& item = list.kids[i];
        if (i + 1 < list.kids.size() && gap(item, list.kids[i + 1]))
            return true;
        for (size_t j = 0; j + 1 < item.kids.size(); ++j)
            if (gap(item.kids[j], item.kids[j + 1]))
                return true;
    }
    return false;
}

// cmark-gfm quirk: a table without body rows "ends with a blank line" (the parser advanced to the
// end of the delimiter row), which makes the list loose when anything follows it in the list.
inline bool looseByTableQuirk(const NB& list)
{
    auto rowless = [](const NB& n) {
        const NB* c = lastLeafThroughLists(n);
        return c->type == QLatin1String("table") && c->to - c->from == 2;
    };
    for (size_t i = 0; i < list.kids.size(); ++i) {
        const NB& item = list.kids[i];
        for (size_t j = 0; j < item.kids.size(); ++j) {
            const bool followed = j + 1 < item.kids.size() || i + 1 < list.kids.size();
            if (followed && rowless(item.kids[j]))
                return true;
        }
    }
    return false;
}

// Structural comparison; list tight/loose differences explained by the two cmark quirks above are
// tolerated (the spec / remark semantics are kept on purpose, see docs/PARITY.md).
inline bool sameNB(const NB& a, const NB& b, bool compareDocRange, int* quirks = nullptr)
{
    if (a.type != b.type || a.kids.size() != b.kids.size())
        return false;
    if (compareDocRange && (a.from != b.from || a.to != b.to))
        return false;
    if (a.attrs != b.attrs) {
        QString x = a.attrs, y = b.attrs;
        x.replace(QLatin1String("loose"), QLatin1String("tight"));
        y.replace(QLatin1String("loose"), QLatin1String("tight"));
        if (a.type != QLatin1String("list") || x != y)
            return false;
        const bool oursLoose = a.attrs.contains(QLatin1String("loose"));
        const bool explained = oursLoose ? !looseWithHrQuirk(a) : looseByTableQuirk(a);
        if (!explained)
            return false;
        if (quirks)
            ++*quirks;
    }
    for (size_t i = 0; i < a.kids.size(); ++i)
        if (!sameNB(a.kids[i], b.kids[i], true, quirks))
            return false;
    return true;
}

struct Comparison {
    bool same = false;
    bool obsidian = false; // our tree contains Obsidian-only syntax
    int quirks = 0;        // tolerated cmark tightness quirks
    QString ours, theirs;
};

inline Comparison compareWithCmark(const QString& md, const QByteArray& xml, bool gfm)
{
    Comparison c;
    const oc::Text text = oc::Text::fromString(md);
    oc::md::SyntaxTree tree;
    tree.parse(text);
    NB mine = ourTree(tree, 0, &c.obsidian);
    NB theirs = parseCmarkXml(xml);
    if (gfm)
        fixGfmTables(theirs);
    fixCmarkQuirks(theirs, text, -1);
    normalizeRanges(mine, text, -1);
    normalizeRanges(theirs, text, -1);
    c.same = sameNB(mine, theirs, false, &c.quirks);
    c.ours = dumpNB(mine);
    c.theirs = dumpNB(theirs);
    return c;
}

} // namespace octest
