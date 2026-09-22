// Perf gate for the fuzzy matcher (label "perf"): 10k vault paths per query must take < 5 ms in total.
// Assertions only with OC_PERF_ASSERT=1 (optimized builds); other runs just report numbers.
#include "core/fuzzy/fuzzy.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

using namespace oc;

namespace {

bool assertPerf() { return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1; }

QStringList syntheticPaths(int count)
{
    const char* folders[] = {"Projects", "Daily", "Archive", "Meetings", "Reading notes", "Templates", "Areas/Health",
                             "Areas/Finance", "Inbox", "Journal/2024", "Journal/2025", "Work/Clients", "Work/Internal"};
    const char* words[] = {"meeting", "notes", "plan", "roadmap", "kernel", "config", "review", "weekly", "design",
                           "draft", "ideas", "reading", "summary", "budget", "retro", "sprint", "garden", "recipe",
                           "travel", "Obsidian", "refactoring", "database", "interview", "onboarding", "q3"};
    QRandomGenerator rng(7);
    QStringList out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        QString p = QString::fromLatin1(folders[rng.bounded(int(std::size(folders)))]);
        if (rng.bounded(3) == 0)
            p += u'/' + QString::fromLatin1(words[rng.bounded(int(std::size(words)))]);
        p += u'/';
        const int n = 1 + rng.bounded(5);
        for (int w = 0; w < n; ++w) {
            QString word = QString::fromLatin1(words[rng.bounded(int(std::size(words)))]);
            if (w == 0)
                word[0] = word[0].toUpper();
            p += (w ? QStringLiteral(" ") : QString()) + word;
        }
        if (rng.bounded(4) == 0)
            p += QStringLiteral(" %1").arg(rng.bounded(2020, 2027));
        out.push_back(p); // quick switcher shows paths without ".md"
    }
    return out;
}

} // namespace

TEST(FuzzyPerf, TenThousandPaths)
{
    const QStringList paths = syntheticPaths(10000);
    const QStringList queries = {"a", "no", "pro", "note", "daily", "meetin", "projnot", "2024 jour", "archivenot",
                                 "refactorin", "wkly rv", "xyzzy"};
    for (const QString& q : queries) {
        const FuzzyQuery fq(q);
        double best = 1e9;
        int matches = 0;
        for (int run = 0; run < 7; ++run) {
            QElapsedTimer t;
            t.start();
            int m = 0;
            for (const QString& p : paths)
                if (fq.matchPath(p))
                    ++m;
            best = std::min(best, t.nsecsElapsed() / 1e6);
            matches = m;
        }
        std::printf("[perf] fuzzy %-12s %5d/%d matches  %.2f ms\n", qPrintable(u'"' + q + u'"'), matches, int(paths.size()), best);
        if (assertPerf()) {
            EXPECT_LT(best, 5.0) << q.toStdString();
        }
    }
}
