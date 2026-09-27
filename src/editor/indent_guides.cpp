#include "editor/indent_guides.h"

#include "markdown/inline_rules.h"
#include "render/layout/decoration.h"

namespace oc::indent {

bool isJsSpace(QChar c) { return md::rules::isJsSpace(c.unicode()); }

namespace {

// /^([>\s]*)(([*+-] |(\d+)([.)] ))(?:\[(.)\] )?)?/ on a line: the lengths of group 1 (the prefix) and group 2
// (marker + task box, 0 when absent). Group 2 can only start where group 1 stops (a marker is neither '>' nor
// whitespace), so the greedy prefix is the only split.
struct ItemMatch {
    int prefix = 0;
    int marker = 0;
    bool matched() const { return prefix + marker > 0; } // m && m[0]
};

bool isLineTerminator(QChar c) { return c == u'\n' || c == u'\r' || c == QChar(0x2028) || c == QChar(0x2029); }

ItemMatch matchItem(QStringView s)
{
    ItemMatch m;
    int i = 0;
    while (i < s.size() && (s[i] == u'>' || isJsSpace(s[i])))
        ++i;
    m.prefix = i;
    int k = i;
    if (k + 1 < s.size() && (s[k] == u'*' || s[k] == u'+' || s[k] == u'-') && s[k + 1] == u' ') {
        k += 2;
    } else {
        int d = k;
        while (d < s.size() && md::rules::isAsciiDigit(s[d].unicode()))
            ++d;
        if (d > k && d + 1 < s.size() && (s[d] == u'.' || s[d] == u')') && s[d + 1] == u' ')
            k = d + 2;
    }
    if (k > i) {
        if (k + 3 < s.size() && s[k] == u'[' && !isLineTerminator(s[k + 1]) && s[k + 2] == u']' && s[k + 3] == u' ')
            k += 4;
        m.marker = k - i;
    }
    return m;
}

bool startsWithSpaces(QStringView s, int n)
{
    if (s.size() < n)
        return false;
    for (int i = 0; i < n; ++i)
        if (s[i] != u' ')
            return false;
    return true;
}

} // namespace

ActiveRange activeRange(const Text& text, int selectionLine)
{
    ActiveRange r;
    if (selectionLine < 0 || selectionLine >= text.lineCount())
        return r;
    int item = selectionLine;
    QStringView prefix;   // Obsidian's `o`: the item's [>\s]* prefix
    int markerSpaces = 0; // `a`: continuation lines indented by the marker's width belong to the item
    const QStringView line = text.lineView(item);
    const ItemMatch s = matchItem(line);
    if (s.matched()) {
        prefix = line.left(s.prefix);
        if (s.marker > 0) {
            markerSpaces = s.marker;
        } else {
            // a continuation line: walk up to the item it belongs to
            for (int l = selectionLine - 1; l >= 0;) {
                const QStringView c = text.lineView(l);
                const ItemMatch u = matchItem(c);
                if (!u.matched())
                    break;
                const QStringView up = c.left(u.prefix);
                if (u.marker > 0 || up != prefix) {
                    if (u.marker > 0 && prefix.startsWith(up) && u.prefix + u.marker <= prefix.size()) {
                        item = l;
                        prefix = up;
                        markerSpaces = u.marker;
                    }
                    break;
                }
                --l;
            }
        }
    }
    int p = item + 1;
    for (const int n = text.lineCount(); p < n;) {
        QStringView d = text.lineView(p);
        if (!d.startsWith(prefix))
            break;
        d = d.mid(prefix.size());
        if (markerSpaces > 0) {
            if (startsWithSpaces(d, markerSpaces)) {
                ++p;
                continue;
            }
            markerSpaces = 0;
        }
        if (!d.startsWith(u'\t') && !startsWithSpaces(d, 4))
            break;
        ++p;
    }
    r.itemLine = item;
    r.endLine = p;
    r.column = int(prefix.size());
    return r;
}

int hangingPrefix(QStringView line)
{
    const ItemMatch m = matchItem(line);
    return m.prefix + m.marker;
}

int lastSpacingUnit(QStringView line)
{
    int n = 0;
    while (n < line.size() && isJsSpace(line[n]))
        ++n;
    int last = -1;
    forEachUnit(line.left(n), [&](int from, int to, bool guide) {
        if (to > from) // (an empty mark makes no element)
            last = guide ? -1 : from;
    });
    return last;
}

QList<QPair<int, int>> changedLines(const ActiveRange& a, const ActiveRange& b)
{
    QList<QPair<int, int>> out;
    auto add = [&](int from, int to) {
        if (to > from)
            out.push_back({from, to});
    };
    const int a0 = a.itemLine + 1, a1 = a.endLine, b0 = b.itemLine + 1, b1 = b.endLine;
    if (a.isEmpty() && b.isEmpty())
        return out;
    if (a.isEmpty() || b.isEmpty() || a.column != b.column || a1 <= b0 || b1 <= a0) {
        add(a0, a1);
        add(b0, b1);
        return out;
    }
    // same column, overlapping: only the lines in one range but not the other change
    add(std::min(a0, b0), std::max(a0, b0));
    add(std::min(a1, b1), std::max(a1, b1));
    return out;
}

void decorateLine(QStringView line, int lineNo, const ActiveRange& active, qreal offsetEm, const StyleSheet& styles, LineDecoration& d)
{
    const LineMetrics& lm = styles.line(d.lineStyle);
    const qreal em = lm.font.pixelSize() > 0 ? lm.font.pixelSize() : styles.textSize();
    const qreal minWidth = styles.textSize() > 0 ? styles.listIndent() / styles.textSize() * em : styles.listIndent();
    const bool inRange = active.contains(lineNo);
    bool tabs = false;
    forEachUnit(line, [&](int from, int to, bool guide) {
        if (!guide)
            return;
        d.guideColumns.push_back(from);
        if (inRange && from == active.column)
            d.activeGuideColumn = from;
        if (to - from == 1) {
            tabs = true;
            return;
        }
        const qreal natural = 4 * lm.spaceWidth;
        if (minWidth > natural + 0.01) {
            StyleSpec spec;
            spec.format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            spec.format.setFontLetterSpacing(minWidth - natural);
            d.marks.push_back({to - 1, to, StyleId::Normal, d.addCustomStyle(spec)});
        }
    });
    if (d.guideColumns.isEmpty())
        return;
    if (tabs) {
        const qreal tab = d.tabStop > 0 ? d.tabStop : lm.spaceWidth * styles.tabSizeSpaces();
        if (tab < minWidth)
            d.tabStop = minWidth;
    }
    d.guideOffset = offsetEm * em + 1;
}

} // namespace oc::indent
