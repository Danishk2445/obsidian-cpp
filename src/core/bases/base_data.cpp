#include "core/bases/base_data.h"

#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "markdown/syntax_tree.h"
#include "text/text.h"

namespace oc::bases {

FileStat DataSource::makeStat(const QString& path, qint64 size, qint64 ctime, qint64 mtime)
{
    FileStat s;
    s.path = path;
    const qsizetype slash = path.lastIndexOf(u'/');
    s.name = path.mid(slash + 1);
    s.folder = slash < 0 ? QStringLiteral("/") : path.left(slash);
    const qsizetype dot = s.name.lastIndexOf(u'.');
    if (dot > 0) {
        s.basename = s.name.left(dot);
        s.extension = s.name.mid(dot + 1);
    } else {
        s.basename = s.name;
    }
    s.size = size;
    s.ctime = ctime;
    s.mtime = mtime;
    return s;
}

// ---------------------------------------------------------------------------------------------
// VaultDataSource
// ---------------------------------------------------------------------------------------------
VaultDataSource::VaultDataSource(VaultSession* session, std::function<bool(const QString&)> ignored)
    : m_session(session)
    , m_ignored(std::move(ignored))
{
}

QStringList VaultDataSource::files() const
{
    QStringList out;
    Vault* v = m_session ? m_session->vault() : nullptr;
    if (!v)
        return out;
    const QList<TFile*> files = v->files();
    out.reserve(files.size());
    for (const TFile* f : files)
        out << f->path();
    return out;
}

std::optional<FileStat> VaultDataSource::stat(const QString& path) const
{
    Vault* v = m_session ? m_session->vault() : nullptr;
    const TFile* f = v ? v->file(path) : nullptr;
    if (!f)
        return std::nullopt;
    FileStat s = makeStat(path, f->size(), f->ctimeMs(), f->mtimeMs());
    s.basename = f->basename();
    s.extension = f->extension();
    return s;
}

const md::CachedMetadata* VaultDataSource::metadata(const QString& path) const
{
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    return mc ? mc->fileCache(path) : nullptr;
}

QString VaultDataSource::resolveLink(const QString& linkpath, const QString& sourcePath) const
{
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    return mc ? mc->resolver().resolve(linkpath, sourcePath) : QString();
}

QStringList VaultDataSource::backlinks(const QString& path) const
{
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    return mc ? mc->backlinksOf(path) : QStringList();
}

QString VaultDataSource::linkText(const QString& path, const QString& sourcePath) const
{
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    return mc ? mc->resolver().linkText(path, sourcePath, LinkResolver::LinkFormat::Shortest) : path;
}

bool VaultDataSource::isIgnored(const QString& path) const { return m_ignored && m_ignored(path); }

// ---------------------------------------------------------------------------------------------
// MemoryDataSource
// ---------------------------------------------------------------------------------------------
void MemoryDataSource::addFile(const QString& path, const QString& content, qint64 ctime, qint64 mtime)
{
    File f;
    f.stat = makeStat(path, content.toUtf8().size(), ctime, mtime);
    if (f.stat.extension.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0) {
        const Text text = Text::fromString(content);
        md::SyntaxTree tree;
        tree.parse(text);
        f.meta = md::extractMetadata(tree, text);
    }
    if (!m_files.contains(path))
        m_order << path;
    m_files.insert(path, f);
}

void MemoryDataSource::removeFile(const QString& path)
{
    m_files.remove(path);
    m_order.removeAll(path);
}

std::optional<FileStat> MemoryDataSource::stat(const QString& path) const
{
    auto it = m_files.constFind(path);
    return it == m_files.cend() ? std::nullopt : std::optional<FileStat>(it->stat);
}

const md::CachedMetadata* MemoryDataSource::metadata(const QString& path) const
{
    auto it = m_files.constFind(path);
    return it == m_files.cend() || !it->meta ? nullptr : &*it->meta;
}

QString MemoryDataSource::resolveLink(const QString& linkpath, const QString& sourcePath) const
{
    Q_UNUSED(sourcePath);
    if (linkpath.isEmpty())
        return {};
    for (const QString& p : m_order)
        if (p.compare(linkpath, Qt::CaseInsensitive) == 0 || p.compare(linkpath + QStringLiteral(".md"), Qt::CaseInsensitive) == 0)
            return p;
    const QString lower = linkpath.toLower();
    for (const QString& p : m_order) {
        const FileStat& s = m_files[p].stat;
        const QString key = s.extension.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? s.basename : s.name;
        if (key.toLower() == lower || p.toLower().endsWith(u'/' + lower) || p.toLower().endsWith(u'/' + lower + QStringLiteral(".md")))
            return p;
    }
    return {};
}

QStringList MemoryDataSource::backlinks(const QString& path) const
{
    QStringList out;
    for (const QString& p : m_order) {
        const md::CachedMetadata* m = metadata(p);
        if (!m)
            continue;
        bool hit = false;
        for (const QList<md::LinkCache>* refs : {&m->links, &m->embeds, &m->frontmatterLinks}) {
            for (const md::LinkCache& l : *refs) {
                const qsizetype hash = l.link.indexOf(u'#');
                if (resolveLink(hash < 0 ? l.link : l.link.left(hash), p) == path) {
                    hit = true;
                    break;
                }
            }
            if (hit)
                break;
        }
        if (hit)
            out << p;
    }
    return out;
}

QString MemoryDataSource::linkText(const QString& path, const QString& sourcePath) const
{
    Q_UNUSED(sourcePath);
    auto it = m_files.constFind(path);
    if (it == m_files.cend())
        return path;
    return it->stat.extension.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? it->stat.basename : it->stat.name;
}

} // namespace oc::bases
