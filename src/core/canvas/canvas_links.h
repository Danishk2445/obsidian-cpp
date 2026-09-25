#pragma once

// The links of a canvas file, the way Obsidian's canvas index reports them to the metadata cache:
// file cards and group backgrounds are embeds of their file (+ subpath), and the links, embeds and
// property links written in text cards are links of the canvas. They count in resolvedLinks /
// unresolvedLinks (graph, backlink counts) like a note's.

#include "core/util/match_range.h"
#include "markdown/metadata.h"

#include <QByteArrayView>
#include <QList>

#include <functional>

namespace oc::canvas {

struct CanvasTextCardMeta {
    QString nodeId;
    md::CachedMetadata meta; // positions are in the card's text
};

struct CanvasLinks {
    QList<md::LinkCache> embeds;         // file cards and group backgrounds: link = path + subpath
    QList<CanvasTextCardMeta> textCards; // metadata of each text card that has links
    // Every reference (Obsidian's iterateReferencesForFile): the cards' links, embeds and property
    // links, then the file embeds.
    QList<md::LinkCache> all() const;
};

// Unreadable or invalid canvas files have no links.
CanvasLinks canvasLinks(QByteArrayView bytes);

struct CanvasData;

// The text cards of a canvas as one text (cards in file order, separated by a blank line), for search
// and backlink snippets; `nodeAt` maps an offset back to its card.
struct CanvasText {
    QString text;
    QList<std::pair<QString, qsizetype>> cards; // node id, offset of the card's text
    QString nodeAt(qsizetype offset) const;
};
CanvasText canvasText(const CanvasData& data);

// Obsidian's canvas backlinks (the backlink pane): the ranges in canvasText() of the text-card links
// whose link path `resolvesToTarget`, and whether a file card or group background points there.
struct CanvasMentions {
    CanvasText text;
    QList<MatchRange> ranges;
    bool embedded = false;
};
CanvasMentions canvasLinkedMentions(QByteArrayView bytes, const std::function<bool(const QString& linkpath)>& resolvesToTarget);

} // namespace oc::canvas
