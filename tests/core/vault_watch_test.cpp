// External change detection: inotify, polling and hybrid backends (selected through the
// OC_VAULT_WATCHER environment variable, like a user would), coalescing, rename-over, temp files,
// bursts, self-write suppression, offline handling and the mass-delete guard.

#include "core/util/busy.h"
#include "core/vault/vault.h"
#include "core/vault/vault_private.h"
#include "vault_test_util.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace oc;
using vt::SignalLog;

namespace {

class WatchFixture {
protected:
    void setUpDirs()
    {
        ASSERT_TRUE(m_tmp.isValid());
        m_root = m_tmp.path() + QStringLiteral("/Watched Vault");
        ASSERT_TRUE(QDir().mkpath(m_root));
    }
    QString abs(const QString& rel) const { return m_root + u'/' + rel; }
    void put(const QString& rel, const QByteArray& bytes = "x") { ASSERT_TRUE(vt::writeFile(abs(rel), bytes)); }
    void mv(const QString& from, const QString& to)
    {
        ASSERT_EQ(::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()), 0)
            << from.toStdString() << " -> " << to.toStdString();
    }
    Vault* openWith(const char* backend)
    {
        qputenv("OC_VAULT_WATCHER", backend);
        m_vault = std::make_unique<Vault>(m_root);
        qunsetenv("OC_VAULT_WATCHER");
        VaultConfig& c = VaultImplAccess::config(*m_vault);
        EXPECT_EQ(c.watcher, QString::fromUtf8(backend));
        c.pollIntervalMs = 100;
        c.hybridSweepMs = 250;
        c.offlinePollMs = 100;
        m_vault->load();
        EXPECT_EQ(VaultImplAccess::watcherBackend(*m_vault), QString::fromUtf8(backend));
        m_log = std::make_unique<SignalLog>(m_vault.get());
        return m_vault.get();
    }
    void tearDownVault()
    {
        m_log.reset();
        m_vault.reset();
    }

    QTemporaryDir m_tmp;
    QString m_root;
    std::unique_ptr<Vault> m_vault;
    std::unique_ptr<SignalLog> m_log;
};

class VaultWatchTest : public ::testing::TestWithParam<const char*>, public WatchFixture {
protected:
    void SetUp() override { setUpDirs(); }
    void TearDown() override { tearDownVault(); }
    Vault* open() { return openWith(GetParam()); }
    bool polling() const { return qstrcmp(GetParam(), "poll") == 0; }
    // Long enough for any late (wrong) signal to show up.
    int settleMs() const { return polling() ? 800 : 600; }
    SignalLog& log() { return *m_log; }
};

} // namespace

TEST_P(VaultWatchTest, ExternalCreateModifyDelete)
{
    Vault* v = open();
    put(QStringLiteral("new.md"), "hello");
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("new.md")) != nullptr; })) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("created"), QStringLiteral("new.md"), true));
    EXPECT_EQ(v->file(QStringLiteral("new.md"))->size(), 5);

    put(QStringLiteral("new.md"), "hello world");
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("new.md"))->size() == 11; })) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("modified"), QStringLiteral("new.md"), true));

    ASSERT_TRUE(QFile::remove(abs(QStringLiteral("new.md"))));
    ASSERT_TRUE(vt::waitFor([&] { return v->item(QStringLiteral("new.md")) == nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().count(QStringLiteral("created")), 1) << log().dump();
    EXPECT_EQ(log().count(QStringLiteral("deleted"), true), 1) << log().dump();
    EXPECT_EQ(log().count(QStringLiteral("renamed")), 0) << log().dump();
    EXPECT_TRUE(log().online.isEmpty());
}

TEST_P(VaultWatchTest, ExternalRenameAndMove)
{
    put(QStringLiteral("a.md"), "aaa");
    put(QStringLiteral("Dir/keep.md"));
    Vault* v = open();
    TFile* a = v->file(QStringLiteral("a.md"));
    ASSERT_NE(a, nullptr);
    mv(abs(QStringLiteral("a.md")), abs(QStringLiteral("Dir/b.md")));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("Dir/b.md")) != nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(v->file(QStringLiteral("Dir/b.md")), a); // same object, now renamed
    EXPECT_EQ(v->item(QStringLiteral("a.md")), nullptr);
    EXPECT_TRUE(log().hasRename(QStringLiteral("a.md"), QStringLiteral("Dir/b.md"), true)) << log().dump();
    EXPECT_EQ(log().evs.size(), 1) << log().dump();
}

TEST_P(VaultWatchTest, ExternalFolderRename)
{
    put(QStringLiteral("F/one.md"));
    put(QStringLiteral("F/sub/two.md"));
    Vault* v = open();
    TFolder* f = v->folder(QStringLiteral("F"));
    TFile* two = v->file(QStringLiteral("F/sub/two.md"));
    ASSERT_TRUE(f && two);
    mv(abs(QStringLiteral("F")), abs(QStringLiteral("G")));
    ASSERT_TRUE(vt::waitFor([&] { return v->folder(QStringLiteral("G")) != nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(v->folder(QStringLiteral("G")), f);
    EXPECT_EQ(two->path(), QStringLiteral("G/sub/two.md"));
    EXPECT_EQ(v->item(QStringLiteral("F")), nullptr);
    EXPECT_EQ(log().count(QStringLiteral("renamed"), true), 4) << log().dump();
    EXPECT_TRUE(log().hasRename(QStringLiteral("F/sub/two.md"), QStringLiteral("G/sub/two.md"), true));
    EXPECT_EQ(log().count(QStringLiteral("created")) + log().count(QStringLiteral("deleted")), 0) << log().dump();

    // changes inside the moved folder are still seen (its watches followed the move)
    put(QStringLiteral("G/sub/three.md"));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("G/sub/three.md")) != nullptr; })) << log().dump();
}

TEST_P(VaultWatchTest, FolderCreatedWithFilesInside)
{
    Vault* v = open();
    for (int i = 0; i < 5; ++i)
        put(QStringLiteral("New Folder/n%1.md").arg(i));
    put(QStringLiteral("New Folder/deep/er/x.md"));
    ASSERT_TRUE(vt::waitFor([&] { return v->fileCount() == 6; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_NE(v->folder(QStringLiteral("New Folder/deep/er")), nullptr);
    EXPECT_EQ(log().count(QStringLiteral("created"), true), 9) << log().dump();
    // parents are announced before their children
    QStringList order;
    for (const vt::Ev& e : log().evs)
        order.append(e.path);
    EXPECT_LT(order.indexOf(QStringLiteral("New Folder")), order.indexOf(QStringLiteral("New Folder/n0.md")));
    EXPECT_LT(order.indexOf(QStringLiteral("New Folder/deep/er")), order.indexOf(QStringLiteral("New Folder/deep/er/x.md")));

    // a file added later inside the new folder is noticed too (the folder got a watch)
    put(QStringLiteral("New Folder/deep/later.md"));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("New Folder/deep/later.md")) != nullptr; }));
}

TEST_P(VaultWatchTest, RenameOverIsOneModification)
{
    put(QStringLiteral("note.md"), "version 1");
    Vault* v = open();
    TFile* note = v->file(QStringLiteral("note.md"));
    ASSERT_NE(note, nullptr);
    // What the user's external tool does: write "<name>.tmp.<pid>.<hex>", rename it over the note.
    put(QStringLiteral("note.md.tmp.1234.abcd"), "version 2 (longer)");
    mv(abs(QStringLiteral("note.md.tmp.1234.abcd")), abs(QStringLiteral("note.md")));
    ASSERT_TRUE(vt::waitFor([&] { return note->size() == 18; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(v->file(QStringLiteral("note.md")), note);
    EXPECT_EQ(log().evs.size(), 1) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("modified"), QStringLiteral("note.md"), true)) << log().dump();
    EXPECT_EQ(v->item(QStringLiteral("note.md.tmp.1234.abcd")), nullptr);
}

TEST_P(VaultWatchTest, TempAndDotFilesNeverAppear)
{
    put(QStringLiteral("real.md"));
    Vault* v = open();
    for (const char* n : {"x.md.tmp.99.ff", ".hidden.md", "y.swp", "z.md~", "4913", ".#lock", ".obsidian/app.json",
                          ".git/objects/ab/cdef", "real.md.tmp.5.a0"})
        put(QString::fromUtf8(n));
    put(QStringLiteral("real.md.tmp.77.beef"), "tmp");
    ASSERT_TRUE(QFile::remove(abs(QStringLiteral("real.md.tmp.77.beef"))));
    QTest::qWait(settleMs());
    EXPECT_TRUE(log().evs.isEmpty()) << log().dump();
    EXPECT_EQ(v->fileCount(), 1);
    EXPECT_TRUE(v->folders().isEmpty());
}

TEST_P(VaultWatchTest, BurstOfThousandFilesIsCoalesced)
{
    Vault* v = open();
    const int passesBefore = VaultImplAccess::watcherPasses(*v);
    for (int i = 0; i < 1000; ++i)
        put(QStringLiteral("burst/file %1.md").arg(i, 4, 10, QLatin1Char('0')), QByteArray::number(i));
    ASSERT_TRUE(vt::waitFor([&] { return v->fileCount() == 1000; }, 30000)) << v->fileCount();
    QTest::qWait(settleMs());
    EXPECT_EQ(v->fileCount(), 1000);
    EXPECT_EQ(log().count(QStringLiteral("created"), true), 1001); // folder + files, each exactly once
    EXPECT_EQ(log().count(QStringLiteral("deleted")), 0);
    EXPECT_EQ(log().count(QStringLiteral("renamed")), 0);
    EXPECT_LE(log().count(QStringLiteral("modified")), 1000);
    for (int i = 0; i < 1000; i += 97) {
        TFile* f = v->file(QStringLiteral("burst/file %1.md").arg(i, 4, 10, QLatin1Char('0')));
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->size(), QByteArray::number(i).size());
    }
    if (qstrcmp(GetParam(), "inotify") == 0) {
        EXPECT_LE(VaultImplAccess::watcherPasses(*v) - passesBefore, 4); // a handful of passes, not one per event
    }
}

TEST_P(VaultWatchTest, OwnOperationsProduceNoExternalSignals)
{
    put(QStringLiteral("s.md"), "start");
    put(QStringLiteral("Folder/in.md"));
    Vault* v = open();
    QString err;
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("s.md"), &st, &err));
    ASSERT_EQ(v->write(QStringLiteral("s.md"), "changed by us", st, &st, &err), Vault::WriteResult::Ok);
    ASSERT_EQ(v->write(QStringLiteral("s.md"), "changed again", st, &st, &err), Vault::WriteResult::Ok);
    TFile* own = v->createFile(QStringLiteral("own/new.md"), "mine", &err);
    ASSERT_NE(own, nullptr) << err.toStdString();
    ASSERT_TRUE(v->rename(own, QStringLiteral("own/renamed.md"), &err)) << err.toStdString();
    ASSERT_TRUE(v->rename(v->folder(QStringLiteral("own")), QStringLiteral("own2"), &err)) << err.toStdString();
    ASSERT_NE(v->copy(v->file(QStringLiteral("s.md")), QStringLiteral("copy.md"), &err), nullptr);
    ASSERT_NE(v->createFolder(QStringLiteral("empty/dir"), &err), nullptr);
    ASSERT_TRUE(v->trash(v->file(QStringLiteral("copy.md")), Vault::TrashMode::Local, &err)) << err.toStdString();
    ASSERT_TRUE(v->trash(v->folder(QStringLiteral("Folder")), Vault::TrashMode::Permanent, &err)) << err.toStdString();
    DiskState none;
    ASSERT_EQ(v->write(QStringLiteral("brand new.md"), "n", none, nullptr, &err), Vault::WriteResult::Ok);
    const int ownSignals = log().evs.size();
    EXPECT_EQ(log().externalCount(), 0);
    QTest::qWait(settleMs() + 300);
    EXPECT_EQ(log().externalCount(), 0) << log().dump();
    EXPECT_EQ(log().evs.size(), ownSignals);
    EXPECT_EQ(vt::sortedPaths(v->files()), QStringList({QStringLiteral("brand new.md"), QStringLiteral("own2/renamed.md"),
                                                        QStringLiteral("s.md")}));
    EXPECT_GT(VaultImplAccess::selfWriteEntries(*v), 0);
}

TEST_P(VaultWatchTest, ExternalEditRightAfterOwnWriteIsReported)
{
    put(QStringLiteral("race.md"), "base");
    Vault* v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("race.md"), &st, nullptr));
    ASSERT_EQ(v->write(QStringLiteral("race.md"), "ours", st, &st, nullptr), Vault::WriteResult::Ok);
    put(QStringLiteral("race.md"), "theirs, longer");
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("race.md"))->size() == 14; })) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("modified"), QStringLiteral("race.md"), true)) << log().dump();
    EXPECT_EQ(v->write(QStringLiteral("race.md"), "stale", st, nullptr, nullptr), Vault::WriteResult::Conflict);
}

TEST_P(VaultWatchTest, OwnWriteSeenWithDifferentMtimeStaysSilent)
{
    put(QStringLiteral("drift.md"), "base");
    Vault* v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("drift.md"), &st, nullptr));
    ASSERT_EQ(v->write(QStringLiteral("drift.md"), "ours!", st, &st, nullptr), Vault::WriteResult::Ok);
    // Like a FUSE daemon reporting a different mtime than the one we saw right after writing.
    const QByteArray a = QFile::encodeName(abs(QStringLiteral("drift.md")));
    const struct timespec times[2] = {{0, UTIME_OMIT}, {st.mtimeNs / 1'000'000'000 - 60, 0}};
    ASSERT_EQ(::utimensat(AT_FDCWD, a.constData(), times, 0), 0);
    QTest::qWait(settleMs());
    EXPECT_EQ(log().externalCount(), 0) << log().dump();
    EXPECT_EQ(v->file(QStringLiteral("drift.md"))->mtimeMs() / 1000, st.mtimeNs / 1'000'000'000 - 60); // adopted
}

TEST_P(VaultWatchTest, BusyReturnsToIdle)
{
    Vault* v = open();
    for (int i = 0; i < 20; ++i)
        put(QStringLiteral("b/%1.md").arg(i));
    ASSERT_TRUE(vt::waitFor([&] { return v->fileCount() == 20; })) << log().dump();
    v->rescan();
    EXPECT_TRUE(vt::waitFor([] { return Busy::idle(); }, 5000));
}

TEST_P(VaultWatchTest, ExternalFolderDeleteReportsEveryItem)
{
    put(QStringLiteral("Doomed/a.md"));
    put(QStringLiteral("Doomed/sub/b.md"));
    put(QStringLiteral("Stays.md"));
    Vault* v = open();
    ASSERT_TRUE(QDir(abs(QStringLiteral("Doomed"))).removeRecursively());
    ASSERT_TRUE(vt::waitFor([&] { return v->item(QStringLiteral("Doomed")) == nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().count(QStringLiteral("deleted"), true), 4) << log().dump();
    EXPECT_EQ(log().evs.last().path, QStringLiteral("Doomed"));
    EXPECT_EQ(v->fileCount(), 1);
}

TEST_P(VaultWatchTest, FoldersMovedOutOfAndIntoTheVault)
{
    put(QStringLiteral("Out/x.md"));
    const QString outside = m_tmp.path() + QStringLiteral("/elsewhere");
    ASSERT_TRUE(vt::writeFile(outside + QStringLiteral("/In/y.md"), "y"));
    Vault* v = open();
    mv(abs(QStringLiteral("Out")), outside + QStringLiteral("/Out"));
    mv(outside + QStringLiteral("/In"), abs(QStringLiteral("In")));
    ASSERT_TRUE(vt::waitFor([&] {
        return v->item(QStringLiteral("Out")) == nullptr && v->file(QStringLiteral("In/y.md")) != nullptr;
    })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_TRUE(log().has(QStringLiteral("deleted"), QStringLiteral("Out/x.md"), true));
    EXPECT_TRUE(log().has(QStringLiteral("created"), QStringLiteral("In/y.md"), true));
    // the moved-out folder is not watched any more, the moved-in one is
    const int before = log().evs.size();
    ASSERT_TRUE(vt::writeFile(outside + QStringLiteral("/Out/ghost.md"), "boo"));
    put(QStringLiteral("In/z.md"));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("In/z.md")) != nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().evs.size(), before + 1) << log().dump();
}

TEST_P(VaultWatchTest, SameSizeSameMtimeEditIsCaughtByHash)
{
    if (polling())
        GTEST_SKIP() << "sweeps are stat-only: an edit that keeps size and mtime is invisible to them";
    put(QStringLiteral("coarse.md"), "AAAA");
    Vault* v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("coarse.md"), &st, nullptr)); // the vault now knows the content hash
    const QByteArray a = QFile::encodeName(abs(QStringLiteral("coarse.md")));
    struct stat before {};
    ASSERT_EQ(::stat(a.constData(), &before), 0);
    // Rewrite in place with the same size, then put the old mtime back (like a coarse clock would).
    {
        QFile f(abs(QStringLiteral("coarse.md")));
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_EQ(f.write("BBBB"), 4);
    }
    const struct timespec times[2] = {before.st_atim, before.st_mtim};
    ASSERT_EQ(::utimensat(AT_FDCWD, a.constData(), times, 0), 0);
    ASSERT_TRUE(vt::waitFor([&] { return log().has(QStringLiteral("modified"), QStringLiteral("coarse.md"), true); }))
        << log().dump();
}

TEST_P(VaultWatchTest, RootRemovedGoesOfflineWithoutDeletions)
{
    put(QStringLiteral("a.md"), "a");
    put(QStringLiteral("b.md"), "b");
    put(QStringLiteral("Sub/c.md"), "c");
    Vault* v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("a.md"), &st, nullptr));
    ASSERT_TRUE(QDir(m_root).removeRecursively());
    ASSERT_TRUE(vt::waitFor([&] { return !log().online.isEmpty(); })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().online, QList<bool>({false}));
    EXPECT_FALSE(v->isOnline());
    EXPECT_EQ(log().evs.size(), 0) << log().dump(); // no deleted signals: the model is kept
    EXPECT_EQ(v->fileCount(), 3);
    EXPECT_EQ(v->write(QStringLiteral("a.md"), "x", st, nullptr, nullptr), Vault::WriteResult::Error);
    EXPECT_FALSE(QFileInfo::exists(m_root)); // and nothing was recreated

    // The folder comes back (drive re-plugged) with some differences.
    put(QStringLiteral("a.md"), "a");
    put(QStringLiteral("Sub/c.md"), "c changed");
    put(QStringLiteral("new.md"), "n");
    ASSERT_TRUE(vt::waitFor([&] { return v->isOnline(); })) << log().dump();
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("new.md")) != nullptr; })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().online, QList<bool>({false, true}));
    EXPECT_TRUE(log().has(QStringLiteral("deleted"), QStringLiteral("b.md"), true)) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("created"), QStringLiteral("new.md"), true)) << log().dump();
    EXPECT_TRUE(log().has(QStringLiteral("modified"), QStringLiteral("Sub/c.md"), true)) << log().dump();
    EXPECT_EQ(v->fileCount(), 3);
    // watching works again
    put(QStringLiteral("after.md"));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("after.md")) != nullptr; })) << log().dump();
}

TEST_P(VaultWatchTest, EmptyMountPointStaysOffline)
{
    for (int i = 0; i < 30; ++i)
        put(QStringLiteral("n%1.md").arg(i));
    Vault* v = open();
    const QString away = m_tmp.path() + QStringLiteral("/unmounted");
    mv(m_root, away);
    ASSERT_TRUE(QDir().mkpath(m_root)); // what's left when a drive is unmounted: an empty mount point
    ASSERT_TRUE(vt::waitFor([&] { return !v->isOnline(); })) << log().dump();
    QTest::qWait(1000); // many offline polls
    EXPECT_FALSE(v->isOnline());
    EXPECT_EQ(log().evs.size(), 0) << log().dump();
    EXPECT_EQ(v->fileCount(), 30);

    ASSERT_TRUE(QDir().rmdir(m_root));
    mv(away, m_root); // re-mounted
    ASSERT_TRUE(vt::waitFor([&] { return v->isOnline(); })) << log().dump();
    QTest::qWait(settleMs());
    EXPECT_EQ(log().evs.size(), 0) << log().dump();
    EXPECT_EQ(log().online, QList<bool>({false, true}));
    EXPECT_EQ(v->fileCount(), 30);
}

TEST_P(VaultWatchTest, LargeLegitimateDeletionIsApplied)
{
    for (int i = 0; i < 40; ++i)
        put(QStringLiteral("f%1.md").arg(i));
    Vault* v = open();
    for (int i = 0; i < 35; ++i)
        ASSERT_TRUE(QFile::remove(abs(QStringLiteral("f%1.md").arg(i))));
    ASSERT_TRUE(vt::waitFor([&] { return v->fileCount() == 5; })) << log().dump();
    EXPECT_EQ(log().count(QStringLiteral("deleted"), true), 35);
    EXPECT_TRUE(v->isOnline());
}

INSTANTIATE_TEST_SUITE_P(Backends, VaultWatchTest, ::testing::Values("inotify", "poll", "hybrid"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

// ---------------------------------------------------------------------------------------------
// The hybrid backend's safety net: changes inotify missed are found by sweeps and rescan().

class VaultHybridTest : public ::testing::Test, public WatchFixture {
protected:
    void SetUp() override { setUpDirs(); }
    void TearDown() override { tearDownVault(); }
};

TEST_F(VaultHybridTest, SweepAndRescanFindMissedChanges)
{
    put(QStringLiteral("seen.md"), "s");
    put(QStringLiteral("renamed-behind-our-back.md"), "r");
    Vault* v = openWith("hybrid");
    VaultImplAccess::setDropWatcherEvents(*v, true); // as if the FUSE daemon changed things itself
    put(QStringLiteral("missed.md"), "m");
    mv(abs(QStringLiteral("renamed-behind-our-back.md")), abs(QStringLiteral("found.md")));
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("missed.md")) != nullptr; })) << m_log->dump();
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("found.md")) != nullptr; })) << m_log->dump();
    EXPECT_TRUE(m_log->has(QStringLiteral("created"), QStringLiteral("missed.md"), true));
    EXPECT_TRUE(m_log->hasRename(QStringLiteral("renamed-behind-our-back.md"), QStringLiteral("found.md"), true))
        << m_log->dump();

    // rescan() (window focus) triggers a sweep right away
    put(QStringLiteral("focus.md"), "f");
    v->rescan();
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("focus.md")) != nullptr; }, 3000)) << m_log->dump();
    ASSERT_TRUE(vt::waitFor([] { return Busy::idle(); }, 3000));
}

TEST_F(VaultHybridTest, InotifyRescanWithoutSweeps)
{
    Vault* v = openWith("inotify");
    VaultImplAccess::setDropWatcherEvents(*v, true);
    put(QStringLiteral("quiet.md"));
    QTest::qWait(400);
    EXPECT_EQ(v->file(QStringLiteral("quiet.md")), nullptr); // no sweeps with plain inotify
    v->rescan();
    ASSERT_TRUE(vt::waitFor([&] { return v->file(QStringLiteral("quiet.md")) != nullptr; }, 3000)) << m_log->dump();
}
