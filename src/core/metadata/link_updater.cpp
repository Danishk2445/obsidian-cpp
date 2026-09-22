#include "core/metadata/link_updater.h"

#include "core/metadata/metadata_cache.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc {

namespace {
// The shapes Obsidian recognizes when rewriting a link's source text.
const QRegularExpression& wikiRe()
{
    static const QRegularExpression re(QStringLiteral(R"(^(!?\[\[)(.*?)(\|(.*))?(\]\])$)"));
    return re;
}
const QRegularExpression& markdownRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(!?\[)(.*?)(\]\(\s*)((<[^>]*?>|[^ "]+?)(\s+([^ ]+|"[^"]+"|'[^']+'|\([^']+\)))?)?(\s*\))$)"));
    return re;
}

QString lastSegment(const QString& p) { return p.mid(p.lastIndexOf(QLatin1Char('/')) + 1); }
// Name without its extension ("a/b.md" -> "b"); dotfiles and trailing dots keep the whole name.
QString stem(const QString& p)
{
    const QString name = lastSegment(p);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 || dot == name.size() - 1 ? name : name.left(dot);
}
QString withoutExtension(const QString& p)
{
    const qsizetype dot = p.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 || dot == p.size() - 1 ? p : p.left(dot);
}
QString extension(const QString& p)
{
    const qsizetype dot = p.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 || dot == p.size() - 1 ? QString() : p.mid(dot + 1).toLower();
}
// Display name: notes without ".md", other files with their extension.
QString displayName(const QString& p)
{
    const QString name = lastSegment(p);
    return extension(name) == u"md" ? stem(name) : name;
}
QString pathWithoutMd(const QString& p) { return extension(lastSegment(p)) == u"md" ? withoutExtension(p) : p; }
QString linkpathOf(const QString& link)
{
    const qsizetype hash = link.indexOf(QLatin1Char('#'));
    return hash < 0 ? link : link.left(hash);
}
QString subpathOf(const QString& link)
{
    const qsizetype hash = link.indexOf(QLatin1Char('#'));
    return hash < 0 ? QString() : link.mid(hash);
}
QString nbspToSpace(QString s)
{
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    s.replace(QChar(0x202F), QLatin1Char(' '));
    return s;
}
// Percent-encode what would break a markdown link destination: spaces, backslashes, some controls.
QString encodeDestination(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u == u'\\' || u == u' ' || u == 0x00 || u == 0x08 || u == 0x0B || u == 0x0C || (u >= 0x0E && u <= 0x1F))
            out += QStringLiteral("%%1").arg(u, 2, 16, QLatin1Char('0')).toUpper();
        else
            out += c;
    }
    return out;
}
} // namespace

LinkUpdater::PathMap LinkUpdater::renameMap(const QString& oldPath, const QString& newPath)
{
    return [oldPath, newPath](const QString& p) -> QString {
        if (p == oldPath)
            return newPath;
        if (p.size() > oldPath.size() && p.startsWith(oldPath) && p.at(oldPath.size()) == u'/')
            return newPath + p.mid(oldPath.size());
        return p;
    };
}

QStringList LinkUpdater::resolveAll(const LinkResolver& resolver, const QString& linkpath, const QString& sourcePath, bool markdownLink)
{
    if (linkpath.trimmed().isEmpty())
        return {};
    QStringList out = resolver.candidates(linkpath, sourcePath);
    const QString best = resolver.resolve(linkpath, sourcePath, markdownLink);
    if (!best.isEmpty()) {
        out.removeAll(best);
        out.prepend(best);
    }
    return out;
}

QList<LinkUpdater::Reference> LinkUpdater::snapshot(const MetadataCache& cache)
{
    return snapshot(cache.resolver(), cache.markdownFiles(), [&cache](const QString& p) { return cache.fileCache(p); });
}

QList<LinkUpdater::Reference> LinkUpdater::snapshot(const LinkResolver& resolver, QStringList files, const MetadataLookup& metadata)
{
    QList<Reference> out;
    files.sort();
    for (const QString& path : std::as_const(files)) {
        const md::CachedMetadata* meta = metadata(path);
        if (!meta)
            continue;
        for (const QList<md::LinkCache>* list : {&meta->frontmatterLinks, &meta->links, &meta->embeds}) {
            for (const md::LinkCache& l : *list) {
                const QStringList resolved = resolveAll(resolver, linkpathOf(l.link), path, l.markdown);
                if (!resolved.isEmpty())
                    out.append(Reference{path, l, resolved});
            }
        }
    }
    return out;
}

QHash<QString, QList<LinkUpdater::Edit>> LinkUpdater::edits(const QList<Reference>& refs, const LinkResolver& after,
                                                            const PathMap& moved, LinkResolver::LinkFormat format)
{
    QHash<QString, QList<Edit>> out;
    for (const Reference& r : refs) {
        const QString source = moved(r.sourcePath);
        const QStringList now = resolveAll(after, linkpathOf(r.link.link), source, r.link.markdown);
        if (!now.isEmpty() && now == r.resolvedPaths)
            continue; // still means the same file(s)
        const bool wiki = wikiRe().match(r.link.original).hasMatch();
        const QString target = moved(r.resolvedPaths.first());
        const QString newLink = after.linkText(target, source, format, wiki) + subpathOf(r.link.link);
        if (newLink == r.link.link)
            continue;
        out[source].append(Edit{r.link.position.start.offset, r.link.position.end.offset, r.link.original, rewrite(r.link, newLink)});
    }
    return out;
}

QString LinkUpdater::rewrite(const md::LinkCache& link, const QString& t)
{
    const QString& r = link.original;
    if (const QRegularExpressionMatch a = wikiRe().match(r); a.hasMatch()) {
        const QString open = a.captured(1), oldLink = a.captured(2), close = a.captured(5);
        QString alias = a.captured(4);
        if (alias.isEmpty())
            return open + t + close;
        const QString pipe = r.contains(QLatin1String("\\|")) ? QStringLiteral("\\|") : QStringLiteral("|");
        // An alias that merely repeated the old file name follows the rename.
        if (oldLink.contains(QLatin1Char('/')) && stem(oldLink) == alias.trimmed())
            alias = stem(linkpathOf(t));
        return open + t + pipe + alias + close;
    }
    const QRegularExpressionMatch f = markdownRe().match(r);
    const bool angle = f.hasMatch() && f.captured(5).startsWith(QLatin1Char('<'));
    const QString dest = angle ? QLatin1Char('<') + t + QLatin1Char('>') : encodeDestination(t);
    if (!f.hasMatch())
        return (r.startsWith(QLatin1Char('!')) ? QStringLiteral("![](") : QStringLiteral("[](")) + dest + QLatin1Char(')');
    QString text = f.captured(2);
    const QString shown = nbspToSpace(text).trimmed();
    const QString oldPath = linkpathOf(link.link);
    // Link text that showed the old name (or path) follows the rename.
    if (shown == displayName(oldPath))
        text = displayName(linkpathOf(t));
    else if (shown.contains(QLatin1Char('/')) && shown == pathWithoutMd(oldPath))
        text = pathWithoutMd(linkpathOf(t));
    return f.captured(1) + text + f.captured(3) + dest + f.captured(6) + f.captured(8);
}

int LinkUpdater::apply(Text& text, QList<Edit> edits)
{
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.from > b.from; });
    int applied = 0;
    qsizetype limit = text.length() + 1; // edits must not overlap the ones already applied (to the right)
    for (const Edit& e : std::as_const(edits)) {
        if (e.from < 0 || e.to > text.length() || e.to < e.from || e.to > limit)
            continue;
        if (text.slice(e.from, e.to) != e.expected)
            continue;
        text = text.replace(e.from, e.to, e.text, text.dominantEol());
        limit = e.from;
        ++applied;
    }
    return applied;
}

} // namespace oc
