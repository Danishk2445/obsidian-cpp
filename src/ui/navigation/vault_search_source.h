#pragma once

#include "core/search/search_engine.h"
#include "text/text.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace oc {

namespace md {
struct CachedMetadata;
}

class Document;
class TFile;
class VaultSession;

namespace detail {
struct SearchDocCache;
}

// One search's view of the vault: the files it covers (sorted by path) and what is needed to load each of them
// on a worker thread. Captured on the GUI thread; load() is thread-safe. Keeps the documents it loaded so the
// search view can build snippets for the results it receives.
class SearchSnapshot : public std::enable_shared_from_this<SearchSnapshot> {
public:
    enum class Kind : quint8 { Note, Canvas, Other };
    struct Entry {
        QString path;
        QString absolutePath;
        Kind kind = Kind::Note;
        qint64 mtimeMs = 0;
        qint64 ctimeMs = 0;
        std::shared_ptr<const search::SearchDocument> cached;   // still valid in the source's cache
        std::shared_ptr<const md::CachedMetadata> metadata;     // notes indexed by the MetadataCache
        std::optional<Text> liveText;                           // an open document with unsaved edits
    };

    qsizetype count() const { return qsizetype(m_entries.size()); }
    const Entry& entry(qsizetype i) const { return m_entries[size_t(i)]; }
    qsizetype indexOf(const QString& path) const;

    // The engine's source over this snapshot (keeps the snapshot alive while the search runs).
    search::SearchSource source();
    // Loads (or returns the cached) document of entry i; nullptr when the file can't be read. Thread-safe.
    std::shared_ptr<const search::SearchDocument> load(qsizetype i);
    // The document loaded for entry i, nullptr if load() hasn't run for it. Thread-safe.
    std::shared_ptr<const search::SearchDocument> document(qsizetype i) const;

private:
    friend class VaultSearchSource;
    std::shared_ptr<detail::SearchDocCache> m_cache;
    quint64 m_epoch = 0;
    std::vector<Entry> m_entries;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<const search::SearchDocument>> m_docs; // guarded by m_mutex
};

// The global search's document source (Obsidian searches app.vault with cachedRead + the metadata cache):
// notes and canvases of the vault, with open documents' live text, excluding "Excluded files". Loaded
// SearchDocuments are cached per path until the file, its metadata or its open document change. One per session.
class VaultSearchSource : public QObject {
    Q_OBJECT
public:
    explicit VaultSearchSource(VaultSession* session, QObject* parent = nullptr);
    ~VaultSearchSource() override;

    // The session's shared instance (created on first use as a child of the session).
    static VaultSearchSource* forSession(VaultSession* session);

    // Every searchable file; with includeNonNotes also attachments (only file:/path: terms can match those).
    std::shared_ptr<SearchSnapshot> snapshot(bool includeNonNotes);
    // Just the given files (missing, excluded or unsearchable paths are skipped), for incremental updates.
    std::shared_ptr<SearchSnapshot> snapshot(const QStringList& paths, bool includeNonNotes);

    void invalidate(const QString& path); // the path itself and everything below it
    void invalidateAll();
    qsizetype cachedDocumentCount() const;
    bool isIndexReady() const; // the metadata cache finished its initial index

signals:
    // Search input of `path` changed: content, metadata, live text, creation, deletion or rename (emitted for the
    // old and the new path; folders are reported as the folder path). Synchronous; views debounce.
    void pathChanged(const QString& path);
    // The metadata cache (re)finished indexing: tags, sections and properties of every note are known now.
    void indexReady();

private:
    void watchDocument(Document* doc);
    std::shared_ptr<SearchSnapshot> makeSnapshot(const QList<TFile*>& files, bool includeNonNotes);

    QPointer<VaultSession> m_session;
    std::shared_ptr<detail::SearchDocCache> m_cache;
    QSet<const QObject*> m_watched; // documents whose live text is followed
    bool m_indexReady = false;
};

} // namespace oc
