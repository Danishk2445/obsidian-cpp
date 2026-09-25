#pragma once

// Obsidian's Outline view model (core plugin "outline"): the heading tree of a note, the heading of
// the section a cursor line is in, the display text of a heading, and the text change that drags a
// heading's section to another place.

#include <QList>
#include <QString>

#include <optional>

namespace oc::md {
struct HeadingCache;
}

namespace oc {

struct OutlineNode {
    int index = -1; // into the headings list
    int level = 1;
    int line = 0;   // the heading's first line
    QString heading; // as written (CachedMetadata heading text)
    QString text;    // display text (markdown stripped)
    QList<OutlineNode> children;
};

// A heading becomes the child of the closest preceding heading with a lower level (Obsidian pops
// headings of the same or a higher level off its stack).
QList<OutlineNode> buildOutline(const QList<md::HeadingCache>& headings);

// The heading whose section contains `line`: the last heading starting at or before it (-1: none).
int headingIndexAtLine(const QList<md::HeadingCache>& headings, int line);

// Obsidian's heading display text: the heading rendered as plain text (emphasis, highlight, strike and
// code markers dropped, links shown as their text, comments and HTML tags removed); the raw heading
// when nothing is left.
QString headingDisplayText(const QString& heading);

// Dragging the section of heading `from` (the heading and everything up to the next heading of the
// same or a higher level) before heading `target` (after the last heading when target == size).
// Content offsets are those of the headings' positions ('\n'-joined text). Returns the replacement
// of content[from, to) that performs the move, or nullopt when nothing moves (dropped onto itself or
// inside the dragged section). Like Obsidian, the moved section and the text left before it end with
// a blank line.
struct TextEdit {
    int from = 0;
    int to = 0;
    QString text;
};
std::optional<TextEdit> moveSectionEdit(const QString& content, const QList<md::HeadingCache>& headings, int from, int target);
// Obsidian's drop placement: "before" heading `node` or "after" it (the next heading's slot).
inline int outlineDropTarget(int node, bool after) { return after ? node + 1 : node; }

} // namespace oc
