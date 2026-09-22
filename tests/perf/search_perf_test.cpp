// Perf gate for global search (label "perf"): 1,000 synthetic notes of ~13 MB in total; a simple term must deliver
// its first batch in < 150 ms and finish in < 500 ms. Assertions only with OC_PERF_ASSERT=1 (optimized builds);
// other runs just report numbers. Other query shapes are reported for reference.
#include "core/search/search_engine.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThreadPool>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

using namespace oc::search;

namespace {

bool assertPerf() { return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1; }

const char* const kWords[] = {
    "the", "of", "and", "kernel", "config", "mount", "partition", "table", "package", "driver", "network", "server",
    "client", "module", "service", "script", "write", "people", "number", "water", "first", "long", "time", "system",
    "review", "design", "notes", "meeting", "budget", "draft", "sprint", "garden", "recipe", "travel", "idea", "plan",
    "reading", "summary", "weekly", "interview", "database", "backup", "update", "release", "memory", "cache",
};

// Builds one note of roughly `target` characters and records its structure like the metadata cache would.
std::shared_ptr<const SearchDocument> makeNote(int index, int target, QRandomGenerator& r)
{
    auto d = std::make_shared<SearchDocument>();
    d->path = QStringLiteral("Folder %1/Note %2.md").arg(index % 37).arg(index);
    QString& s = d->content;
    s.reserve(target + 512);
    int line = 0;
    const auto word = [&] { return QString::fromLatin1(kWords[r.bounded(int(std::size(kWords)))]); };
    const auto sentence = [&](int n) -> QString {
        QString out = word();
        out[0] = out[0].toUpper();
        for (int i = 1; i < n; ++i)
            out += u' ' + word();
        return out + u'.';
    };
    const auto addLine = [&](const QString& text) {
        const int from = int(s.size());
        s += text;
        s += u'\n';
        ++line;
        return TextRange{from, from + int(text.size())};
    };
    if (index % 3 == 0) {
        static const char* const statuses[] = {"draft", "review", "done"};
        const QString status = QString::fromLatin1(statuses[r.bounded(3)]);
        addLine(QStringLiteral("---"));
        addLine(QStringLiteral("status: ") + status);
        addLine(QStringLiteral("tags: [inbox]"));
        addLine(QStringLiteral("rating: %1").arg(r.bounded(10)));
        addLine(QStringLiteral("---"));
        d->properties = {{"status", status}, {"tags", QVariantList{"inbox"}}, {"rating", double(r.bounded(10))}};
        d->tags.push_back({"#inbox", -1, -1, -1});
    }
    int sectionStart = 0;
    while (s.size() < target) {
        const int k = r.bounded(100);
        if (k < 4) { // heading: closes the current section
            const int from = int(s.size());
            if (from > sectionStart)
                d->sections.push_back({sectionStart, from});
            sectionStart = from;
            d->blocks.push_back(addLine(QStringLiteral("## ") + sentence(3)));
        } else if (k < 30) { // list with some tasks
            for (int i = 0, n = 2 + r.bounded(6); i < n; ++i) {
                const int kind = r.bounded(5);
                const QString marker = kind == 0 ? QStringLiteral("- [ ] ") : kind == 1 ? QStringLiteral("- [x] ") : QStringLiteral("- ");
                const TextRange item = addLine(marker + sentence(4 + r.bounded(8)));
                d->blocks.push_back(item);
                if (kind < 2)
                    d->tasks.push_back({item, kind == 0 ? QChar(u' ') : QChar(u'x')});
            }
            addLine(QString());
        } else if (k < 33) { // a line with tags
            QString text = sentence(5);
            const int tagAt = int(s.size() + text.size() + 1);
            const QString tag = r.bounded(2) ? QStringLiteral("#project/alpha") : QStringLiteral("#kernel");
            d->tags.push_back({tag, line, tagAt, tagAt + int(tag.size())});
            d->blocks.push_back(addLine(text + u' ' + tag));
            addLine(QString());
        } else if (k < 35) { // a code block
            const int from = int(s.size());
            addLine(QStringLiteral("```bash"));
            for (int i = 0, n = 2 + r.bounded(6); i < n; ++i)
                addLine(QStringLiteral("sudo mount /dev/sda%1 /mnt/%2 # %3").arg(r.bounded(9)).arg(word(), word()));
            const TextRange end = addLine(QStringLiteral("```"));
            d->blocks.push_back({from, end.to});
            addLine(QString());
        } else { // paragraph, sometimes with a date or a long line
            QString para;
            const int n = r.bounded(20) == 0 ? 60 : 2 + r.bounded(5);
            for (int i = 0; i < n; ++i)
                para += (i ? QStringLiteral(" ") : QString()) + sentence(6 + r.bounded(10));
            if (r.bounded(8) == 0)
                para += QStringLiteral(" Due 2024-%1-%2.").arg(1 + r.bounded(12), 2, 10, QChar(u'0')).arg(1 + r.bounded(28), 2, 10, QChar(u'0'));
            d->blocks.push_back(addLine(para));
            addLine(QString());
        }
    }
    d->sections.push_back({sectionStart, int(s.size())});
    return d;
}

QList<std::shared_ptr<const SearchDocument>> corpus(qsizetype* totalChars)
{
    QRandomGenerator r(2024);
    QList<std::shared_ptr<const SearchDocument>> docs;
    *totalChars = 0;
    for (int i = 0; i < 1000; ++i) {
        // 10 big notes (~250 KB), 90 medium (~40 KB), 900 small (~7.7 KB): ~13 MB.
        const int target = i % 100 == 0 ? 250'000 : i % 10 == 0 ? 40'000 : 7'700;
        docs.push_back(makeNote(i, target, r));
        *totalChars += docs.back()->content.size();
    }
    return docs;
}

struct Timing {
    double firstMs = -1;
    double totalMs = 0;
    qsizetype matched = 0;
    qsizetype contentMatches = 0;
};

Timing runOnce(SearchEngine& engine, const SearchQuery& query, const SearchSource& source)
{
    std::mutex mutex;
    qsizetype contentMatches = 0;
    SearchCallbacks cb;
    cb.onResults = [&](QList<SearchResult> batch) {
        std::lock_guard lock(mutex);
        for (const SearchResult& r : batch)
            contentMatches += r.contentMatches.size();
    };
    QElapsedTimer t;
    t.start();
    const auto job = engine.search(query, source, cb);
    job->waitForFinished();
    Timing timing;
    timing.totalMs = t.nsecsElapsed() / 1e6;
    const SearchStats stats = job->stats();
    timing.firstMs = stats.firstResultNs < 0 ? -1 : stats.firstResultNs / 1e6;
    timing.matched = stats.matched;
    timing.contentMatches = contentMatches;
    return timing;
}

} // namespace

TEST(SearchPerf, ThousandNotesThirteenMegabytes)
{
    QElapsedTimer gen;
    gen.start();
    qsizetype chars = 0;
    const auto docs = corpus(&chars);
    std::printf("[perf] search corpus: %d notes, %.1f M chars, generated in %.0f ms, %d threads\n", int(docs.size()),
                chars / 1e6, gen.nsecsElapsed() / 1e6, QThreadPool::globalInstance()->maxThreadCount());
    EXPECT_GT(chars, 12'000'000);
    EXPECT_LT(chars, 14'500'000);
    const SearchSource source = SearchSource::fromDocuments(docs);
    SearchEngine engine;

    const QStringList queries = {
        "kernel", // the gated simple term
        "kernel config", "\"partition table\"", "kernel -network", "network OR backup", R"(/\d{4}-\d{2}-\d{2}/)",
        "match-case:Kernel", "line:(kernel driver)", "block:(mount partition)", "section:(kernel network)",
        "task-todo:write", "tag:#project", "[status:draft]", "path:\"Folder 1/\"", "zzzyx", "the",
    };
    for (const QString& q : queries) {
        const SearchQuery query = parseSearchQuery(q);
        Timing best;
        best.totalMs = 1e18;
        for (int run = 0; run < 5; ++run) {
            const Timing t = runOnce(engine, query, source);
            if (t.totalMs < best.totalMs) {
                best = t;
            }
        }
        std::printf("[perf] search %-28s %4lld notes %8lld hits  first batch %7.2f ms  total %7.2f ms\n",
                    qPrintable(u'\'' + q + u'\''), static_cast<long long>(best.matched), static_cast<long long>(best.contentMatches),
                    best.firstMs, best.totalMs);
        if (q == QLatin1StringView("kernel")) {
            EXPECT_GT(best.matched, 500);
            if (assertPerf()) {
                EXPECT_LT(best.firstMs, 150.0);
                EXPECT_LT(best.totalMs, 500.0);
            }
        }
    }
}
