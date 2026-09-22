#include "core/documents/document_manager.h"
#include "support/dir_storage.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

namespace {
Transaction insertAt(const Document& doc, qsizetype pos, const QString& s)
{
    Transaction tr;
    tr.changes = ChangeSet::of({{pos, pos, s}}, doc.text().length());
    tr.selection = Selection::cursor(pos + s.size());
    tr.userEvent = QStringLiteral("input.type");
    return tr;
}
void processDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

class DocumentManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        storage = std::make_unique<test::DirStorage>(dir.path());
    }
    void put(const QString& name, const QByteArray& bytes)
    {
        QDir().mkpath(QFileInfo(dir.path() + "/" + name).absolutePath());
        QFile f(dir.path() + "/" + name);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    QTemporaryDir dir;
    std::unique_ptr<test::DirStorage> storage;
};

TEST_F(DocumentManagerTest, SharesOneDocumentPerPath)
{
    put("a.md", "alpha");
    DocumentManager dm(storage.get());
    QSignalSpy opened(&dm, &DocumentManager::documentOpened);
    Document* d1 = dm.acquire("a.md");
    Document* d2 = dm.acquire("a.md");
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(d1, d2);
    EXPECT_EQ(opened.count(), 1);
    EXPECT_EQ(dm.refCount(d1), 2);
    EXPECT_EQ(storage->reads, 1);
    EXPECT_EQ(dm.find("a.md"), d1);
    EXPECT_EQ(dm.documents().size(), 1);
    dm.release(d1);
    EXPECT_EQ(dm.refCount(d1), 1);
    dm.release(d2);
    EXPECT_EQ(dm.refCount(d1), 0);
    EXPECT_TRUE(dm.documents().isEmpty());
}

TEST_F(DocumentManagerTest, MissingFileFails)
{
    DocumentManager dm(storage.get());
    QString err;
    EXPECT_EQ(dm.acquire("nope.md", &err), nullptr);
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(dm.find("nope.md"), nullptr);
}

TEST_F(DocumentManagerTest, ReleaseSavesDirtyDocument)
{
    put("a.md", "alpha");
    DocumentManager dm(storage.get());
    Document* d = dm.acquire("a.md");
    d->dispatch(insertAt(*d, 5, "!"), nullptr, Selection::cursor(5));
    EXPECT_TRUE(d->isDirty());
    dm.release(d);
    EXPECT_EQ(storage->bytes("a.md"), QByteArray("alpha!"));
    EXPECT_EQ(storage->writes, 1);
}

TEST_F(DocumentManagerTest, CacheKeepsRecentDocumentsButNotHistory)
{
    put("a.md", "alpha");
    DocumentManager dm(storage.get());
    Document* d = dm.acquire("a.md");
    d->dispatch(insertAt(*d, 5, "!"), nullptr, Selection::cursor(5));
    dm.release(d);
    Document* again = dm.acquire("a.md");
    EXPECT_EQ(again, d);              // served from the cache
    EXPECT_EQ(storage->reads, 1);     // no re-read
    EXPECT_FALSE(again->history().canUndo()); // closing a file drops its undo history
    dm.release(again);
}

TEST_F(DocumentManagerTest, CacheIsBounded)
{
    for (int i = 0; i < 6; ++i)
        put(QString("n%1.md").arg(i), "x");
    DocumentManager dm(storage.get());
    dm.setCacheCapacity(2);
    QList<QPointer<Document>> docs;
    for (int i = 0; i < 6; ++i) {
        Document* d = dm.acquire(QString("n%1.md").arg(i));
        docs.append(d);
        dm.release(d);
    }
    processDeferredDeletes();
    int alive = 0;
    for (const auto& p : docs)
        alive += p ? 1 : 0;
    EXPECT_EQ(alive, 2);
    EXPECT_TRUE(docs[4] && docs[5]); // the most recently used survive
}

TEST_F(DocumentManagerTest, ExternalModificationReloadsOpenDocument)
{
    put("a.md", "one\ntwo\n");
    DocumentManager dm(storage.get());
    Document* d = dm.acquire("a.md");
    QSignalSpy ext(d, &Document::externallyChanged);
    storage->externalWrite("a.md", "one\ntwo\nthree\n");
    dm.fileModified("a.md");
    EXPECT_EQ(ext.count(), 1);
    EXPECT_EQ(d->text().toString(), "one\ntwo\nthree\n");
    EXPECT_FALSE(d->isDirty());
    dm.release(d);
}

TEST_F(DocumentManagerTest, ExternalModificationDropsCachedDocument)
{
    put("a.md", "one");
    DocumentManager dm(storage.get());
    QPointer<Document> d = dm.acquire("a.md");
    dm.release(d);
    storage->externalWrite("a.md", "two");
    dm.fileModified("a.md");
    processDeferredDeletes();
    EXPECT_TRUE(d.isNull());
    Document* fresh = dm.acquire("a.md");
    EXPECT_EQ(fresh->text().toString(), "two");
    dm.release(fresh);
}

TEST_F(DocumentManagerTest, RenameFollowsFilesAndFolders)
{
    put("f/a.md", "a");
    put("f/sub/b.md", "b");
    put("fa.md", "not under f/");
    DocumentManager dm(storage.get());
    Document* a = dm.acquire("f/a.md");
    Document* b = dm.acquire("f/sub/b.md");
    Document* c = dm.acquire("fa.md");
    QSignalSpy renamed(&dm, &DocumentManager::documentRenamed);
    ASSERT_TRUE(QDir(dir.path()).rename("f", "g"));
    dm.fileRenamed("f", "g");
    EXPECT_EQ(a->path(), "g/a.md");
    EXPECT_EQ(b->path(), "g/sub/b.md");
    EXPECT_EQ(c->path(), "fa.md");
    EXPECT_EQ(renamed.count(), 2);
    EXPECT_EQ(dm.find("g/a.md"), a);
    EXPECT_EQ(dm.find("f/a.md"), nullptr);
    // Edits after the move save to the new place (the disk state is unchanged by a rename).
    a->dispatch(insertAt(*a, 1, "!"), nullptr, Selection::cursor(1));
    ASSERT_TRUE(a->save());
    EXPECT_EQ(storage->bytes("g/a.md"), QByteArray("a!"));
    // A child event for an already-moved file is a no-op.
    dm.fileRenamed("f/a.md", "g/a.md");
    EXPECT_EQ(a->path(), "g/a.md");
    dm.release(a);
    dm.release(b);
    dm.release(c);
}

TEST_F(DocumentManagerTest, DeleteMarksOpenDocumentsAndNeverRecreates)
{
    put("a.md", "alpha");
    DocumentManager dm(storage.get());
    Document* d = dm.acquire("a.md");
    QSignalSpy deleted(&dm, &DocumentManager::documentDeleted);
    d->dispatch(insertAt(*d, 5, "!"), nullptr, Selection::cursor(5)); // pending autosave
    ASSERT_TRUE(QFile::remove(dir.path() + "/a.md"));
    dm.fileDeleted("a.md");
    EXPECT_EQ(deleted.count(), 1);
    EXPECT_TRUE(dm.isDeleted(d));
    EXPECT_TRUE(d->isReadOnly());
    EXPECT_EQ(dm.find("a.md"), nullptr);
    EXPECT_FALSE(d->save());
    QPointer<Document> p = d;
    dm.release(d);
    processDeferredDeletes();
    EXPECT_TRUE(p.isNull());
    EXPECT_FALSE(QFile::exists(dir.path() + "/a.md"));
    // A new file at the same path gets a fresh document.
    put("a.md", "new");
    Document* fresh = dm.acquire("a.md");
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->text().toString(), "new");
    EXPECT_FALSE(fresh->isReadOnly());
    dm.release(fresh);
}

TEST_F(DocumentManagerTest, FolderDeleteClosesDescendants)
{
    put("f/a.md", "a");
    put("f/b.md", "b");
    DocumentManager dm(storage.get());
    Document* a = dm.acquire("f/a.md");
    QPointer<Document> b = dm.acquire("f/b.md");
    dm.release(b); // cached only
    QSignalSpy deleted(&dm, &DocumentManager::documentDeleted);
    dm.fileDeleted("f");
    processDeferredDeletes();
    EXPECT_EQ(deleted.count(), 1); // only the acquired one is announced
    EXPECT_TRUE(b.isNull());
    EXPECT_TRUE(dm.isDeleted(a));
    dm.release(a);
}

TEST_F(DocumentManagerTest, OfflineMakesDocumentsReadOnlyAndSavesOnReturn)
{
    put("a.md", "alpha");
    DocumentManager dm(storage.get());
    Document* d = dm.acquire("a.md");
    d->dispatch(insertAt(*d, 5, "!"), nullptr, Selection::cursor(5));
    dm.setOnline(false);
    EXPECT_TRUE(d->isReadOnly());
    EXPECT_FALSE(d->save());
    EXPECT_TRUE(d->isDirty());
    Document* other = nullptr;
    put("b.md", "b");
    other = dm.acquire("b.md");
    EXPECT_TRUE(other->isReadOnly()); // opened while offline
    dm.setOnline(true);
    EXPECT_FALSE(d->isReadOnly());
    EXPECT_FALSE(other->isReadOnly());
    EXPECT_TRUE(d->isDirty());
    EXPECT_TRUE(dm.saveAll());
    EXPECT_EQ(storage->bytes("a.md"), QByteArray("alpha!"));
    dm.release(d);
    dm.release(other);
}

TEST_F(DocumentManagerTest, SaveAllAndDestructorFlush)
{
    put("a.md", "alpha");
    {
        DocumentManager dm(storage.get());
        Document* d = dm.acquire("a.md");
        d->dispatch(insertAt(*d, 0, ">"), nullptr, Selection::cursor(0));
        // no release: quitting with the tab open still saves
    }
    EXPECT_EQ(storage->bytes("a.md"), QByteArray(">alpha"));
}
