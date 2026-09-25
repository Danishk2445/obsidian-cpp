#include "core/canvas/canvas_links.h"

#include "core/canvas/canvas_model.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/link_updater.h"
#include "core/metadata/mentions.h"
#include "markdown/syntax_tree.h"
#include "text/text.h"

namespace oc::canvas {

QList<md::LinkCache> CanvasLinks::all() const
{
    QList<md::LinkCache> out;
    for (const CanvasTextCardMeta& c : textCards)
        out << c.meta.links << c.meta.embeds << c.meta.frontmatterLinks;
    out << embeds;
    return out;
}

CanvasLinks canvasLinks(QByteArrayView bytes)
{
    CanvasLinks out;
    const std::optional<CanvasData> data = parseCanvas(bytes);
    if (!data)
        return out;
    for (const Node& n : data->nodes) {
        switch (n.type) {
        case NodeType::File:
            if (!n.file.isEmpty()) {
                md::LinkCache l;
                l.link = n.file + n.subpath;
                l.embed = true;
                out.embeds << l;
            }
            break;
        case NodeType::Group:
            if (!n.background.isEmpty()) {
                md::LinkCache l;
                l.link = n.background;
                l.embed = true;
                out.embeds << l;
            }
            break;
        case NodeType::Text: {
            if (n.text.isEmpty())
                break;
            const Text text = Text::fromString(n.text);
            md::SyntaxTree tree;
            tree.parse(text);
            md::CachedMetadata meta = md::extractMetadata(tree, text);
            if (!meta.links.isEmpty() || !meta.embeds.isEmpty() || !meta.frontmatterLinks.isEmpty())
                out.textCards << CanvasTextCardMeta{n.id, std::move(meta)};
            break;
        }
        default:
            break;
        }
    }
    return out;
}

QString CanvasText::nodeAt(qsizetype offset) const
{
    QString id;
    for (const auto& [node, start] : cards)
        if (start <= offset)
            id = node;
    return id;
}

CanvasText canvasText(const CanvasData& data)
{
    CanvasText out;
    for (const Node& n : data.nodes) {
        if (n.type != NodeType::Text)
            continue;
        if (!out.cards.isEmpty())
            out.text += QStringLiteral("\n\n");
        out.cards.append({n.id, out.text.size()});
        out.text += n.text;
    }
    return out;
}

int updateTextCardLinks(CanvasData& data, const QString& canvasPath, const LinkResolver& before, const LinkResolver& after,
                        const std::function<QString(const QString& oldPath)>& moved, LinkResolver::LinkFormat format)
{
    int changed = 0;
    for (Node& n : data.nodes) {
        if (n.type != NodeType::Text || n.text.isEmpty())
            continue;
        Text text = Text::fromString(n.text);
        md::SyntaxTree tree;
        tree.parse(text);
        const md::CachedMetadata meta = md::extractMetadata(tree, text);
        if (meta.links.isEmpty() && meta.embeds.isEmpty() && meta.frontmatterLinks.isEmpty())
            continue;
        // The card is a source at the canvas' path (links resolve from the canvas' folder).
        const QList<LinkUpdater::Reference> refs = LinkUpdater::snapshot(before, {canvasPath}, [&](const QString&) { return &meta; });
        const QList<LinkUpdater::Edit> edits = LinkUpdater::edits(refs, after, moved, format).value(moved(canvasPath));
        if (edits.isEmpty())
            continue;
        const int applied = LinkUpdater::apply(text, edits);
        if (applied == 0)
            continue;
        QString out;
        for (int i = 0; i < text.lineCount(); ++i)
            out += text.lineView(i) + eolString(text.lineEol(i));
        n.text = out;
        changed += applied;
    }
    return changed;
}

CanvasMentions canvasLinkedMentions(QByteArrayView bytes, const std::function<bool(const QString& linkpath)>& resolvesToTarget)
{
    CanvasMentions out;
    const std::optional<CanvasData> data = parseCanvas(bytes);
    if (!data)
        return out;
    out.text = canvasText(*data);
    qsizetype card = 0;
    for (const Node& n : data->nodes) {
        switch (n.type) {
        case NodeType::Text: {
            const qsizetype base = out.text.cards[card++].second;
            if (n.text.isEmpty())
                break;
            const Text text = Text::fromString(n.text);
            md::SyntaxTree tree;
            tree.parse(text);
            const md::CachedMetadata meta = md::extractMetadata(tree, text);
            for (const MatchRange& r : linkedMentionRanges(meta, resolvesToTarget))
                out.ranges.append({int(base + r.from), int(base + r.to)});
            break;
        }
        case NodeType::File:
            if (!n.file.isEmpty() && resolvesToTarget(n.file))
                out.embedded = true;
            break;
        case NodeType::Group:
            if (!n.background.isEmpty() && resolvesToTarget(n.background))
                out.embedded = true;
            break;
        default:
            break;
        }
    }
    return out;
}

} // namespace oc::canvas
