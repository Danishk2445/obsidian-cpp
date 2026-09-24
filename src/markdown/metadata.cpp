#include "markdown/metadata.h"

#include <yaml-cpp/yaml.h>

#include <QRegularExpression>
#include <QUrl>

namespace oc::md {

namespace {

Pos posAt(const Text& t, qsizetype off)
{
    Pos p;
    p.offset = off;
    p.line = t.lineAt(off);
    p.col = int(off - t.lineStart(p.line));
    return p;
}

Range rangeOf(const Text& t, qsizetype a, qsizetype b) { return Range{posAt(t, a), posAt(t, b)}; }

Range lineRange(const Text& t, int fromLine, int toLineExcl)
{
    return rangeOf(t, t.lineStart(fromLine), t.lineEnd(std::max(fromLine, toLineExcl - 1)));
}

QVariant yamlToVariant(const YAML::Node& n)
{
    switch (n.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
        return QVariant();
    case YAML::NodeType::Scalar: {
        const std::string s = n.Scalar();
        if (n.Tag() == "!") // quoted scalar: always a string
            return QString::fromStdString(s);
        const QString q = QString::fromStdString(s);
        if (q == u"true" || q == u"True" || q == u"TRUE")
            return true;
        if (q == u"false" || q == u"False" || q == u"FALSE")
            return false;
        if (q == u"null" || q == u"~")
            return QVariant();
        bool ok = false;
        const double d = q.toDouble(&ok);
        if (ok && !q.isEmpty() && !q.startsWith(u'0') && !q.contains(u' ') && !q.endsWith(u'.'))
            return d;
        if (ok && (q == u"0" || q.startsWith(u"0.")))
            return d;
        return q;
    }
    case YAML::NodeType::Sequence: {
        QVariantList l;
        for (const auto& it : n)
            l.push_back(yamlToVariant(it));
        return l;
    }
    case YAML::NodeType::Map: {
        QVariantMap m;
        for (const auto& it : n)
            m.insert(QString::fromStdString(it.first.as<std::string>()), yamlToVariant(it.second));
        return m;
    }
    }
    return QVariant();
}

QString sectionType(const SyntaxTree& tree, int id)
{
    switch (tree.node(id).type) {
    case NodeType::AtxHeading:
    case NodeType::SetextHeading:
        return QStringLiteral("heading");
    case NodeType::Paragraph:
        return QStringLiteral("paragraph");
    case NodeType::BulletList:
    case NodeType::OrderedList:
        return QStringLiteral("list");
    case NodeType::FencedCode:
    case NodeType::IndentedCode:
        return QStringLiteral("code");
    case NodeType::BlockQuote:
        return QStringLiteral("blockquote");
    case NodeType::Callout:
        return QStringLiteral("callout");
    case NodeType::Table:
        return QStringLiteral("table");
    case NodeType::ThematicBreak:
        return QStringLiteral("thematicBreak");
    case NodeType::HtmlBlock:
        return QStringLiteral("html");
    case NodeType::MathBlock:
        return QStringLiteral("math");
    case NodeType::CommentBlock:
        return QStringLiteral("comment");
    case NodeType::Frontmatter:
        return QStringLiteral("yaml");
    case NodeType::FootnoteDef:
        return QStringLiteral("footnoteDefinition");
    case NodeType::LinkRefDef:
        return QStringLiteral("definition");
    default:
        return QStringLiteral("paragraph");
    }
}

void collectInline(const SyntaxTree& tree, const Text& text, const LeafContent& content, CachedMetadata& md, QString* blockIdOut)
{
    const InlineTreePtr it = tree.inlineTree(content);
    if (!it)
        return;
    for (int id = 0; id < it->size(); ++id) {
        const InlineNode& n = it->node(id);
        const qsizetype a = content.toDoc(n.from, text), b = content.toDoc(n.to, text);
        switch (n.type) {
        case InlineType::WikiLink:
        case InlineType::Embed: {
            const WikiLinkInfo& wl = it->wikiLink(id);
            LinkCache lc;
            lc.link = wl.target + wl.subpath;
            lc.original = it->content().mid(n.from, n.to - n.from);
            lc.displayText = wl.displayText(); // alias, else "Note > Heading" like Obsidian
            lc.position = rangeOf(text, a, b);
            lc.embed = n.type == InlineType::Embed;
            (lc.embed ? md.embeds : md.links).push_back(lc);
            break;
        }
        case InlineType::Link:
        case InlineType::Image: {
            const LinkInfo& li = it->link(id);
            static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
            if (li.destination.isEmpty() || scheme.match(li.destination).hasMatch())
                break; // external links are not part of the vault graph
            LinkCache lc;
            lc.link = QUrl::fromPercentEncoding(li.destination.toUtf8());
            lc.original = it->content().mid(n.from, n.to - n.from);
            // the link text: between the LinkOpen and LinkClose marks
            int textFrom = n.from, textTo = n.to;
            for (int c = n.firstChild; c >= 0; c = it->node(c).next) {
                if (it->node(c).mark == MarkKind::LinkOpen)
                    textFrom = it->node(c).to;
                else if (it->node(c).mark == MarkKind::LinkClose)
                    textTo = it->node(c).from;
            }
            lc.displayText = it->content().mid(textFrom, textTo - textFrom);
            lc.position = rangeOf(text, a, b);
            lc.embed = n.type == InlineType::Image;
            lc.markdown = true;
            (lc.embed ? md.embeds : md.links).push_back(lc);
            break;
        }
        case InlineType::Tag:
            md.tags.push_back({QLatin1Char('#') + it->tag(id).tag, rangeOf(text, a, b)});
            break;
        case InlineType::FootnoteRef:
            md.footnoteRefs.push_back({it->footnote(id).label, rangeOf(text, a, b)});
            break;
        case InlineType::BlockId:
            if (blockIdOut)
                *blockIdOut = it->blockId(id);
            break;
        default:
            break;
        }
    }
}

} // namespace

QList<FrontmatterValue> parseFrontmatter(const QString& yaml, QString* error)
{
    QList<FrontmatterValue> out;
    try {
        const YAML::Node root = YAML::Load(yaml.toStdString());
        if (!root.IsMap())
            return out;
        for (const auto& it : root) {
            FrontmatterValue v;
            v.key = QString::fromStdString(it.first.as<std::string>());
            v.value = yamlToVariant(it.second);
            out.push_back(v);
        }
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
    }
    return out;
}

QStringList CachedMetadata::allTags() const
{
    QStringList out;
    auto addTag = [&](QString t) {
        t = t.trimmed();
        if (t.isEmpty())
            return;
        if (!t.startsWith(u'#'))
            t.prepend(u'#');
        out << t;
    };
    const QVariant fm = property(QStringLiteral("tags")).isValid() ? property(QStringLiteral("tags")) : property(QStringLiteral("tag"));
    if (fm.typeId() == QMetaType::QVariantList) {
        for (const QVariant& v : fm.toList())
            addTag(v.toString());
    } else if (fm.isValid()) {
        for (const QString& t : fm.toString().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
            addTag(t);
    }
    for (const TagCache& t : tags)
        out << t.tag;
    return out;
}

QStringList CachedMetadata::aliases() const
{
    QStringList out;
    QVariant v = property(QStringLiteral("aliases"));
    if (!v.isValid())
        v = property(QStringLiteral("alias"));
    if (v.typeId() == QMetaType::QVariantList) {
        for (const QVariant& a : v.toList())
            if (!a.toString().trimmed().isEmpty())
                out << a.toString().trimmed();
    } else if (v.isValid() && !v.toString().trimmed().isEmpty()) {
        for (const QString& a : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
            out << a.trimmed();
    }
    return out;
}

QVariant CachedMetadata::property(const QString& key) const
{
    if (!frontmatter)
        return {};
    for (const FrontmatterValue& v : *frontmatter)
        if (v.key.compare(key, Qt::CaseInsensitive) == 0)
            return v.value.isValid() ? v.value : QVariant(QString());
    return {};
}

CachedMetadata extractMetadata(const SyntaxTree& tree, const Text& text)
{
    CachedMetadata md;
    // Frontmatter
    const int fm = tree.frontmatter();
    if (fm >= 0) {
        const BlockNode& n = tree.node(fm);
        md.frontmatterPosition = lineRange(text, n.fromLine, n.toLine);
        const QString yaml = n.toLine - n.fromLine >= 2 ? text.slice(text.lineStart(n.fromLine + 1), text.lineEnd(n.toLine - 2)) : QString();
        md.frontmatter = parseFrontmatter(yaml, &md.frontmatterError);
        // links inside property values
        static const QRegularExpression wl(QStringLiteral("\\[\\[([^\\[\\]|#]*)(#[^\\[\\]|]*)?(?:\\|([^\\[\\]]*))?\\]\\]"));
        for (int line = n.fromLine + 1; line < n.toLine - 1; ++line) {
            const QString l = text.line(line);
            auto it = wl.globalMatch(l);
            while (it.hasNext()) {
                const auto m = it.next();
                LinkCache lc;
                lc.link = m.captured(1).trimmed() + m.captured(2);
                lc.original = m.captured(0);
                lc.displayText = m.captured(3).isEmpty() ? lc.link : m.captured(3);
                const qsizetype a = text.lineStart(line) + m.capturedStart(0);
                lc.position = rangeOf(text, a, a + m.capturedLength(0));
                md.frontmatterLinks.push_back(lc);
            }
        }
    }

    // Sections: top-level blocks.
    for (int id = tree.firstChild(0); id >= 0; id = tree.nextSibling(id)) {
        const BlockNode& n = tree.node(id);
        SectionCache s;
        s.type = sectionType(tree, id);
        s.position = lineRange(text, n.fromLine, n.toLine);
        md.sections.push_back(s);
    }

    // Walk all nodes.
    for (int id = 1; id < tree.nodeCount(); ++id) {
        const BlockNode& n = tree.node(id);
        switch (n.type) {
        case NodeType::AtxHeading:
        case NodeType::SetextHeading: {
            const HeadingInfo h = tree.heading(id);
            HeadingCache hc;
            hc.level = h.level;
            hc.heading = text.lineView(n.fromLine).mid(h.textFrom, h.textTo - h.textFrom).toString().trimmed();
            hc.position = lineRange(text, n.fromLine, n.toLine);
            md.headings.push_back(hc);
            QString bid;
            collectInline(tree, text, tree.leafContent(id, text), md, &bid);
            break;
        }
        case NodeType::Paragraph: {
            QString bid;
            collectInline(tree, text, tree.leafContent(id, text), md, &bid);
            if (!bid.isEmpty()) {
                // A paragraph that is only "^id" names the previous block (Obsidian convention).
                const QString trimmed = text.slice(text.lineStart(n.fromLine), text.lineEnd(n.toLine - 1)).trimmed();
                Range r = lineRange(text, n.fromLine, n.toLine);
                if (trimmed == QLatin1Char('^') + bid) {
                    for (int prev = id - 1; prev > 0; --prev) {
                        if (tree.node(prev).toLine <= n.fromLine && tree.node(prev).depth == n.depth) {
                            r = lineRange(text, tree.node(prev).fromLine, tree.node(prev).toLine);
                            break;
                        }
                    }
                }
                md.blocks.insert(bid.toLower(), BlockCache{bid, r});
                // attach to the containing list item / section
                for (ListItemCache& li : md.listItems)
                    if (li.position.start.line <= n.fromLine && li.position.end.line >= n.toLine - 1)
                        li.id = bid;
                for (SectionCache& sc : md.sections)
                    if (sc.position.start.line <= n.fromLine && sc.position.end.line >= n.toLine - 1)
                        sc.id = bid;
            }
            break;
        }
        case NodeType::Table: {
            for (int line = n.fromLine; line < n.toLine; ++line) {
                if (tree.role(line) == LineRole::TableDelimiter)
                    continue;
                const int cells = int(tree.tableCells(line, text).size());
                for (int c = 0; c < cells; ++c)
                    collectInline(tree, text, tree.tableCellContent(line, c, text), md, nullptr);
            }
            break;
        }
        case NodeType::ListItem: {
            ListItemCache li;
            li.position = lineRange(text, n.fromLine, n.toLine);
            if (n.flags & FlagTask)
                li.task = tree.listItem(id).taskChar;
            // parent: nearest enclosing ListItem, else -(list start line)
            int p = tree.parent(id); // the list
            const int listStart = p >= 0 ? tree.node(p).fromLine : n.fromLine;
            int pp = p >= 0 ? tree.parent(p) : -1;
            li.parent = (pp >= 0 && tree.node(pp).type == NodeType::ListItem) ? tree.node(pp).fromLine : -listStart;
            md.listItems.push_back(li);
            break;
        }
        case NodeType::FootnoteDef:
            md.footnotes.push_back({tree.footnoteDef(id).label, lineRange(text, n.fromLine, n.toLine)});
            break;
        default:
            break;
        }
    }
    return md;
}

} // namespace oc::md
