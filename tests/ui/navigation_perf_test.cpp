// Opt-in timings over a large vault (skipped unless OC_PERF_VAULT names a vault to copy, e.g. one made by
// tools/gen_vault.py in a scratch dir): search time to first result / completion and the longest GUI-thread stall
// while results stream in, quick switcher and command palette query times.
#include "navigation_test_support.h"

#include <QElapsedTimer>
#include <QTimer>

#include <iostream>

using namespace oc;
using namespace oc::test;

namespace {

QString perfVault() { return QString::fromLocal8Bit(qgetenv("OC_PERF_VAULT")); }

// Longest gap between ticks of a 1 ms timer while `run` is pumping events: the GUI thread's worst stall.
struct StallProbe {
    StallProbe()
    {
        timer.setInterval(1);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            if (clock.isValid())
                maxGapMs = std::max(maxGapMs, clock.restart());
            else
                clock.start();
        });
        timer.start();
    }
    QTimer timer;
    QElapsedTimer clock;
    qint64 maxGapMs = 0;
};

} // namespace

TEST(NavigationPerf, LargeVault)
{
    if (perfVault().isEmpty())
        GTEST_SKIP() << "set OC_PERF_VAULT to a scratch vault (tools/gen_vault.py) to measure";
    NavigationEnv env([](const QString& root) { ASSERT_TRUE(copyTree(perfVault(), root)); }, false);
    std::cout << "files: " << env.vault->fileCount() << "\n";

    SearchView* v = openGlobalSearch(env.session);
    ASSERT_NE(v, nullptr);
    for (const char* q : {"the", "zebra", "/[0-9]{4}-[0-9]{2}/", "tag:#project"}) {
        QElapsedTimer t;
        qint64 first = -1;
        QMetaObject::Connection c = QObject::connect(v, &SearchView::resultsChanged, [&] {
            if (first < 0 && v->resultCount() > 0)
                first = t.elapsed();
        });
        StallProbe probe;
        t.start();
        v->setQuery(QString::fromLatin1(q));
        const qint64 start = t.elapsed();
        probe.maxGapMs = 0; // the stall while results stream in (the synchronous start is reported separately)
        probe.clock.invalidate();
        ASSERT_TRUE(env.waitIdle(v, 120000));
        const qint64 total = t.elapsed();
        QObject::disconnect(c);
        std::cout << "search \"" << q << "\": " << v->resultCount() << " files, start " << start << " ms, first result " << first
                  << " ms, done " << total << " ms, max GUI stall " << probe.maxGapMs << " ms\n";
    }
    // second run of the same query: documents come from the cache
    {
        QElapsedTimer t;
        t.start();
        v->setQuery("the");
        ASSERT_TRUE(env.waitIdle(v, 120000));
        std::cout << "search \"the\" (cached): " << t.elapsed() << " ms\n";
    }

    const SwitcherSource src = SwitcherSource::fromSession(env.session);
    for (const char* q : {"n", "note", "zzz"}) {
        QElapsedTimer t;
        t.start();
        const QList<SwitcherItem> items = quickSwitcherItems(QString::fromLatin1(q), src);
        std::cout << "switcher \"" << q << "\": " << items.size() << " items in " << t.nsecsElapsed() / 1000 << " us\n";
    }
    {
        QElapsedTimer t;
        t.start();
        const QList<PaletteItem> items = commandPaletteItems("to", *env.commands);
        std::cout << "palette \"to\": " << items.size() << " items in " << t.nsecsElapsed() / 1000 << " us\n";
    }
}
