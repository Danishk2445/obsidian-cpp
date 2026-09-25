#pragma once

// The links of a canvas file, the way Obsidian's canvas index reports them to the metadata cache:
// file cards and group backgrounds are embeds of their file (+ subpath), and the links, embeds and
// property links written in text cards are links of the canvas. They count in resolvedLinks /
// unresolvedLinks (graph, backlink counts) like a note's.

#include "markdown/metadata.h"

#include <QByteArrayView>
#include <QList>

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

} // namespace oc::canvas
