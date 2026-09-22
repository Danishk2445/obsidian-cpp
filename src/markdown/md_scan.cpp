#include "markdown/md_scan.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace oc::md::scan {

namespace {

// cmark's "spacechar" for HTML tags and table rows also admits \v and \f.
inline bool isTableSpace(QChar c) { return c == u' ' || c == u'\t' || c == u'\v' || c == u'\f'; }
// cmark_isspace: the character after a list marker (end of line counts as space).
inline bool isCmarkSpace(QChar c) { return c == u' ' || c == u'\t' || c == u'\v' || c == u'\f' || c == u'\r' || c == u'\n'; }

inline QChar at(QStringView s, qsizetype i) { return i < s.size() ? s[i] : QChar(); }

bool startsWithAt(QStringView s, int pos, QLatin1StringView lit)
{
    if (pos + lit.size() > s.size())
        return false;
    return s.mid(pos, lit.size()) == lit;
}

bool startsWithCiAt(QStringView s, int pos, QLatin1StringView lit)
{
    if (pos + lit.size() > s.size())
        return false;
    return s.mid(pos, lit.size()).compare(lit, Qt::CaseInsensitive) == 0;
}

bool containsCi(QStringView s, int from, QLatin1StringView lit)
{
    if (from >= s.size())
        return false;
    return s.mid(from).indexOf(lit, 0, Qt::CaseInsensitive) >= 0;
}

bool contains(QStringView s, int from, QLatin1StringView lit)
{
    if (from >= s.size())
        return false;
    return s.mid(from).indexOf(lit) >= 0;
}

// CommonMark 0.31.2 HTML block start condition 6 tag names (sorted for binary search).
constexpr std::array<const char*, 62> kBlockTags = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col",
    "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5",
    "h6", "head", "header", "hr", "html", "iframe", "legend", "li", "link", "main",
    "menu", "menuitem", "nav", "noframes", "ol", "optgroup", "option", "p", "param", "search",
    "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead", "title", "tr",
    "track", "ul"};

bool isBlockTag(QStringView name)
{
    if (name.isEmpty() || name.size() > 10)
        return false;
    char buf[16];
    for (qsizetype i = 0; i < name.size(); ++i) {
        const char16_t u = name[i].unicode();
        if (u >= u'A' && u <= u'Z')
            buf[i] = char(u - u'A' + 'a');
        else if (u < 128)
            buf[i] = char(u);
        else
            return false;
    }
    buf[name.size()] = 0;
    const auto it = std::lower_bound(kBlockTags.begin(), kBlockTags.end(), buf,
                                     [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    return it != kBlockTags.end() && std::strcmp(*it, buf) == 0;
}

bool isTagNameStart(QChar c) { return isAsciiLetter(c); }
bool isTagNameChar(QChar c) { return isAsciiLetter(c) || isAsciiDigit(c) || c == u'-'; }
bool isAttrNameStart(QChar c) { return isAsciiLetter(c) || c == u'_' || c == u':'; }
bool isAttrNameChar(QChar c) { return isAsciiLetter(c) || isAsciiDigit(c) || c == u'_' || c == u'.' || c == u':' || c == u'-'; }

// Complete open tag at pos (s[pos] == '<'); returns the end position or -1. Like cmark, type-1
// tag names are not excluded here (they are caught by start condition 1 first when it applies).
int scanOpenTag(QStringView s, int pos)
{
    const int n = int(s.size());
    int i = pos + 1;
    if (i >= n || !isTagNameStart(s[i]))
        return -1;
    while (i < n && isTagNameChar(s[i]))
        ++i;
    for (;;) {
        const int wsFrom = i;
        while (i < n && isTableSpace(s[i]))
            ++i;
        const bool hadSpace = i > wsFrom;
        if (i < n && s[i] == u'>')
            return i + 1;
        if (i + 1 < n && s[i] == u'/' && s[i + 1] == u'>')
            return i + 2;
        if (!hadSpace || i >= n || !isAttrNameStart(s[i]))
            return -1;
        while (i < n && isAttrNameChar(s[i]))
            ++i;
        // optional value spec
        int j = i;
        while (j < n && isTableSpace(s[j]))
            ++j;
        if (j < n && s[j] == u'=') {
            ++j;
            while (j < n && isTableSpace(s[j]))
                ++j;
            if (j >= n)
                return -1;
            const QChar q = s[j];
            if (q == u'"' || q == u'\'') {
                const int close = int(s.mid(j + 1).indexOf(q));
                if (close < 0)
                    return -1;
                j = j + 1 + close + 1;
            } else {
                const int vFrom = j;
                while (j < n) {
                    const QChar c = s[j];
                    if (isCmarkSpace(c) || c == u'"' || c == u'\'' || c == u'=' || c == u'<' || c == u'>' || c == u'`')
                        break;
                    ++j;
                }
                if (j == vFrom)
                    return -1;
            }
            i = j;
        }
    }
}

int scanClosingTag(QStringView s, int pos)
{
    const int n = int(s.size());
    int i = pos + 1;
    if (i >= n || s[i] != u'/')
        return -1;
    ++i;
    if (i >= n || !isTagNameStart(s[i]))
        return -1;
    while (i < n && isTagNameChar(s[i]))
        ++i;
    while (i < n && isTableSpace(s[i]))
        ++i;
    if (i < n && s[i] == u'>')
        return i + 1;
    return -1;
}

bool isTableSpaceOnlyFrom(QStringView s, int from)
{
    for (qsizetype i = from; i < s.size(); ++i)
        if (!isTableSpace(s[i]))
            return false;
    return true;
}

// One delimiter cell of a table delimiter row: spaces? ':'? '-'+ ':'? spaces?. Returns end or -1.
int scanDelimiterCell(QStringView s, int i)
{
    const int n = int(s.size());
    while (i < n && isTableSpace(s[i]))
        ++i;
    if (i < n && s[i] == u':')
        ++i;
    const int dashFrom = i;
    while (i < n && s[i] == u'-')
        ++i;
    if (i == dashFrom)
        return -1;
    if (i < n && s[i] == u':')
        ++i;
    while (i < n && isTableSpace(s[i]))
        ++i;
    return i;
}

// Iterate the cells of a table row (cmark-gfm row_from_string on a single line). fn(cellFrom, cellTo).
template<typename Fn>
int forEachCell(QStringView s, int pos, Fn&& fn)
{
    const int n = int(s.size());
    int offset = pos;
    if (offset < n && s[offset] == u'|') {
        ++offset;
        while (offset < n && isTableSpace(s[offset]))
            ++offset;
    }
    int cells = 0;
    while (offset < n) {
        int i = offset;
        while (i < n) {
            if (s[i] == u'\\' && i + 1 < n && s[i + 1] == u'|')
                i += 2;
            else if (s[i] == u'|')
                break;
            else
                ++i;
        }
        const int cellEnd = i;
        int pipeLen = 0;
        if (i < n && s[i] == u'|') {
            pipeLen = 1;
            while (i + pipeLen < n && isTableSpace(s[i + pipeLen]))
                ++pipeLen;
        }
        if (cellEnd > offset || pipeLen > 0) {
            ++cells;
            fn(offset, cellEnd);
        }
        offset = cellEnd + pipeLen;
        if (pipeLen == 0)
            break;
    }
    return cells;
}

} // namespace

bool isBlankFrom(QStringView s, int from)
{
    for (qsizetype i = from; i < s.size(); ++i)
        if (!isSpaceOrTab(s[i]))
            return false;
    return true;
}

int skipSpaces(QStringView s, int from)
{
    const int n = int(s.size());
    while (from < n && isSpaceOrTab(s[from]))
        ++from;
    return from;
}

int rtrim(QStringView s, int from, int to)
{
    while (to > from && isSpaceOrTab(s[to - 1]))
        --to;
    return to;
}

bool thematicBreak(QStringView s, int pos)
{
    const QChar c = at(s, pos);
    if (c != u'*' && c != u'-' && c != u'_')
        return false;
    int count = 0;
    for (qsizetype i = pos; i < s.size(); ++i) {
        if (s[i] == c)
            ++count;
        else if (!isSpaceOrTab(s[i]))
            return false;
    }
    return count >= 3;
}

int atxStart(QStringView s, int pos, int* after)
{
    const int n = int(s.size());
    int i = pos;
    while (i < n && s[i] == u'#')
        ++i;
    const int level = i - pos;
    if (level < 1 || level > 6)
        return 0;
    if (i < n && !isSpaceOrTab(s[i]))
        return 0;
    if (after)
        *after = skipSpaces(s, i);
    return level;
}

int openFence(QStringView s, int pos, QChar* ch)
{
    const QChar c = at(s, pos);
    if (c != u'`' && c != u'~')
        return 0;
    const int n = int(s.size());
    int i = pos;
    while (i < n && s[i] == c)
        ++i;
    const int len = i - pos;
    if (len < 3)
        return 0;
    if (c == u'`') {
        for (int j = i; j < n; ++j)
            if (s[j] == u'`')
                return 0;
    }
    if (ch)
        *ch = c;
    return len;
}

int closeFence(QStringView s, int pos, QChar ch)
{
    if (at(s, pos) != ch)
        return 0;
    const int n = int(s.size());
    int i = pos;
    while (i < n && s[i] == ch)
        ++i;
    const int len = i - pos;
    if (len < 3 || !isBlankFrom(s, i))
        return 0;
    return len;
}

int setextLine(QStringView s, int pos)
{
    const QChar c = at(s, pos);
    if (c != u'=' && c != u'-')
        return 0;
    const int n = int(s.size());
    int i = pos;
    while (i < n && s[i] == c)
        ++i;
    if (!isBlankFrom(s, i))
        return 0;
    return c == u'=' ? 1 : 2;
}

int htmlBlockStart(QStringView s, int pos, bool allow7)
{
    const int n = int(s.size());
    if (at(s, pos) != u'<')
        return 0;
    const int i = pos + 1;
    if (startsWithAt(s, i, QLatin1StringView("!--")))
        return 2;
    if (startsWithAt(s, i, QLatin1StringView("![CDATA[")))
        return 5;
    if (at(s, i) == u'!' && isAsciiLetter(at(s, i + 1)))
        return 4;
    if (at(s, i) == u'?')
        return 3;
    for (const char* tag : {"script", "pre", "style", "textarea"}) {
        const QLatin1StringView t(tag);
        if (startsWithCiAt(s, i, t)) {
            const int j = i + int(t.size());
            if (j >= n || isTableSpace(s[j]) || s[j] == u'>')
                return 1;
        }
    }
    {
        int j = i;
        if (at(s, j) == u'/')
            ++j;
        const int nameFrom = j;
        while (j < n && (isAsciiLetter(s[j]) || isAsciiDigit(s[j])))
            ++j;
        if (j > nameFrom && isBlockTag(s.mid(nameFrom, j - nameFrom))) {
            if (j >= n || isTableSpace(s[j]) || s[j] == u'>' || (s[j] == u'/' && at(s, j + 1) == u'>'))
                return 6;
        }
    }
    if (allow7) {
        int end = scanOpenTag(s, pos);
        if (end < 0)
            end = scanClosingTag(s, pos);
        if (end >= 0 && isTableSpaceOnlyFrom(s, end))
            return 7;
    }
    return 0;
}

bool htmlBlockEnd(int kind, QStringView s, int pos)
{
    switch (kind) {
    case 1:
        return containsCi(s, pos, QLatin1StringView("</script>")) || containsCi(s, pos, QLatin1StringView("</pre>"))
            || containsCi(s, pos, QLatin1StringView("</style>")) || containsCi(s, pos, QLatin1StringView("</textarea>"));
    case 2:
        return contains(s, pos, QLatin1StringView("-->"));
    case 3:
        return contains(s, pos, QLatin1StringView("?>"));
    case 4:
        return contains(s, pos, QLatin1StringView(">"));
    case 5:
        return contains(s, pos, QLatin1StringView("]]>"));
    default:
        return false;
    }
}

int listMarker(QStringView s, int pos, bool interruptsParagraph, ListMarker* out)
{
    const int n = int(s.size());
    const QChar c = at(s, pos);
    if (c == u'*' || c == u'-' || c == u'+') {
        const int i = pos + 1;
        if (i < n && !isCmarkSpace(s[i]))
            return 0;
        if (interruptsParagraph && isBlankFrom(s, i))
            return 0;
        if (out) {
            out->ordered = false;
            out->ch = c;
            out->start = 0;
            out->length = 1;
        }
        return 1;
    }
    if (isAsciiDigit(c)) {
        int i = pos;
        int start = 0;
        int digits = 0;
        do {
            start = 10 * start + (s[i].unicode() - u'0');
            ++i;
            ++digits;
        } while (digits < 9 && i < n && isAsciiDigit(s[i]));
        if (interruptsParagraph && start != 1)
            return 0;
        const QChar d = at(s, i);
        if (d != u'.' && d != u')')
            return 0;
        ++i;
        if (i < n && !isCmarkSpace(s[i]))
            return 0;
        if (interruptsParagraph && isBlankFrom(s, i))
            return 0;
        if (out) {
            out->ordered = true;
            out->ch = d;
            out->start = start;
            out->length = i - pos;
        }
        return i - pos;
    }
    return 0;
}

int footnoteDef(QStringView s, int pos, int* labelFrom, int* labelTo)
{
    const int n = int(s.size());
    if (!startsWithAt(s, pos, QLatin1StringView("[^")))
        return 0;
    int i = pos + 2;
    const int from = i;
    while (i < n && s[i] != u']' && s[i] != u' ' && s[i] != u'\t' && s[i] != u'\r' && s[i] != u'\n' && s[i] != QChar(0))
        ++i;
    if (i == from || i >= n || s[i] != u']' || at(s, i + 1) != u':')
        return 0;
    if (labelFrom)
        *labelFrom = from;
    if (labelTo)
        *labelTo = i;
    return skipSpaces(s, i + 2) - pos;
}

bool calloutHeader(QStringView s, int pos, CalloutScan* out)
{
    const int n = int(s.size());
    if (!startsWithAt(s, pos, QLatin1StringView("[!")))
        return false;
    int i = pos + 2;
    const int innerFrom = i;
    while (i < n && s[i] != u']')
        ++i;
    if (i == innerFrom || i >= n)
        return false;
    const int innerTo = i;
    int j = i + 1;
    QChar fold;
    if (j < n && (s[j] == u'+' || s[j] == u'-')) {
        fold = s[j];
        ++j;
    }
    if (j < n && !s[j].isSpace())
        return false;
    if (out) {
        out->markerFrom = pos;
        out->markerTo = j;
        out->fold = fold;
        int pipe = int(s.mid(innerFrom, innerTo - innerFrom).indexOf(u'|'));
        if (pipe >= 0) {
            pipe += innerFrom;
            out->typeFrom = innerFrom;
            out->typeTo = pipe;
            out->metaFrom = pipe + 1;
            out->metaTo = innerTo;
        } else {
            out->typeFrom = innerFrom;
            out->typeTo = innerTo;
            out->metaFrom = out->metaTo = -1;
        }
        int t = j;
        while (t < n && s[t].isSpace())
            ++t;
        int e = n;
        while (e > t && s[e - 1].isSpace())
            --e;
        out->titleFrom = t;
        out->titleTo = e;
    }
    return true;
}

int findPair(QStringView s, int from, char16_t c)
{
    const int n = int(s.size());
    for (int i = std::max(0, from); i + 1 < n; ++i) {
        if (s[i] == c) {
            if (s[i + 1] == c)
                return i;
        }
    }
    return -1;
}

DelimBlock mathStart(QStringView s, int pos)
{
    if (!startsWithAt(s, pos, QLatin1StringView("$$")))
        return DelimBlock::None;
    const int k = findPair(s, pos + 2, u'$');
    if (k < 0)
        return DelimBlock::Open;
    return isBlankFrom(s, k + 2) ? DelimBlock::OneLine : DelimBlock::None;
}

DelimBlock commentStart(QStringView s, int pos)
{
    if (!startsWithAt(s, pos, QLatin1StringView("%%")))
        return DelimBlock::None;
    return findPair(s, pos + 2, u'%') < 0 ? DelimBlock::Open : DelimBlock::None;
}

bool isFrontmatterDelim(QStringView s)
{
    return s.size() >= 3 && s[0] == u'-' && s[1] == u'-' && s[2] == u'-' && isBlankFrom(s, 3);
}

int tableRowCells(QStringView s, int pos)
{
    return forEachCell(s, pos, [](int, int) {});
}

bool tableDelimiterRow(QStringView s, int pos, int* columns)
{
    const int n = int(s.size());
    int i = pos;
    if (i < n && s[i] == u'|')
        ++i;
    i = scanDelimiterCell(s, i);
    if (i < 0)
        return false;
    for (;;) {
        if (i >= n)
            break;
        if (s[i] != u'|')
            return false;
        ++i;
        if (isTableSpaceOnlyFrom(s, i))
            break; // trailing pipe
        i = scanDelimiterCell(s, i);
        if (i < 0)
            return false;
    }
    if (columns)
        *columns = tableRowCells(s, pos);
    return true;
}

void tableCellRanges(QStringView s, int pos, QList<TableCell>* out)
{
    forEachCell(s, pos, [&](int from, int to) {
        while (from < to && isTableSpace(s[from]))
            ++from;
        while (to > from && isTableSpace(s[to - 1]))
            --to;
        out->push_back(TableCell{from, to});
    });
}

void tableAlignments(QStringView s, int pos, QList<Qt::Alignment>* out)
{
    QList<TableCell> cells;
    tableCellRanges(s, pos, &cells);
    for (const TableCell& c : cells) {
        const bool left = c.to > c.from && s[c.from] == u':';
        const bool right = c.to > c.from && s[c.to - 1] == u':' && (c.to - c.from > 1 || !left);
        if (left && right)
            out->push_back(Qt::AlignHCenter);
        else if (left)
            out->push_back(Qt::AlignLeft);
        else if (right)
            out->push_back(Qt::AlignRight);
        else
            out->push_back(Qt::Alignment());
    }
}

QChar taskMarker(QStringView s, int pos)
{
    const int n = int(s.size());
    if (pos >= n || s[pos] != u'[')
        return {};
    const int i = pos + 1;
    if (i >= n || s[i] == u']')
        return {};
    const int width = (s[i].isHighSurrogate() && i + 1 < n && s[i + 1].isLowSurrogate()) ? 2 : 1;
    const int j = i + width;
    if (j >= n || s[j] != u']')
        return {};
    if (j + 1 < n && !isSpaceOrTab(s[j + 1]))
        return {};
    return s[i];
}

HeadingInfo atxInfo(QStringView s, int col)
{
    HeadingInfo h;
    const int n = int(s.size());
    const int p = skipSpaces(s, col);
    int i = p;
    while (i < n && s[i] == u'#')
        ++i;
    h.level = std::clamp(i - p, 1, 6);
    h.markerFrom = p;
    h.markerTo = skipSpaces(s, i);
    const int end = rtrim(s, 0, n);
    // cmark chop_trailing_hashtags on the whole line: a run of '#' preceded by a space/tab.
    int k = end - 1;
    while (k >= 0 && s[k] == u'#')
        --k;
    int textTo = std::max(end, h.markerTo);
    h.closeFrom = -1;
    if (k != end - 1 && k >= 0 && isSpaceOrTab(s[k])) {
        const int closeRaw = rtrim(s, 0, k + 1);
        h.closeFrom = std::max(closeRaw, h.markerTo);
        textTo = h.closeFrom;
    }
    h.textTo = textTo;
    h.textFrom = std::min(h.markerTo, textTo);
    return h;
}

QString normalizeLabel(QStringView raw)
{
    const QString folded = raw.toString().toCaseFolded();
    QString out;
    out.reserve(folded.size());
    bool pendingSpace = false;
    for (const QChar c : folded) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            out += u' ';
            pendingSpace = false;
        }
        out += c;
    }
    return out;
}

QString unescapeBackslashes(QStringView s)
{
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == u'\\' && i + 1 < s.size() && isAsciiPunct(s[i + 1])) {
            out += s[i + 1];
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace oc::md::scan
