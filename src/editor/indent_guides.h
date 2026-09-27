#pragma once

#include "markdown/inline_rules.h"
#include "text/text.h"

#include <QStringView>

namespace oc {

struct LineDecoration;
class StyleSheet;

// Obsidian's indentation guides in the editor (app.json "showIndentGuide", on by default). Obsidian installs a
// CodeMirror plugin only while the option is on; for every visible line it walks the leading `[\s>]+`:
//  - '>' (and one space after it) is skipped;
//  - a TAB, or exactly four spaces, is one indent unit (.cm-indent): an inline-block at least --list-indent wide
//    whose ::before draws the guide, 1px wide, --indentation-guide-color, spanning the whole line (wrapped lines
//    and list padding included), offset by --indentation-guide-editing-indent (0.85em, 0 inside code blocks) in
//    Live Preview and --indentation-guide-source-indent (0.25em) in Source mode;
//  - a shorter run of spaces is .cm-indent-spacing (no guide), and the character after it is skipped.
// The unit at the prefix column of the list item owning the main selection is .cm-active-indent
// (--indentation-guide-color-active) on the lines of the item's body (see activeRange()).
namespace indent {

// Calls fn(from, to, guide) for each unit of the line's leading [\s>]+, in order: guide = true for an indent unit
// (TAB / four spaces), false for spacing (possibly empty). Quote markers produce nothing.
template <class F>
void forEachUnit(QStringView line, F&& fn)
{
    int n = 0;
    while (n < line.size() && (line[n] == u'>' || md::rules::isJsSpace(line[n].unicode())))
        ++n;
    for (int b = 0; b < n;) {
        const QChar k = line[b];
        if (k == u'>') {
            ++b;
            if (b < n && line[b] == u' ')
                ++b;
        } else if (k == u'\t') {
            fn(b, b + 1, true);
            ++b;
        } else {
            int t = b;
            while (t < b + 4 && t < n && line[t] == u' ')
                ++t;
            if (t < b + 4) {
                // Obsidian's walk: a spacing mark up to the first non-space, then that character is skipped too.
                fn(b, t, false);
                b = t + 1;
                continue;
            }
            fn(b, b + 4, true);
            b += 4;
        }
    }
}

// The list item owning the main selection (Obsidian's walk with /^([>\s]*)(([*+-] |(\d+)([.)] ))(?:\[(.)\] )?)?/):
// from the selection's line (a continuation line walks up to its item), then down while the lines are
// continuation lines indented by the marker's width or deeper (a TAB / four spaces after the item's prefix).
// Lines strictly between itemLine and endLine draw the unit that starts at `column` (the item's prefix length)
// as the active guide. Cost: the lines walked (the item's body), once per selection / document change.
struct ActiveRange {
    int itemLine = -1;
    int endLine = -1; // exclusive
    int column = 0;
    bool contains(int line) const { return line > itemLine && line < endLine; }
    bool isEmpty() const { return endLine <= itemLine + 1; }
    bool operator==(const ActiveRange&) const = default;
};
ActiveRange activeRange(const Text& text, int selectionLine);

// Lines whose active guide differs between two ranges, as [from, to) runs.
QList<QPair<int, int>> changedLines(const ActiveRange& a, const ActiveRange& b);

// Appends the line's guides to d.guideColumns (unit start columns), marks the active one (d.activeGuideColumn),
// sets d.guideOffset (px: the CSS margin plus the ::before's 1px content width) and applies .cm-indent's
// min-width: var(--list-indent) (in ems of the line's font): a four-space unit gets the missing width as letter
// spacing on its last space, a TAB outside list lines a tab stop of that width (list lines already use it).
// `offsetEm`: the margin in ems of the line's font (0.85 Live Preview, 0 in its code blocks, 0.25 Source).
void decorateLine(QStringView line, int lineNo, const ActiveRange& active, qreal offsetEm, const StyleSheet& styles, LineDecoration& d);

} // namespace indent
} // namespace oc
