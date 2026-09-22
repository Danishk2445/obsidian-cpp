#include "ui/navigation/vault_search_source.h"

#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"
#include "markdown/syntax_tree.h"
#include "text/codec.h"
#include "ui/navigation/navigation_util.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace oc {

namespace detail {

// Loaded documents shared by all snapshots of a session. An invalidation bumps the path's epoch, so a load that
// started before it (from an older snapshot) can't put a stale document back.
struct SearchDocCache {
    // Cap on the cached text (UTF-16 code units); beyond it new documents are simply not cached.
    static constexpr qsizetype kMaxChars = qsizetype(48) * 1024 * 1024;

    mutable std::mutex mutex;
    QHash<QString, std::shared_ptr<const search::SearchDocument>> docs;
    QHash<QString, quint64> invalidatedAt;
    quint64 clock = 0;
    quint64 allInvalidatedAt = 0;
    qsizetype chars = 0;

    std::shared_ptr<const search::SearchDocument> find(const QString& path) const
    {
        std::lock_guard lock(mutex);
        return docs.value(path);
    }
    quint64 epoch() const
    {
        std::lock_guard lock(mutex);
        return clock;
    }
    void insert(const QString& path, const std::shared_ptr<const search::SearchDocument>& doc, quint64 epoch)
    {
        std::lock_guard lock(mutex);
        if (allInvalidatedAt > epoch || invalidatedAt.value(path) > epoch || docs.contains(path))
            return;
        const qsizetype size = doc->content.size() + doc->path.size();
        if (chars + size > kMaxChars)
            return;
        chars += size;
        docs.insert(path, doc);
    }
    void invalidate(const QString& path)
    {
        std::lock_guard lock(mutex);
        ++clock;
        invalidatedAt.insert(path, clock);
        const QString prefix = path + QLatin1Char('/');
        for (auto it = docs.begin(); it != docs.end();) {
            if (it.key() == path || it.key().startsWith(prefix)) {
                chars -= it.value()->content.size() + it.value()->path.size();
                invalidatedAt.insert(it.key(), clock);
                it = docs.erase(it);
            } else {
                ++it;
            }
        }
    }
    void invalidateAll()
    {
        std::lock_guard lock(mutex);
        allInvalidatedAt = ++clock;
        docs.clear();
        chars = 0;
    }
};

} // namespace detail

namespace {

SearchSnapshot::Kind kindOf(const TFile* f)
{
    const QString ext = f->extension().toLower();
    if (ext == u"md")
        return SearchSnapshot::Kind::Note;
    if (ext == u"canvas")
        return SearchSnapshot::Kind::Canvas;
    return SearchSnapshot::Kind::Other;
}

// The searchable text of a canvas: its text cards and group labels, one paragraph each.
QString canvasText(const QByteArray& bytes)
{
    const QJsonArray nodes = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("nodes")).toArray();
    QStringList parts;
    for (const QJsonValue& v : nodes) {
        const QJsonObject n = v.toObject();
        const QString type = n.value(QStringLiteral("type")).toString();
        if (type == u"text")
            parts << n.value(QStringLiteral("text")).toString();
        else if (type == u"group" && n.contains(QStringLiteral("label")))
            parts << n.value(QStringLiteral("label")).toString();
    }
    return parts.join(QStringLiteral("\n\n"));
}

std::shared_ptr<const search::SearchDocument> buildDocument(const SearchSnapshot::Entry& e)
{
    auto doc = std::make_shared<search::SearchDocument>();
    doc->path = e.path;
    if (e.kind == SearchSnapshot::Kind::Other) {
        doc->hasContent = false;
        return doc;
    }
    if (e.kind == SearchSnapshot::Kind::Note) {
        Text text;
        if (e.liveText) {
            text = *e.liveText;
        } else {
            QFile f(e.absolutePath);
            if (!f.open(QIODevice::ReadOnly))
                return nullptr;
            text = decodeFile(f.readAll()).text;
        }
        const QString content = text.toString();
        if (e.metadata && !e.liveText) {
            *doc = search::SearchDocument::fromMetadata(e.path, content, *e.metadata);
        } else {
            // Unsaved edits or not indexed yet: parse like the metadata cache would.
            md::SyntaxTree tree;
            tree.parse(text);
            *doc = search::SearchDocument::fromMetadata(e.path, content, md::extractMetadata(tree, text));
        }
        return doc;
    }
    QFile f(e.absolutePath);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    doc->content = canvasText(f.readAll());
    return doc;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// SearchSnapshot
// ---------------------------------------------------------------------------------------------

qsizetype SearchSnapshot::indexOf(const QString& path) const
{
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), path,
                                     [](const Entry& e, const QString& p) { return e.path < p; });
    return it != m_entries.end() && it->path == path ? qsizetype(it - m_entries.begin()) : -1;
}

search::SearchSource SearchSnapshot::source()
{
    search::SearchSource s;
    s.count = count();
    std::shared_ptr<SearchSnapshot> self = shared_from_this();
    s.load = [self](qsizetype i) { return self->load(i); };
    return s;
}

std::shared_ptr<const search::SearchDocument> SearchSnapshot::load(qsizetype i)
{
    if (i < 0 || i >= count())
        return nullptr;
    const Entry& e = m_entries[size_t(i)];
    std::shared_ptr<const search::SearchDocument> doc = e.cached;
    if (!doc) {
        doc = buildDocument(e);
        if (doc && m_cache && !e.liveText)
            m_cache->insert(e.path, doc, m_epoch);
    }
    std::lock_guard lock(m_mutex);
    m_docs[size_t(i)] = doc;
    return doc;
}

std::shared_ptr<const search::SearchDocument> SearchSnapshot::document(qsizetype i) const
{
    std::lock_guard lock(m_mutex);
    return i >= 0 && i < qsizetype(m_docs.size()) ? m_docs[size_t(i)] : nullptr;
}

// ---------------------------------------------------------------------------------------------
// VaultSearchSource
// ---------------------------------------------------------------------------------------------

VaultSearchSource::VaultSearchSource(VaultSession* session, QObject* parent)
    : QObject(parent)
    , m_session(session)
    , m_cache(std::make_shared<detail::SearchDocCache>())
{
    setObjectName(QStringLiteral("ocVaultSearchSource"));
    if (!session)
        return;
    auto changed = [this](const QString& path) {
        invalidate(path);
        emit pathChanged(path);
    };
    if (Vault* v = session->vault()) {
        connect(v, &Vault::created, this, [this](TAbstractFile* item, bool) { emit pathChanged(item->path()); });
        connect(v, &Vault::modified, this, [changed](TFile* f, bool) { changed(f->path()); });
        connect(v, &Vault::deleted, this, [changed](TAbstractFile* item, bool) { changed(item->path()); });
        connect(v, &Vault::renamed, this, [changed](TAbstractFile* item, const QString& oldPath, bool) {
            changed(oldPath);
            changed(item->path());
        });
    }
    if (MetadataCache* mc = session->metadata()) {
        m_indexReady = mc->isReady();
        connect(mc, &MetadataCache::changed, this, changed);
        connect(mc, &MetadataCache::deleted, this, changed);
        connect(mc, &MetadataCache::resolved, this, [this, mc] {
            if (mc->isReady() == m_indexReady)
                return;
            m_indexReady = mc->isReady();
            if (m_indexReady) {
                invalidateAll();
                emit indexReady();
            }
        });
    } else {
        m_indexReady = true;
    }
    if (DocumentManager* dm = session->documents()) {
        for (Document* d : dm->documents())
            watchDocument(d);
        connect(dm, &DocumentManager::documentOpened, this, &VaultSearchSource::watchDocument);
        connect(dm, &DocumentManager::documentClosed, this, changed);
    }
    if (VaultConfig* cfg = session->vaultConfig()) {
        connect(cfg, &VaultConfig::configChanged, this, [this](const QString& key) {
            if (key == u"userIgnoreFilters") {
                invalidateAll();
                emit pathChanged(QString()); // the root: everything may have changed
            }
        });
    }
}

VaultSearchSource::~VaultSearchSource() = default;

VaultSearchSource* VaultSearchSource::forSession(VaultSession* session)
{
    if (!session)
        return nullptr;
    if (auto* s = session->findChild<VaultSearchSource*>(QStringLiteral("ocVaultSearchSource"), Qt::FindDirectChildrenOnly))
        return s;
    return new VaultSearchSource(session, session);
}

void VaultSearchSource::watchDocument(Document* doc)
{
    if (!doc || m_watched.contains(doc))
        return;
    m_watched.insert(doc);
    connect(doc, &QObject::destroyed, this, [this, doc] { m_watched.remove(doc); });
    QPointer<Document> d = doc;
    connect(doc, &Document::changed, this, [this, d] {
        if (!d)
            return;
        invalidate(d->path());
        emit pathChanged(d->path());
    });
}

void VaultSearchSource::invalidate(const QString& path)
{
    if (path.isEmpty())
        m_cache->invalidateAll();
    else
        m_cache->invalidate(path);
}

void VaultSearchSource::invalidateAll() { m_cache->invalidateAll(); }

qsizetype VaultSearchSource::cachedDocumentCount() const
{
    std::lock_guard lock(m_cache->mutex);
    return m_cache->docs.size();
}

bool VaultSearchSource::isIndexReady() const { return m_indexReady; }

std::shared_ptr<SearchSnapshot> VaultSearchSource::snapshot(bool includeNonNotes)
{
    Vault* v = m_session ? m_session->vault() : nullptr;
    return makeSnapshot(v ? v->files() : QList<TFile*>(), includeNonNotes);
}

std::shared_ptr<SearchSnapshot> VaultSearchSource::snapshot(const QStringList& paths, bool includeNonNotes)
{
    Vault* v = m_session ? m_session->vault() : nullptr;
    QList<TFile*> files;
    if (v) {
        for (const QString& p : paths)
            if (TFile* f = v->file(p))
                files.append(f);
    }
    return makeSnapshot(files, includeNonNotes);
}

std::shared_ptr<SearchSnapshot> VaultSearchSource::makeSnapshot(const QList<TFile*>& files, bool includeNonNotes)
{
    auto snap = std::make_shared<SearchSnapshot>();
    snap->m_cache = m_cache;
    snap->m_epoch = m_cache->epoch();
    Vault* v = m_session ? m_session->vault() : nullptr;
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    DocumentManager* dm = m_session ? m_session->documents() : nullptr;
    const UserIgnoreFilters ignore(m_session ? m_session->vaultConfig() : nullptr);
    snap->m_entries.reserve(size_t(files.size()));
    for (TFile* f : files) {
        const SearchSnapshot::Kind kind = kindOf(f);
        if (kind == SearchSnapshot::Kind::Other && !includeNonNotes)
            continue;
        if (!ignore.isEmpty() && ignore.matches(f->path()))
            continue;
        SearchSnapshot::Entry e;
        e.path = f->path();
        e.kind = kind;
        e.mtimeMs = f->mtimeMs();
        e.ctimeMs = f->ctimeMs();
        e.absolutePath = v->absolutePath(e.path);
        if (kind == SearchSnapshot::Kind::Note && dm) {
            if (Document* d = dm->find(e.path); d && d->isLoaded() && d->isDirty())
                e.liveText = d->text();
        }
        if (!e.liveText)
            e.cached = m_cache->find(e.path);
        if (!e.cached && kind == SearchSnapshot::Kind::Note && mc && !e.liveText) {
            if (const md::CachedMetadata* m = mc->fileCache(e.path))
                e.metadata = std::make_shared<const md::CachedMetadata>(*m); // implicitly shared lists: cheap
        }
        snap->m_entries.push_back(std::move(e));
    }
    std::sort(snap->m_entries.begin(), snap->m_entries.end(),
              [](const SearchSnapshot::Entry& a, const SearchSnapshot::Entry& b) { return a.path < b.path; });
    snap->m_docs.resize(snap->m_entries.size());
    return snap;
}

} // namespace oc
