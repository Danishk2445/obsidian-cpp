// File recovery modal ("Open local history"): snapshot list, text/diff view, restore, and the diff rows.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "core/recovery/file_recovery.h"
#include "core/recovery/snapshot_store.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/plugins/file_recovery_ui.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/widgets/search_input.h"

#include <QApplication>
#include <QClipboard>

using namespace oc;
using namespace oc::test;

namespace {

QString rowsText(const QList<RecoveryDiffRow>& rows)
{
    QStringList out;
    for (const RecoveryDiffRow& r : rows) {
        switch (r.kind) {
        case RecoveryDiffRow::Equal:
            out << "  " + r.text;
            break;
        case RecoveryDiffRow::Left:
            out << "- " + r.text;
            break;
        case RecoveryDiffRow::Right:
            out << "+ " + r.text;
            break;
        case RecoveryDiffRow::Folded:
            out << QString("[%1 folded]").arg(r.folded.size());
            break;
        }
    }
    return out.join('\n');
}

} // namespace

TEST(RecoveryDiff, ChangedLinePairsGetWordHighlights)
{
    const auto rows = recoveryDiff("one\ntwo words here\nthree\n", "one\ntwo WORDS here\nthree\n");
    EXPECT_EQ(rowsText(rows), "  one\n- two words here\n+ two WORDS here\n  three");
    ASSERT_EQ(rows[1].changed.size(), 1);
    EXPECT_EQ(rows[1].changed[0], qMakePair(4, 9));
    EXPECT_EQ(rows[2].changed[0], qMakePair(4, 9));
}

TEST(RecoveryDiff, InsertionsPairWithAnEmptyRemovedLine)
{
    // Obsidian pairs each added line with a (blank) removed line.
    const auto rows = recoveryDiff("a\nb\n", "a\nnew\nb\n");
    EXPECT_EQ(rowsText(rows), "  a\n- \n+ new\n  b");
    EXPECT_TRUE(rows[1].changed.isEmpty());
}

TEST(RecoveryDiff, DissimilarLinesAreShownWhole)
{
    const auto rows = recoveryDiff("x\nabcdef\ny\n", "x\nqrstuv\ny\n");
    EXPECT_EQ(rowsText(rows), "  x\n- abcdef\n+ qrstuv\n  y");
    EXPECT_TRUE(rows[1].changed.isEmpty());
    EXPECT_TRUE(rows[2].changed.isEmpty());
}

TEST(RecoveryDiff, LongEqualRunsFold)
{
    QStringList before, after;
    for (int i = 0; i < 30; ++i)
        before << QString("line %1").arg(i);
    after = before;
    after[15] = "changed";
    const auto rows = recoveryDiff(before.join('\n'), after.join('\n'));
    // start: 15 equal lines -> fold 12, keep 3; end: 14 equal lines -> keep 3, fold 11
    EXPECT_EQ(rowsText(rows), "[12 folded]\n  line 12\n  line 13\n  line 14\n- line 15\n+ changed\n  line 16\n  line 17\n  line 18\n[11 folded]");
    // Short runs stay visible.
    EXPECT_EQ(rowsText(recoveryDiff("a\nb\nc\n", "a\nb\nc\nd\n")), "  a\n  b\n  c\n- \n+ d");
    // Everything equal: one fold at the start.
    EXPECT_EQ(rowsText(recoveryDiff(before.join('\n'), before.join('\n'))).count('\n'), 3);
}

TEST(RecoveryDiff, ByteSizes)
{
    EXPECT_EQ(formatByteSize(0), "0 B");
    EXPECT_EQ(formatByteSize(512), "512 B");
    EXPECT_EQ(formatByteSize(1536), "1.50 KB");
    EXPECT_EQ(formatByteSize(1023 * 1024 + 1000), "1,023.98 KB");
    EXPECT_EQ(formatByteSize(5 * 1024 * 1024), "5.00 MB");
}

class FileRecoveryUiTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", "version three\r\n"}, {"Other.md", "other\n"}};
    }
    void SetUp() override
    {
        AppFixture::SetUp();
        store = std::make_unique<SnapshotStore>(tmp.path() + "/data/recovery.sqlite");
        ASSERT_TRUE(store->open());
        recovery = new FileRecovery(store.get(), [this](const QString& p) { return vault->read(p, nullptr, nullptr); }, session.get());
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        store->add("Note.md", now - 3 * 3600'000, "version one\n");
        store->add("Note.md", now - 2 * 3600'000, "version two\nmore\n");
        store->add("Other.md", now - 3600'000, "other\n");
        store->add("Folder/Gone.md", now - 60'000, "deleted note\n");
    }
    void TearDown() override
    {
        delete recovery;
        AppFixture::TearDown();
        store.reset();
    }
    FileRecoveryPanel* panelOf(Modal* m) { return m ? m->findChild<FileRecoveryPanel*>() : nullptr; }

    std::unique_ptr<SnapshotStore> store;
    FileRecovery* recovery = nullptr;
};

TEST_F(FileRecoveryUiTest, ListsFilesAndSnapshots)
{
    Modal* m = openFileRecoveryModal(window);
    ASSERT_TRUE(m);
    EXPECT_TRUE(m->isSidebarLayout());
    FileRecoveryPanel* p = panelOf(m);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->paths(), QStringList({"Folder/Gone.md", "Note.md", "Other.md"}));
    EXPECT_EQ(p->selectedVersion(), -1);

    // The input suggests every file with snapshots (markdown without extension); filtering by substring.
    p->input()->setFocus();
    p->focusInput();
    ASSERT_TRUE(p->suggestPopup());
    ASSERT_TRUE(p->suggestPopup()->isVisible());
    EXPECT_EQ(p->suggestPopup()->list()->suggestions().size(), 3);
    EXPECT_EQ(p->suggestPopup()->list()->suggestions()[0].title, "Folder/Gone");
    QTest::keyClicks(p->input(), "note");
    ASSERT_EQ(p->suggestPopup()->list()->suggestions().size(), 1);
    QTest::keyClick(p->input(), Qt::Key_Return);
    EXPECT_FALSE(p->suggestPopup()->isVisible());

    EXPECT_EQ(p->currentPath(), "Note.md");
    EXPECT_EQ(p->title(), "Note");
    EXPECT_EQ(p->input()->text(), "Note");
    ASSERT_EQ(p->versions().size(), 2);
    EXPECT_EQ(p->selectedVersion(), 0); // newest first, selected
    EXPECT_EQ(p->selectedText(), "version two\nmore\n");
    p->selectVersion(1);
    EXPECT_EQ(p->selectedText(), "version one\n");
    m->close();
}

TEST_F(FileRecoveryUiTest, ShowChangesDiffsAgainstThePreviousSnapshot)
{
    Modal* m = openFileRecoveryModal(window, "Note.md");
    FileRecoveryPanel* p = panelOf(m);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->selectedVersion(), 0);
    p->setShowChanges(true);
    EXPECT_EQ(rowsText(p->diffRows()), "- version one\n- \n+ version two\n+ more");
    p->selectVersion(1); // the oldest: compared with nothing
    EXPECT_EQ(rowsText(p->diffRows()), "- \n+ version one");
    m->close();
}

TEST_F(FileRecoveryUiTest, RestoreWritesExactBytesAndUpdatesTheOpenEditor)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    Modal* m = openFileRecoveryModal(window, "Note.md");
    FileRecoveryPanel* p = panelOf(m);
    ASSERT_TRUE(p);
    p->selectVersion(1);
    QString err;
    ASSERT_TRUE(p->restore(&err)) << err.toStdString();
    EXPECT_EQ(readFile(root + "/Note.md"), QByteArray("version one\n"));
    EXPECT_EQ(mv->editor()->document()->text().toString(), "version one\n");
    // The restored bytes were snapshotted again and the list reloaded (newest selected).
    ASSERT_EQ(p->versions().size(), 3);
    EXPECT_EQ(p->selectedText(), "version one\n");
    m->close();
}

TEST_F(FileRecoveryUiTest, RestoreSavesUnsavedEditsFirst)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    mv->editor()->insertText("edited ");
    ASSERT_TRUE(mv->editor()->document()->isDirty());
    const int before = store->count();
    Modal* m = openFileRecoveryModal(window, "Note.md");
    FileRecoveryPanel* p = panelOf(m);
    p->selectVersion(1);
    ASSERT_TRUE(p->restore());
    EXPECT_EQ(readFile(root + "/Note.md"), QByteArray("version one\n"));
    // The edit was saved (and snapshotted by the recovery binding, if any) before being replaced.
    EXPECT_FALSE(mv->editor()->document()->isDirty());
    EXPECT_GE(store->count(), before + 1);
    m->close();
}

TEST_F(FileRecoveryUiTest, RestoreRecreatesDeletedFilesAndFolders)
{
    Modal* m = openFileRecoveryModal(window, "Folder/Gone.md");
    FileRecoveryPanel* p = panelOf(m);
    ASSERT_TRUE(p);
    ASSERT_EQ(p->versions().size(), 1);
    ASSERT_TRUE(p->restore());
    EXPECT_EQ(readFile(root + "/Folder/Gone.md"), QByteArray("deleted note\n"));
    EXPECT_TRUE(vault->file("Folder/Gone.md"));
    m->close();
}

TEST_F(FileRecoveryUiTest, CopyAndCommand)
{
    Modal* m = openFileRecoveryModal(window, "Other.md");
    FileRecoveryPanel* p = panelOf(m);
    p->copy();
    EXPECT_EQ(QApplication::clipboard()->text(), "other\n");
    m->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    CommandRegistry commands;
    registerFileRecoveryCommands(commands, window);
    ASSERT_TRUE(commands.find("file-recovery:open"));
    EXPECT_EQ(commands.find("file-recovery:open")->name, "Open local history");
    open("Note.md");
    commands.execute("file-recovery:open");
    auto* modal = window->overlay()->findChild<Modal*>("ocFileRecoveryModal");
    ASSERT_TRUE(modal);
    EXPECT_EQ(panelOf(modal)->currentPath(), "Note.md");
    modal->close();
}

TEST_F(FileRecoveryUiTest, NoSnapshots)
{
    store->clear();
    Modal* m = openFileRecoveryModal(window);
    FileRecoveryPanel* p = panelOf(m);
    ASSERT_TRUE(p);
    EXPECT_TRUE(p->paths().isEmpty());
    EXPECT_FALSE(p->suggestPopup());
    m->close();
}
