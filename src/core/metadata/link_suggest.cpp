#include "core/metadata/link_suggest.h"

#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "markdown/metadata.h"

#include <QCollator>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

namespace oc {

using namespace Qt::StringLiterals;

namespace {
const QStringList kImageExt = {u"bmp"_s, u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"svg"_s, u"webp"_s, u"avif"_s};

// Obsidian's extension helper: "" when there's no dot, a trailing dot or a leading dot.
QString extensionOf(QStringView name)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0 || dot == name.size() - 1)
        return {};
    return name.mid(dot + 1).toString().toLower();
}
QString lastSegment(const QString& path)
{
    const qsizetype s = path.lastIndexOf(u'/');
    return s < 0 ? path : path.mid(s + 1);
}
// "Folder/Note.md" -> "Folder/Note" (only for .md)
QString withoutMd(const QString& path) { return extensionOf(lastSegment(path)) == u"md" ? path.left(path.size() - 3) : path; }

QPair<QString, QString> splitSubpath(const QString& link)
{
    const qsizetype h = link.indexOf(u'#');
    return h < 0 ? QPair<QString, QString>{link, {}} : QPair<QString, QString>{link.left(h), link.mid(h)};
}

struct Match {
    double score = 0;
    QList<MatchRange> ranges;
};
std::optional<Match> fuzzy(const FuzzyQuery& q, QStringView text)
{
    if (q.isEmpty())
        return Match{};
    const auto r = q.match(text);
    if (!r)
        return std::nullopt;
    return Match{r->score, matchIndicesToRanges(r->matches)};
}

// Obsidian's word search used for blocks: every non-empty word must occur (all occurrences are
// highlighted); null when a word is missing.
std::optional<QList<MatchRange>> wordMatches(const QStringList& words, const QString& text)
{
    const QString hay = text.toLower();
    QList<MatchRange> out;
    for (const QString& w : words) {
        if (w.isEmpty())
            continue;
        bool found = false;
        for (qsizetype at = hay.indexOf(w); at >= 0; at = hay.indexOf(w, at + w.size() + 1)) {
            found = true;
            out.append({int(at), int(at + w.size())});
        }
        if (!found)
            return std::nullopt;
    }
    std::sort(out.begin(), out.end(), [](const MatchRange& a, const MatchRange& b) { return a.from < b.from; });
    QList<MatchRange> merged;
    for (const MatchRange& r : out) {
        if (!merged.isEmpty() && merged.last().to >= r.from)
            merged.last().to = std::max(merged.last().to, r.to);
        else
            merged.append(r);
    }
    return merged;
}
double wordScore(const std::optional<QList<MatchRange>>& m)
{
    if (!m)
        return 0;
    return -(double(m->size()) + (m->isEmpty() ? 0 : m->first().from / 10.0));
}

void sortByScore(QList<LinkSuggestion>& list)
{
    std::stable_sort(list.begin(), list.end(), [](const LinkSuggestion& a, const LinkSuggestion& b) { return a.score > b.score; });
}

QString normalizedNewlines(QString s)
{
    s.replace(u"\r\n"_s, u"\n"_s);
    s.replace(u'\r', u'\n');
    return s;
}

// Plain text of a block's source for display: container markers, list markers and a trailing block
// id stripped per line, lines trimmed.
QString blockDisplay(const QString& src)
{
    static const QRegularExpression quote(u"^(\\s{0,3}>\\s?)+"_s);
    static const QRegularExpression bullet(u"^\\s*(?:[-*+]|\\d{1,9}[.)])(?:\\s+\\[.\\])?(?:\\s+|$)"_s);
    static const QRegularExpression blockId(u"\\s+\\^[A-Za-z0-9-]+\\s*$"_s);
    QStringList lines = src.split(u'\n');
    for (QString& l : lines) {
        l.remove(quote);
        l.remove(bullet);
        l.remove(blockId);
        l = l.trimmed();
    }
    return lines.join(u'\n').trimmed();
}
} // namespace

// ---------------------------------------------------------------------------------------------
// triggers
// ---------------------------------------------------------------------------------------------
std::optional<SuggestTrigger> linkSuggestTrigger(QStringView line, int cursor)
{
    cursor = std::clamp(cursor, 0, int(line.size()));
    const QStringView before = line.left(cursor);
    const qsizetype open = before.lastIndexOf(u"[[");
    const qsizetype close = before.lastIndexOf(u']');
    if (open < 0 || close >= open)
        return std::nullopt;
    const QString query = before.mid(open + 2).toString();
    const qsizetype pipe = query.indexOf(u'|');
    if (pipe >= 0) {
        const QString target = splitSubpath(query.left(pipe)).first;
        if (kImageExt.contains(extensionOf(target)))
            return std::nullopt; // "[[img.png|100" is an image size
    }
    return SuggestTrigger{int(open + 2), cursor, query};
}

std::optional<SuggestTrigger> tagSuggestTrigger(QStringView line, int cursor)
{
    static const QRegularExpression re(uR"((^|\s)#[^\x{2000}-\x{206F}\x{2E00}-\x{2E7F}'!"#$%&()*+,.:;<=>?@^`{|}~\[\]\\\s]*$)"_s);
    cursor = std::clamp(cursor, 0, int(line.size()));
    const QStringView before = line.left(cursor);
    if (!re.matchView(before).hasMatch())
        return std::nullopt;
    if (cursor < line.size() && line[cursor] == u'#')
        return std::nullopt;
    const qsizetype hash = before.lastIndexOf(u'#');
    return SuggestTrigger{int(hash), cursor, before.mid(hash + 1).toString()};
}

std::optional<SuggestTrigger> footnoteSuggestTrigger(QStringView line, int cursor)
{
    static const QRegularExpression re(uR"((?:^|[^\[])(\[\^)([^\]]*)$)"_s);
    cursor = std::clamp(cursor, 0, int(line.size()));
    const QStringView before = line.left(cursor);
    const QRegularExpressionMatch m = re.matchView(before);
    if (!m.hasMatch())
        return std::nullopt;
    const QString label = m.captured(2);
    SuggestTrigger t{int(before.size() - 2 - label.size()), cursor, label};
    const QStringView after = line.mid(cursor);
    for (qsizetype i = 0; i < after.size(); ++i) {
        if (after[i] == u']') {
            t.end = int(cursor + i + 1);
            if (t.end < line.size() && line[t.end] == u':')
                return std::nullopt; // a footnote definition
            break;
        }
        if (after[i].isSpace())
            break;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
// link suggestions
// ---------------------------------------------------------------------------------------------
LinkSuggestProvider::LinkSuggestProvider(const Vault* vault, const MetadataCache* cache)
    : m_vault(vault)
    , m_cache(cache)
{
}

QList<QPair<QString, QString>> LinkSuggestProvider::instructions() const
{
    if (m_mode == Mode::File)
        return {{QObject::tr("Type #"), QObject::tr("to link heading")},
                {QObject::tr("Type ^"), QObject::tr("to link blocks")},
                {QObject::tr("Type |"), QObject::tr("to change display text")}};
    return {{u"↵"_s, QObject::tr("to accept")}};
}

QList<LinkSuggestProvider::Entry> LinkSuggestProvider::linkSuggestionEntries() const
{
    QList<Entry> out;
    QSet<QString> seen;
    if (m_vault) {
        for (TFile* f : m_vault->files()) {
            const bool md = f->isMarkdown();
            if (!md && !(m_supported ? m_supported(f->extension().toLower()) : true))
                continue;
            const QString path = md ? withoutMd(f->path()) : f->path();
            seen.insert(path);
            out.append({f->path(), path, {}, f->mtimeMs()});
            if (md && m_cache) {
                if (const md::CachedMetadata* meta = m_cache->fileCache(f->path())) {
                    for (const QString& a : meta->aliases()) {
                        const QString alias = a.trimmed();
                        if (!alias.isEmpty())
                            out.append({f->path(), path, alias, f->mtimeMs()});
                    }
                }
            }
        }
    }
    if (m_cache) {
        const auto& unresolved = m_cache->unresolvedLinks();
        QStringList sources = unresolved.keys();
        sources.sort(); // deterministic order
        for (const QString& src : sources) {
            QStringList targets = unresolved.value(src).keys();
            targets.sort();
            for (QString t : targets) {
                if (t.size() > 500)
                    t.truncate(500);
                if (seen.contains(t))
                    continue;
                seen.insert(t);
                out.append({{}, t, {}, 0});
            }
        }
    }
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::suggestions(const QString& queryIn, const QString& sourcePath)
{
    QString query = queryIn;
    std::optional<QString> display;
    const qsizetype pipe = query.indexOf(u'|');
    if (pipe >= 0) {
        display = query.mid(pipe + 1);
        query = query.left(pipe);
    }
    QList<LinkSuggestion> out;
    m_global = false;
    if (!display && query.startsWith(u"##")) {
        m_mode = Mode::Heading;
        m_global = true;
        out = globalHeadingSuggestions(query.mid(2));
    } else if (!display && query.startsWith(u"^^")) {
        m_mode = Mode::Block;
        m_global = true;
        out = globalBlockSuggestions(query.mid(2), sourcePath);
    } else {
        auto [path, subpath] = splitSubpath(query);
        if (display) {
            m_mode = Mode::Display;
            out = displaySuggestions(path, subpath, *display, sourcePath);
        } else if (!subpath.isEmpty()) {
            if (subpath.startsWith(u"#^")) {
                m_mode = Mode::Block;
                out = blockSuggestions(path, subpath.mid(2), sourcePath);
            } else {
                m_mode = Mode::Heading;
                out = headingSuggestions(path, subpath, sourcePath);
            }
        } else if (const qsizetype caret = path.lastIndexOf(u'^'); caret >= 0) {
            m_mode = Mode::Block;
            out = blockSuggestions(path.left(caret), path.mid(caret + 1), sourcePath);
        } else {
            m_mode = Mode::File;
            out = fileSuggestions(path);
        }
    }
    if (out.isEmpty())
        out.append(LinkSuggestion{});
    sortByScore(out);
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::fileSuggestions(const QString& query)
{
    if (!m_fileSuggestions)
        m_fileSuggestions = linkSuggestionEntries();
    const QList<Entry>& entries = *m_fileSuggestions;
    QList<LinkSuggestion> out;
    auto make = [](const Entry& e) {
        LinkSuggestion s;
        s.file = e.file;
        s.path = e.path;
        s.alias = e.alias;
        s.type = e.file.isEmpty() ? LinkSuggestion::Type::Linktext
                                  : (e.alias.isEmpty() ? LinkSuggestion::Type::File : LinkSuggestion::Type::Alias);
        return s;
    };
    if (query.isEmpty()) {
        for (const Entry& e : entries) {
            LinkSuggestion s = make(e);
            s.score = e.file.isEmpty() ? 0 : double(e.mtime);
            out.append(s);
        }
        return out;
    }
    // Obsidian switches to the cheaper word search for huge vaults.
    const bool useFuzzy = entries.size() < 10000;
    const FuzzyQuery fq(query);
    const SimpleSearch ss(query);
    auto search = [&](const QString& text) -> std::optional<Match> {
        if (useFuzzy)
            return fuzzy(fq, text);
        const auto r = ss.match(text);
        if (!r)
            return std::nullopt;
        return Match{r->score, r->matches};
    };
    for (const Entry& e : entries) {
        LinkSuggestion s = make(e);
        if (e.file.isEmpty() || !e.alias.isEmpty()) {
            const auto m = search(e.file.isEmpty() ? e.path : e.alias);
            if (!m)
                continue;
            s.score = m->score;
            s.matches = m->ranges;
        } else {
            // prefer hits in the file name; fall back to the whole path (score - 1)
            const QString name = lastSegment(e.path);
            if (auto m = search(name)) {
                const int shift = int(e.path.size() - name.size());
                for (MatchRange& r : m->ranges) {
                    r.from += shift;
                    r.to += shift;
                }
                s.score = m->score;
                s.matches = m->ranges;
            } else if (auto m2 = search(e.path)) {
                s.score = m2->score - 1;
                s.matches = m2->ranges;
            } else {
                continue;
            }
        }
        out.append(s);
    }
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::displaySuggestions(const QString& target, const QString& subpath, const QString& display,
                                                              const QString& sourcePath)
{
    QList<LinkSuggestion> out;
    const QString dest = m_cache ? m_cache->firstLinkpathDest(target, sourcePath) : QString();
    if (!dest.isEmpty()) {
        if (const md::CachedMetadata* meta = m_cache->fileCache(dest)) {
            const FuzzyQuery q(display);
            for (const QString& a : meta->aliases()) {
                const QString alias = a.trimmed();
                if (alias.isEmpty())
                    continue;
                if (const auto m = fuzzy(q, alias)) {
                    LinkSuggestion s;
                    s.type = LinkSuggestion::Type::Alias;
                    s.alias = alias;
                    s.file = dest;
                    s.path = target + subpath;
                    s.score = m->score;
                    s.matches = m->ranges;
                    out.append(s);
                }
            }
        }
    }
    if (out.isEmpty()) {
        LinkSuggestion s;
        s.type = LinkSuggestion::Type::Alias;
        s.path = target + subpath;
        s.alias = display;
        if (!display.isEmpty())
            s.matches = {{0, int(display.size())}};
        out.append(s);
    }
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::headingSuggestions(const QString& target, const QString& subpath, const QString& sourcePath)
{
    const QString dest = m_cache ? m_cache->firstLinkpathDest(target, sourcePath) : QString();
    if (dest.isEmpty() || extensionOf(lastSegment(dest)) != u"md")
        return {};
    QList<LinkSuggestion> out;
    const md::CachedMetadata* meta = m_cache->fileCache(dest);
    if (meta && !meta->headings.isEmpty()) {
        // "#A#B": walk into heading A's section, then fuzzy-match B among the headings below it.
        QStringList parts;
        for (const QString& p : subpath.split(u'#'))
            if (!p.isEmpty())
                parts << p;
        if (subpath.endsWith(u'#'))
            parts << QString();
        QString prefix = u"#"_s + parts.mid(0, parts.size() - 1).join(u'#');
        if (!prefix.endsWith(u'#'))
            prefix += u'#';
        qsizetype seg = 0;
        int level = 0;
        for (const md::HeadingCache& h : meta->headings) {
            if (h.level <= level || seg >= parts.size())
                break;
            if (seg == parts.size() - 1) {
                LinkSuggestion s;
                s.type = LinkSuggestion::Type::Heading;
                s.file = dest;
                s.path = target;
                s.subpath = prefix + headingSubpathText(h.heading);
                s.level = h.level;
                s.heading = h.heading;
                if (parts[seg].isEmpty()) {
                    out.append(s);
                } else if (const auto m = fuzzy(FuzzyQuery(parts[seg]), h.heading)) {
                    s.score = m->score;
                    s.matches = m->ranges;
                    out.append(s);
                }
            } else if (h.heading.toLower() == parts[seg].toLower()) {
                ++seg;
                level = h.level;
            }
        }
    }
    if (out.isEmpty()) {
        LinkSuggestion s; // "#New heading": link to a heading that doesn't exist yet
        s.type = LinkSuggestion::Type::Heading;
        s.path = target;
        s.subpath = subpath;
        s.heading = subpath.mid(1);
        if (!s.heading.isEmpty())
            s.matches = {{0, int(s.heading.size())}};
        out.append(s);
    }
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::globalHeadingSuggestions(const QString& query)
{
    QList<LinkSuggestion> out;
    if (!m_cache)
        return out;
    const FuzzyQuery q(query);
    QStringList files = m_cache->markdownFiles();
    files.sort();
    for (const QString& f : files) {
        const md::CachedMetadata* meta = m_cache->fileCache(f);
        if (!meta || (m_vault && !m_vault->file(f)))
            continue;
        for (const md::HeadingCache& h : meta->headings) {
            if (const auto m = fuzzy(q, h.heading)) {
                LinkSuggestion s;
                s.type = LinkSuggestion::Type::Heading;
                s.file = f;
                s.subpath = u"#"_s + headingSubpathText(h.heading);
                s.level = h.level;
                s.heading = h.heading;
                s.score = m->score;
                s.matches = m->ranges;
                out.append(s);
            }
        }
    }
    return out;
}

void LinkSuggestProvider::appendBlocks(QList<LinkSuggestion>& out, const QString& file, const QString& path, const QStringList& words) const
{
    const md::CachedMetadata* meta = m_cache ? m_cache->fileCache(file) : nullptr;
    if (!meta)
        return;
    std::optional<QString> raw = m_reader ? m_reader(file) : std::nullopt;
    if (!raw)
        return;
    const QString content = normalizedNewlines(*raw);
    auto sliceLines = [&](int fromLine, int toLine) -> QPair<int, int> { // [from, to) offsets of lines fromLine..toLine
        int from = 0;
        for (int l = 0; l < fromLine && from >= 0; ++l)
            from = int(content.indexOf(u'\n', from)) + 1;
        if (from < 0)
            from = int(content.size());
        int to = from;
        for (int l = fromLine; l <= toLine; ++l) {
            const qsizetype nl = content.indexOf(u'\n', to);
            if (nl < 0) {
                to = int(content.size());
                break;
            }
            to = l == toLine ? int(nl) : int(nl) + 1;
        }
        return {from, std::max(from, to)};
    };
    auto add = [&](const QString& type, int fromLine, int toLine, int ownToLine, const QString& id, const QString& display) {
        if (display.isEmpty())
            return;
        LinkSuggestion s;
        s.type = LinkSuggestion::Type::Block;
        s.file = file;
        s.path = path;
        s.display = display;
        s.blockId = id;
        s.blockType = type;
        s.content = content;
        const auto [from, to] = sliceLines(fromLine, toLine);
        s.blockFrom = from;
        s.blockTo = to;
        if (ownToLine >= fromLine && ownToLine < toLine)
            s.blockParagraphEnd = sliceLines(fromLine, ownToLine).second;
        const auto m = wordMatches(words, display);
        s.score = wordScore(m);
        std::optional<QList<MatchRange>> idm;
        if (!id.isEmpty() && (idm = wordMatches(words, id)))
            s.score = 0;
        if (!m && !idm)
            return;
        if (m)
            s.matches = *m;
        if (idm)
            s.idMatches = *idm;
        out.append(s);
    };
    for (const md::SectionCache& sec : meta->sections) {
        const int from = sec.position.start.line, to = sec.position.end.line;
        if (sec.type == u"yaml")
            continue;
        if (sec.type == u"list") {
            for (int i = 0; i < meta->listItems.size(); ++i) {
                const md::ListItemCache& li = meta->listItems[i];
                if (li.position.start.line < from || li.position.end.line > to)
                    continue;
                // the item's own lines: up to its first child item
                int ownTo = li.position.end.line;
                for (int j = i + 1; j < meta->listItems.size(); ++j) {
                    const md::ListItemCache& c = meta->listItems[j];
                    if (c.position.start.line > li.position.end.line)
                        break;
                    if (c.parent == li.position.start.line) {
                        ownTo = c.position.start.line - 1;
                        break;
                    }
                }
                const auto [a, b] = sliceLines(li.position.start.line, li.position.end.line);
                add(u"listItem"_s, li.position.start.line, li.position.end.line, ownTo, li.id, blockDisplay(content.mid(a, b - a)));
            }
            continue;
        }
        const auto [a, b] = sliceLines(from, to);
        QString display;
        if (sec.type == u"heading") {
            for (const md::HeadingCache& h : meta->headings) {
                if (h.position.start.line == from) {
                    display = QString(h.level, u'#') + u' ' + h.heading.trimmed();
                    break;
                }
            }
        } else if (sec.type == u"code") {
            QStringList lines = content.mid(a, b - a).split(u'\n');
            if (lines.size() >= 2 && (lines.first().trimmed().startsWith(u"```") || lines.first().trimmed().startsWith(u"~~~"))) {
                lines.removeFirst();
                if (!lines.isEmpty() && (lines.last().trimmed().startsWith(u"```") || lines.last().trimmed().startsWith(u"~~~")))
                    lines.removeLast();
            }
            display = lines.join(u'\n').trimmed();
        } else if (sec.type == u"comment" || sec.type == u"html") {
            continue; // no text content
        } else {
            display = blockDisplay(content.mid(a, b - a));
        }
        add(sec.type, from, to, -1, sec.id, display);
    }
}

QList<LinkSuggestion> LinkSuggestProvider::blockSuggestions(const QString& target, const QString& query, const QString& sourcePath)
{
    const QString dest = m_cache ? m_cache->firstLinkpathDest(target, sourcePath) : QString();
    if (dest.isEmpty() || extensionOf(lastSegment(dest)) != u"md")
        return {};
    QList<LinkSuggestion> out;
    appendBlocks(out, dest, target, query.toLower().split(u' '));
    return out;
}

QList<LinkSuggestion> LinkSuggestProvider::globalBlockSuggestions(const QString& query, const QString&)
{
    QList<LinkSuggestion> out;
    if (!m_cache)
        return out;
    const QStringList words = query.toLower().split(u' ');
    QStringList files = m_cache->markdownFiles();
    files.sort();
    for (const QString& f : files) {
        const qsizetype before = out.size();
        appendBlocks(out, f, QString(), words);
        for (qsizetype i = before; i < out.size(); ++i)
            out[i].path.clear(); // global: the link text is derived from the file when chosen
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// replacement
// ---------------------------------------------------------------------------------------------
LinkReplacement linkSuggestReplacement(const LinkSuggestion& s, const QString& query, QStringView line, int linkStart, int cursor,
                                       const QString& sourcePath, const QString& key, bool useMarkdownLinks, const LinktextFn& linktext)
{
    using T = LinkSuggestion::Type;
    LinkReplacement r;
    QString text;    // display text for markdown links
    QString alias;   // wikilink alias
    QString link;    // the link target
    const bool mdLinks = useMarkdownLinks;
    const bool omitMd = !mdLinks;
    int end = cursor;
    auto fileExt = [](const QString& path) { return extensionOf(lastSegment(path)); };
    // the typed path for heading/block suggestions, or the file's link text (empty = this file)
    auto linkPath = [&](const QString& file, const QString& path, bool hasPath) -> QString {
        if (hasPath) {
            if (omitMd || path.isEmpty() || fileExt(file) != u"md" || path.endsWith(u".md"))
                return path;
            return path + u".md"_s;
        }
        if (file == sourcePath)
            return {};
        return linktext(file, sourcePath, omitMd);
    };
    switch (s.type) {
    case T::None:
        if (key == u"Enter" || key == u"Tab" || key == u"#" || key == u"^")
            link = query;
        break;
    case T::File:
        text = lastSegment(s.file);
        text = text.left(text.lastIndexOf(u'.') > 0 ? text.lastIndexOf(u'.') : text.size());
        link = linktext(s.file, sourcePath, omitMd);
        break;
    case T::Alias: {
        alias = text = s.alias;
        auto [p, sub] = splitSubpath(s.path);
        if (!s.file.isEmpty())
            p = linktext(s.file, sourcePath, omitMd);
        link = p + sub;
        break;
    }
    case T::Linktext:
        text = s.path;
        link = s.path;
        break;
    case T::Heading:
        link = linkPath(s.file, s.path, !s.path.isEmpty() || s.file.isEmpty()) + s.subpath;
        text = s.heading;
        break;
    case T::Block: {
        const QString p = linkPath(s.file, s.path, !s.path.isEmpty());
        if (!s.blockId.isEmpty()) {
            link = p + u"#^"_s + s.blockId;
        } else {
            r.newBlockId = generateBlockId();
            link = p + u"#^"_s + r.newBlockId;
        }
        break;
    }
    }
    // an existing "|alias]]" / "]]" right after the cursor is kept and replaced over
    static const QRegularExpression after(uR"(^(\s*)(\|)?(.*?)(]]))"_s);
    const QRegularExpressionMatch m = after.matchView(line.mid(cursor));
    if (m.hasMatch() && !m.captured(0).contains(u"[[")) {
        if (!m.captured(2).isEmpty() && alias.isEmpty()) {
            alias = m.captured(3);
            if (text.isEmpty())
                text = alias;
        }
        end += int(m.capturedLength(0));
    }
    if (alias.isEmpty() && s.type == T::File && !kImageExt.contains(fileExt(s.file)) && link.contains(u'/'))
        alias = text; // a link with folders gets the plain name as display text
    QString repl;
    int selFrom = 0, selTo = 0;
    if (mdLinks && key != u"#" && key != u"^") {
        // encodeURIComponent for "\\", control characters and spaces
        QString dest;
        for (const QChar c : link) {
            const char16_t u = c.unicode();
            if (u == u'\\' || u == u' ' || u == 0 || u == 0x08 || u == 0x0B || u == 0x0C || (u >= 0x0E && u <= 0x1F))
                dest += u'%' + QString::number(u, 16).toUpper().rightJustified(2, u'0');
            else
                dest += c;
        }
        repl = u"["_s + text + u"]("_s + dest + u")"_s;
        selFrom = selTo = !text.isEmpty() ? linkStart + int(repl.size()) : linkStart + 1;
    } else {
        if (key == u"#") {
            link += u'#';
            if (s.type == T::File && fileExt(s.file) == u"pdf")
                link += u"page="_s;
        } else if (key == u"^") {
            link += u'^';
        }
        if (!alias.isEmpty())
            link += u'|' + alias;
        const QString close = link.endsWith(u"]]") ? QString() : u"]]"_s;
        repl = u"[["_s + link + close;
        const int beforeClose = linkStart + int(repl.size()) - 2;
        if (key == u"Tab") {
            selFrom = alias.isEmpty() ? beforeClose : beforeClose - int(alias.size());
            selTo = beforeClose;
        } else if (key == u"#" || key == u"^" || key == u"|") {
            selFrom = selTo = alias.isEmpty() ? beforeClose : beforeClose - int(alias.size()) - 1;
        } else {
            selFrom = selTo = linkStart + int(repl.size());
        }
    }
    r.replacement = repl;
    r.start = linkStart;
    r.end = end;
    r.selectionStart = selFrom;
    r.selectionEnd = selTo;
    return r;
}

BlockIdInsertion blockIdInsertion(const LinkSuggestion& b, const QString& id)
{
    BlockIdInsertion ins;
    const bool inlineId = b.blockType == u"paragraph" || b.blockType == u"listItem";
    int end = b.blockTo;
    QString text = u"^"_s + id;
    if (inlineId) {
        text = u' ' + text;
        if (b.blockType == u"listItem" && b.blockParagraphEnd >= 0)
            end = b.blockParagraphEnd;
    } else {
        text = u"\n\n"_s + text;
        ins.newlines = 2;
    }
    const QString& c = b.content;
    if (!inlineId && !(end + 1 < c.size() && c[end] == u'\n' && c[end + 1] == u'\n')) {
        text += u'\n';
        ++ins.newlines;
    }
    ins.at = end;
    ins.text = text;
    return ins;
}

QString generateBlockId()
{
    QString id;
    for (int i = 0; i < 6; ++i)
        id += QString::number(QRandomGenerator::global()->bounded(16), 16);
    return id;
}

QString headingSubpathText(const QString& heading)
{
    static const QRegularExpression special(uR"(([:#|^\\\r\n]|%%|\[\[|]]))"_s);
    static const QRegularExpression ws(u"\\s+"_s);
    QString s = heading;
    s.replace(special, u" "_s);
    s.replace(ws, u" "_s);
    return s.trimmed();
}

// ---------------------------------------------------------------------------------------------
// tags
// ---------------------------------------------------------------------------------------------
QHash<QString, int> vaultTags(const MetadataCache& cache)
{
    static const QRegularExpression digitsOnly(u"^#\\d+$"_s);
    QHash<QString, int> counts;
    const QStringList files = cache.markdownFiles();
    for (const QString& f : files) {
        const md::CachedMetadata* meta = cache.fileCache(f);
        if (!meta)
            continue;
        for (QString tag : meta->allTags()) {
            if (tag.endsWith(u'/'))
                tag.chop(1);
            while (tag.size() > 1 && !digitsOnly.matchView(tag).hasMatch()) {
                counts[tag] += 1;
                const qsizetype slash = tag.lastIndexOf(u'/');
                if (slash < 0)
                    break;
                tag.truncate(slash);
            }
        }
    }
    // merge spellings case-insensitively; the most frequent spelling wins
    struct Agg {
        QString tag;
        int count = 0;
        int max = 0;
    };
    QHash<QString, Agg> merged;
    QStringList keys = counts.keys();
    keys.sort(); // deterministic tie-breaks
    for (const QString& t : keys) {
        const int n = counts.value(t);
        Agg& a = merged[t.toLower()];
        if (a.tag.isEmpty()) {
            a = {t, n, n};
        } else {
            a.count += n;
            if (n > a.max) {
                a.max = n;
                a.tag = t;
            }
        }
    }
    QHash<QString, int> out;
    for (const Agg& a : std::as_const(merged))
        out.insert(a.tag, a.count);
    return out;
}

QList<TagSuggestion> tagSuggestions(const MetadataCache& cache, const QString& queryIn)
{
    const QString query = queryIn.toLower();
    QStringList tags;
    for (const QString& t : vaultTags(cache).keys())
        tags << t.mid(1);
    QCollator col;
    col.setNumericMode(true);
    col.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(tags.begin(), tags.end(), [&](const QString& a, const QString& b) {
        const int c = col.compare(a, b);
        return c != 0 ? c < 0 : a < b;
    });
    QList<TagSuggestion> out;
    const FuzzyQuery q(query);
    for (const QString& t : tags) {
        if (query.isEmpty()) {
            out.append({t, 0, {}});
        } else if (const auto m = q.match(t.toLower())) {
            out.append({t, m->score, matchIndicesToRanges(m->matches)});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const TagSuggestion& a, const TagSuggestion& b) { return a.score > b.score; });
    return out;
}

} // namespace oc
