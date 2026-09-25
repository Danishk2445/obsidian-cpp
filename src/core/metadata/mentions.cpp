#include "core/metadata/mentions.h"

#include "core/metadata/link_resolver.h"
#include "markdown/metadata.h"

#include <QCollator>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace oc {

namespace {

// Obsidian leaves these scripts out of its word-character class (RA): names written in them match
// inside words too.
bool isUnbrokenScript(char16_t u)
{
    return u == 0x0F00 || (u >= 0x0F40 && u <= 0x0F8C) || (u >= 0x3041 && u <= 0x30FF) || (u >= 0x4E00 && u <= 0x9FD5)
        || (u >= 0xA960 && u <= 0xA97C) || (u >= 0xAC00 && u <= 0xD7A3) || (u >= 0xD7B0 && u <= 0xD7C6);
}

int clampOffset(qsizetype v, QStringView content) { return int(std::clamp<qsizetype>(v, 0, content.size())); }

bool insideLink(const md::CachedMetadata* meta, int from, int to)
{
    if (!meta)
        return false;
    auto covers = [&](const QList<md::LinkCache>& links) {
        for (const md::LinkCache& l : links)
            if (l.position.start.offset <= from && l.position.end.offset >= to)
                return true;
        return false;
    };
    return covers(meta->links) || covers(meta->embeds);
}

bool insideFrontmatter(const md::CachedMetadata* meta, int from, int to)
{
    return meta && meta->frontmatterPosition && meta->frontmatterPosition->start.offset <= from
        && meta->frontmatterPosition->end.offset >= to;
}

int lineStart(QStringView s, int pos)
{
    pos = std::clamp(pos, 0, int(s.size()));
    while (pos > 0 && s[pos - 1] != u'\n')
        --pos;
    return pos;
}

int lineEnd(QStringView s, int pos)
{
    pos = std::clamp(pos, 0, int(s.size()));
    while (pos < s.size() && s[pos] != u'\n')
        ++pos;
    return pos;
}

// The innermost list item containing [from, to] (items come in document order, parents first).
int listItemAt(const md::CachedMetadata* meta, int from, int to)
{
    int found = -1;
    if (!meta)
        return found;
    for (int i = 0; i < meta->listItems.size(); ++i) {
        const md::ListItemCache& li = meta->listItems[i];
        if (li.position.start.offset - li.position.start.col <= from && li.position.end.offset >= to)
            found = i;
    }
    return found;
}

// A list item and the items nested in it: the last one (Obsidian walks items whose parent line is in
// the family).
int lastOfFamily(const md::CachedMetadata& meta, int index)
{
    QSet<int> family{meta.listItems[index].position.start.line};
    int last = index;
    for (int j = index + 1; j < meta.listItems.size(); ++j) {
        const md::ListItemCache& c = meta.listItems[j];
        if (!family.contains(c.parent))
            break;
        family.insert(c.position.start.line);
        last = j;
    }
    return last;
}

const md::SectionCache* sectionAt(const md::CachedMetadata* meta, int from, int to)
{
    if (!meta)
        return nullptr;
    for (const md::SectionCache& s : meta->sections)
        if (s.position.start.offset <= from && s.position.end.offset >= to)
            return &s;
    return nullptr;
}

// Obsidian's list-line prefix /^([>\s]*)(([*+-] |(\d+)([.)] ))(?:\[(.)\] )?)?/: the indentation
// (quote markers and whitespace) and whether a list marker follows it.
struct ListPrefix {
    int indent = 0;
    bool marker = false;
};
ListPrefix listPrefix(QStringView line)
{
    ListPrefix p;
    int i = 0;
    while (i < line.size() && (line[i] == u'>' || line[i].isSpace()))
        ++i;
    p.indent = i;
    if (i + 1 < line.size() && (line[i] == u'*' || line[i] == u'+' || line[i] == u'-') && line[i + 1] == u' ') {
        p.marker = true;
    } else {
        int j = i;
        while (j < line.size() && line[j].isDigit())
            ++j;
        if (j > i && j + 1 < line.size() && (line[j] == u'.' || line[j] == u')') && line[j + 1] == u' ')
            p.marker = true;
    }
    return p;
}

bool paragraphBreakAt(QStringView s, int pos)
{
    // /\r?\n\r?\n/y
    int i = pos;
    if (i < s.size() && s[i] == u'\r')
        ++i;
    if (i >= s.size() || s[i] != u'\n')
        return false;
    ++i;
    if (i < s.size() && s[i] == u'\r')
        ++i;
    return i < s.size() && s[i] == u'\n';
}

} // namespace

bool isMentionWordChar(QChar c)
{
    const char16_t u = c.unicode();
    if (u < 0x80)
        return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9');
    if (isUnbrokenScript(u))
        return false;
    return c.isLetter() || c.category() == QChar::Number_DecimalDigit;
}

bool isMentionAt(QStringView content, int pos, int length)
{
    if (length <= 0 || pos < 0 || pos + length > content.size())
        return false;
    if (isMentionWordChar(content[pos]) && pos > 0 && isMentionWordChar(content[pos - 1]))
        return false;
    const int end = pos + length;
    if (isMentionWordChar(content[end - 1]) && end < content.size() && isMentionWordChar(content[end]))
        return false;
    return true;
}

QList<MatchRange> findMentions(QStringView content, const QString& name, int from, int to)
{
    QList<MatchRange> out;
    if (name.isEmpty())
        return out;
    const int limit = to < 0 ? int(content.size()) : std::min(to, int(content.size()));
    int pos = std::max(0, from);
    while (pos < limit) {
        const qsizetype hit = content.indexOf(QStringView(name), pos, Qt::CaseInsensitive);
        if (hit < 0 || hit + name.size() > limit)
            break;
        if (isMentionAt(content, int(hit), int(name.size()))) {
            out.push_back({int(hit), int(hit + name.size())});
            pos = int(hit + name.size());
        } else {
            pos = int(hit) + 1;
        }
    }
    return out;
}

QList<MatchRange> linkedMentionRanges(const md::CachedMetadata& meta, const std::function<bool(const QString&)>& resolvesToTarget)
{
    QList<MatchRange> out;
    auto consider = [&](const QList<md::LinkCache>& links) {
        for (const md::LinkCache& l : links)
            if (resolvesToTarget(LinkResolver::splitSubpath(l.link).first))
                out.push_back({int(l.position.start.offset), int(l.position.end.offset)});
    };
    consider(meta.frontmatterLinks);
    consider(meta.links);
    consider(meta.embeds);
    std::stable_sort(out.begin(), out.end(), [](const MatchRange& a, const MatchRange& b) { return a.from < b.from; });
    return out;
}

QList<MatchRange> unlinkedMentionRanges(QStringView content, const QStringList& names, const md::CachedMetadata* meta)
{
    QList<MatchRange> out;
    QSet<QString> seen;
    for (const QString& name : names) {
        if (name.isEmpty() || seen.contains(name.toLower()))
            continue;
        seen.insert(name.toLower());
        for (const MatchRange& m : findMentions(content, name))
            if (!insideLink(meta, m.from, m.to) && !insideFrontmatter(meta, m.from, m.to) && !out.contains(m))
                out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const MatchRange& a, const MatchRange& b) { return a.from < b.from; });
    return out;
}

QList<MentionCandidate> mentionCandidates(const QList<QPair<QString, QStringList>>& files, const QString& self)
{
    QList<MentionCandidate> out;
    QHash<QString, qsizetype> index;
    auto add = [&](const QString& rawText, const QString& path) {
        const QString text = rawText.toLower();
        if (text.isEmpty())
            return;
        auto it = index.constFind(text);
        if (it == index.cend()) {
            index.insert(text, out.size());
            out.push_back({text, {path}});
        } else if (!out[*it].targets.contains(path)) {
            out[*it].targets.push_back(path);
        }
    };
    for (const auto& [path, aliases] : files) {
        if (path == self)
            continue;
        QString base = path.mid(path.lastIndexOf(u'/') + 1);
        if (base.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            base.chop(3);
        add(base, path);
        for (const QString& a : aliases)
            add(a, path);
    }
    QCollator col;
    col.setNumericMode(true);
    col.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(out.begin(), out.end(), [&](const MentionCandidate& a, const MentionCandidate& b) { return col.compare(a.text, b.text) < 0; });
    return out;
}

QList<OutgoingMention> outgoingUnlinkedMentions(QStringView content, const md::CachedMetadata* meta, const QList<MentionCandidate>& candidates)
{
    QList<OutgoingMention> out;
    const int start = meta && meta->frontmatterPosition ? clampOffset(meta->frontmatterPosition->end.offset, content) : 0;
    for (const MentionCandidate& c : candidates) {
        const int len = int(c.text.size());
        if (len == 0)
            continue;
        // Obsidian continues after each occurrence, whether or not it was a whole-word mention.
        qsizetype m = content.indexOf(QStringView(c.text), start, Qt::CaseInsensitive);
        while (m >= 0) {
            const int from = int(m);
            const int to = from + len;
            m = content.indexOf(QStringView(c.text), to, Qt::CaseInsensitive);
            if (!isMentionAt(content, from, len) || insideLink(meta, from, to))
                continue;
            out.push_back({{from, to}, c.targets});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const OutgoingMention& a, const OutgoingMention& b) { return a.range.from < b.range.from; });
    return out;
}

MatchSnippet lineSnippet(QStringView content, MatchRange match, int maxChars)
{
    MatchSnippet s;
    const int size = int(content.size());
    int i = std::clamp(match.from, 0, size) - 1;
    int back = 0;
    while (back < maxChars && i >= 0) {
        if (content[i] == u'\n')
            break;
        --i;
        ++back;
    }
    s.from = i + 1;
    s.ellipsisBefore = back == maxChars;
    int a = std::clamp(match.to, 0, size);
    int fwd = 0;
    while (fwd < maxChars && a < size) {
        if (content[a] == u'\n')
            break;
        ++a;
        ++fwd;
    }
    s.to = a;
    s.ellipsisAfter = fwd == maxChars;
    s.matches = {match};
    return s;
}

MatchSnippet extraContextSnippet(QStringView content, MatchRange match, const md::CachedMetadata* meta)
{
    MatchSnippet s;
    s.matches = {match};
    const int size = int(content.size());
    if (const int li = listItemAt(meta, match.from, match.to); li >= 0) {
        const md::ListItemCache& item = meta->listItems[li];
        const md::ListItemCache& last = meta->listItems[lastOfFamily(*meta, li)];
        s.from = clampOffset(item.position.start.offset - item.position.start.col, content);
        s.to = clampOffset(std::max(last.position.end.offset, item.position.end.offset), content);
        return s;
    }
    if (const md::SectionCache* sec = sectionAt(meta, match.from, match.to)) {
        s.from = clampOffset(sec->position.start.offset, content);
        s.to = clampOffset(sec->position.end.offset, content);
        return s;
    }
    // No metadata: the list block or the paragraph around the match (at most 1000 characters away).
    int p = std::clamp(match.from, 0, size);
    const int minP = p - 1000;
    while (p > 0 && p > minP && content[p - 1] != u'\n')
        --p;
    int f = std::clamp(match.from, 0, size);
    const int maxF = f + 1000;
    while (f < size && f < maxF && content[f] != u'\n')
        ++f;
    const ListPrefix first = listPrefix(content.mid(p, f - p));
    if (first.marker) {
        int g = f;
        while (g < size) {
            qsizetype y = content.indexOf(u'\n', g + 1);
            if (y < 0)
                y = size;
            const ListPrefix next = listPrefix(content.mid(g + 1, y - g - 1));
            if (next.indent <= first.indent)
                break;
            g = int(y);
        }
        s.from = p;
        s.to = g;
        return s;
    }
    int w = p;
    while (w > 0 && w > minP && !paragraphBreakAt(content, w))
        --w;
    while (w < p && content[w] == u'\n')
        ++w;
    int k = f;
    while (k < size && k < maxF && !paragraphBreakAt(content, k))
        ++k;
    s.from = w;
    s.to = k;
    return s;
}

QList<MatchSnippet> buildMatchSnippets(QStringView content, QList<MatchRange> matches, bool extraContext, const md::CachedMetadata* meta, bool separate)
{
    std::stable_sort(matches.begin(), matches.end(), [](const MatchRange& a, const MatchRange& b) { return a.from < b.from; });
    QList<MatchSnippet> out;
    qsizetype o = 0;
    while (o < matches.size()) {
        MatchSnippet s = extraContext ? extraContextSnippet(content, matches[o], meta) : lineSnippet(content, matches[o]);
        qsizetype h = o + 1;
        if (!separate) {
            while (h < matches.size()) {
                const MatchRange& p = matches[h];
                if (p.from >= s.to)
                    break;
                if (p.to > s.to) {
                    s.to = p.to;
                    ++h;
                    break;
                }
                ++h;
            }
        }
        s.matches = matches.mid(o, h - o);
        out.push_back(s);
        o = h;
    }
    return out;
}

namespace {

int prevPos(QStringView content, const md::CachedMetadata* meta, int e)
{
    if (meta) {
        if (const int li = listItemAt(meta, e, e); li >= 0) {
            const md::Pos& s = meta->listItems[li].position.start;
            return int(s.offset - s.col);
        }
        if (const md::SectionCache* sec = sectionAt(meta, e, e); sec && sec->type != u"list") {
            const md::Pos& s = sec->position.start;
            return int(s.offset - s.col);
        }
    }
    return lineStart(content, e);
}

int nextPos(QStringView content, const md::CachedMetadata* meta, int e)
{
    if (meta) {
        if (const int li = listItemAt(meta, e, e); li >= 0) {
            const md::ListItemCache& item = meta->listItems[li];
            const md::ListItemCache& last = meta->listItems[lastOfFamily(*meta, li)];
            return int(std::max(item.position.end.offset, last.position.end.offset));
        }
        if (const md::SectionCache* sec = sectionAt(meta, e, e); sec && sec->type != u"list")
            return int(sec->position.end.offset);
    }
    return lineEnd(content, e);
}

} // namespace

int moreContextBefore(QStringView content, const md::CachedMetadata* meta, int start)
{
    int e = std::clamp(start, 0, int(content.size()));
    while (e > 0) {
        --e;
        const int t = prevPos(content, meta, e);
        if (t < e) {
            e = t;
            break;
        }
    }
    return std::max(0, e);
}

int moreContextAfter(QStringView content, const md::CachedMetadata* meta, int end)
{
    const int size = int(content.size());
    int e = std::clamp(end, 0, size);
    while (e < size) {
        ++e;
        const int n = nextPos(content, meta, e);
        if (n > e) {
            e = n;
            break;
        }
    }
    return std::min(e, size);
}

QString mentionLinkText(const QString& link, bool inTable)
{
    if (!inTable)
        return link;
    QString out = link;
    const qsizetype bar = out.indexOf(u'|');
    if (bar >= 0)
        out.replace(bar, 1, QStringLiteral("\\|"));
    return out;
}

bool isInTable(const md::CachedMetadata* meta, MatchRange range)
{
    if (!meta)
        return false;
    for (const md::SectionCache& s : meta->sections)
        if (s.type == u"table" && s.position.start.offset <= range.from && s.position.end.offset >= range.to)
            return true;
    return false;
}

} // namespace oc
