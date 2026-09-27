#include "core/search/search_engine.h"

#include "core/util/threads.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QRunnable>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace oc::search {

SearchSource SearchSource::fromDocuments(QList<std::shared_ptr<const SearchDocument>> documents)
{
    SearchSource s;
    s.count = documents.size();
    auto docs = std::make_shared<const QList<std::shared_ptr<const SearchDocument>>>(std::move(documents));
    s.load = [docs](qsizetype i) { return docs->at(i); };
    return s;
}

struct SearchJob::Private {
    SearchQuery query;
    SearchSource source;
    SearchCallbacks callbacks;
    QPointer<QObject> contextGuard; // callbacks.context, checked on the GUI thread before a queued delivery
    SearchEngineOptions options;
    QElapsedTimer timer;

    std::atomic<bool> canceled{false};
    std::atomic<qsizetype> next{0};
    std::atomic<int> workers{0};
    std::atomic<qsizetype> searched{0};
    std::atomic<qsizetype> matched{0};
    std::atomic<qint64> firstResultNs{-1};
    std::atomic<int> pendingCount{0};

    // std primitives rather than QMutex: ThreadSanitizer sees them (the "threads" tests run under TSan).
    std::mutex pendingMutex;
    QList<SearchResult> pending; // guarded by pendingMutex
    qint64 lastFlushNs = 0;      // guarded by pendingMutex
    bool flushedOnce = false;    // guarded by pendingMutex

    std::mutex deliverMutex; // serializes callbacks invoked on worker threads

    mutable std::mutex doneMutex;
    std::condition_variable doneCond;
    bool finished = false; // guarded by doneMutex
    SearchStats finalStats; // guarded by doneMutex

    SearchStats snapshot() const
    {
        SearchStats s;
        s.total = source.count;
        s.searched = searched.load(std::memory_order_relaxed);
        s.matched = matched.load(std::memory_order_relaxed);
        s.canceled = canceled.load(std::memory_order_relaxed);
        s.elapsedNs = timer.nsecsElapsed();
        s.firstResultNs = firstResultNs.load(std::memory_order_relaxed);
        return s;
    }

    static void runWorker(const std::shared_ptr<SearchJob>& job)
    {
        Private& p = *job->d;
        const qsizetype count = p.source.count;
        while (!p.canceled.load(std::memory_order_relaxed)) {
            const qsizetype i = p.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count)
                break;
            const std::shared_ptr<const SearchDocument> doc = p.source.load(i);
            if (!doc)
                continue;
            std::optional<SearchResult> r = matchDocument(p.query, *doc, &p.canceled);
            if (p.canceled.load(std::memory_order_relaxed))
                break;
            p.searched.fetch_add(1, std::memory_order_relaxed);
            if (r) {
                r->index = i;
                p.matched.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(p.pendingMutex);
                p.pending.push_back(std::move(*r));
                p.pendingCount.store(int(p.pending.size()), std::memory_order_relaxed);
            }
            if (p.pendingCount.load(std::memory_order_relaxed) > 0)
                flush(job, false);
        }
        if (p.workers.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finish(job);
    }

    // Hands the pending results to delivery when a batch is due (always when `force`).
    static void flush(const std::shared_ptr<SearchJob>& job, bool force)
    {
        Private& p = *job->d;
        QList<SearchResult> batch;
        {
            std::lock_guard lock(p.pendingMutex);
            if (p.pending.isEmpty())
                return;
            const qint64 now = p.timer.nsecsElapsed();
            const bool due = force || !p.flushedOnce || p.pending.size() >= p.options.batchSize
                || now - p.lastFlushNs >= qint64(p.options.batchIntervalMs) * 1'000'000;
            if (!due)
                return;
            batch.swap(p.pending);
            p.pendingCount.store(0, std::memory_order_relaxed);
            p.lastFlushNs = now;
            p.flushedOnce = true;
        }
        deliver(job, std::move(batch));
    }

    static void deliver(const std::shared_ptr<SearchJob>& job, QList<SearchResult> batch)
    {
        Private& p = *job->d;
        if (batch.isEmpty() || p.canceled.load(std::memory_order_relaxed))
            return;
        qint64 none = -1;
        p.firstResultNs.compare_exchange_strong(none, p.timer.nsecsElapsed(), std::memory_order_relaxed);
        if (!p.callbacks.onResults)
            return;
        if (p.callbacks.context) {
            postToMainThread([job, batch = std::move(batch)]() mutable {
                if (job->d->contextGuard && !job->d->canceled.load(std::memory_order_relaxed))
                    job->d->callbacks.onResults(std::move(batch));
            });
            return;
        }
        std::lock_guard lock(p.deliverMutex);
        if (!p.canceled.load(std::memory_order_relaxed))
            p.callbacks.onResults(std::move(batch));
    }

    // Run by the last worker: final batch, onFinished, then waiters are released.
    static void finish(const std::shared_ptr<SearchJob>& job)
    {
        Private& p = *job->d;
        flush(job, true);
        const SearchStats stats = p.snapshot();
        if (p.callbacks.onFinished) {
            if (p.callbacks.context) {
                postToMainThread([job, stats] {
                    if (job->d->contextGuard)
                        job->d->callbacks.onFinished(stats);
                });
            } else {
                std::lock_guard lock(p.deliverMutex);
                p.callbacks.onFinished(stats);
            }
        }
        std::lock_guard lock(p.doneMutex);
        p.finalStats = stats;
        p.finished = true;
        p.doneCond.notify_all();
    }
};

SearchJob::SearchJob()
    : d(std::make_unique<Private>())
{
}

SearchJob::~SearchJob() = default;

void SearchJob::cancel() { d->canceled.store(true, std::memory_order_relaxed); }

bool SearchJob::isCanceled() const { return d->canceled.load(std::memory_order_relaxed); }

bool SearchJob::isFinished() const
{
    std::lock_guard lock(d->doneMutex);
    return d->finished;
}

bool SearchJob::waitForFinished(int timeoutMs)
{
    std::unique_lock lock(d->doneMutex);
    if (timeoutMs < 0) {
        d->doneCond.wait(lock, [this] { return d->finished; });
        return true;
    }
    return d->doneCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return d->finished; });
}

SearchStats SearchJob::stats() const
{
    {
        std::lock_guard lock(d->doneMutex);
        if (d->finished)
            return d->finalStats;
    }
    return d->snapshot();
}

SearchEngine::SearchEngine(QThreadPool* pool)
    : m_pool(pool ? pool : QThreadPool::globalInstance())
{
}

SearchEngine::~SearchEngine()
{
    for (const std::weak_ptr<SearchJob>& w : m_jobs)
        if (const std::shared_ptr<SearchJob> job = w.lock())
            job->cancel();
    for (const std::weak_ptr<SearchJob>& w : m_jobs)
        if (const std::shared_ptr<SearchJob> job = w.lock())
            job->waitForFinished();
}

void SearchEngine::cancel()
{
    if (m_current)
        m_current->cancel();
}

std::shared_ptr<SearchJob> SearchEngine::search(const SearchQuery& query, SearchSource source, SearchCallbacks callbacks,
                                                const SearchEngineOptions& options)
{
    cancel();
    std::erase_if(m_jobs, [](const std::weak_ptr<SearchJob>& w) {
        const std::shared_ptr<SearchJob> job = w.lock();
        return !job || job->isFinished();
    });

    std::shared_ptr<SearchJob> job(new SearchJob);
    SearchJob::Private& p = *job->d;
    p.query = query;
    p.source = std::move(source);
    p.callbacks = std::move(callbacks);
    p.contextGuard = p.callbacks.context;
    p.options = options;
    p.options.batchSize = std::max(1, options.batchSize);
    p.options.batchIntervalMs = std::max(0, options.batchIntervalMs);
    if (query.isEmpty() || !p.source.load || p.source.count < 0) // an empty query matches nothing
        p.next.store(std::max<qsizetype>(p.source.count, 0));
    const int threads = options.maxThreads > 0 ? options.maxThreads : std::max(1, m_pool->maxThreadCount());
    const int workers = int(std::clamp<qsizetype>(p.source.count, 1, threads));
    p.workers.store(workers);
    p.timer.start();

    m_current = job;
    m_jobs.push_back(job);
    QList<QRunnable*> runnables;
    for (int w = 0; w < workers; ++w)
        runnables.push_back(QRunnable::create([job] {
            tsan::acquire();
            SearchJob::Private::runWorker(job);
        }));
    tsan::release(); // everything above (job state, runnables) happens before the workers run
    for (QRunnable* r : std::as_const(runnables))
        m_pool->start(r);
    return job;
}

} // namespace oc::search
