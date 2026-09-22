#include "core/documents/document.h"
#include "support/dir_storage.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <fcntl.h>

using namespace oc;

namespace {
Transaction insertAt(const Document& doc, qsizetype pos, const QString& s, const QString& ev = QStringLiteral("input.type"))
{
    Transaction tr;
    tr.changes = ChangeSet::of({{pos, pos, s}}, doc.text().length());
    tr.selection = Selection::cursor(pos + s.size());
    tr.userEvent = ev;
    return tr;
}
} // namespace

class DocumentTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        storage = std::make_unique<test::DirStorage>(dir.path());
    }
    void put(const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.path() + "/" + name);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    QTemporaryDir dir;
    std::unique_ptr<test::DirStorage> storage;
};

TEST_F(DocumentTest, LoadEditSaveIsByteExact)
{
    const QByteArray original("line one\r\nline two  \r\n\tthree\r\nno final newline");
    put("a.md", original);
    Document doc(storage.get(), "a.md");
    ASSERT_TRUE(doc.load());
    EXPECT_EQ(doc.text().lineCount(), 4);
    EXPECT_EQ(doc.newlineEol(), Eol::CRLF);
    EXPECT_FALSE(doc.isDirty());
    // save without edits writes nothing
    EXPECT_TRUE(doc.save());
    EXPECT_EQ(storage->writes, 0);

    doc.dispatch(insertAt(doc, doc.text().lineEnd(0), "!"), nullptr, Selection::cursor(0));
    EXPECT_TRUE(doc.isDirty());
    ASSERT_TRUE(doc.save());
    EXPECT_EQ(storage->writes, 1);
    EXPECT_EQ(storage->bytes("a.md"), QByteArray("line one!\r\nline two  \r\n\tthree\r\nno final newline"));
    EXPECT_FALSE(doc.isDirty());

    // new line breaks use the dominant EOL
    doc.dispatch(insertAt(doc, doc.text().lineEnd(1), "\nNEW"), nullptr, Selection::cursor(0));
    ASSERT_TRUE(doc.save());
    EXPECT_EQ(storage->bytes("a.md"), QByteArray("line one!\r\nline two  \r\nNEW\r\n\tthree\r\nno final newline"));
}

TEST_F(DocumentTest, UndoBackToSavedStateWritesNothing)
{
    put("b.md", "hello");
    Document doc(storage.get(), "b.md");
    ASSERT_TRUE(doc.load());
    doc.dispatch(insertAt(doc, 5, " world"), nullptr, Selection::cursor(5));
    ASSERT_TRUE(doc.undo(nullptr, Selection::cursor(11)).has_value());
    EXPECT_EQ(doc.text().toString(), "hello");
    EXPECT_TRUE(doc.isDirty()); // version moved
    ASSERT_TRUE(doc.save());
    EXPECT_EQ(storage->writes, 0); // bytes equal the disk: no write
    EXPECT_FALSE(doc.isDirty());
    ASSERT_TRUE(doc.redo(nullptr, Selection::cursor(5)).has_value());
    EXPECT_EQ(doc.text().toString(), "hello world");
}

TEST_F(DocumentTest, ExternalChangeWhenCleanIsDiffedIn)
{
    put("c.md", "alpha\nbeta\ngamma\n");
    Document doc(storage.get(), "c.md");
    ASSERT_TRUE(doc.load());
    QSignalSpy changed(&doc, &Document::changed);
    storage->externalWrite("c.md", "alpha\nBETA\ngamma\ndelta\r\n");
    ASSERT_TRUE(doc.reloadFromDisk());
    EXPECT_EQ(doc.text().toString(), "alpha\nBETA\ngamma\ndelta\n");
    EXPECT_FALSE(doc.isDirty());
    EXPECT_EQ(changed.count(), 1);
    // exact EOLs adopted from disk; saving writes nothing
    EXPECT_TRUE(doc.save());
    EXPECT_EQ(storage->writes, 0);
    EXPECT_EQ(doc.text().lineEol(3), Eol::CRLF);
}

TEST_F(DocumentTest, ExternalChangeWhenDirtyIsMerged)
{
    put("d.md", "one\ntwo\nthree\nfour\n");
    Document doc(storage.get(), "d.md");
    ASSERT_TRUE(doc.load());
    doc.dispatch(insertAt(doc, 0, "ONE "), nullptr, Selection::cursor(0)); // local edit on line 0
    storage->externalWrite("d.md", "one\ntwo\nthree\nFOUR\n");            // external edit on line 3
    QSignalSpy ext(&doc, &Document::externallyChanged);
    ASSERT_TRUE(doc.reloadFromDisk());
    ASSERT_EQ(ext.count(), 1);
    EXPECT_FALSE(ext.first().first().toBool());
    EXPECT_EQ(doc.text().toString(), "ONE one\ntwo\nthree\nFOUR\n");
    ASSERT_TRUE(doc.save());
    EXPECT_EQ(storage->bytes("d.md"), QByteArray("ONE one\ntwo\nthree\nFOUR\n"));
    // undo still works for the local edit after the merge
    ASSERT_TRUE(doc.undo(nullptr, Selection::cursor(0)).has_value());
    EXPECT_EQ(doc.text().toString(), "one\ntwo\nthree\nFOUR\n");
}

TEST_F(DocumentTest, OverlappingExternalChangeIsAConflictKeepingOurs)
{
    put("e.md", "same line\n");
    Document doc(storage.get(), "e.md");
    ASSERT_TRUE(doc.load());
    doc.dispatch(insertAt(doc, 0, "mine "), nullptr, Selection::cursor(0));
    storage->externalWrite("e.md", "theirs line\n");
    QSignalSpy ext(&doc, &Document::externallyChanged);
    ASSERT_TRUE(doc.reloadFromDisk());
    ASSERT_EQ(ext.count(), 1);
    EXPECT_TRUE(ext.first().first().toBool());
    EXPECT_EQ(doc.text().toString(), "mine same line\n");
    EXPECT_EQ(doc.conflictText().toString(), "theirs line\n");
}

TEST_F(DocumentTest, SaveDetectsConcurrentModificationAndMerges)
{
    put("f.md", "a\nb\nc\n");
    Document doc(storage.get(), "f.md");
    ASSERT_TRUE(doc.load());
    doc.dispatch(insertAt(doc, 0, "A"), nullptr, Selection::cursor(0));
    storage->externalWrite("f.md", "a\nb\nC\n"); // changed behind our back, no reload yet
    ASSERT_TRUE(doc.save());                     // CAS conflict -> merge -> write
    EXPECT_EQ(storage->bytes("f.md"), QByteArray("Aa\nb\nC\n"));
}

TEST_F(DocumentTest, InvalidUtf8RoundTripsThroughEdits)
{
    const QByteArray original("bad \xFF byte\nkeep \xC3\x28 this\n");
    put("g.md", original);
    Document doc(storage.get(), "g.md");
    ASSERT_TRUE(doc.load());
    EXPECT_TRUE(doc.hadInvalidUtf8());
    doc.dispatch(insertAt(doc, doc.text().length(), "tail"), nullptr, Selection::cursor(0));
    ASSERT_TRUE(doc.save());
    EXPECT_EQ(storage->bytes("g.md"), original + "tail");
}

TEST_F(DocumentTest, AutosaveIsDebounced)
{
    put("h.md", "x");
    Document doc(storage.get(), "h.md");
    ASSERT_TRUE(doc.load());
    doc.setAutosaveDelayMs(30);
    for (int i = 0; i < 5; ++i)
        doc.dispatch(insertAt(doc, doc.text().length(), "y"), nullptr, Selection::cursor(0));
    EXPECT_EQ(storage->writes, 0);
    QElapsedTimer t;
    t.start();
    while (doc.isDirty() && t.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_FALSE(doc.isDirty());
    EXPECT_EQ(storage->writes, 1);
    EXPECT_EQ(storage->bytes("h.md"), QByteArray("xyyyyy"));
}
