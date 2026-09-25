#include "core/canvas/canvas_links.h"

#include "core/canvas/canvas_model.h"
#include "core/metadata/link_resolver.h"
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
