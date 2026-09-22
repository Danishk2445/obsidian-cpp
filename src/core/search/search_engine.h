#pragma once

#include "core/search/search_match.h"

#include <QList>

#include <functional>
#include <memory>
#include <vector>

class QObject;
class QThreadPool;

namespace oc::search {

// The documents a search runs over, addressed by index 0..count-1. `load` is called concurrently from worker
// threads (it must be thread-safe) and may return nullptr to skip a document (deleted, unreadable, excluded).
struct SearchSource {
    qsizetype count = 0;
    std::function<std::shared_ptr<const SearchDocument>(qsizetype index)> load;

    static SearchSource fromDocuments(QList<std::shared_ptr<const SearchDocument>> documents);
};

struct SearchStats {
    qsizetype total = 0;       // documents in the source
    qsizetype searched = 0;    // documents evaluated completely
    qsizetype matched = 0;     // documents that matched
    bool canceled = false;
    qint64 elapsedNs = 0;      // start -> all workers done
    qint64 firstResultNs = -1; // start -> first batch handed to delivery (-1: no result)
};

struct SearchCallbacks {
    // A batch of matching documents, in no particular order (the view sorts). Never called concurrently.
    std::function<void(QList<SearchResult> batch)> onResults;
    // Called exactly once, after the last batch, also when the search was canceled.
    std::function<void(const SearchStats& stats)> onFinished;
    // With a context, both callbacks are queued to its thread, and batches still queued when the search is
    // canceled are dropped. The context must outlive the search (cancel and wait, or destroy the engine, first).
    // Without one, they run on the worker threads (serialized); onFinished may run before search() returns.
    QObject* context = nullptr;
};

struct SearchEngineOptions {
    int batchSize = 64;       // a batch is delivered as soon as this many results are pending...
    int batchIntervalMs = 25; // ...or this long after the previous one (the first result goes out immediately)
    int maxThreads = 0;       // 0: the pool's maxThreadCount()
};

// Handle of one running search.
class SearchJob {
public:
    ~SearchJob();
    SearchJob(const SearchJob&) = delete;
    SearchJob& operator=(const SearchJob&) = delete;

    // Asks the workers to stop: they check between documents and every ~1k lines inside one. No new batch is
    // delivered afterwards (with a context, queued batches are dropped when they are processed; without one, a
    // batch already inside onResults on a worker thread completes). onFinished still follows, with canceled set.
    void cancel();
    bool isCanceled() const;
    bool isFinished() const; // all workers are done and onFinished was invoked (direct) or posted (context)
    bool waitForFinished(int timeoutMs = -1); // true when finished
    SearchStats stats() const;               // live counters; final once finished

private:
    friend class SearchEngine;
    struct Private;
    SearchJob();
    std::unique_ptr<Private> d;
};

// Global search over a document source on a thread pool: documents are evaluated in parallel (work stealing by
// index), matches are streamed to the callbacks in batches. Starting a new search cancels the previous one
// (the search view restarts on every keystroke). The engine object itself belongs to one thread (usually the GUI
// thread); jobs may be canceled/waited for from any thread. Do not destroy the engine from inside its callbacks.
class SearchEngine {
public:
    explicit SearchEngine(QThreadPool* pool = nullptr); // nullptr: QThreadPool::globalInstance()
    ~SearchEngine();                                    // cancels running searches and waits for their workers
    SearchEngine(const SearchEngine&) = delete;
    SearchEngine& operator=(const SearchEngine&) = delete;

    std::shared_ptr<SearchJob> search(const SearchQuery& query, SearchSource source, SearchCallbacks callbacks,
                                      const SearchEngineOptions& options = {});
    void cancel(); // cancels the current search, if any
    std::shared_ptr<SearchJob> currentJob() const { return m_current; }

private:
    QThreadPool* m_pool;
    std::shared_ptr<SearchJob> m_current;
    std::vector<std::weak_ptr<SearchJob>> m_jobs; // not yet finished when last checked
};

} // namespace oc::search
