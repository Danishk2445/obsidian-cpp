#pragma once

#include <QString>

#include <optional>

namespace oc::md {
struct CachedMetadata;
}

namespace oc {

// The part of a note a link subpath points at (Obsidian's resolveSubpath):
//   "#Heading" / "#Parent#Child"  the heading's section: from the heading line to the line before the
//                                  next heading of the same or a higher level (or the end)
//   "#^block-id"                   the block's lines; a list item includes its nested items
//   "#[^1]"                        a footnote definition
// Heading segments match case-insensitively after normalizing like Obsidian (":#|^\\ %% [[ ]]" ->
// spaces, whitespace collapsed); each segment must be deeper than the previous match.
struct SubpathRange {
    enum class Type { Heading, Block, Footnote };
    Type type = Type::Heading;
    int startLine = 0;
    int endLine = -1; // inclusive; -1 = to the end of the note
    int level = 0;    // heading level
};

std::optional<SubpathRange> resolveSubpath(const md::CachedMetadata& meta, const QString& subpath);

} // namespace oc
