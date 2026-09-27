// Perf gate for Bases (label "perf"): a base over 5,000 notes with properties — a tag + comparison
// filter, a formula column, a two-key sort and a group-by — must evaluate (every file through the
// filter, then the view's rows, columns and groups) in < 500 ms. Assertions only with
// OC_PERF_ASSERT=1 (optimized builds); other runs just report the numbers.

#include "core/bases/base_query.h"

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <gtest/gtest.h>

#include <cstdio>

using namespace oc::bases;

namespace {

bool assertPerf() { return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1; }

void addNotes(MemoryDataSource& ds, int count)
{
    QRandomGenerator r(42);
    static const char* const statuses[] = {"todo", "doing", "done", "blocked", "later"};
    for (int i = 0; i < count; ++i) {
        const QString text = QStringLiteral("---\nstatus: %1\nrating: %2\ntags: [%3, project/%4]\ndue: 2024-%5-%6\nowner: \"[[Person %7]]\"\n---\n"
                                            "# Note %8\nSome text with a [[Note %9]] link and #inline tag.\n")
                                 .arg(QString::fromLatin1(statuses[r.bounded(5)]))
                                 .arg(r.bounded(100) / 10.0)
                                 .arg(i % 2 ? QStringLiteral("book") : QStringLiteral("paper"))
                                 .arg(r.bounded(20))
                                 .arg(1 + r.bounded(12), 2, 10, QLatin1Char('0'))
                                 .arg(1 + r.bounded(28), 2, 10, QLatin1Char('0'))
                                 .arg(r.bounded(50))
                                 .arg(i)
                                 .arg(r.bounded(count));
        ds.addFile(QStringLiteral("Folder %1/Note %2.md").arg(i % 40).arg(i), text, 1700000000000 + i, 1710000000000 + i);
    }
}

TEST(BasesPerf, FiveThousandNotes)
{
    MemoryDataSource ds;
    addNotes(ds, 5000);
    const BaseFile base = *BaseFile::fromString(QStringLiteral(R"(filters:
  and:
    - file.hasTag("book")
    - rating > 2
formulas:
  score: (rating * 10).round() + " pts"
  overdue: due < today()
views:
  - type: table
    name: T
    order:
      - file.name
      - status
      - rating
      - due
      - owner
      - formula.score
      - formula.overdue
    sort:
      - property: status
        direction: ASC
      - property: rating
        direction: DESC
    groupBy:
      property: status
      direction: ASC
)"));
    QElapsedTimer clock;
    clock.start();
    auto ctx = QueryContext::create(&ds, base, base.view(QString()), QString());
    QList<EntryPtr> results;
    for (const QString& p : ds.files()) {
        auto e = std::make_shared<Entry>(ctx, p);
        if (evaluateFile(ctx, e))
            results << e;
    }
    const qint64 filterMs = clock.elapsed();
    const ViewData d = buildViewData(base, *base.view(QString()), results);
    // what a table shows: every cell of every row
    int cells = 0;
    for (const EntryPtr& e : d.rows)
        for (const QString& c : d.columns)
            cells += !e->valueOrError(c).isNull();
    const qint64 totalMs = clock.elapsed();
    std::printf("bases: %d files, %lld results, filter %lld ms, view + %d cells %lld ms total\n", int(ds.files().size()),
                qlonglong(results.size()), qlonglong(filterMs), cells, qlonglong(totalMs));
    EXPECT_GT(results.size(), 1000);
    EXPECT_EQ(d.groups.size(), 5);
    if (assertPerf()) {
        EXPECT_LT(totalMs, 500);
    }
}

} // namespace
