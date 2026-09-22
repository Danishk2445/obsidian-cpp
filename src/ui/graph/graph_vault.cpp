#include "ui/graph/graph_vault.h"

#include "core/metadata/metadata_cache.h"
#include "core/search/search_engine.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>

#include <algorithm>
#include <optional>

namespace oc {

bool isGraphAttachment(const QString& path)
{
    // Obsidian's Cb(): extension (text after the last '.' of the name, not leading/trailing) not in
    // md / canvas / base.
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    const QStringView name = QStringView(path).mid(slash + 1);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return true; // no extension: not a note
    const QStringView ext = name.mid(dot + 1);
    return ext.compare(u"md", Qt::CaseInsensitive) != 0 && ext.compare(u"canvas", Qt::CaseInsensitive) != 0
           && ext.compare(u"base", Qt::CaseInsensitive) != 0;
}

graph::GraphData buildVaultGraph(const Vault* vault, const MetadataCache& metadata)
{
    using graph::NodeKind;
    graph::GraphData g;
    if (vault) {
        QList<TFile*> files = vault->files();
        std::sort(files.begin(), files.end(), [](const TFile* a, const TFile* b) { return a->path() < b->path(); });
        for (const TFile* f : std::as_const(files)) {
            const qint64 ctime = f->ctimeMs() > 0 ? f->ctimeMs() : f->mtimeMs();
            g.addNode(f->path(), isGraphAttachment(f->path()) ? NodeKind::Attachment : NodeKind::File, {}, std::min(ctime, f->mtimeMs()));
        }
    }
    QStringList sources = metadata.markdownFiles();
    std::sort(sources.begin(), sources.end());

    // Obsidian's getTags() spelling: per case-insensitive tag the spelling counted most often (nested
    // tags count towards their parents), the first one seen on ties.
    QHash<QString, QHash<QString, int>> counts;
    QHash<QString, QStringList> seen;
    for (const QString& src : std::as_const(sources)) {
        const md::CachedMetadata* meta = metadata.fileCache(src);
        if (!meta)
            continue;
        for (QString tag : meta->allTags()) {
            while (tag.endsWith(QLatin1Char('/')))
                tag.chop(1);
            for (;;) {
                const QString lower = tag.toLower();
                QHash<QString, int>& c = counts[lower];
                if (!c.contains(tag))
                    seen[lower].append(tag);
                ++c[tag];
                const qsizetype slash = tag.lastIndexOf(QLatin1Char('/'));
                if (slash <= 0)
                    break;
                tag.truncate(slash);
            }
        }
    }
    QHash<QString, QString> canonical;
    for (auto it = seen.cbegin(); it != seen.cend(); ++it) {
        const QHash<QString, int>& c = counts[it.key()];
        QString best;
        int max = -1;
        for (const QString& spelling : it.value())
            if (c.value(spelling) > max) {
                max = c.value(spelling);
                best = spelling;
            }
        canonical.insert(it.key(), best);
    }

    const auto& resolved = metadata.resolvedLinks();
    const auto& unresolved = metadata.unresolvedLinks();
    for (const QString& src : std::as_const(sources)) {
        int s = g.indexOf(src);
        if (s < 0)
            s = g.addNode(src, NodeKind::File);
        if (const auto r = resolved.constFind(src); r != resolved.cend()) {
            QStringList targets = r->keys();
            std::sort(targets.begin(), targets.end());
            for (const QString& t : std::as_const(targets)) {
                int ti = g.indexOf(t);
                if (ti < 0)
                    ti = g.addNode(t, isGraphAttachment(t) ? NodeKind::Attachment : NodeKind::File);
                g.addEdge(s, ti);
            }
        }
        if (const auto u = unresolved.constFind(src); u != unresolved.cend()) {
            QStringList targets = u->keys();
            std::sort(targets.begin(), targets.end());
            for (const QString& t : std::as_const(targets))
                g.addEdge(s, g.addNode(t, NodeKind::Unresolved));
        }
        if (const md::CachedMetadata* meta = metadata.fileCache(src)) {
            for (const QString& tag : meta->allTags()) {
                QString id = tag;
                while (id.endsWith(QLatin1Char('/')))
                    id.chop(1);
                id = canonical.value(id.toLower(), id);
                g.addEdge(s, g.addNode(id, NodeKind::Tag));
            }
        }
    }
    g.finalize(true);
    return g;
}

graph::GraphData graphDataFromSession(VaultSession& session)
{
    if (!session.metadata()) {
        graph::GraphData g;
        g.finalize();
        return g;
    }
    return buildVaultGraph(session.vault(), *session.metadata());
}

// ---------------------------------------------------------------------------------------------
// GraphVaultSearch
// ---------------------------------------------------------------------------------------------
namespace {

constexpr int kMaxEntries = 32;

struct SourceItem {
    QString path;
    bool note = true;
    std::optional<md::CachedMetadata> meta;
};

// Notes as the search sees them: '\n' line breaks (CachedMetadata offsets), no BOM.
std::shared_ptr<search::SearchDocument> loadDocument(const QString& root, const SourceItem& item)
{
    auto doc = std::make_shared<search::SearchDocument>();
    doc->path = item.path;
    if (!item.note || !item.path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) {
        doc->hasContent = false;
        return doc;
    }
    QFile f(root + QLatin1Char('/') + item.path);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    QString content = QString::fromUtf8(f.readAll());
    if (content.startsWith(QChar(0xFEFF)))
        content.remove(0, 1);
    content.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (item.meta)
        *doc = search::SearchDocument::fromMetadata(item.path, content, *item.meta);
    else
        doc->content = std::move(content);
    return doc;
}

} // namespace

struct GraphVaultSearch::Entry {
    QString text;
    search::SearchQuery query;
    QSet<QString> matches; // matching note paths
    std::unique_ptr<search::SearchEngine> engine;
    bool running = false;
    quint64 used = 0;
    QHash<QString, bool> otherNodes; // memoised results for attachment and tag nodes
};

GraphVaultSearch::GraphVaultSearch(VaultSession* session, QObject* parent)
    : QObject(parent)
    , m_session(session)
{
    m_notify.setSingleShot(true);
    m_notify.setInterval(100);
    connect(&m_notify, &QTimer::timeout, this, &GraphVaultSearch::updated);
    if (MetadataCache* mc = session ? session->metadata() : nullptr) {
        connect(mc, &MetadataCache::changed, this, &GraphVaultSearch::fileChanged);
        connect(mc, &MetadataCache::deleted, this, &GraphVaultSearch::fileDeleted);
    }
}

GraphVaultSearch::~GraphVaultSearch()
{
    // Search callbacks are queued to this object: stop every worker first.
    for (const std::shared_ptr<Entry>& e : std::as_const(m_entries))
        e->engine.reset();
}

bool GraphVaultSearch::isSearching() const { return m_searching; }

bool GraphVaultSearch::waitForIdle(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        bool running = false;
        for (const std::shared_ptr<Entry>& e : std::as_const(m_entries))
            running |= e->running;
        if (!running)
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return false;
}

graph::NodePredicate GraphVaultSearch::compile(const QString& text)
{
    std::shared_ptr<Entry> e = m_entries.value(text);
    if (!e) {
        search::SearchQuery query = search::parseSearchQuery(text);
        if (query.isEmpty())
            return {};
        e = std::make_shared<Entry>();
        e->text = text;
        e->query = std::move(query);
        if (m_entries.size() >= kMaxEntries) {
            auto lru = std::min_element(m_entries.begin(), m_entries.end(),
                                        [](const std::shared_ptr<Entry>& a, const std::shared_ptr<Entry>& b) { return a->used < b->used; });
            (*lru)->engine.reset(); // predicates may keep the results; the search stops
            (*lru)->running = false;
            m_entries.erase(lru);
        }
        m_entries.insert(text, e);
        start(e);
    }
    e->used = ++m_clock;
    return [e](const graph::GraphData& g, int i) -> bool {
        const graph::GraphNode& n = g.node(i);
        switch (n.kind) {
        case graph::NodeKind::File:
            return e->matches.contains(n.id);
        case graph::NodeKind::Unresolved:
            return true;
        case graph::NodeKind::Attachment:
        case graph::NodeKind::Tag:
            break;
        }
        const auto memo = e->otherNodes.constFind(n.id);
        if (memo != e->otherNodes.cend())
            return *memo;
        search::SearchDocument doc;
        doc.path = n.id;
        if (n.kind == graph::NodeKind::Attachment) {
            doc.hasContent = false; // attachments: file:/path: terms only (Obsidian's matchFilepath)
        } else {
            doc.content = n.id; // a tag: its text, as content and as the document's only tag
            doc.tags.append(search::SearchTag{n.id, 0, 0, int(n.id.size())});
        }
        const bool match = search::documentMatches(e->query, doc);
        e->otherNodes.insert(n.id, match);
        return match;
    };
}

void GraphVaultSearch::start(const std::shared_ptr<Entry>& entry)
{
    Vault* vault = m_session ? m_session->vault() : nullptr;
    MetadataCache* mc = m_session ? m_session->metadata() : nullptr;
    auto items = std::make_shared<std::vector<SourceItem>>();
    QString root = m_session ? m_session->root() : QString();
    if (vault) {
        root = vault->root();
        for (const TFile* f : vault->files()) {
            if (isGraphAttachment(f->path()))
                continue;
            SourceItem it;
            it.path = f->path();
            it.note = f->isMarkdown();
            if (mc)
                if (const md::CachedMetadata* meta = mc->fileCache(it.path))
                    it.meta = *meta;
            items->push_back(std::move(it));
        }
    } else if (mc) {
        for (const QString& path : mc->markdownFiles()) {
            SourceItem it;
            it.path = path;
            if (const md::CachedMetadata* meta = mc->fileCache(path))
                it.meta = *meta;
            items->push_back(std::move(it));
        }
    }
    search::SearchSource source;
    source.count = qsizetype(items->size());
    source.load = [items, root](qsizetype i) -> std::shared_ptr<const search::SearchDocument> { return loadDocument(root, (*items)[size_t(i)]); };

    std::weak_ptr<Entry> weak = entry;
    search::SearchCallbacks cb;
    cb.context = this;
    cb.onResults = [this, weak](QList<search::SearchResult> batch) {
        if (auto e = weak.lock()) {
            for (const search::SearchResult& r : std::as_const(batch))
                e->matches.insert(r.path);
            scheduleUpdate();
        }
    };
    cb.onFinished = [this, weak](const search::SearchStats&) {
        if (auto e = weak.lock())
            e->running = false;
        updateSearching();
        scheduleUpdate();
    };
    entry->engine = std::make_unique<search::SearchEngine>();
    entry->running = true;
    updateSearching();
    entry->engine->search(entry->query, std::move(source), std::move(cb));
}

void GraphVaultSearch::fileChanged(const QString& path)
{
    if (m_entries.isEmpty() || isGraphAttachment(path))
        return;
    SourceItem item;
    item.path = path;
    if (MetadataCache* mc = m_session->metadata())
        if (const md::CachedMetadata* meta = mc->fileCache(path))
            item.meta = *meta;
    const QString root = m_session->vault() ? m_session->vault()->root() : m_session->root();
    const std::shared_ptr<search::SearchDocument> doc = loadDocument(root, item);
    for (const std::shared_ptr<Entry>& e : std::as_const(m_entries)) {
        const bool match = doc && search::documentMatches(e->query, *doc);
        if (match != e->matches.contains(path)) {
            if (match)
                e->matches.insert(path);
            else
                e->matches.remove(path);
            scheduleUpdate();
        }
    }
}

void GraphVaultSearch::fileDeleted(const QString& path)
{
    for (const std::shared_ptr<Entry>& e : std::as_const(m_entries))
        if (e->matches.remove(path))
            scheduleUpdate();
}

void GraphVaultSearch::scheduleUpdate()
{
    if (!m_notify.isActive())
        m_notify.start();
}

void GraphVaultSearch::updateSearching()
{
    bool running = false;
    for (const std::shared_ptr<Entry>& e : std::as_const(m_entries))
        running |= e->running;
    if (running != m_searching) {
        m_searching = running;
        emit searchingChanged(running);
    }
}

} // namespace oc
