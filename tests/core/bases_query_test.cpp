// Evaluating bases over a vault: filters, columns, sorting, grouping, limits, search, summaries,
// new-item defaults, export, and the incremental runner.

#include "core/bases/base_runner.h"
#include "core/bases_test_support.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

using namespace oc::bases;

namespace {

QList<EntryPtr> evaluateAll(const std::shared_ptr<QueryContext>& ctx, const DataSource& ds, QStringList* errors = nullptr)
{
    QList<EntryPtr> out;
    for (const QString& p : ds.files()) {
        auto e = std::make_shared<Entry>(ctx, p);
        QString err;
        if (evaluateFile(ctx, e, &err))
            out << e;
        else if (errors && !err.isEmpty())
            *errors << err;
    }
    return out;
}

QStringList names(const QList<EntryPtr>& rows)
{
    QStringList out;
    for (const EntryPtr& e : rows)
        out << e->value(QStringLiteral("file.name")).toString();
    return out;
}

struct QueryEnv {
    MemoryDataSource ds;
    BaseFile base;
    QueryEnv(const QString& yaml)
    {
        (void)bases_test::kUtc;
        bases_test::addBooks(ds);
        QString error;
        std::optional<BaseFile> b = BaseFile::fromString(yaml, &error);
        EXPECT_TRUE(b) << error.toStdString();
        if (b)
            base = *b;
    }
    ViewData run(const QString& view = {}, const QString& search = {}, QStringList* errors = nullptr)
    {
        const BaseView* v = base.view(view);
        auto ctx = QueryContext::create(&ds, base, v, QStringLiteral("Books.base"));
        return buildViewData(base, *v, evaluateAll(ctx, ds, errors), search);
    }
};

TEST(BasesQuery, NoFilterListsEveryFileSortedByName)
{
    QueryEnv q{QString()};
    const ViewData d = q.run();
    EXPECT_EQ(names(d.rows), (QStringList{QStringLiteral("Books.base"), QStringLiteral("cover.png"), QStringLiteral("Draft"),
                                           QStringLiteral("Dune"), QStringLiteral("Frank Herbert"), QStringLiteral("Hyperion"),
                                           QStringLiteral("Neuromancer")}));
    EXPECT_EQ(d.columns, QStringList{QStringLiteral("file.name")});
    EXPECT_EQ(d.groups.size(), 1);
    EXPECT_FALSE(d.groups[0].hasKey);
}

TEST(BasesQuery, FiltersColumnsAndSorting)
{
    QueryEnv q(QStringLiteral(R"(filters:
  and:
    - file.hasTag("book")
formulas:
  ppp: (price / pages).toFixed(3)
views:
  - type: table
    name: By pages
    order:
      - file.name
      - author
      - pages
      - formula.ppp
      - formula.gone
      - nothere
    sort:
      - property: pages
        direction: DESC
  - type: table
    name: By rating
    filters: rating >= 4
    sort:
      - property: rating
        direction: ASC
      - property: file.name
        direction: DESC
)"));
    ViewData d = q.run(QStringLiteral("By pages"));
    EXPECT_EQ(names(d.rows), (QStringList{QStringLiteral("Hyperion"), QStringLiteral("Dune"), QStringLiteral("Neuromancer")}));
    // formula.gone is not a formula of the base; unknown note properties stay (empty columns)
    EXPECT_EQ(d.columns, (QStringList{QStringLiteral("file.name"), QStringLiteral("note.author"), QStringLiteral("note.pages"),
                                      QStringLiteral("formula.ppp"), QStringLiteral("note.nothere")}));
    EXPECT_EQ(d.rows[1]->value(QStringLiteral("formula.ppp")).toString(), QStringLiteral("0.024"));
    EXPECT_TRUE(d.rows[2]->value(QStringLiteral("formula.ppp")).isNull()); // Neuromancer has no price: null / 271 is null
    d = q.run(QStringLiteral("By rating"));
    EXPECT_EQ(names(d.rows), (QStringList{QStringLiteral("Hyperion"), QStringLiteral("Dune")}));
}

TEST(BasesQuery, NullsSortLastInBothDirections)
{
    QueryEnv q(QStringLiteral(R"(views:
  - type: table
    name: Asc
    sort:
      - property: pages
        direction: ASC
  - type: table
    name: Desc
    sort:
      - property: pages
        direction: DESC
)"));
    ViewData d = q.run(QStringLiteral("Asc"));
    EXPECT_EQ(names(d.rows).mid(0, 3), (QStringList{QStringLiteral("Neuromancer"), QStringLiteral("Dune"), QStringLiteral("Hyperion")}));
    d = q.run(QStringLiteral("Desc"));
    EXPECT_EQ(names(d.rows).mid(0, 3), (QStringList{QStringLiteral("Hyperion"), QStringLiteral("Dune"), QStringLiteral("Neuromancer")}));
    // files without pages keep their name order after the others
    EXPECT_EQ(names(d.rows).mid(3), (QStringList{QStringLiteral("Frank Herbert"), QStringLiteral("Books.base"), QStringLiteral("Draft"),
                                                  QStringLiteral("cover.png")}));
}

TEST(BasesQuery, GroupsAreLooselyKeyedAndNoneComesLast)
{
    QueryEnv q(QStringLiteral(R"(filters: file.ext == "md"
views:
  - type: table
    name: G
    groupBy:
      property: status
      direction: ASC
)"));
    const ViewData d = q.run();
    ASSERT_EQ(d.groups.size(), 3);
    EXPECT_EQ(d.groups[0].key.toString(), QStringLiteral("read"));
    EXPECT_EQ(names(d.groups[0].entries), (QStringList{QStringLiteral("Dune"), QStringLiteral("Neuromancer")}));
    EXPECT_EQ(d.groups[1].key.toString(), QStringLiteral("reading"));
    EXPECT_FALSE(d.groups[2].hasKey);
    EXPECT_EQ(names(d.groups[2].entries), (QStringList{QStringLiteral("Draft"), QStringLiteral("Frank Herbert")}));
}

TEST(BasesQuery, LimitAndSearch)
{
    QueryEnv q(QStringLiteral(R"(views:
  - type: table
    name: L
    order:
      - file.name
      - author
    limit: 2
    sort:
      - property: file.name
        direction: DESC
)"));
    ViewData d = q.run();
    EXPECT_EQ(d.matched, 7);
    EXPECT_EQ(names(d.rows), (QStringList{QStringLiteral("Neuromancer"), QStringLiteral("Hyperion")}));
    d = q.run(QString(), QStringLiteral("DAN simm"));
    EXPECT_EQ(names(d.rows), QStringList{QStringLiteral("Hyperion")});
    d = q.run(QString(), QStringLiteral("frank"));
    EXPECT_EQ(names(d.rows), (QStringList{QStringLiteral("Frank Herbert"), QStringLiteral("Dune")}));
}

TEST(BasesQuery, FilterErrorsAreReported)
{
    QueryEnv q(QStringLiteral("filters: pages.foo()\n"));
    QStringList errors;
    const ViewData d = q.run(QString(), QString(), &errors);
    EXPECT_EQ(d.rows.size(), 0); // books: an error (reported); others: null.foo() is null (falsy, no error)
    ASSERT_FALSE(errors.isEmpty());
    EXPECT_EQ(errors[0], QStringLiteral("Failed to evaluate a filter: Cannot find function \"foo\" on type Number"));
}

TEST(BasesQuery, Summaries)
{
    QueryEnv q(QStringLiteral(R"(filters: file.hasTag("book")
summaries:
  Total pages: values.sum() * 1
views:
  - type: table
    name: S
)"));
    const ViewData d = q.run();
    QList<Value> pages, published, statuses, flags;
    for (const EntryPtr& e : d.rows) {
        pages << e->value(QStringLiteral("note.pages"));
        published << e->value(QStringLiteral("note.published"));
        statuses << e->value(QStringLiteral("note.status"));
        flags << Value::boolean(e->value(QStringLiteral("note.rating")).number() >= 4);
    }
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Sum")).toString(), QStringLiteral("1165"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Average")).toString(), QStringLiteral("388.33"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Min")).toString(), QStringLiteral("271"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Max")).toString(), QStringLiteral("482"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Range")).toString(), QStringLiteral("211"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Median")).toString(), QStringLiteral("412"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, pages, QStringLiteral("Total pages")).toString(), QStringLiteral("1165"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, published, QStringLiteral("Earliest")).toString(), QStringLiteral("1965-08-01T00:00:00"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, statuses, QStringLiteral("Unique")).toString(), QStringLiteral("2"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, statuses, QStringLiteral("Filled")).toString(), QStringLiteral("3"));
    EXPECT_EQ(summaryValue(q.base, &q.ds, flags, QStringLiteral("Checked")).toString(), QStringLiteral("2"));
    EXPECT_TRUE(summaryValue(q.base, &q.ds, statuses, QStringLiteral("Sum")).isNull());
    EXPECT_EQ(BaseFile::builtinSummaries(ValueType::Number),
              (QStringList{QStringLiteral("Empty"), QStringLiteral("Filled"), QStringLiteral("Unique"), QStringLiteral("Average"),
                           QStringLiteral("Min"), QStringLiteral("Max"), QStringLiteral("Sum"), QStringLiteral("Range"),
                           QStringLiteral("Median"), QStringLiteral("Stddev")}));
    EXPECT_EQ(q.base.customSummaryNames(), QStringList{QStringLiteral("Total pages")});
}

TEST(BasesQuery, NewItemDefaultsFollowTheFilters)
{
    QueryEnv q(QStringLiteral(R"(filters:
  and:
    - file.inFolder("Books")
    - file.hasTag("book")
    - status == "todo"
    - genres.contains("scifi")
    - '!rating.isEmpty()'
    - not:
        - author == "Nobody"
views:
  - type: table
    name: T
    order:
      - file.name
      - author
      - year
)"));
    const NewItemDefaults d = newItemDefaults(q.base, q.base.view(QString()), &q.ds, {}, [](const QString& key) {
        return key == u"genres" ? QStringLiteral("multitext") : QStringLiteral("text");
    });
    EXPECT_EQ(d.folder, QStringLiteral("Books"));
    QStringList keys;
    for (const auto& [k, v] : d.frontmatter)
        keys << k;
    EXPECT_EQ(keys, (QStringList{QStringLiteral("tags"), QStringLiteral("status"), QStringLiteral("genres"), QStringLiteral("rating"),
                                 QStringLiteral("author"), QStringLiteral("year")}));
    EXPECT_EQ(d.frontmatter[0].second.toList(), QVariantList{QStringLiteral("book")});
    EXPECT_EQ(d.frontmatter[1].second.toString(), QStringLiteral("todo"));
    EXPECT_EQ(d.frontmatter[2].second.toList(), QVariantList{QStringLiteral("scifi")});
    EXPECT_EQ(d.frontmatter[3].second.toString(), QString());
    EXPECT_FALSE(d.frontmatter[4].second.isValid());
}

TEST(BasesQuery, Export)
{
    QueryEnv q(QStringLiteral(R"(filters: file.hasTag("book")
properties:
  note.author:
    displayName: "Author, name"
views:
  - type: table
    name: T
    order:
      - file.name
      - author
      - pages
)"));
    const TableExport t = exportTable(q.base, q.run());
    EXPECT_EQ(t.toTsv(), QStringLiteral("file name\tAuthor, name\tpages\nDune\tFrank Herbert\t412\nHyperion\tDan Simmons\t482\n"
                                        "Neuromancer\tWilliam Gibson\t271"));
    EXPECT_EQ(t.toCsv().section(u'\n', 0, 0), QStringLiteral("file name,\"Author, name\",pages"));
    EXPECT_EQ(t.toMarkdown().section(u'\n', 0, 1), QStringLiteral("|  file name  |  Author, name  | pages |\n"
                                                                  "| ----------- | -------------- | ----- |"));
}

TEST(BasesRunner, FollowsChanges)
{
    auto ds = std::make_shared<MemoryDataSource>();
    (void)bases_test::kUtc;
    bases_test::addBooks(*ds);
    QueryRunner runner(ds);
    runner.setSynchronous(true);
    QSignalSpy updated(&runner, &QueryRunner::updated);
    runner.setQuery(*BaseFile::fromString(QStringLiteral("filters: file.hasTag(\"book\")\n")), QString());
    ASSERT_TRUE(runner.isReady());
    EXPECT_EQ(names(runner.data().rows), (QStringList{QStringLiteral("Dune"), QStringLiteral("Hyperion"), QStringLiteral("Neuromancer")}));
    const int before = int(updated.count());
    ds->addFile(QStringLiteral("Books/Snow Crash.md"), QStringLiteral("---\ntags: [book]\n---\n"));
    runner.fileChanged(QStringLiteral("Books/Snow Crash.md"));
    EXPECT_GT(updated.count(), before);
    EXPECT_EQ(runner.data().rows.size(), 4);
    ds->addFile(QStringLiteral("Books/Dune.md"), QStringLiteral("no longer a book\n"));
    runner.fileChanged(QStringLiteral("Books/Dune.md"));
    EXPECT_EQ(names(runner.data().rows), (QStringList{QStringLiteral("Hyperion"), QStringLiteral("Neuromancer"), QStringLiteral("Snow Crash")}));
    ds->removeFile(QStringLiteral("Books/Hyperion.md"));
    runner.fileRemoved(QStringLiteral("Books/Hyperion.md"));
    EXPECT_EQ(runner.data().rows.size(), 2);
    // sort-only changes republish without a rescan
    BaseFile b = runner.base();
    b.views[0].setSortProperty(QStringLiteral("file.name"), QStringLiteral("DESC"));
    runner.setQuery(b, QString());
    EXPECT_EQ(names(runner.data().rows), (QStringList{QStringLiteral("Snow Crash"), QStringLiteral("Neuromancer")}));
    runner.setSearch(QStringLiteral("snow"));
    EXPECT_EQ(runner.data().rows.size(), 1);
}

TEST(BasesRunner, ScansInSlicesAndReportsErrors)
{
    auto ds = std::make_shared<MemoryDataSource>();
    for (int i = 0; i < 300; ++i)
        ds->addFile(QStringLiteral("n%1.md").arg(i), QStringLiteral("---\nv: %1\n---\n").arg(i));
    QueryRunner runner(ds);
    QSignalSpy updated(&runner, &QueryRunner::updated);
    QSignalSpy errors(&runner, &QueryRunner::errorsReported);
    runner.setQuery(*BaseFile::fromString(QStringLiteral("filters: v > 10 && v.x()\n")), QString());
    EXPECT_TRUE(runner.isScanning());
    ASSERT_TRUE(QTest::qWaitFor([&] { return runner.isReady(); }, 10000));
    EXPECT_EQ(runner.data().rows.size(), 0);
    ASSERT_EQ(errors.count(), 1);
    EXPECT_EQ(errors[0][0].toStringList().value(0), QStringLiteral("Failed to evaluate a filter: Cannot find function \"x\" on type Number"));
}

} // namespace
