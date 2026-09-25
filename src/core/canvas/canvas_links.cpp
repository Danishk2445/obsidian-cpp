#include "core/canvas/canvas_links.h"

#include "core/canvas/canvas_model.h"
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

} // namespace oc::canvas
