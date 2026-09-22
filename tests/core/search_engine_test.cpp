// SearchEngine: parallel evaluation, streaming batches (worker-thread and queued delivery), cancellation,
// restarting and teardown. Runs with a QApplication (event loop for queued delivery); label "threads" (TSan).
#include "core/search/search_engine.h"

#include <QEventLoop>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

using namespace oc;
using namespace oc::search;

namespace {

using DocList = QList<std::shared_ptr<const SearchDocument>>;

// Even documents mention "alpha"; every document has several lines with "common".
DocList makeDocs(int n)
{
    DocList docs;
    for (int i = 0; i < n; ++i) {
        auto d = std::make_shared<SearchDocument>();
        d->path = QStringLiteral("Notes/note-%1.md").arg(i);
        QString content;
        for (int line = 0; line < 20; ++line)
            content += QStringLiteral("common line %1 of %2\n").arg(line).arg(i);
        if (i % 2 == 0)
            content += QStringLiteral("the alpha marker\n");
        d->content = content;
        docs.push_back(d);
    }
    return docs;
}

std::map<qsizetype, SearchResult> sequential(const SearchQuery& q, const DocList& docs)
{
    std::map<qsizetype, SearchResult> out;
    for (qsizetype i = 0; i < docs.size(); ++i)
        if (auto r = matchDocument(q, *docs[i]))
            out.emplace(i, *r);
    return out;
}

// A source whose documents take `delayMs` each to "load".
SearchSource slowSource(const DocList& docs, int count, int delayMs, std::atomic<int>* loads = nullptr)
{
    SearchSource s;
    s.count = count;
    s.load = [docs, delayMs, loads](qsizetype i) {
        if (loads)
            loads->fetch_add(1);
        QThread::msleep(delayMs);
        return docs[i % docs.size()];
    };
    return s;
}

// Collects callbacks invoked on worker threads.
struct Collector {
    std::mutex mutex;
    std::condition_variable cond;
    QList<SearchResult> results;
    int batches = 0;
    int finishedCalls = 0;
    bool resultsAfterFinish = false;
    SearchStats stats;

    SearchCallbacks callbacks()
    {
        SearchCallbacks cb;
        cb.onResults = [this](QList<SearchResult> batch) {
            std::lock_guard lock(mutex);
            if (finishedCalls)
                resultsAfterFinish = true;
            ++batches;
            results.append(std::move(batch));
            cond.notify_all();
        };
        cb.onFinished = [this](const SearchStats& s) {
            std::lock_guard lock(mutex);
            ++finishedCalls;
            stats = s;
            cond.notify_all();
        };
        return cb;
    }
};

} // namespace

TEST(SearchEngine, StreamsEveryMatchOnceInBatches)
{
    const DocList docs = makeDocs(500);
    const SearchQuery query = parseSearchQuery("alpha common");
    const auto expected = sequential(query, docs);
    ASSERT_EQ(expected.size(), 250u);

    SearchEngine engine;
    Collector c;
    SearchEngineOptions options;
    options.batchSize = 16;
    const auto job = engine.search(query, SearchSource::fromDocuments(docs), c.callbacks(), options);
    ASSERT_TRUE(job->waitForFinished(60000));
    EXPECT_TRUE(job->isFinished());
    EXPECT_EQ(engine.currentJob(), job);

    std::lock_guard lock(c.mutex);
    EXPECT_EQ(c.finishedCalls, 1);
    EXPECT_FALSE(c.resultsAfterFinish);
    EXPECT_GT(c.batches, 1);
    ASSERT_EQ(c.results.size(), qsizetype(expected.size()));
    std::map<qsizetype, int> seen;
    for (const SearchResult& r : c.results) {
        ++seen[r.index];
        ASSERT_TRUE(expected.contains(r.index)) << r.index;
        const SearchResult& e = expected.at(r.index);
        EXPECT_EQ(r.path, docs[r.index]->path);
        EXPECT_EQ(r.contentMatches, e.contentMatches);
        EXPECT_EQ(r.fileMatches, e.fileMatches);
    }
    for (const auto& [index, count] : seen)
        EXPECT_EQ(count, 1) << index;
    EXPECT_EQ(c.stats.total, 500);
    EXPECT_EQ(c.stats.searched, 500);
    EXPECT_EQ(c.stats.matched, 250);
    EXPECT_FALSE(c.stats.canceled);
    EXPECT_GE(c.stats.firstResultNs, 0);
    EXPECT_GE(c.stats.elapsedNs, c.stats.firstResultNs);
    EXPECT_EQ(job->stats().matched, 250);
}

TEST(SearchEngine, UsesTheGivenPoolAndThreadLimit)
{
    const DocList docs = makeDocs(64);
    QThreadPool pool;
    pool.setMaxThreadCount(3);
    SearchEngine engine(&pool);
    Collector c;
    SearchEngineOptions options;
    options.maxThreads = 2;
    const auto job = engine.search(parseSearchQuery("common -alpha"), SearchSource::fromDocuments(docs), c.callbacks(), options);
    ASSERT_TRUE(job->waitForFinished(60000));
    std::lock_guard lock(c.mutex);
    EXPECT_EQ(c.results.size(), 32);
    EXPECT_EQ(c.stats.matched, 32);
}

TEST(SearchEngine, DeliversOnTheContextThread)
{
    const DocList docs = makeDocs(300);
    const SearchQuery query = parseSearchQuery("line:(common 7)");
    const auto expected = sequential(query, docs);
    QObject context;
    SearchEngine engine;
    QEventLoop loop;
    QList<SearchResult> results;
    bool finished = false;
    bool wrongThread = false;
    bool resultsAfterFinish = false;
    SearchStats stats;
    SearchCallbacks cb;
    cb.context = &context;
    cb.onResults = [&](QList<SearchResult> batch) {
        wrongThread |= QThread::currentThread() != context.thread();
        resultsAfterFinish |= finished;
        results.append(std::move(batch));
    };
    cb.onFinished = [&](const SearchStats& s) {
        wrongThread |= QThread::currentThread() != context.thread();
        finished = true;
        stats = s;
        loop.quit();
    };
    SearchEngineOptions options;
    options.batchSize = 8;
    engine.search(query, SearchSource::fromDocuments(docs), cb, options);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    ASSERT_TRUE(finished);
    EXPECT_FALSE(wrongThread);
    EXPECT_FALSE(resultsAfterFinish);
    EXPECT_EQ(results.size(), qsizetype(expected.size()));
    EXPECT_EQ(stats.matched, qsizetype(expected.size()));
    for (const SearchResult& r : results)
        EXPECT_TRUE(expected.contains(r.index));
}

TEST(SearchEngine, CancelOnTheContextThreadDropsQueuedBatches)
{
    const DocList docs = makeDocs(8);
    QObject context;
    SearchEngine engine;
    QEventLoop loop;
    std::shared_ptr<SearchJob> job;
    int batchesAfterCancel = 0;
    int batches = 0;
    bool canceled = false;
    SearchStats stats;
    bool finished = false;
    SearchCallbacks cb;
    cb.context = &context;
    cb.onResults = [&](QList<SearchResult>) {
        ++batches;
        if (canceled) {
            ++batchesAfterCancel;
            return;
        }
        QThread::msleep(20); // let more batches queue up behind this one
        job->cancel();
        canceled = true;
    };
    cb.onFinished = [&](const SearchStats& s) {
        stats = s;
        finished = true;
        loop.quit();
    };
    SearchEngineOptions options;
    options.batchSize = 1;
    options.batchIntervalMs = 0;
    options.maxThreads = 4;
    job = engine.search(parseSearchQuery("common"), slowSource(docs, 400, 1), cb, options);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    ASSERT_TRUE(finished);
    EXPECT_EQ(batches, 1);
    EXPECT_EQ(batchesAfterCancel, 0);
    EXPECT_TRUE(stats.canceled);
    EXPECT_LT(stats.searched, 400);
    EXPECT_TRUE(job->waitForFinished(60000));
    EXPECT_TRUE(job->isCanceled());
}

TEST(SearchEngine, CancelFromAnotherThreadStopsTheWorkers)
{
    const DocList docs = makeDocs(8);
    SearchEngine engine;
    Collector c;
    std::atomic<int> loads{0};
    SearchEngineOptions options;
    options.batchSize = 1;
    options.maxThreads = 2;
    const auto job = engine.search(parseSearchQuery("common"), slowSource(docs, 2000, 2, &loads), c.callbacks(), options);
    {
        std::unique_lock lock(c.mutex);
        ASSERT_TRUE(c.cond.wait_for(lock, std::chrono::seconds(60), [&] { return c.batches > 0; }));
    }
    job->cancel();
    const int batchesAtCancel = [&] {
        std::lock_guard lock(c.mutex);
        return c.batches;
    }();
    ASSERT_TRUE(job->waitForFinished(60000));
    std::lock_guard lock(c.mutex);
    EXPECT_EQ(c.finishedCalls, 1);
    EXPECT_TRUE(c.stats.canceled);
    EXPECT_LE(c.batches, batchesAtCancel + 1); // at most the batch that was being delivered
    EXPECT_LT(loads.load(), 2000);
    EXPECT_LT(c.stats.searched, 2000);
}

TEST(SearchEngine, NewSearchCancelsThePreviousOne)
{
    const DocList docs = makeDocs(8);
    SearchEngine engine;
    Collector first;
    Collector second;
    SearchEngineOptions options;
    options.maxThreads = 2;
    const auto a = engine.search(parseSearchQuery("common"), slowSource(docs, 1000, 2), first.callbacks(), options);
    const auto b = engine.search(parseSearchQuery("alpha"), SearchSource::fromDocuments(docs), second.callbacks(), options);
    EXPECT_TRUE(a->isCanceled());
    ASSERT_TRUE(b->waitForFinished(60000));
    ASSERT_TRUE(a->waitForFinished(60000));
    EXPECT_EQ(engine.currentJob(), b);
    std::scoped_lock lock(first.mutex, second.mutex);
    EXPECT_TRUE(first.stats.canceled);
    EXPECT_EQ(first.finishedCalls, 1);
    EXPECT_FALSE(second.stats.canceled);
    EXPECT_EQ(second.results.size(), 4);
}

TEST(SearchEngine, EmptyQueriesSourcesAndSkippedDocuments)
{
    const DocList docs = makeDocs(10);
    SearchEngine engine;
    {
        Collector c;
        const auto job = engine.search(parseSearchQuery("   "), SearchSource::fromDocuments(docs), c.callbacks());
        ASSERT_TRUE(job->waitForFinished(60000));
        std::lock_guard lock(c.mutex);
        EXPECT_EQ(c.finishedCalls, 1);
        EXPECT_EQ(c.batches, 0);
        EXPECT_EQ(c.stats.searched, 0);
        EXPECT_EQ(c.stats.total, 10);
    }
    {
        Collector c;
        const auto job = engine.search(parseSearchQuery("common"), SearchSource{}, c.callbacks());
        ASSERT_TRUE(job->waitForFinished(60000));
        std::lock_guard lock(c.mutex);
        EXPECT_EQ(c.finishedCalls, 1);
        EXPECT_EQ(c.stats.total, 0);
    }
    {
        Collector c;
        SearchSource skipOdd;
        skipOdd.count = docs.size();
        skipOdd.load = [&docs](qsizetype i) { return i % 2 ? nullptr : docs[i]; };
        const auto job = engine.search(parseSearchQuery("common"), skipOdd, c.callbacks());
        ASSERT_TRUE(job->waitForFinished(60000));
        std::lock_guard lock(c.mutex);
        EXPECT_EQ(c.stats.searched, 5);
        EXPECT_EQ(c.results.size(), 5);
    }
    {
        SearchCallbacks none; // callbacks are optional
        const auto job = engine.search(parseSearchQuery("common"), SearchSource::fromDocuments(docs), none);
        ASSERT_TRUE(job->waitForFinished(60000));
        EXPECT_EQ(job->stats().matched, 10);
    }
}

TEST(SearchEngine, DestructorCancelsAndWaits)
{
    const DocList docs = makeDocs(8);
    std::shared_ptr<SearchJob> job;
    Collector c;
    {
        SearchEngine engine;
        SearchEngineOptions options;
        options.maxThreads = 2;
        job = engine.search(parseSearchQuery("common"), slowSource(docs, 5000, 2), c.callbacks(), options);
        QThread::msleep(20);
    }
    EXPECT_TRUE(job->isFinished());
    EXPECT_TRUE(job->isCanceled());
    std::lock_guard lock(c.mutex);
    EXPECT_EQ(c.finishedCalls, 1);
    EXPECT_TRUE(c.stats.canceled);
}
