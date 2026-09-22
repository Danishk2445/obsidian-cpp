#include "core/metadata/metadata_cache.h"

#include "core/util/busy.h"
#include "text/codec.h"

#include <QPointer>
#include <QThreadPool>
#include <QtConcurrent>

namespace oc {

namespace {
bool isMarkdownPath(const QString& p) { return p.endsWith(QLatin1String(".md"), Qt::CaseInsensitive); }

QString linkNameKey(const QString& linkpath)
{
    QString l = linkpath.mid(linkpath.lastIndexOf(QLatin1Char('/')) + 1).toLower();
    if (l.endsWith(QLatin1String(".md")))
        l.chop(3);
    return l;
}
} // namespace

MetadataCache::MetadataCache(QObject* parent)
    : QObject(parent)
{
}

MetadataCache::~MetadataCache() { *m_alive = false; }

md::CachedMetadata MetadataCache::parseBytes(const QByteArray& bytes)
{
    const DecodedFile dec = decodeFile(bytes);
    md::SyntaxTree tree;
    tree.parse(dec.text);
    return md::extractMetadata(tree, dec.text);
}

void MetadataCache::indexAll(const QStringList& files)
{
    m_ready = false;
    m_resolver.setFiles(files);
    QStringList md;
    for (const QString& f : files)
        if (isMarkdownPath(f))
            md << f;
    if (md.isEmpty()) {
        m_ready = true;
        emit resolved();
        return;
    }
    parseAsync(md);
}

void MetadataCache::parseAsync(const QStringList& paths)
{
    if (!m_provider)
        return;
    // Read on the GUI thread? No: reading may be slow (FUSE); do read+parse on the pool.
    ContentProvider provider = m_provider;
    std::shared_ptr<bool> alive = m_alive;
    QPointer<MetadataCache> self(this);
    const int batch = 16;
    for (qsizetype i = 0; i < paths.size(); i += batch) {
        const QStringList chunk = paths.mid(i, batch);
        ++m_pending;
        Busy::begin();
        QThreadPool::globalInstance()->start([chunk, provider, alive, self] {
            QList<Result> results;
            for (const QString& p : chunk) {
                if (!*alive)
                    break;
                const std::optional<QByteArray> bytes = provider(p);
                if (!bytes)
                    continue;
                results.push_back({p, parseBytes(*bytes)});
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, results = std::move(results)]() mutable {
                    Busy::end();
                    if (!self)
                        return;
                    for (Result& r : results)
                        self->applyResult(r.path, std::move(r.meta));
                    if (--self->m_pending == 0) {
                        self->m_ready = true;
                        emit self->resolved();
                    }
                },
                Qt::QueuedConnection);
        });
    }
}

void MetadataCache::unresolveSource(const QString& source)
{
    // Remove the source's contribution to the link graph.
    const QHash<QString, int> old = m_resolved.take(source);
    for (auto it = old.cbegin(); it != old.cend(); ++it) {
        auto b = m_backlinks.find(it.key());
        if (b != m_backlinks.end()) {
            b->remove(source);
            if (b->isEmpty())
                m_backlinks.erase(b);
        }
    }
    const QHash<QString, int> oldU = m_unresolved.take(source);
    for (auto it = oldU.cbegin(); it != oldU.cend(); ++it) {
        auto u = m_unresolvedByName.find(linkNameKey(it.key()));
        if (u != m_unresolvedByName.end()) {
            u->remove(source);
            if (u->isEmpty())
                m_unresolvedByName.erase(u);
        }
    }
}

void MetadataCache::resolveSource(const QString& source)
{
    unresolveSource(source);
    const auto it = m_cache.constFind(source);
    if (it == m_cache.cend())
        return;
    QHash<QString, int> res, unres;
    auto consider = [&](const md::LinkCache& l) {
        const auto [path, sub] = LinkResolver::splitSubpath(l.link);
        Q_UNUSED(sub);
        if (path.isEmpty())
            return; // same-file heading/block link
        const QString dest = m_resolver.resolve(path, source, l.markdown);
        if (dest.isEmpty())
            unres[path] += 1;
        else
            res[dest] += 1;
    };
    for (const md::LinkCache& l : it->links)
        consider(l);
    for (const md::LinkCache& l : it->embeds)
        consider(l);
    for (const md::LinkCache& l : it->frontmatterLinks)
        consider(l);
    if (!res.isEmpty())
        m_resolved.insert(source, res);
    if (!unres.isEmpty())
        m_unresolved.insert(source, unres);
    for (auto r = res.cbegin(); r != res.cend(); ++r)
        m_backlinks[r.key()].insert(source);
    for (auto u = unres.cbegin(); u != unres.cend(); ++u)
        m_unresolvedByName[linkNameKey(u.key())].insert(source);
}

void MetadataCache::applyResult(const QString& path, md::CachedMetadata meta)
{
    m_cache.insert(path, std::move(meta));
    resolveSource(path);
    emit changed(path);
}

void MetadataCache::fileCreated(const QString& path)
{
    m_resolver.addFile(path);
    // Unresolved links to this name may now resolve.
    const QSet<QString> sources = m_unresolvedByName.value(linkNameKey(path));
    for (const QString& s : sources)
        resolveSource(s);
    if (isMarkdownPath(path))
        parseAsync({path});
    if (!sources.isEmpty())
        emit resolved();
}

void MetadataCache::fileModified(const QString& path)
{
    if (isMarkdownPath(path))
        parseAsync({path});
}

void MetadataCache::updateFromText(const QString& path, const Text& text)
{
    md::SyntaxTree tree;
    tree.parse(text);
    applyResult(path, md::extractMetadata(tree, text));
    emit resolved();
}

void MetadataCache::fileDeleted(const QString& path)
{
    const QSet<QString> sources = m_backlinks.value(path);
    m_resolver.removeFile(path);
    unresolveSource(path);
    m_cache.remove(path);
    for (const QString& s : sources)
        resolveSource(s); // their links to `path` become unresolved (or resolve elsewhere)
    emit deleted(path);
    emit resolved();
}

void MetadataCache::fileRenamed(const QString& oldPath, const QString& newPath)
{
    const QSet<QString> sources = m_backlinks.value(oldPath);
    m_resolver.removeFile(oldPath);
    m_resolver.addFile(newPath);
    if (m_cache.contains(oldPath)) {
        unresolveSource(oldPath);
        m_cache.insert(newPath, m_cache.take(oldPath));
        resolveSource(newPath); // relative links may resolve differently now
    }
    QSet<QString> affected = sources;
    affected.unite(m_unresolvedByName.value(linkNameKey(newPath)));
    for (const QString& s : affected)
        if (s != newPath)
            resolveSource(s);
    emit deleted(oldPath);
    emit changed(newPath);
    emit resolved();
}

const md::CachedMetadata* MetadataCache::fileCache(const QString& path) const
{
    auto it = m_cache.constFind(path);
    return it == m_cache.cend() ? nullptr : &*it;
}

QString MetadataCache::firstLinkpathDest(const QString& link, const QString& sourcePath) const
{
    const auto [path, sub] = LinkResolver::splitSubpath(link);
    Q_UNUSED(sub);
    return m_resolver.resolve(path, sourcePath);
}

QStringList MetadataCache::backlinksOf(const QString& path) const
{
    QStringList out = m_backlinks.value(path).values();
    out.sort(Qt::CaseInsensitive);
    return out;
}

int MetadataCache::backlinkCount(const QString& path) const
{
    int n = 0;
    for (const QString& s : m_backlinks.value(path))
        n += m_resolved.value(s).value(path);
    return n;
}

QHash<QString, int> MetadataCache::tagCounts() const
{
    QHash<QString, int> out;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        for (const QString& t : it->allTags())
            out[t] += 1;
    return out;
}

QStringList MetadataCache::filesWithTag(const QString& tag) const
{
    const QString want = (tag.startsWith(u'#') ? tag : QLatin1Char('#') + tag).toLower();
    QStringList out;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        for (const QString& t : it->allTags()) {
            const QString l = t.toLower();
            if (l == want || l.startsWith(want + QLatin1Char('/'))) {
                out << it.key();
                break;
            }
        }
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

QHash<QString, QStringList> MetadataCache::aliases() const
{
    QHash<QString, QStringList> out;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        const QStringList a = it->aliases();
        if (!a.isEmpty())
            out.insert(it.key(), a);
    }
    return out;
}

} // namespace oc
