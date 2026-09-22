// File recovery: snapshot store (SQLite + zstd) and Obsidian's snapshot timing rules.
#include "core/recovery/file_recovery.h"
#include "core/recovery/snapshot_store.h"

#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

TEST(SnapshotStore, AddListReadCleanup)
{
    QTemporaryDir dir;
    SnapshotStore store(dir.filePath("sub/recovery.sqlite"));
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    const QByteArray a("line 1\r\nline 2  \r\n\tno final newline");
    QByteArray big;
    for (int i = 0; i < 20000; ++i)
        big += "repetitive content " + QByteArray::number(i % 7) + "\n";
    ASSERT_TRUE(store.add("n/a.md", 1000, a));
    ASSERT_TRUE(store.add("n/a.md", 2000, big));
    ASSERT_TRUE(store.add("b.md", 1500, QByteArray()));
    EXPECT_EQ(store.count(), 3);
    EXPECT_EQ(store.paths(), (QStringList{"n/a.md", "b.md"}));
    const QList<SnapshotStore::Info> h = store.history("n/a.md");
    ASSERT_EQ(h.size(), 2);
    EXPECT_EQ(h[0].ts, 2000);
    EXPECT_EQ(h[0].size, big.size());
    EXPECT_EQ(*store.bytes(h[1].id), a); // byte-exact round trip
    EXPECT_EQ(*store.bytes(h[0].id), big);
    EXPECT_EQ(*store.bytes(store.latest("b.md")->id), QByteArray());
    EXPECT_EQ(store.latest("n/a.md")->ts, 2000);
    EXPECT_FALSE(store.latest("missing.md").has_value());
    EXPECT_EQ(store.removeOlderThan(1600), 2);
    EXPECT_EQ(store.count(), 1);
    // persists across reopen
    {
        SnapshotStore again(dir.filePath("sub/recovery.sqlite"));
        ASSERT_TRUE(again.open());
        EXPECT_EQ(again.count(), 1);
    }
    store.clear();
    EXPECT_EQ(store.count(), 0);
}

TEST(FileRecovery, IntervalPendingAndDedup)
{
    QTemporaryDir dir;
    SnapshotStore store(dir.filePath("r.sqlite"));
    ASSERT_TRUE(store.open());
    QHash<QString, QByteArray> files{{"a.md", "v1"}, {"img.png", "binary"}};
    qint64 clock = 10'000'000;
    FileRecovery fr(&store, [&](const QString& p) -> std::optional<QByteArray> {
        if (!files.contains(p))
            return std::nullopt;
        return files.value(p);
    });
    fr.setClock([&] { return clock; });

    fr.fileChanged("img.png"); // not tracked
    fr.fileChanged("a.md");    // first snapshot
    EXPECT_EQ(store.history("a.md").size(), 1);
    EXPECT_EQ(store.count(), 1);

    files["a.md"] = "v2";
    clock += 60'000; // 1 min later: within the 5 min interval -> pending
    fr.fileChanged("a.md");
    EXPECT_EQ(store.history("a.md").size(), 1);
    EXPECT_EQ(fr.pendingCount(), 1);
    clock += 5 * 60'000;
    fr.resave(); // interval elapsed: snapshot taken
    EXPECT_EQ(fr.pendingCount(), 0);
    ASSERT_EQ(store.history("a.md").size(), 2);
    EXPECT_EQ(*store.bytes(store.latest("a.md")->id), QByteArray("v2"));

    clock += 10 * 60'000;
    fr.fileChanged("a.md"); // unchanged content: no new snapshot
    EXPECT_EQ(store.history("a.md").size(), 2);

    fr.forceAdd("a.md", "forced");
    EXPECT_EQ(store.history("a.md").size(), 3);

    // cleanup keeps `keepDays`
    clock += 8LL * 24 * 3600 * 1000;
    fr.cleanup();
    EXPECT_EQ(store.count(), 0);
}
