// Vault model, lookups, file operations, compare-and-swap IO and trash (no watcher involved unless
// stated). All tests run inside a QTemporaryDir.

#include "core/vault/ignore_rules.h"
#include "core/vault/vault.h"
#include "core/vault/vault_io.h"
#include "core/vault/vault_private.h"
#include "text/codec.h"
#include "vault_test_util.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace oc;
using vt::SignalLog;

namespace {

class VaultModelTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_tmp.isValid());
        m_root = m_tmp.path() + QStringLiteral("/My Vault"); // spaces, like the real vault's path
        ASSERT_TRUE(QDir().mkpath(m_root));
    }
    QString abs(const QString& rel) const { return m_root + u'/' + rel; }
    void put(const QString& rel, const QByteArray& bytes = "x") { ASSERT_TRUE(vt::writeFile(abs(rel), bytes)); }
    bool exists(const QString& rel) const
    {
        struct stat st {};
        return ::lstat(QFile::encodeName(abs(rel)).constData(), &st) == 0;
    }
    std::unique_ptr<Vault> open(const QString& watcher = QStringLiteral("none"))
    {
        auto v = std::make_unique<Vault>(m_root);
        VaultImplAccess::config(*v).watcher = watcher;
        v->load();
        return v;
    }

    QTemporaryDir m_tmp;
    QString m_root;
};

QStringList childNames(const TFolder* f)
{
    QStringList out;
    for (const TAbstractFile* c : f->children())
        out.append(c->name());
    out.sort();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Ignore rules

TEST(VaultIgnoreRules, Names)
{
    for (const char* n : {".obsidian", ".git", ".trash", ".Trash-1000", ".DS_Store", ".#note.md", ".note.md.oc~123.ab",
                          ".note.md.swp", "note.md.tmp.1234.abcd", "a.tmp.1.F", "x.md.tmp.99999.deadBEEF", "note.swp",
                          "note.md~", "4913", "a..tmp.1.a"})
        EXPECT_TRUE(Vault::isIgnoredName(QString::fromUtf8(n))) << n;
    for (const char* n : {"note.md", "tmp.md", "note.tmp", "note.tmp.12", "note.tmp.12.xyz", "tmp.1.a", "4914", "a4913",
                          "x.swpx", "~x", "note.tmp.1.2.md", "Ünïcödé 日本.md", "My note (1).md", "a.tmp..a"})
        EXPECT_FALSE(Vault::isIgnoredName(QString::fromUtf8(n))) << n;
    EXPECT_TRUE(vaultfs::isIgnoredPath(u"a/.git/b"));
    EXPECT_TRUE(vaultfs::isIgnoredPath(u"a/b.md.tmp.1.a"));
    EXPECT_FALSE(vaultfs::isIgnoredPath(u"a/b/c.md"));
    EXPECT_FALSE(vaultfs::isIgnoredPath(u""));
}

TEST(VaultIgnoreRules, AtomicTempMatcherEqualsRegex)
{
    const QRegularExpression re(QStringLiteral("^.+\\.tmp\\.\\d+\\.[0-9a-fA-F]+$"));
    const QString alphabet = QStringLiteral("abfgAF19.tmpx");
    QRandomGenerator rng(42);
    int matches = 0;
    for (int i = 0; i < 50000; ++i) {
        QString s;
        const int len = 1 + int(rng.bounded(14));
        for (int k = 0; k < len; ++k)
            s += alphabet.at(int(rng.bounded(alphabet.size())));
        if (rng.bounded(3) == 0) // bias towards the interesting shape
            s = s.left(3) + QStringLiteral(".tmp.") + QString::number(rng.bounded(99999)) + u'.' + s.mid(3);
        const bool expected = re.match(s).hasMatch();
        matches += expected;
        ASSERT_EQ(vaultfs::isAtomicTempName(s), expected) << s.toStdString();
    }
    EXPECT_GT(matches, 1000);
}

TEST(VaultIgnoreRules, NormalizeAndSplit)
{
    QString out;
    EXPECT_TRUE(vaultfs::normalizePath(QStringLiteral("/a//b/c.md/"), &out));
    EXPECT_EQ(out, QStringLiteral("a/b/c.md"));
    EXPECT_FALSE(vaultfs::normalizePath(QStringLiteral("a/../b"), &out));
    EXPECT_FALSE(vaultfs::normalizePath(QStringLiteral("./a"), &out));
    EXPECT_FALSE(vaultfs::normalizePath(QStringLiteral("/"), &out));
    EXPECT_TRUE(vaultfs::normalizePath(QStringLiteral("/"), &out, true));
    EXPECT_EQ(out, QString());

    QString base, ext;
    vaultfs::splitName(QStringLiteral("archive.tar.gz"), &base, &ext);
    EXPECT_EQ(base, QStringLiteral("archive.tar"));
    EXPECT_EQ(ext, QStringLiteral("gz"));
    vaultfs::splitName(QStringLiteral("noext"), &base, &ext);
    EXPECT_EQ(base, QStringLiteral("noext"));
    EXPECT_EQ(ext, QString());
    vaultfs::splitName(QStringLiteral("trailing."), &base, &ext);
    EXPECT_EQ(base, QStringLiteral("trailing."));
    EXPECT_EQ(ext, QString());
}

// ---------------------------------------------------------------------------------------------
// Scan + lookups

TEST_F(VaultModelTest, ScanBuildsModelAndIgnoresDotAndTempEntries)
{
    put(QStringLiteral("Welcome.md"), "# Welcome\n");
    put(QStringLiteral("Notes/Idea one.md"));
    put(QStringLiteral("Notes/a/b/c/d/e/f/g/Deep note.md"), "deep");
    put(QStringLiteral("Ünïcödé 日本.md"));
    put(QStringLiteral("attachments/image.png"), QByteArray(1000, 'p'));
    put(QStringLiteral("Folder with spaces/File name with spaces.MD"));
    put(QStringLiteral("noext"));
    put(QStringLiteral("archive.tar.gz"));
    ASSERT_TRUE(QDir().mkpath(abs(QStringLiteral("Empty folder"))));
    // ignored
    put(QStringLiteral(".obsidian/app.json"), "{}");
    put(QStringLiteral(".git/HEAD"));
    put(QStringLiteral(".trash/old.md"));
    put(QStringLiteral("Notes/.hidden.md"));
    put(QStringLiteral("Notes/draft.md.tmp.1234.abcd"));
    put(QStringLiteral("Notes/.draft.md.oc~123.ff"));
    put(QStringLiteral("Notes/x.swp"));
    put(QStringLiteral("Notes/backup.md~"));
    put(QStringLiteral("Notes/4913"));
    put(QStringLiteral("Notes/.#lock"));
    put(QStringLiteral(".hidden folder/inside.md"));

    auto v = open();
    EXPECT_TRUE(v->isLoaded());
    EXPECT_TRUE(v->isOnline());
    EXPECT_EQ(v->name(), QStringLiteral("My Vault"));
    EXPECT_EQ(vt::sortedPaths(v->files()),
              QStringList({QStringLiteral("Folder with spaces/File name with spaces.MD"), QStringLiteral("Notes/Idea one.md"),
                           QStringLiteral("Notes/a/b/c/d/e/f/g/Deep note.md"), QStringLiteral("Welcome.md"),
                           QStringLiteral("archive.tar.gz"), QStringLiteral("attachments/image.png"), QStringLiteral("noext"),
                           QStringLiteral("Ünïcödé 日本.md")}));
    EXPECT_EQ(v->fileCount(), 8);
    EXPECT_EQ(v->markdownFiles().size(), 5);
    EXPECT_EQ(vt::sortedPaths(v->folders()),
              QStringList({QStringLiteral("Empty folder"), QStringLiteral("Folder with spaces"), QStringLiteral("Notes"),
                           QStringLiteral("Notes/a"), QStringLiteral("Notes/a/b"), QStringLiteral("Notes/a/b/c"),
                           QStringLiteral("Notes/a/b/c/d"), QStringLiteral("Notes/a/b/c/d/e"),
                           QStringLiteral("Notes/a/b/c/d/e/f"), QStringLiteral("Notes/a/b/c/d/e/f/g"),
                           QStringLiteral("attachments")}));
    EXPECT_EQ(childNames(v->folder(QStringLiteral("Notes"))), QStringList({QStringLiteral("Idea one.md"), QStringLiteral("a")}));
    EXPECT_EQ(v->item(QStringLiteral(".obsidian")), nullptr);
    EXPECT_EQ(v->item(QStringLiteral("Notes/draft.md.tmp.1234.abcd")), nullptr);
}

TEST_F(VaultModelTest, NamesStatsAndLookups)
{
    put(QStringLiteral("Folder with spaces/File name with spaces.MD"), "12345");
    put(QStringLiteral("archive.tar.gz"));
    put(QStringLiteral("noext"));
    auto v = open();

    TFolder* root = v->rootFolder();
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->isRoot());
    EXPECT_EQ(root->path(), QString());
    EXPECT_EQ(root->parent(), nullptr);
    EXPECT_EQ(v->folder(QString()), root);
    EXPECT_EQ(v->item(QStringLiteral("/")), root);

    TFile* f = v->file(QStringLiteral("Folder with spaces/File name with spaces.MD"));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->name(), QStringLiteral("File name with spaces.MD"));
    EXPECT_EQ(f->basename(), QStringLiteral("File name with spaces"));
    EXPECT_EQ(f->extension(), QStringLiteral("MD"));
    EXPECT_TRUE(f->isMarkdown());
    EXPECT_EQ(f->size(), 5);
    const QFileInfo fi(abs(f->path()));
    EXPECT_EQ(f->mtimeMs(), fi.lastModified().toMSecsSinceEpoch());
    EXPECT_GT(f->ctimeMs(), 0);
    EXPECT_EQ(f->parent(), v->folder(QStringLiteral("Folder with spaces")));
    EXPECT_EQ(f->parent()->parent(), root);

    TFile* tgz = v->file(QStringLiteral("archive.tar.gz"));
    ASSERT_NE(tgz, nullptr);
    EXPECT_EQ(tgz->basename(), QStringLiteral("archive.tar"));
    EXPECT_EQ(tgz->extension(), QStringLiteral("gz"));
    EXPECT_FALSE(tgz->isMarkdown());
    EXPECT_EQ(v->file(QStringLiteral("noext"))->extension(), QString());

    // exact, case-sensitive lookups; folder()/file() check the type
    EXPECT_EQ(v->item(QStringLiteral("folder with spaces/File name with spaces.MD")), nullptr);
    EXPECT_EQ(v->file(QStringLiteral("Folder with spaces")), nullptr);
    EXPECT_EQ(v->folder(QStringLiteral("archive.tar.gz")), nullptr);

    // absolute <-> vault paths
    EXPECT_EQ(v->root(), m_root);
    EXPECT_EQ(v->absolutePath(QStringLiteral("a/b.md")), m_root + QStringLiteral("/a/b.md"));
    EXPECT_EQ(v->absolutePath(QString()), m_root);
    EXPECT_EQ(v->vaultPath(m_root + QStringLiteral("/a/b.md")), QStringLiteral("a/b.md"));
    EXPECT_EQ(v->vaultPath(m_root + QStringLiteral("/a/../c.md")), QStringLiteral("c.md"));
    EXPECT_EQ(v->vaultPath(m_root), QString());
    EXPECT_EQ(v->vaultPath(m_root + QStringLiteral("2/x.md")), std::nullopt);
    EXPECT_EQ(v->vaultPath(QStringLiteral("/etc/passwd")), std::nullopt);
}

TEST_F(VaultModelTest, SymlinksAreFollowedWithoutCycles)
{
    put(QStringLiteral("real/note.md"), "hello");
    const QString outside = m_tmp.path() + QStringLiteral("/outside");
    ASSERT_TRUE(vt::writeFile(outside + QStringLiteral("/ext.md"), "external"));
    ASSERT_EQ(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(abs(QStringLiteral("linked"))).constData()), 0);
    ASSERT_EQ(::symlink("note.md", QFile::encodeName(abs(QStringLiteral("real/alias.md"))).constData()), 0);
    ASSERT_EQ(::symlink("..", QFile::encodeName(abs(QStringLiteral("real/loop"))).constData()), 0); // cycle
    ASSERT_EQ(::symlink("../real", QFile::encodeName(abs(QStringLiteral("real/self"))).constData()), 0); // alias
    ASSERT_EQ(::symlink("missing.md", QFile::encodeName(abs(QStringLiteral("broken.md"))).constData()), 0);

    auto v = open();
    EXPECT_EQ(vt::sortedPaths(v->files()),
              QStringList({QStringLiteral("linked/ext.md"), QStringLiteral("real/alias.md"), QStringLiteral("real/note.md")}));
    EXPECT_EQ(vt::sortedPaths(v->folders()), QStringList({QStringLiteral("linked"), QStringLiteral("real")}));
    EXPECT_EQ(v->file(QStringLiteral("real/alias.md"))->size(), 5); // stat of the target
}

TEST_F(VaultModelTest, ScanFixtureVault)
{
    Vault v(QStringLiteral(OC_FIXTURES_DIR "/vault-basic"));
    VaultImplAccess::config(v).watcher = QStringLiteral("none"); // read-only use of the fixture
    v.load();
    EXPECT_NE(v.file(QStringLiteral("Welcome.md")), nullptr);
    EXPECT_NE(v.file(QStringLiteral("Projects/Alpha.md")), nullptr);
    EXPECT_NE(v.file(QStringLiteral("attachments/image.png")), nullptr);
    EXPECT_NE(v.folder(QStringLiteral("Daily")), nullptr);
    EXPECT_EQ(v.fileCount(), v.markdownFiles().size() + 1);
}

// ---------------------------------------------------------------------------------------------
// Operations

TEST_F(VaultModelTest, CreateFileAndFolder)
{
    auto v = open();
    SignalLog log(v.get());
    QString err;
    TFile* f = v->createFile(QStringLiteral("New/Sub dir/Note.md"), "body", &err);
    ASSERT_NE(f, nullptr) << err.toStdString();
    EXPECT_EQ(f->path(), QStringLiteral("New/Sub dir/Note.md"));
    EXPECT_EQ(f->size(), 4);
    EXPECT_EQ(vt::readFile(abs(f->path())), QByteArray("body"));
    ASSERT_EQ(log.evs.size(), 3) << log.dump();
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("New"), false));
    EXPECT_EQ(log.evs[0].path, QStringLiteral("New"));
    EXPECT_EQ(log.evs[1].path, QStringLiteral("New/Sub dir"));
    EXPECT_EQ(log.evs[2].path, QStringLiteral("New/Sub dir/Note.md"));
    EXPECT_EQ(v->file(QStringLiteral("New/Sub dir/Note.md")), f);
    EXPECT_EQ(f->parent(), v->folder(QStringLiteral("New/Sub dir")));

    log.clear();
    TFolder* folder = v->createFolder(QStringLiteral("/A/B/C/"), &err);
    ASSERT_NE(folder, nullptr) << err.toStdString();
    EXPECT_EQ(folder->path(), QStringLiteral("A/B/C"));
    EXPECT_TRUE(QFileInfo(abs(QStringLiteral("A/B/C"))).isDir());
    EXPECT_EQ(log.count(QStringLiteral("created"), false), 3);
    log.clear();
    EXPECT_EQ(v->createFolder(QStringLiteral("A/B")), v->folder(QStringLiteral("A/B"))); // mkpath semantics
    EXPECT_TRUE(log.evs.isEmpty());

    // a folder that exists on disk but not in the model is adopted with its content
    put(QStringLiteral("Outside/x.md"));
    TFolder* adopted = v->createFolder(QStringLiteral("Outside"), &err);
    ASSERT_NE(adopted, nullptr) << err.toStdString();
    EXPECT_NE(v->file(QStringLiteral("Outside/x.md")), nullptr);
}

TEST_F(VaultModelTest, CreateRejectsExistingAndIgnoredNames)
{
    put(QStringLiteral("exists.md"));
    auto v = open();
    put(QStringLiteral("unknown-to-model.md")); // on disk only
    QString err;
    EXPECT_EQ(v->createFile(QStringLiteral("exists.md"), "x", &err), nullptr);
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("exists.md"))), QByteArray("x"));
    EXPECT_EQ(v->createFile(QStringLiteral("unknown-to-model.md"), "y"), nullptr);
    EXPECT_EQ(v->createFile(QStringLiteral(".obsidian/evil.json"), "{}"), nullptr);
    EXPECT_FALSE(exists(QStringLiteral(".obsidian")));
    EXPECT_EQ(v->createFile(QStringLiteral("a.md.tmp.1.a"), "x"), nullptr);
    EXPECT_EQ(v->createFile(QStringLiteral("../escape.md"), "x"), nullptr);
    EXPECT_EQ(v->createFile(QString(), "x"), nullptr);
    EXPECT_EQ(v->createFolder(QStringLiteral("exists.md")), nullptr);
    EXPECT_EQ(v->createFile(QStringLiteral("exists.md/child.md"), "x"), nullptr);
}

TEST_F(VaultModelTest, RenameFileAndMove)
{
    put(QStringLiteral("a.md"), "aaa");
    auto v = open();
    SignalLog log(v.get());
    TFile* f = v->file(QStringLiteral("a.md"));
    ASSERT_NE(f, nullptr);
    QString err;
    ASSERT_TRUE(v->rename(f, QStringLiteral("b.md"), &err)) << err.toStdString();
    EXPECT_EQ(f->path(), QStringLiteral("b.md"));
    EXPECT_EQ(f->basename(), QStringLiteral("b"));
    EXPECT_EQ(v->item(QStringLiteral("a.md")), nullptr);
    EXPECT_EQ(v->file(QStringLiteral("b.md")), f);
    EXPECT_FALSE(exists(QStringLiteral("a.md")));
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("b.md"))), QByteArray("aaa"));
    ASSERT_EQ(log.evs.size(), 1);
    EXPECT_TRUE(log.hasRename(QStringLiteral("a.md"), QStringLiteral("b.md"), false));

    // move into a folder that doesn't exist yet (created on the way), changing the extension
    log.clear();
    ASSERT_TRUE(v->rename(f, QStringLiteral("x/y/c.txt"), &err)) << err.toStdString();
    EXPECT_EQ(f->extension(), QStringLiteral("txt"));
    EXPECT_FALSE(f->isMarkdown());
    EXPECT_EQ(f->parent(), v->folder(QStringLiteral("x/y")));
    EXPECT_EQ(log.count(QStringLiteral("created")), 2);
    EXPECT_TRUE(log.hasRename(QStringLiteral("b.md"), QStringLiteral("x/y/c.txt"), false));
    EXPECT_TRUE(v->rename(f, QStringLiteral("x/y/c.txt"))); // no-op
}

TEST_F(VaultModelTest, RenameFolderRepathsDescendants)
{
    put(QStringLiteral("Proj/one.md"));
    put(QStringLiteral("Proj/sub/two.md"));
    put(QStringLiteral("Proj/sub/deeper/three.md"));
    put(QStringLiteral("Other/x.md"));
    auto v = open();
    SignalLog log(v.get());
    TFolder* proj = v->folder(QStringLiteral("Proj"));
    TFile* three = v->file(QStringLiteral("Proj/sub/deeper/three.md"));
    ASSERT_TRUE(proj && three);
    QString err;
    ASSERT_TRUE(v->rename(proj, QStringLiteral("Other/Projects 2026"), &err)) << err.toStdString();
    EXPECT_EQ(proj->path(), QStringLiteral("Other/Projects 2026"));
    EXPECT_EQ(three->path(), QStringLiteral("Other/Projects 2026/sub/deeper/three.md"));
    EXPECT_EQ(v->file(QStringLiteral("Other/Projects 2026/sub/deeper/three.md")), three);
    EXPECT_EQ(v->folder(QStringLiteral("Other/Projects 2026/sub")), three->parent()->parent());
    for (const char* old : {"Proj", "Proj/one.md", "Proj/sub", "Proj/sub/two.md", "Proj/sub/deeper/three.md"})
        EXPECT_EQ(v->item(QString::fromUtf8(old)), nullptr) << old;
    EXPECT_EQ(childNames(v->folder(QStringLiteral("Other"))), QStringList({QStringLiteral("Projects 2026"), QStringLiteral("x.md")}));
    EXPECT_EQ(childNames(v->rootFolder()), QStringList({QStringLiteral("Other")}));
    // one renamed() per item, folder first, each with its own old path
    ASSERT_EQ(log.count(QStringLiteral("renamed"), false), 6) << log.dump();
    EXPECT_EQ(log.evs.first().path, QStringLiteral("Other/Projects 2026"));
    EXPECT_TRUE(log.hasRename(QStringLiteral("Proj/sub/deeper/three.md"), QStringLiteral("Other/Projects 2026/sub/deeper/three.md"), false));
    EXPECT_TRUE(log.hasRename(QStringLiteral("Proj/sub"), QStringLiteral("Other/Projects 2026/sub"), false));
    EXPECT_TRUE(QFileInfo::exists(abs(QStringLiteral("Other/Projects 2026/sub/deeper/three.md"))));
}

TEST_F(VaultModelTest, RenameFailures)
{
    put(QStringLiteral("a.md"));
    put(QStringLiteral("b.md"));
    put(QStringLiteral("F/inner.md"));
    auto v = open();
    TFile* a = v->file(QStringLiteral("a.md"));
    QString err;
    EXPECT_FALSE(v->rename(a, QStringLiteral("b.md"), &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("b.md"))), QByteArray("x"));
    put(QStringLiteral("disk-only.md"));
    EXPECT_FALSE(v->rename(a, QStringLiteral("disk-only.md")));
    EXPECT_FALSE(v->rename(v->folder(QStringLiteral("F")), QStringLiteral("F/sub/F")));
    EXPECT_FALSE(v->rename(v->rootFolder(), QStringLiteral("x")));
    EXPECT_FALSE(v->rename(a, QStringLiteral(".hidden.md")));
    EXPECT_FALSE(v->rename(nullptr, QStringLiteral("z.md")));
    EXPECT_EQ(a->path(), QStringLiteral("a.md"));
}

TEST_F(VaultModelTest, CaseOnlyRename)
{
    put(QStringLiteral("note.md"));
    auto v = open();
    TFile* f = v->file(QStringLiteral("note.md"));
    QString err;
    ASSERT_TRUE(v->rename(f, QStringLiteral("Note.md"), &err)) << err.toStdString();
    EXPECT_EQ(f->path(), QStringLiteral("Note.md"));
    EXPECT_TRUE(QFileInfo::exists(abs(QStringLiteral("Note.md"))));
}

TEST_F(VaultModelTest, Copy)
{
    put(QStringLiteral("src.md"), "copy me\r\n");
    ASSERT_EQ(::chmod(QFile::encodeName(abs(QStringLiteral("src.md"))).constData(), 0640), 0);
    auto v = open();
    SignalLog log(v.get());
    QString err;
    TFile* c = v->copy(v->file(QStringLiteral("src.md")), QStringLiteral("dir/copy.md"), &err);
    ASSERT_NE(c, nullptr) << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("dir/copy.md"))), QByteArray("copy me\r\n"));
    EXPECT_EQ(QFileInfo(abs(QStringLiteral("dir/copy.md"))).permissions() & 0x7777,
              QFileInfo(abs(QStringLiteral("src.md"))).permissions() & 0x7777);
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("dir/copy.md"), false));
    EXPECT_EQ(v->copy(v->file(QStringLiteral("src.md")), QStringLiteral("dir/copy.md")), nullptr);
}

TEST_F(VaultModelTest, AvailablePath)
{
    put(QStringLiteral("Untitled.md"));
    put(QStringLiteral("Untitled 1.md"));
    put(QStringLiteral("F/Untitled.md"));
    put(QStringLiteral("F/untitled 1.md")); // case-insensitive collision (NTFS/Windows safety)
    ASSERT_TRUE(QDir().mkpath(abs(QStringLiteral("Untitled"))));
    auto v = open();
    EXPECT_EQ(v->availablePath(QString(), QStringLiteral("Untitled"), QStringLiteral("md")), QStringLiteral("Untitled 2.md"));
    EXPECT_EQ(v->availablePath(QStringLiteral("F"), QStringLiteral("Untitled"), QStringLiteral("md")),
              QStringLiteral("F/Untitled 2.md"));
    EXPECT_EQ(v->availablePath(QStringLiteral("/F/"), QStringLiteral("New"), QStringLiteral("canvas")),
              QStringLiteral("F/New.canvas"));
    EXPECT_EQ(v->availablePath(QString(), QStringLiteral("Untitled"), QString()), QStringLiteral("Untitled 1"));
    EXPECT_EQ(v->availablePath(QStringLiteral("Missing"), QStringLiteral("Untitled"), QStringLiteral("md")),
              QStringLiteral("Missing/Untitled.md"));
}

// ---------------------------------------------------------------------------------------------
// Compare-and-swap IO

TEST_F(VaultModelTest, ReadReturnsBytesAndState)
{
    put(QStringLiteral("r.md"), "hello\n");
    auto v = open();
    DiskState st;
    QString err;
    const std::optional<QByteArray> bytes = v->read(QStringLiteral("r.md"), &st, &err);
    ASSERT_TRUE(bytes.has_value()) << err.toStdString();
    EXPECT_EQ(*bytes, QByteArray("hello\n"));
    EXPECT_EQ(st.size, 6);
    EXPECT_EQ(st.hash, contentHash(QByteArrayView("hello\n")));
    EXPECT_EQ(st.mtimeNs / 1'000'000, v->file(QStringLiteral("r.md"))->mtimeMs());
    EXPECT_TRUE(st.sameAs(v->stat(QStringLiteral("r.md"))));

    EXPECT_FALSE(v->read(QStringLiteral("missing.md"), &st, &err).has_value());
    EXPECT_FALSE(st.exists());
    EXPECT_FALSE(v->stat(QStringLiteral("missing.md")).exists());
    EXPECT_FALSE(v->read(QStringLiteral(".obsidian/app.json"), &st, &err).has_value());
}

TEST_F(VaultModelTest, WriteCasOk)
{
    put(QStringLiteral("w.md"), "one");
    auto v = open();
    SignalLog log(v.get());
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("w.md"), &st, nullptr));
    DiskState ns;
    QString err;
    ASSERT_EQ(v->write(QStringLiteral("w.md"), "two!", st, &ns, &err), Vault::WriteResult::Ok) << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("w.md"))), QByteArray("two!"));
    EXPECT_EQ(ns.size, 4);
    EXPECT_EQ(ns.hash, contentHash(QByteArrayView("two!")));
    EXPECT_TRUE(ns.sameAs(v->stat(QStringLiteral("w.md"))));
    EXPECT_EQ(v->file(QStringLiteral("w.md"))->size(), 4);
    ASSERT_EQ(log.evs.size(), 1);
    EXPECT_TRUE(log.has(QStringLiteral("modified"), QStringLiteral("w.md"), false));
    // chained writes with the returned state
    ASSERT_EQ(v->write(QStringLiteral("w.md"), "three", ns, &ns, &err), Vault::WriteResult::Ok);
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("w.md"))), QByteArray("three"));
    // no temp files left behind
    EXPECT_EQ(QDir(m_root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList({QStringLiteral("w.md")}));
}

TEST_F(VaultModelTest, WriteConflictWhenChangedExternally)
{
    put(QStringLiteral("c.md"), "base");
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("c.md"), &st, nullptr));
    QTest::qWait(5);
    put(QStringLiteral("c.md"), "external edit");
    SignalLog log(v.get());
    DiskState ns;
    EXPECT_EQ(v->write(QStringLiteral("c.md"), "mine", st, &ns, nullptr), Vault::WriteResult::Conflict);
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("c.md"))), QByteArray("external edit"));
    // same size, different content: the hash catches it
    put(QStringLiteral("c.md"), "BASE");
    DiskState same = st;
    same.mtimeNs = v->stat(QStringLiteral("c.md")).mtimeNs;
    EXPECT_EQ(v->write(QStringLiteral("c.md"), "mine", same, &ns, nullptr), Vault::WriteResult::Conflict);
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("c.md"))), QByteArray("BASE"));
    // deleted behind our back
    ASSERT_TRUE(QFile::remove(abs(QStringLiteral("c.md"))));
    EXPECT_EQ(v->write(QStringLiteral("c.md"), "mine", st, &ns, nullptr), Vault::WriteResult::Conflict);
    EXPECT_FALSE(exists(QStringLiteral("c.md")));
    EXPECT_TRUE(log.evs.isEmpty()) << log.dump();
}

TEST_F(VaultModelTest, WriteCreateOnly)
{
    put(QStringLiteral("taken.md"), "t");
    auto v = open();
    SignalLog log(v.get());
    DiskState mustNotExist; // size -1
    DiskState ns;
    EXPECT_EQ(v->write(QStringLiteral("taken.md"), "t", mustNotExist, &ns, nullptr), Vault::WriteResult::Conflict);
    QString err;
    ASSERT_EQ(v->write(QStringLiteral("new/fresh.md"), "fresh", mustNotExist, &ns, &err), Vault::WriteResult::Ok)
        << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("new/fresh.md"))), QByteArray("fresh"));
    ASSERT_NE(v->file(QStringLiteral("new/fresh.md")), nullptr);
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("new"), false));
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("new/fresh.md"), false));
    EXPECT_EQ(v->write(QStringLiteral("new/fresh.md"), "again", mustNotExist, &ns, nullptr), Vault::WriteResult::Conflict);
    EXPECT_EQ(v->write(QStringLiteral(".git/config"), "x", mustNotExist, &ns, nullptr), Vault::WriteResult::Error);
    EXPECT_FALSE(exists(QStringLiteral(".git")));
}

TEST_F(VaultModelTest, WriteSameBytesIsNoop)
{
    put(QStringLiteral("same.md"), "identical");
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("same.md"), &st, nullptr));
    struct stat before {};
    ASSERT_EQ(::stat(QFile::encodeName(abs(QStringLiteral("same.md"))).constData(), &before), 0);
    SignalLog log(v.get());
    QTest::qWait(5);
    DiskState ns;
    ASSERT_EQ(v->write(QStringLiteral("same.md"), "identical", st, &ns, nullptr), Vault::WriteResult::Ok);
    struct stat after {};
    ASSERT_EQ(::stat(QFile::encodeName(abs(QStringLiteral("same.md"))).constData(), &after), 0);
    EXPECT_EQ(before.st_ino, after.st_ino); // not rewritten
    EXPECT_EQ(before.st_mtim.tv_nsec, after.st_mtim.tv_nsec);
    EXPECT_TRUE(ns.sameAs(st));
    EXPECT_TRUE(log.evs.isEmpty());
}

TEST_F(VaultModelTest, WriteIsByteExact)
{
    QByteArray all;
    for (int i = 0; i < 256; ++i)
        all.append(char(i));
    static const char kMixed[] = "invalid \xC3\x28 utf-8 \xFF\xFE and NUL \0 inside";
    const QList<QByteArray> samples = {
        QByteArray("\xEF\xBB\xBF# BOM\r\nCRLF line\r\nLF line\nCR only\rno final newline  \t"),
        all,
        QByteArray(kMixed, sizeof(kMixed) - 1),
        QByteArray(),
        QByteArray(3 * 1024 * 1024, 'z') + QByteArray("\n\ttail  "),
    };
    put(QStringLiteral("bytes.md"), "seed");
    auto v = open();
    for (const QByteArray& sample : samples) {
        DiskState st;
        ASSERT_TRUE(v->read(QStringLiteral("bytes.md"), &st, nullptr));
        DiskState ns;
        QString err;
        ASSERT_EQ(v->write(QStringLiteral("bytes.md"), sample, st, &ns, &err), Vault::WriteResult::Ok) << err.toStdString();
        EXPECT_EQ(vt::readFile(abs(QStringLiteral("bytes.md"))), sample);
        DiskState rs;
        const std::optional<QByteArray> back = v->read(QStringLiteral("bytes.md"), &rs, nullptr);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, sample);
        EXPECT_EQ(rs.hash, contentHash(sample));
        EXPECT_EQ(rs.size, sample.size());
        EXPECT_EQ(v->file(QStringLiteral("bytes.md"))->size(), sample.size());
    }
}

TEST_F(VaultModelTest, WritePreservesPermissions)
{
    put(QStringLiteral("perm.md"), "p");
    const QByteArray a = QFile::encodeName(abs(QStringLiteral("perm.md")));
    ASSERT_EQ(::chmod(a.constData(), 0604), 0);
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("perm.md"), &st, nullptr));
    ASSERT_EQ(v->write(QStringLiteral("perm.md"), "changed", st, nullptr, nullptr), Vault::WriteResult::Ok);
    struct stat sb {};
    ASSERT_EQ(::stat(a.constData(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 07777, 0604u);
}

TEST_F(VaultModelTest, WriteAcceptsTouchedButIdenticalFile)
{
    put(QStringLiteral("t.md"), "same content");
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("t.md"), &st, nullptr));
    // "touch": new mtime, same bytes -> the caller's edit is still based on what's on disk
    const QByteArray a = QFile::encodeName(abs(QStringLiteral("t.md")));
    struct timespec times[2] = {{0, UTIME_NOW}, {st.mtimeNs / 1'000'000'000 + 10, 0}};
    ASSERT_EQ(::utimensat(AT_FDCWD, a.constData(), times, 0), 0);
    ASSERT_FALSE(v->stat(QStringLiteral("t.md")).sameAs(st));
    QString err;
    EXPECT_EQ(v->write(QStringLiteral("t.md"), "edited", st, nullptr, &err), Vault::WriteResult::Ok) << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("t.md"))), QByteArray("edited"));
    // without a hash there's no way to tell: conflict
    ASSERT_TRUE(v->read(QStringLiteral("t.md"), &st, nullptr));
    times[1] = {st.mtimeNs / 1'000'000'000 + 20, 0};
    ASSERT_EQ(::utimensat(AT_FDCWD, a.constData(), times, 0), 0);
    DiskState noHash = st;
    noHash.hash.clear();
    EXPECT_EQ(v->write(QStringLiteral("t.md"), "edited twice", noHash, nullptr, nullptr), Vault::WriteResult::Conflict);
}

TEST_F(VaultModelTest, WriteThroughSymlinkKeepsLink)
{
    put(QStringLiteral("target.md"), "target");
    ASSERT_EQ(::symlink("target.md", QFile::encodeName(abs(QStringLiteral("link.md"))).constData()), 0);
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("link.md"), &st, nullptr));
    ASSERT_EQ(v->write(QStringLiteral("link.md"), "via link", st, nullptr, nullptr), Vault::WriteResult::Ok);
    EXPECT_TRUE(QFileInfo(abs(QStringLiteral("link.md"))).isSymLink());
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("target.md"))), QByteArray("via link"));
}

TEST_F(VaultModelTest, HardLinkedFileIsWrittenInPlace)
{
    put(QStringLiteral("h.md"), "hard");
    ASSERT_EQ(::link(QFile::encodeName(abs(QStringLiteral("h.md"))).constData(),
                     QFile::encodeName(m_tmp.path() + QStringLiteral("/other-link.md")).constData()),
              0);
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("h.md"), &st, nullptr));
    ASSERT_EQ(v->write(QStringLiteral("h.md"), "both links see this", st, nullptr, nullptr), Vault::WriteResult::Ok);
    EXPECT_EQ(vt::readFile(m_tmp.path() + QStringLiteral("/other-link.md")), QByteArray("both links see this"));
}

// ---------------------------------------------------------------------------------------------
// Trash

TEST_F(VaultModelTest, TrashPermanent)
{
    put(QStringLiteral("gone.md"));
    put(QStringLiteral("Dir/a.md"));
    put(QStringLiteral("Dir/Sub/b.md"));
    put(QStringLiteral("Dir/.hidden")); // ignored content goes with the folder
    const QString outside = m_tmp.path() + QStringLiteral("/keep");
    ASSERT_TRUE(vt::writeFile(outside + QStringLiteral("/precious.md"), "keep me"));
    ASSERT_EQ(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(abs(QStringLiteral("Dir/link"))).constData()), 0);
    auto v = open();
    ASSERT_NE(v->file(QStringLiteral("Dir/link/precious.md")), nullptr);
    SignalLog log(v.get());
    QString err;
    ASSERT_TRUE(v->trash(v->file(QStringLiteral("gone.md")), Vault::TrashMode::Permanent, &err)) << err.toStdString();
    EXPECT_FALSE(exists(QStringLiteral("gone.md")));
    EXPECT_EQ(v->item(QStringLiteral("gone.md")), nullptr);
    EXPECT_TRUE(log.has(QStringLiteral("deleted"), QStringLiteral("gone.md"), false));

    log.clear();
    ASSERT_TRUE(v->trash(v->folder(QStringLiteral("Dir")), Vault::TrashMode::Permanent, &err)) << err.toStdString();
    EXPECT_FALSE(exists(QStringLiteral("Dir")));
    EXPECT_EQ(vt::readFile(outside + QStringLiteral("/precious.md")), QByteArray("keep me")); // symlink not followed
    EXPECT_EQ(v->fileCount(), 0);
    EXPECT_TRUE(v->folders().isEmpty());
    // every descendant, deepest first, the folder last
    ASSERT_EQ(log.count(QStringLiteral("deleted"), false), 6) << log.dump();
    EXPECT_EQ(log.evs.last().path, QStringLiteral("Dir"));
    auto indexOf = [&](const char* p) {
        for (int i = 0; i < log.evs.size(); ++i)
            if (log.evs[i].path == QString::fromUtf8(p))
                return i;
        return -1;
    };
    EXPECT_LT(indexOf("Dir/Sub/b.md"), indexOf("Dir/Sub"));
    EXPECT_LT(indexOf("Dir/link/precious.md"), indexOf("Dir/link"));
}

TEST_F(VaultModelTest, TrashLocalKeepsNameAndNumbersCollisions)
{
    put(QStringLiteral("Note.md"), "first");
    put(QStringLiteral("Sub/Note.md"), "second");
    put(QStringLiteral("Folder/x.md"));
    auto v = open();
    SignalLog log(v.get());
    QString err;
    ASSERT_TRUE(v->trash(v->file(QStringLiteral("Note.md")), Vault::TrashMode::Local, &err)) << err.toStdString();
    ASSERT_TRUE(v->trash(v->file(QStringLiteral("Sub/Note.md")), Vault::TrashMode::Local, &err)) << err.toStdString();
    ASSERT_TRUE(v->trash(v->folder(QStringLiteral("Folder")), Vault::TrashMode::Local, &err)) << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral(".trash/Note.md"))), QByteArray("first"));
    EXPECT_EQ(vt::readFile(abs(QStringLiteral(".trash/Note 1.md"))), QByteArray("second"));
    EXPECT_TRUE(QFileInfo::exists(abs(QStringLiteral(".trash/Folder/x.md"))));
    EXPECT_EQ(v->item(QStringLiteral(".trash")), nullptr);
    EXPECT_EQ(v->fileCount(), 0);
    EXPECT_EQ(log.count(QStringLiteral("deleted"), false), 4);
}

TEST_F(VaultModelTest, TrashSystem)
{
    if (qEnvironmentVariable("OC_TEST_SYSTEM_TRASH") != u"1")
        GTEST_SKIP() << "set OC_TEST_SYSTEM_TRASH=1 to exercise KIO::trash";
    // Keep even this opt-in run away from the user's trash: the home trash lives under XDG_DATA_HOME
    // (same filesystem as the temp dir), which we point into the temp dir.
    // (QFile::moveToTrash, the fallback, needs XDG_DATA_HOME to exist.)
    ASSERT_TRUE(QDir().mkpath(m_tmp.path() + QStringLiteral("/xdg-data")));
    qputenv("XDG_DATA_HOME", QFile::encodeName(m_tmp.path() + QStringLiteral("/xdg-data")));
    put(QStringLiteral("to-trash.md"), "bye");
    put(QStringLiteral("TrashDir/inner.md"));
    auto v = open();
    QString err;
    ASSERT_TRUE(v->trash(v->file(QStringLiteral("to-trash.md")), Vault::TrashMode::System, &err)) << err.toStdString();
    ASSERT_TRUE(v->trash(v->folder(QStringLiteral("TrashDir")), Vault::TrashMode::System, &err)) << err.toStdString();
    EXPECT_FALSE(exists(QStringLiteral("to-trash.md")));
    EXPECT_FALSE(exists(QStringLiteral("TrashDir")));
    EXPECT_EQ(v->fileCount(), 0);
    const QString trashFiles = m_tmp.path() + QStringLiteral("/xdg-data/Trash/files");
    EXPECT_TRUE(QFileInfo::exists(trashFiles + QStringLiteral("/to-trash.md"))) << trashFiles.toStdString();
}

// ---------------------------------------------------------------------------------------------
// Offline + lifecycle (no watcher: offline is detected by rescan())

TEST(VaultSelfWrites, RegistryMatchesBySizeAndMtimeOrHash)
{
    vaultfs::SelfWriteRegistry reg(10'000);
    const QByteArray h = contentHash(QByteArrayView("abc"));
    reg.recordWrite(QStringLiteral("a.md"), 3, 111, h, {1, 2});
    int hashed = 0;
    auto hashAbc = [&] {
        ++hashed;
        return h;
    };
    auto hashOther = [&] {
        ++hashed;
        return contentHash(QByteArrayView("xyz"));
    };
    EXPECT_TRUE(reg.isOwnWrite(QStringLiteral("a.md"), 3, 111, hashOther)); // exact stat: no hashing
    EXPECT_EQ(hashed, 0);
    EXPECT_TRUE(reg.isOwnWrite(QStringLiteral("a.md"), 3, 222, hashAbc)); // stat drifted, same bytes
    EXPECT_FALSE(reg.isOwnWrite(QStringLiteral("a.md"), 3, 222, hashOther));
    EXPECT_FALSE(reg.isOwnWrite(QStringLiteral("a.md"), 4, 111, hashAbc)); // other size
    EXPECT_FALSE(reg.isOwnWrite(QStringLiteral("b.md"), 3, 111, hashAbc));
    reg.recordRename(QStringLiteral("a.md"), QStringLiteral("c.md"), {1, 2});
    EXPECT_FALSE(reg.isOwnWrite(QStringLiteral("a.md"), 3, 111, hashAbc)); // not a write any more
    ASSERT_NE(reg.find(QStringLiteral("c.md")), nullptr);
    EXPECT_EQ(reg.find(QStringLiteral("c.md"))->kind, vaultfs::SelfWriteRegistry::Kind::RenamedHere);

    vaultfs::SelfWriteRegistry shortLived(1);
    shortLived.recordRemoved(QStringLiteral("gone.md"), {1, 3});
    QTest::qWait(5);
    EXPECT_EQ(shortLived.find(QStringLiteral("gone.md")), nullptr);
    shortLived.expire();
    EXPECT_EQ(shortLived.count(), 0);
}

TEST_F(VaultModelTest, WriteFallsBackToInPlaceWhenFolderIsReadOnly)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "root ignores directory permissions";
    put(QStringLiteral("ro/note.md"), "before");
    auto v = open();
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("ro/note.md"), &st, nullptr));
    const QByteArray dir = QFile::encodeName(abs(QStringLiteral("ro")));
    ASSERT_EQ(::chmod(dir.constData(), 0555), 0); // no temp file can be created next to the note
    QString err;
    const Vault::WriteResult r = v->write(QStringLiteral("ro/note.md"), "after, in place", st, &st, &err);
    ASSERT_EQ(::chmod(dir.constData(), 0755), 0);
    ASSERT_EQ(r, Vault::WriteResult::Ok) << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("ro/note.md"))), QByteArray("after, in place"));
    EXPECT_EQ(QDir(abs(QStringLiteral("ro"))).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
              QStringList({QStringLiteral("note.md")}));
    EXPECT_TRUE(st.sameAs(v->stat(QStringLiteral("ro/note.md"))));
}

TEST_F(VaultModelTest, WriteIntoFolderDeletedBehindOurBack)
{
    put(QStringLiteral("Dir/Sub/a.md"));
    auto v = open(); // no watcher: the model still has Dir/Sub after it's gone from disk
    ASSERT_TRUE(QDir(abs(QStringLiteral("Dir"))).removeRecursively());
    DiskState mustNotExist;
    QString err;
    ASSERT_EQ(v->write(QStringLiteral("Dir/Sub/new.md"), "n", mustNotExist, nullptr, &err), Vault::WriteResult::Ok)
        << err.toStdString();
    EXPECT_EQ(vt::readFile(abs(QStringLiteral("Dir/Sub/new.md"))), QByteArray("n"));
    ASSERT_TRUE(QDir(abs(QStringLiteral("Dir"))).removeRecursively());
    EXPECT_NE(v->createFolder(QStringLiteral("Dir/Sub"), &err), nullptr) << err.toStdString();
    EXPECT_TRUE(QFileInfo(abs(QStringLiteral("Dir/Sub"))).isDir());
}

TEST_F(VaultModelTest, RescanWithoutWatcherFindsChanges)
{
    put(QStringLiteral("keep.md"), "k");
    put(QStringLiteral("old name.md"), "renamed later");
    put(QStringLiteral("gone.md"), "g");
    auto v = open(); // watcher "none"
    SignalLog log(v.get());
    TFile* renamed = v->file(QStringLiteral("old name.md"));
    put(QStringLiteral("Sub/new.md"), "n");
    put(QStringLiteral("keep.md"), "k changed");
    ASSERT_TRUE(QFile::remove(abs(QStringLiteral("gone.md"))));
    ASSERT_TRUE(QFile::rename(abs(QStringLiteral("old name.md")), abs(QStringLiteral("Sub/new name.md"))));
    v->rescan(); // synchronous without a watcher
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("Sub"), true)) << log.dump();
    EXPECT_TRUE(log.has(QStringLiteral("created"), QStringLiteral("Sub/new.md"), true)) << log.dump();
    EXPECT_TRUE(log.has(QStringLiteral("modified"), QStringLiteral("keep.md"), true)) << log.dump();
    EXPECT_TRUE(log.has(QStringLiteral("deleted"), QStringLiteral("gone.md"), true)) << log.dump();
    EXPECT_TRUE(log.hasRename(QStringLiteral("old name.md"), QStringLiteral("Sub/new name.md"), true)) << log.dump();
    EXPECT_EQ(v->file(QStringLiteral("Sub/new name.md")), renamed);
    EXPECT_EQ(log.evs.size(), 5) << log.dump();
    log.clear();
    v->rescan();
    EXPECT_TRUE(log.evs.isEmpty()) << log.dump();
}

TEST_F(VaultModelTest, VaultPathAcceptsCanonicalRoot)
{
    const QString link = m_tmp.path() + QStringLiteral("/link to vault");
    ASSERT_EQ(::symlink(QFile::encodeName(m_root).constData(), QFile::encodeName(link).constData()), 0);
    Vault v(link);
    EXPECT_EQ(v.root(), link);
    EXPECT_EQ(v.vaultPath(link + QStringLiteral("/a.md")), QStringLiteral("a.md"));
    EXPECT_EQ(v.vaultPath(QFileInfo(m_root).canonicalFilePath() + QStringLiteral("/b/c.md")), QStringLiteral("b/c.md"));
}

TEST_F(VaultModelTest, MissingRootLoadsOffline)
{
    Vault v(m_tmp.path() + QStringLiteral("/does-not-exist"));
    VaultImplAccess::config(v).watcher = QStringLiteral("none");
    SignalLog log(&v);
    v.load();
    EXPECT_TRUE(v.isLoaded());
    EXPECT_FALSE(v.isOnline());
    EXPECT_EQ(v.fileCount(), 0);
    EXPECT_EQ(log.online, QList<bool>({false}));
    EXPECT_EQ(v.createFile(QStringLiteral("x.md"), "x"), nullptr);
}

TEST_F(VaultModelTest, OfflineKeepsModelAndRejectsWrites)
{
    put(QStringLiteral("keep.md"), "k");
    auto v = open();
    VaultImplAccess::config(*v).offlinePollMs = 50;
    DiskState st;
    ASSERT_TRUE(v->read(QStringLiteral("keep.md"), &st, nullptr));
    SignalLog log(v.get());
    const QString moved = m_tmp.path() + QStringLiteral("/moved away");
    ASSERT_TRUE(QDir().rename(m_root, moved));
    v->rescan();
    EXPECT_FALSE(v->isOnline());
    EXPECT_EQ(log.online, QList<bool>({false}));
    EXPECT_EQ(log.count(QStringLiteral("deleted")), 0);
    ASSERT_NE(v->file(QStringLiteral("keep.md")), nullptr);
    EXPECT_TRUE(v->stat(QStringLiteral("keep.md")).sameAs(st)); // last known state, not "deleted"
    QString err;
    EXPECT_EQ(v->write(QStringLiteral("keep.md"), "new", st, nullptr, &err), Vault::WriteResult::Error);
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(v->rename(v->file(QStringLiteral("keep.md")), QStringLiteral("x.md")));

    ASSERT_TRUE(QDir().rename(moved, m_root));
    ASSERT_TRUE(vt::waitFor([&] { return v->isOnline(); }, 5000));
    EXPECT_EQ(log.online, QList<bool>({false, true}));
    EXPECT_EQ(v->write(QStringLiteral("keep.md"), "new", st, nullptr, &err), Vault::WriteResult::Ok) << err.toStdString();
}

TEST_F(VaultModelTest, DestroyWhileWatcherIsBusy)
{
    for (int i = 0; i < 40; ++i)
        put(QStringLiteral("d%1/f.md").arg(i));
    for (const char* backend : {"inotify", "poll", "hybrid"}) {
        for (int round = 0; round < 3; ++round) {
            auto v = std::make_unique<Vault>(m_root);
            VaultImplAccess::config(*v).watcher = QString::fromUtf8(backend);
            VaultImplAccess::config(*v).pollIntervalMs = 1;
            VaultImplAccess::config(*v).hybridSweepMs = 1;
            v->load();
            for (int i = 0; i < 20; ++i)
                put(QStringLiteral("d%1/g%2.md").arg(i).arg(round));
            v->rescan();
            QTest::qWait(round * 7);
            v.reset(); // must join the watcher thread cleanly
        }
    }
    SUCCEED();
}
