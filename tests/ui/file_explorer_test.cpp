// File explorer ("Files" view) on a scratch copy of tests/fixtures/vault-basic: tree contents and sort
// orders, create / rename / delete / copy / move through the UI (disk + model), rename validation,
// drag & drop, live updates from disk, keyboard navigation, context menus, state round-trip, and
// screenshots (only with OC_SHOT_DIR).

#include "ui/file_explorer_test_support.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "ui/views/markdown/inline_title.h"

#include <QClipboard>
#include <QJsonArray>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTimer>

using namespace oc;
using namespace oc::test;

namespace {
QStringList actionTexts(const QMenu* menu)
{
    QStringList out;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator())
            out << QStringLiteral("|");
        else if (a->menu())
            out << a->text() + QStringLiteral(" >(") + actionTexts(a->menu()).join(QLatin1Char(',')) + QLatin1Char(')');
        else
            out << a->text();
    }
    return out;
}

QAction* findAction(const QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions()) {
        if (a->text() == text)
            return a;
        if (a->menu())
            if (QAction* sub = findAction(a->menu(), text))
                return sub;
    }
    return nullptr;
}

const QStringList kRootOrder{
    "Archive/", "attachments/", "Daily/", "Projects/", "Templates/", "4chan thread.md", "Bash snippets.md", "Code.md", "Embeds.md",
    "Footnotes.md", "Formatting.md", "Frontmatter.md", "HTML.md", "Lists.md", "Math.md", "Quotes and callouts.md", "Tables.md", "Unicode.md", "Welcome.md",
};
} // namespace

class FileExplorerTest : public ExplorerFixture {};

TEST_F(FileExplorerTest, RegistersAsSidebarView)
{
    FileExplorerView* v = explorer();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->viewType(), "file-explorer");
    EXPECT_EQ(v->displayText(), "Files");
    EXPECT_EQ(v->icon(), "folder-closed");
    EXPECT_FALSE(v->showsViewHeader());
    EXPECT_FALSE(v->navigation());
    EXPECT_TRUE(v->leaf()->isInSidedock());
    EXPECT_TRUE(v->isVisible());
    // nav buttons, in Obsidian's order and size
    EXPECT_EQ(v->newNoteButton()->toolTip(), "New note");
    EXPECT_EQ(v->newFolderButton()->toolTip(), "New folder");
    EXPECT_EQ(v->sortButton()->toolTip(), "Change sort order");
    EXPECT_EQ(v->autoRevealButton()->toolTip(), "Auto-reveal current file");
    EXPECT_EQ(v->collapseAllButton()->toolTip(), "Expand all");
    EXPECT_EQ(v->newNoteButton()->size(), QSize(30, 26));
    EXPECT_LT(v->newNoteButton()->x(), v->newFolderButton()->x());
    EXPECT_LT(v->autoRevealButton()->x(), v->collapseAllButton()->x());
    EXPECT_EQ(v->newNoteButton()->y(), 8);
    EXPECT_EQ(v->newFolderButton()->x() - (v->newNoteButton()->x() + 30), 2); // gap 2px
}

TEST_F(FileExplorerTest, TreeListsFoldersFirstInNaturalOrder)
{
    EXPECT_EQ(tree()->visiblePaths(), kRootOrder);
    // geometry: 13 px items, 4 px container padding, 26.9 px pitch, 12 px side padding
    const QRect r0 = tree()->rowRect(0);
    const QRect r1 = tree()->rowRect(1);
    EXPECT_EQ(r0.top(), 4);
    EXPECT_EQ(r0.left(), 12);
    EXPECT_NEAR(r0.height(), 25, 1);
    EXPECT_NEAR(r1.top() - r0.top(), 27, 1);
    EXPECT_EQ(r0.right() + 1, vp()->width() - 12);

    tree()->setExpanded(m_vault->folder("Projects"), true);
    const QStringList rows = tree()->visiblePaths();
    const qsizetype at = rows.indexOf("Projects/");
    ASSERT_GE(at, 0);
    EXPECT_EQ(rows.mid(at, 4), (QStringList{"Projects/", "Projects/Alpha.md", "Projects/Beta.md", "Projects/Notes.md"}));
    // children are indented by 17 px (12 margin + 1 guide + 4 padding)
    EXPECT_EQ(tree()->rowRect(tree()->rowOf(m_vault->item("Projects/Alpha.md"))).left(), 12 + 17);
    // attachments show (png is a type Obsidian opens), with its extension tag
    tree()->setExpanded(m_vault->folder("attachments"), true);
    EXPECT_TRUE(tree()->visiblePaths().contains("attachments/image.png"));
}

TEST_F(FileExplorerTest, NaturalSortAndUnsupportedFiles)
{
    ASSERT_NE(m_vault->createFile("Sort/Note 10.md", "x"), nullptr);
    ASSERT_NE(m_vault->createFile("Sort/Note 2.md", "x"), nullptr);
    ASSERT_NE(m_vault->createFile("Sort/note 1.md", "x"), nullptr);
    ASSERT_NE(m_vault->createFile("Sort/data.xyz", "x"), nullptr);
    ASSERT_NE(m_vault->createFolder("Sort/zeta"), nullptr);
    ASSERT_NE(m_vault->createFolder("Sort/Alpha 10"), nullptr);
    ASSERT_NE(m_vault->createFolder("Sort/alpha 9"), nullptr);
    tree()->setExpanded(m_vault->folder("Sort"), true);
    auto sortRows = [&] {
        QStringList out;
        for (const QString& p : tree()->visiblePaths())
            if (p.startsWith("Sort/") && p != "Sort/")
                out << p.mid(5);
        return out;
    };
    // folders first (by name, numeric), unsupported files hidden
    EXPECT_EQ(sortRows(), (QStringList{"alpha 9/", "Alpha 10/", "zeta/", "note 1.md", "Note 2.md", "Note 10.md"}));
    m_session->vaultConfig()->set("showUnsupportedFiles", true);
    EXPECT_EQ(sortRows(), (QStringList{"alpha 9/", "Alpha 10/", "zeta/", "data.xyz", "note 1.md", "Note 2.md", "Note 10.md"}));
    // Z to A reverses the files only
    explorer()->setSortOrder(ExplorerSortOrder::AlphabeticalReverse);
    EXPECT_EQ(sortRows(), (QStringList{"alpha 9/", "Alpha 10/", "zeta/", "Note 10.md", "Note 2.md", "note 1.md", "data.xyz"}));
    EXPECT_EQ(naturalCompare("Note 2", "note 10"), -1);
}

class SortOrderTest : public ExplorerFixture {
protected:
    void prepareVault(const QString& root) override
    {
        // mtimes: b newest, then c, then a
        const QDateTime base = QDateTime::currentDateTime().addDays(-10);
        const QList<QPair<QString, int>> files{{"Times/a.md", 1}, {"Times/b.md", 9}, {"Times/c.md", 5}};
        for (const auto& [name, days] : files) {
            ASSERT_TRUE(writeFile(root + "/" + name, "x"));
            QFile f(root + "/" + name);
            ASSERT_TRUE(f.open(QIODevice::ReadWrite));
            ASSERT_TRUE(f.setFileTime(base.addDays(days), QFileDevice::FileModificationTime));
        }
    }
    QStringList times()
    {
        QStringList out;
        for (const QString& p : tree()->visiblePaths())
            if (p.startsWith("Times/") && p != "Times/")
                out << p.mid(6);
        return out;
    }
};

TEST_F(SortOrderTest, SortMenuChangesTheOrderAndState)
{
    tree()->setExpanded(m_vault->folder("Times"), true);
    EXPECT_EQ(times(), (QStringList{"a.md", "b.md", "c.md"}));
    QMenu menu;
    explorer()->populateSortMenu(&menu);
    EXPECT_EQ(actionTexts(&menu), (QStringList{"File name (A to Z)", "File name (Z to A)", "|", "Modified time (new to old)",
                                               "Modified time (old to new)", "|", "Created time (new to old)", "Created time (old to new)"}));
    EXPECT_TRUE(findAction(&menu, "File name (A to Z)")->isChecked());
    QSignalSpy state(explorer(), &View::stateChanged);
    findAction(&menu, "Modified time (new to old)")->trigger();
    EXPECT_EQ(times(), (QStringList{"b.md", "c.md", "a.md"}));
    EXPECT_EQ(explorer()->state().value("sortOrder").toString(), "byModifiedTime");
    EXPECT_EQ(state.count(), 1);
    explorer()->setSortOrder(ExplorerSortOrder::ByModifiedTimeReverse);
    EXPECT_EQ(times(), (QStringList{"a.md", "c.md", "b.md"}));
    // a modification re-sorts (the modified file becomes the newest)
    explorer()->setSortOrder(ExplorerSortOrder::ByModifiedTime);
    DiskState st;
    ASSERT_TRUE(m_vault->read("Times/a.md", &st, nullptr));
    ASSERT_EQ(m_vault->write("Times/a.md", "changed", st, nullptr, nullptr), DocumentStorage::WriteResult::Ok);
    EXPECT_EQ(times().value(0), "a.md");
    // the created-time orders are accepted and deterministic
    explorer()->setSortOrder(ExplorerSortOrder::ByCreatedTime);
    EXPECT_EQ(times().size(), 3);
    EXPECT_EQ(explorer()->state().value("sortOrder").toString(), "byCreatedTime");
}

TEST_F(FileExplorerTest, StateAndFoldsRoundTrip)
{
    FileExplorerView* v = explorer();
    v->setSortOrder(ExplorerSortOrder::AlphabeticalReverse);
    v->setAutoReveal(true);
    tree()->setExpanded(m_vault->folder("Projects"), true);
    tree()->setExpanded(m_vault->folder("Daily"), true);
    const QJsonObject st = v->state();
    EXPECT_EQ(st.value("sortOrder").toString(), "alphabeticalReverse");
    EXPECT_TRUE(st.value("autoReveal").toBool());
    EXPECT_FALSE(st.value("showSearch").toBool());
    EXPECT_EQ(st.value("searchQuery").toString(), "");
    EXPECT_FALSE(v->autoRevealButton()->isChecked()); // a toggle shown as .is-active, not a checkable
    EXPECT_EQ(v->collapseAllButton()->toolTip(), "Collapse all");

    // the workspace layout carries the state; the folds go to the clone's data dir
    m_ws->requestSaveLayout();
    destroySession();
    const QJsonObject folds = JsonFile::read(ClonePaths::vaultDataDir("0123456789abcdef") + "/file-explorer.json");
    EXPECT_EQ(folds.value("file-explorer-unfold").toArray(), (QJsonArray{"Daily", "Projects"}));
    createSession();
    FileExplorerView* again = explorer();
    ASSERT_NE(again, nullptr);
    ASSERT_NE(again, v);
    EXPECT_EQ(again->sortOrder(), ExplorerSortOrder::AlphabeticalReverse);
    EXPECT_TRUE(again->autoReveal());
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Daily")));
    EXPECT_FALSE(tree()->isExpanded(m_vault->folder("Archive")));
    EXPECT_EQ(explorer()->collapseAllButton()->toolTip(), "Collapse all");
    // Collapse all / Expand all
    explorer()->collapseAllButton()->click();
    EXPECT_EQ(tree()->expandedPaths(), QStringList());
    EXPECT_EQ(explorer()->collapseAllButton()->toolTip(), "Expand all");
    explorer()->collapseAllButton()->click();
    EXPECT_EQ(tree()->expandedPaths().size(), m_vault->folders().size() - (m_vault->folders().contains(m_vault->rootFolder()) ? 1 : 0));
}

TEST_F(FileExplorerTest, ClickOpensFilesAndTogglesFolders)
{
    click("Code.md");
    ASSERT_NE(activeRootFile(), nullptr);
    EXPECT_EQ(activeRootFile()->path(), "Code.md");
    EXPECT_EQ(tree()->activeFile(), m_vault->file("Code.md"));
    const int leaves = rootLeafCount();
    // Ctrl+click and middle click open new tabs
    click("Lists.md", Qt::LeftButton, Qt::ControlModifier);
    EXPECT_EQ(rootLeafCount(), leaves + 1);
    EXPECT_EQ(activeRootFile()->path(), "Lists.md");
    click("Math.md", Qt::MiddleButton);
    EXPECT_EQ(rootLeafCount(), leaves + 2);
    EXPECT_EQ(activeRootFile()->path(), "Math.md");
    // folders toggle
    click("Projects");
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    click("Projects");
    EXPECT_FALSE(tree()->isExpanded(m_vault->folder("Projects")));
    // opening a file elsewhere moves the highlight
    m_ws->openFile(m_vault->file("Welcome.md"));
    EXPECT_EQ(tree()->activeFile(), m_vault->file("Welcome.md"));
}

TEST_F(FileExplorerTest, AutoRevealExpandsToTheActiveFile)
{
    explorer()->autoRevealButton()->click();
    EXPECT_TRUE(explorer()->autoReveal());
    m_ws->openFile(m_vault->file("Projects/Beta.md"));
    QApplication::processEvents();
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    EXPECT_GE(tree()->rowOf(m_vault->item("Projects/Beta.md")), 0);
    // the command reveals, focuses and activates the explorer
    tree()->setExpanded(m_vault->folder("Projects"), false);
    explorer()->setAutoReveal(false);
    ASSERT_TRUE(m_commands->execute("file-explorer:reveal-active-file"));
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Projects/Beta.md"));
    EXPECT_EQ(m_ws->activeLeaf(), explorer()->leaf());
}

TEST_F(FileExplorerTest, SelectionWithAltAndShift)
{
    click("Code.md", Qt::LeftButton);
    click("Lists.md", Qt::LeftButton, Qt::AltModifier);
    click("Math.md", Qt::LeftButton, Qt::AltModifier);
    EXPECT_EQ(tree()->selectedItems(), (QList<TAbstractFile*>{m_vault->item("Lists.md"), m_vault->item("Math.md")}));
    click("Math.md", Qt::LeftButton, Qt::AltModifier); // toggles off
    EXPECT_EQ(tree()->selectedItems(), (QList<TAbstractFile*>{m_vault->item("Lists.md")}));
    // Shift: a range from the last item Alt+clicked on (deselecting doesn't move that anchor)
    click("Embeds.md", Qt::LeftButton, Qt::ShiftModifier);
    EXPECT_EQ(tree()->selectedItems(), (QList<TAbstractFile*>{m_vault->item("Embeds.md"), m_vault->item("Footnotes.md"), m_vault->item("Formatting.md"),
                                                              m_vault->item("Frontmatter.md"), m_vault->item("HTML.md"), m_vault->item("Lists.md"),
                                                              m_vault->item("Math.md")}));
    // a plain click clears it
    click("Welcome.md");
    EXPECT_TRUE(tree()->selectedItems().isEmpty());
}

TEST_F(FileExplorerTest, NewNoteOpensForRenaming)
{
    const int leaves = rootLeafCount();
    explorer()->newNoteButton()->click();
    QApplication::processEvents();
    ASSERT_TRUE(exists("Untitled.md"));
    EXPECT_NE(tree()->rowOf(m_vault->item("Untitled.md")), -1);
    EXPECT_EQ(rootLeafCount(), leaves); // the empty tab is reused (Obsidian's new tab)
    ASSERT_NE(activeRootFile(), nullptr);
    EXPECT_EQ(activeRootFile()->path(), "Untitled.md");
    auto* mv = qobject_cast<MarkdownView*>(m_ws->activeRootLeaf()->view());
    ASSERT_NE(mv, nullptr);
    EXPECT_TRUE(mv->inlineTitle()->hasFocus());
    // the next one is "Untitled 1"
    explorer()->newNoteButton()->click();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Untitled 1.md"));
    // "New note" in a folder's menu creates it there
    QMenu menu;
    explorer()->populateContextMenu(&menu, m_vault->folder("Projects"));
    findAction(&menu, "New note")->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Projects/Untitled.md"));
}

TEST_F(FileExplorerTest, NewFolderIsRenamedInTheTree)
{
    explorer()->newFolderButton()->click();
    QApplication::processEvents();
    ASSERT_TRUE(QFileInfo(m_root + "/Untitled").isDir());
    ASSERT_EQ(tree()->renamingItem(), m_vault->item("Untitled"));
    QLineEdit* ed = tree()->renameEditor();
    ASSERT_TRUE(ed->isVisible());
    EXPECT_EQ(ed->text(), "Untitled");
    EXPECT_EQ(ed->selectedText(), "Untitled");
    ed->setText("My Folder");
    QTest::keyClick(ed, Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_EQ(tree()->renamingItem(), nullptr);
    EXPECT_TRUE(QFileInfo(m_root + "/My Folder").isDir());
    EXPECT_FALSE(exists("Untitled"));
    EXPECT_TRUE(tree()->visiblePaths().contains("My Folder/"));
    // the command does the same
    ASSERT_TRUE(m_commands->execute("file-explorer:new-folder"));
    QApplication::processEvents();
    EXPECT_EQ(tree()->renamingItem(), m_vault->item("Untitled"));
    QTest::keyClick(tree()->renameEditor(), Qt::Key_Escape);
    EXPECT_EQ(tree()->renamingItem(), nullptr);
    EXPECT_TRUE(QFileInfo(m_root + "/Untitled").isDir()); // Escape keeps the name
}

TEST_F(FileExplorerTest, RenameValidatesLikeObsidian)
{
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Code.md"));
    key(Qt::Key_F2);
    ASSERT_EQ(tree()->renamingItem(), m_vault->item("Code.md"));
    QLineEdit* ed = tree()->renameEditor();
    EXPECT_EQ(ed->text(), "Code"); // the basename; the extension stays
    auto typeText = [&](const QString& t) {
        ed->selectAll();
        QTest::keyClicks(ed, t);
        QApplication::processEvents();
    };
    typeText("bad/name");
    ASSERT_NE(tree()->errorTooltip(), nullptr);
    EXPECT_TRUE(tree()->errorTooltip()->isVisible());
    EXPECT_EQ(tree()->errorTooltip()->message(), "File name cannot contain any of these characters: \\ / :");
    QTest::keyClick(ed, Qt::Key_Return);
    EXPECT_EQ(tree()->renamingItem(), m_vault->item("Code.md")); // stays in rename
    typeText("Welcome");
    EXPECT_EQ(tree()->errorTooltip()->message(), "There's already a file with the same name");
    typeText(".hidden");
    EXPECT_EQ(tree()->errorTooltip()->message(), "File name must not start with a dot.");
    typeText("a#b");
    EXPECT_EQ(tree()->errorTooltip()->message(), "Links will not work with file names containing any of these characters: # ^ [ ] |");
    QTest::keyClick(ed, Qt::Key_Return);
    EXPECT_TRUE(exists("Code.md"));
    typeText("Code 2");
    EXPECT_FALSE(tree()->errorTooltip()->isVisible());
    QTest::keyClick(ed, Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_EQ(tree()->renamingItem(), nullptr);
    EXPECT_TRUE(exists("Code 2.md"));
    EXPECT_FALSE(exists("Code.md"));
    EXPECT_GE(tree()->rowOf(m_vault->item("Code 2.md")), 0);
    // folder rename keeps the expansion
    tree()->setExpanded(m_vault->folder("Projects"), true);
    explorer()->startRename(m_vault->folder("Projects"));
    tree()->renameEditor()->setText("Work");
    QTest::keyClick(tree()->renameEditor(), Qt::Key_Return);
    EXPECT_TRUE(QFileInfo(m_root + "/Work/Alpha.md").exists());
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Work")));
    EXPECT_TRUE(tree()->visiblePaths().contains("Work/Alpha.md"));
}

TEST_F(FileExplorerTest, F2InTheTreeBeatsTheGlobalHotkey)
{
    int globalRuns = 0;
    Command c;
    c.id = "workspace:edit-file-title";
    c.name = "Rename file";
    c.defaultHotkeys = {Hotkey::parse("F2")};
    c.callback = [&] { ++globalRuns; };
    m_commands->add(c);
    // Like the application: the dispatcher exists before the tree takes the focus.
    explorer()->newNoteButton()->setFocusPolicy(Qt::StrongFocus);
    explorer()->newNoteButton()->setFocus();
    HotkeyDispatcher dispatcher(m_commands);
    dispatcher.setWindow(m_window);
    qApp->installEventFilter(&dispatcher);
    tree()->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(tree()->hasFocus());
    tree()->setFocusedItem(m_vault->item("Lists.md"));
    QTest::keyClick(tree(), Qt::Key_F2);
    EXPECT_EQ(tree()->renamingItem(), m_vault->item("Lists.md"));
    EXPECT_EQ(globalRuns, 0);
    QTest::keyClick(tree()->renameEditor(), Qt::Key_Escape);
    qApp->removeEventFilter(&dispatcher);
}

TEST_F(FileExplorerTest, KeyboardNavigation)
{
    tree()->setFocus();
    QApplication::processEvents();
    key(Qt::Key_Down);
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Archive"));
    key(Qt::Key_Right); // expand
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Archive")));
    key(Qt::Key_Right); // into it
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Archive/Notes.md"));
    key(Qt::Key_Left); // collapses the parent, focus goes up
    EXPECT_FALSE(tree()->isExpanded(m_vault->folder("Archive")));
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Archive"));
    key(Qt::Key_Down);
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("attachments"));
    key(Qt::Key_Up);
    key(Qt::Key_Up); // stays at the top
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Archive"));
    // Enter toggles folders
    key(Qt::Key_Return);
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Archive")));
    key(Qt::Key_Space);
    EXPECT_FALSE(tree()->isExpanded(m_vault->folder("Archive")));
    // Shift+Down extends the selection
    key(Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(tree()->selectedItems(), (QList<TAbstractFile*>{m_vault->item("Archive"), m_vault->item("attachments")}));
    key(Qt::Key_Escape);
    EXPECT_TRUE(tree()->selectedItems().isEmpty());
    // Enter opens a file (and focuses it); Ctrl+Down previews the next one without leaving the tree
    tree()->setFocusedItem(m_vault->item("Code.md"));
    key(Qt::Key_Down, Qt::ControlModifier);
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Embeds.md"));
    ASSERT_NE(activeRootFile(), nullptr);
    EXPECT_EQ(activeRootFile()->path(), "Embeds.md");
    EXPECT_TRUE(tree()->hasFocus());
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Lists.md"));
    QTest::keyClick(tree(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_EQ(activeRootFile()->path(), "Lists.md");
    EXPECT_FALSE(tree()->hasFocus());
    // Left on a root-level file collapses everything
    tree()->setExpanded(m_vault->folder("Daily"), true);
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Math.md"));
    QTest::keyClick(tree(), Qt::Key_Left);
    EXPECT_TRUE(tree()->expandedPaths().isEmpty());
}

TEST_F(FileExplorerTest, DeleteAsksAndUsesTheTrashOption)
{
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Math.md"));
    QTest::keyClick(tree(), Qt::Key_Delete);
    QApplication::processEvents();
    Modal* m = openModal();
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->title(), "Delete file");
    // Welcome.md links [[Math]]
    EXPECT_EQ(m->paragraphs(), (QStringList{"Are you sure you want to delete “Math.md”?",
                                             "It will be moved to your Obsidian trash, which is located in the “.trash” hidden folder in your vault.",
                                             "There is currently 1 link pointing to this file."}));
    ASSERT_EQ(m->buttonCount(), 2);
    EXPECT_EQ(m->button(0)->text(), "Cancel");
    EXPECT_EQ(m->button(1)->text(), "Delete");
    ASSERT_NE(m->checkbox(), nullptr);
    EXPECT_EQ(m->checkbox()->text(), "Don't ask again");
    // Cancel keeps it
    m->button(0)->click();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Math.md"));
    // Delete (Enter = the red CTA) moves it to .trash; the focus goes to the previous item
    QTest::keyClick(tree(), Qt::Key_Delete);
    m = openModal();
    ASSERT_NE(m, nullptr);
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : static_cast<QWidget*>(m), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_FALSE(exists("Math.md"));
    EXPECT_TRUE(exists(".trash/Math.md"));
    EXPECT_EQ(tree()->rowOf(m_vault->item("Math.md")), -1);
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Lists.md"));
    // a non-empty folder warns; "Don't ask again" turns the prompt off
    explorer()->deleteItems({m_vault->item("Projects")});
    m = openModal();
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->title(), "Delete folder");
    EXPECT_TRUE(m->paragraphs().contains("This folder is not empty."));
    m->checkbox()->click();
    m->button(1)->click();
    QApplication::processEvents();
    EXPECT_FALSE(exists("Projects"));
    EXPECT_FALSE(m_session->vaultConfig()->flag("promptDelete"));
    explorer()->deleteItems({m_vault->item("Lists.md")});
    QApplication::processEvents();
    EXPECT_EQ(openModal(), nullptr);
    EXPECT_FALSE(exists("Lists.md"));
}

TEST_F(FileExplorerTest, MultiDeletePromptsForEach)
{
    m_session->vaultConfig()->set("trashOption", "none");
    tree()->setSelectedItems({m_vault->item("Code.md"), m_vault->item("Tables.md")});
    tree()->setFocus();
    QTest::keyClick(tree(), Qt::Key_Delete);
    for (const char* name : {"Code.md", "Tables.md"}) {
        QApplication::processEvents();
        Modal* m = openModal();
        ASSERT_NE(m, nullptr) << name;
        EXPECT_TRUE(m->paragraphs().value(0).contains(QString::fromUtf8(name)));
        EXPECT_EQ(m->paragraphs().value(1), "The file will be permanent deleted.");
        m->button(1)->click();
    }
    QApplication::processEvents();
    EXPECT_FALSE(exists("Code.md"));
    EXPECT_FALSE(exists("Tables.md"));
    EXPECT_FALSE(exists(".trash/Code.md"));
}

TEST_F(FileExplorerTest, ContextMenusMatchObsidian)
{
    QMenu file;
    explorer()->populateContextMenu(&file, m_vault->item("Code.md"));
    EXPECT_EQ(actionTexts(&file),
              (QStringList{"Open in new tab", "Open to the right", "Open in new window", "|", "Make a copy", "Move file to...", "|",
                           "Copy path >(as Obsidian URL,from vault folder,from system root)", "|", "Open in default app", "Show in system explorer",
                           "|", "Rename...", "Delete"}));
    QMenu folder;
    explorer()->populateContextMenu(&folder, m_vault->item("Projects"));
    EXPECT_EQ(actionTexts(&folder), (QStringList{"New note", "New folder", "|", "Make a copy", "Move folder to...", "|",
                                                 "Copy path >(from vault folder,from system root)", "|", "Show in system explorer", "|",
                                                 "Rename...", "Delete"}));
    QMenu root;
    explorer()->populateContextMenu(&root, nullptr);
    EXPECT_EQ(actionTexts(&root), (QStringList{"New note", "New folder"}));
    tree()->setSelectedItems({m_vault->item("Code.md"), m_vault->item("Lists.md")});
    QMenu multi;
    explorer()->populateContextMenu(&multi, m_vault->item("Lists.md"));
    EXPECT_EQ(actionTexts(&multi), (QStringList{"New folder with selection (2 items)", "|", "Move 2 items to...", "|", "Delete"}));

    // actions
    findAction(&file, "from vault folder")->trigger();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "Code.md");
    findAction(&file, "from system root")->trigger();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), m_root + "/Code.md");
    findAction(&file, "as Obsidian URL")->trigger();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "obsidian://open?vault=vault&file=Code");
    findAction(&file, "Show in system explorer")->trigger();
    EXPECT_EQ(shown, QStringList{m_root + "/Code.md"});
    findAction(&file, "Open in default app")->trigger();
    EXPECT_EQ(opened, QList<QUrl>{QUrl::fromLocalFile(m_root + "/Code.md")});
    const int leaves = rootLeafCount();
    findAction(&file, "Open to the right")->trigger();
    EXPECT_EQ(rootLeafCount(), leaves + 1);
    EXPECT_EQ(activeRootFile()->path(), "Code.md");
    findAction(&file, "Make a copy")->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Code 1.md"));
    EXPECT_EQ(activeRootFile()->path(), "Code 1.md");
    findAction(&folder, "Make a copy")->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Projects 1/Alpha.md"));
    EXPECT_TRUE(exists("Projects 1/Notes.md"));
    EXPECT_EQ(tree()->renamingItem(), m_vault->item("Projects 1"));
    tree()->cancelRename();
    findAction(&multi, "New folder with selection (2 items)")->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Untitled/Code.md"));
    EXPECT_TRUE(exists("Untitled/Lists.md"));
    EXPECT_EQ(tree()->renamingItem(), m_vault->item("Untitled"));
}

TEST_F(FileExplorerTest, MoveModalMovesAndCreates)
{
    openMoveModal(m_session, {m_vault->item("Code.md")});
    SuggestModal* m = openSuggest();
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->input()->placeholderText(), "Type a folder");
    EXPECT_EQ(m->suggestions().value(0).title, "/");
    m->setQuery("arch");
    ASSERT_FALSE(m->suggestions().isEmpty());
    EXPECT_EQ(m->suggestions().first().title, "Archive");
    QTest::keyClick(m->input(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Archive/Code.md"));
    EXPECT_FALSE(exists("Code.md"));
    // a folder can't go into itself; Shift+Enter creates the typed folder
    openMoveModal(m_session, {m_vault->item("Projects")});
    m = openSuggest();
    ASSERT_NE(m, nullptr);
    for (const Suggestion& s : m->suggestions())
        EXPECT_FALSE(s.title == "Projects" || s.title.startsWith("Projects/")) << qPrintable(s.title);
    m->setQuery("Work/2026");
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ShiftModifier);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Work/2026/Projects/Alpha.md"));
    // the command moves the active file
    m_ws->openFile(m_vault->file("Lists.md"));
    ASSERT_TRUE(m_commands->execute("file-explorer:move-file"));
    m = openSuggest();
    ASSERT_NE(m, nullptr);
    m->setQuery("Daily");
    QTest::keyClick(m->input(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Daily/Lists.md"));
}

TEST_F(FileExplorerTest, DragAndDropMovesIntoFolders)
{
    const QPoint from = rowCenter("Code.md");
    sendMouse(vp(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    sendMouse(vp(), QEvent::MouseMove, from + QPoint(2, 2), Qt::NoButton, Qt::LeftButton);
    EXPECT_FALSE(tree()->isDragging()); // under the threshold
    const QPoint over = rowCenter("Projects");
    sendMouse(vp(), QEvent::MouseMove, over, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(tree()->isDragging());
    EXPECT_EQ(tree()->draggedItems(), QList<TAbstractFile*>{m_vault->item("Code.md")});
    EXPECT_EQ(tree()->dropTarget(), m_vault->folder("Projects"));
    // hovering a collapsed folder expands it after ~750 ms
    EXPECT_FALSE(tree()->isExpanded(m_vault->folder("Projects")));
    QTest::qWait(900);
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    sendMouse(vp(), QEvent::MouseButtonRelease, over, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_FALSE(tree()->isDragging());
    EXPECT_TRUE(exists("Projects/Code.md"));
    EXPECT_FALSE(exists("Code.md"));

    // a folder can't be dropped into its own subtree; files inside a folder row target that folder
    const QPoint alpha = rowCenter("Projects/Alpha.md");
    sendMouse(vp(), QEvent::MouseButtonPress, rowCenter("Projects"), Qt::LeftButton, Qt::LeftButton);
    sendMouse(vp(), QEvent::MouseMove, alpha, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(tree()->isDragging());
    EXPECT_EQ(tree()->dropTarget(), nullptr);
    QTest::keyClick(tree(), Qt::Key_Escape); // cancels
    EXPECT_FALSE(tree()->isDragging());
    sendMouse(vp(), QEvent::MouseButtonRelease, alpha, Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(exists("Projects/Alpha.md"));

    // the selection moves together; an empty area is the vault root
    tree()->setSelectedItems({m_vault->item("Projects/Alpha.md"), m_vault->item("Projects/Beta.md")});
    const QPoint beta = rowCenter("Projects/Beta.md");
    sendMouse(vp(), QEvent::MouseButtonPress, beta, Qt::LeftButton, Qt::LeftButton);
    const QPoint empty(vp()->width() / 2, vp()->height() - 5);
    sendMouse(vp(), QEvent::MouseMove, empty, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(tree()->isDragging());
    EXPECT_EQ(tree()->draggedItems().size(), 2);
    EXPECT_EQ(tree()->dropTarget(), m_vault->rootFolder());
    sendMouse(vp(), QEvent::MouseButtonRelease, empty, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Alpha.md"));
    EXPECT_TRUE(exists("Beta.md"));
    // moving onto a name that exists picks a free one
    ASSERT_NE(m_vault->createFile("Archive/Welcome.md", "other"), nullptr);
    moveItems(m_session, {m_vault->item("Welcome.md")}, m_vault->folder("Archive"));
    EXPECT_TRUE(exists("Archive/Welcome 1.md"));
}

TEST_F(FileExplorerTest, DragOntoTheWorkspaceOpensTheFile)
{
    const int leaves = rootLeafCount();
    QWidget* wsw = m_ws->widget();
    const QPoint from = rowCenter("Tables.md");
    sendMouse(vp(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    // the middle of the main area (the tab group's center: a new tab)
    const QPoint target = vp()->mapFromGlobal(wsw->mapToGlobal(QPoint(wsw->width() * 2 / 3, wsw->height() / 2)));
    sendMouse(vp(), QEvent::MouseMove, from + QPoint(20, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(vp(), QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(tree()->isDragging());
    EXPECT_EQ(tree()->dropTarget(), nullptr);
    sendMouse(vp(), QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Tables.md")); // not moved
    ASSERT_NE(activeRootFile(), nullptr);
    EXPECT_EQ(activeRootFile()->path(), "Tables.md");
    EXPECT_GE(rootLeafCount(), leaves);
}

TEST_F(FileExplorerTest, CopyCutAndPaste)
{
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Code.md"));
    QTest::keyClick(tree(), Qt::Key_C, Qt::ControlModifier);
    ASSERT_NE(QGuiApplication::clipboard()->mimeData(), nullptr);
    EXPECT_TRUE(QGuiApplication::clipboard()->mimeData()->hasFormat("obsidian/files"));
    tree()->setFocusedItem(m_vault->item("Projects"));
    QTest::keyClick(tree(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(exists("Projects/Code.md"));
    EXPECT_TRUE(exists("Code.md"));
    EXPECT_EQ(tree()->selectedItems(), QList<TAbstractFile*>{m_vault->item("Projects/Code.md")});
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Projects/Code.md"));
    // again, next to the copy: a free name
    QTest::keyClick(tree(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(exists("Projects/Code 1.md"));
    // cut the selection, paste into another folder
    tree()->setSelectedItems({m_vault->item("Lists.md"), m_vault->item("Math.md")});
    QTest::keyClick(tree(), Qt::Key_X, Qt::ControlModifier);
    tree()->setFocusedItem(m_vault->item("Archive"));
    QTest::keyClick(tree(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(exists("Archive/Lists.md"));
    EXPECT_TRUE(exists("Archive/Math.md"));
    EXPECT_FALSE(exists("Lists.md"));
    EXPECT_EQ(tree()->selectedItems().size(), 2);
}

TEST_F(FileExplorerTest, DropFromOtherApplicationsImports)
{
    const QString outside = m_tmp.path() + "/outside";
    ASSERT_TRUE(writeFile(outside + "/photo.png", "png"));
    ASSERT_TRUE(writeFile(outside + "/dir/a.md", "a"));
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(outside + "/photo.png"), QUrl::fromLocalFile(outside + "/dir")});
    const QPoint pos = rowCenter("Projects");
    QDragEnterEvent enter(pos, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp(), &enter);
    EXPECT_TRUE(enter.isAccepted());
    QDragMoveEvent move(pos, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp(), &move);
    EXPECT_TRUE(move.isAccepted());
    QDropEvent drop(QPointF(pos), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp(), &drop);
    QApplication::processEvents();
    EXPECT_TRUE(exists("Projects/photo.png"));
    EXPECT_TRUE(exists("Projects/dir/a.md"));
    EXPECT_TRUE(QFileInfo::exists(outside + "/photo.png")); // copied, not moved
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
}

TEST_F(FileExplorerTest, FollowsExternalChanges)
{
    tree()->setExpanded(m_vault->folder("Projects"), true);
    tree()->setFocusedItem(m_vault->item("Welcome.md"));
    const int scroll = tree()->verticalScrollBar()->value();
    ASSERT_TRUE(writeFile(m_root + "/External note.md", "hi"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return tree()->rowOf(m_vault->item("External note.md")) >= 0; }, 10000));
    ASSERT_TRUE(writeFile(m_root + "/Projects/Gamma.md", "hi"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return tree()->rowOf(m_vault->item("Projects/Gamma.md")) >= 0; }, 10000));
    EXPECT_TRUE(tree()->isExpanded(m_vault->folder("Projects")));
    EXPECT_EQ(tree()->focusedItem(), m_vault->item("Welcome.md"));
    EXPECT_EQ(tree()->verticalScrollBar()->value(), scroll);
    ASSERT_TRUE(QFile::rename(m_root + "/Projects/Gamma.md", m_root + "/Projects/Delta.md"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return tree()->rowOf(m_vault->item("Projects/Delta.md")) >= 0; }, 10000));
    EXPECT_FALSE(tree()->visiblePaths().contains("Projects/Gamma.md"));
    ASSERT_TRUE(QFile::remove(m_root + "/External note.md"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !tree()->visiblePaths().contains("External note.md"); }, 10000));
    ASSERT_TRUE(QDir(m_root + "/Projects").removeRecursively());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !tree()->visiblePaths().contains("Projects/"); }, 10000));
    EXPECT_EQ(tree()->visiblePaths().filter("Projects").size(), 0);
}

TEST_F(FileExplorerTest, OfflineIsReadOnly)
{
    emit m_vault->onlineChanged(false);
    EXPECT_TRUE(tree()->isReadOnly());
    EXPECT_FALSE(explorer()->newNoteButton()->isEnabled());
    EXPECT_FALSE(explorer()->newFolderButton()->isEnabled());
    QMenu menu;
    explorer()->populateContextMenu(&menu, m_vault->item("Code.md"));
    EXPECT_FALSE(findAction(&menu, "Rename...")->isEnabled());
    EXPECT_FALSE(findAction(&menu, "Delete")->isEnabled());
    EXPECT_TRUE(findAction(&menu, "Open in new tab")->isEnabled());
    tree()->setFocus();
    tree()->setFocusedItem(m_vault->item("Code.md"));
    QTest::keyClick(tree(), Qt::Key_F2);
    EXPECT_EQ(tree()->renamingItem(), nullptr);
    const QPoint from = rowCenter("Code.md");
    sendMouse(vp(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    sendMouse(vp(), QEvent::MouseMove, rowCenter("Projects"), Qt::NoButton, Qt::LeftButton);
    EXPECT_FALSE(tree()->isDragging());
    sendMouse(vp(), QEvent::MouseButtonRelease, rowCenter("Projects"), Qt::LeftButton, Qt::NoButton);
    emit m_vault->onlineChanged(true);
    EXPECT_FALSE(tree()->isReadOnly());
    EXPECT_TRUE(explorer()->newNoteButton()->isEnabled());
}

TEST_F(FileExplorerTest, CommandsUseObsidianIdsAndHotkeys)
{
    for (const char* id : {"file-explorer:open", "file-explorer:reveal-active-file", "file-explorer:new-file", "file-explorer:new-file-in-current-tab",
                           "file-explorer:new-file-in-new-pane", "file-explorer:new-folder", "file-explorer:move-file", "file-explorer:duplicate-file"})
        EXPECT_NE(m_commands->find(QString::fromLatin1(id)), nullptr) << id;
    EXPECT_EQ(m_commands->find("file-explorer:new-file")->defaultHotkeys, QList<Hotkey>{Hotkey::parse("Mod+N")});
    EXPECT_EQ(m_commands->find("file-explorer:new-file-in-new-pane")->defaultHotkeys, QList<Hotkey>{Hotkey::parse("Mod+Shift+N")});
    EXPECT_EQ(m_commands->find("file-explorer:new-file")->name, "Create new note");
    EXPECT_FALSE(m_commands->isAvailable("file-explorer:move-file")); // no active file
    ASSERT_TRUE(m_commands->execute("file-explorer:new-file"));
    QApplication::processEvents();
    EXPECT_TRUE(exists("Untitled.md"));
    EXPECT_EQ(activeRootFile()->path(), "Untitled.md");
    ASSERT_TRUE(m_commands->execute("file-explorer:duplicate-file"));
    QApplication::processEvents();
    EXPECT_TRUE(exists("Untitled 1.md"));
    EXPECT_EQ(activeRootFile()->path(), "Untitled 1.md");
    // new-file-in-new-pane splits
    const int leaves = rootLeafCount();
    ASSERT_TRUE(m_commands->execute("file-explorer:new-file-in-new-pane"));
    QApplication::processEvents();
    EXPECT_EQ(rootLeafCount(), leaves + 1);
    ASSERT_TRUE(m_commands->execute("file-explorer:open"));
    EXPECT_EQ(m_ws->activeLeaf(), explorer()->leaf());
}

TEST_F(FileExplorerTest, NewNotesFollowTheNewFileLocation)
{
    m_session->vaultConfig()->set("newFileLocation", "current");
    m_ws->openFile(m_vault->file("Projects/Alpha.md"));
    explorer()->newNoteButton()->click();
    QApplication::processEvents();
    EXPECT_TRUE(exists("Projects/Untitled.md"));
    m_session->vaultConfig()->set("newFileLocation", "folder");
    m_session->vaultConfig()->set("newFileFolderPath", "Daily");
    ASSERT_TRUE(m_commands->execute("file-explorer:new-file-in-current-tab"));
    QApplication::processEvents();
    EXPECT_TRUE(exists("Daily/Untitled.md"));
}

TEST(ModalLinkPrompt, UpdateLinksModalAnswers)
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = new ThemeManager(qApp);
        tm->apply();
    }
    QWidget w;
    w.resize(900, 700);
    auto* layer = new OverlayLayer(&w);
    w.show();
    QApplication::processEvents();
    QTimer::singleShot(0, [&] {
        Modal* m = findOpen<Modal>(layer);
        ASSERT_NE(m, nullptr);
        EXPECT_EQ(m->title(), "Update links");
        EXPECT_EQ(m->paragraphs(), (QStringList{"Do you want to update internal links that link to this file?", "This will affect 3 links in 2 files."}));
        m->button(1)->click(); // Just once
    });
    EXPECT_EQ(promptLinkUpdate(layer, 3, 2), FileManager::LinkUpdateChoice::JustOnce);
    QTimer::singleShot(0, [&] { findOpen<Modal>(layer)->close(); });
    EXPECT_EQ(promptLinkUpdate(layer, 1, 1), FileManager::LinkUpdateChoice::DoNotUpdate);
    QTimer::singleShot(0, [&] { findOpen<Modal>(layer)->button(0)->click(); });
    EXPECT_EQ(promptLinkUpdate(layer, 1, 1), FileManager::LinkUpdateChoice::AlwaysUpdate);
}

// ---- screenshots (OC_SHOT_DIR) ----------------------------------------------------------------------

class ExplorerShots : public ExplorerFixture {};

TEST_F(ExplorerShots, DarkAndLight)
{
    const QString dir = qEnvironmentVariable("OC_SHOT_DIR");
    if (dir.isEmpty())
        GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
    QDir().mkpath(dir);
    m_window->resize(1400, 850);
    tree()->setExpanded(m_vault->folder("Projects"), true);
    tree()->setExpanded(m_vault->folder("attachments"), true);
    m_ws->openFile(m_vault->file("Projects/Beta.md"));
    tree()->setSelectedItems({});
    QApplication::processEvents();
    for (const auto& [mode, name] : {std::pair{ThemeManager::Mode::Dark, "dark"}, std::pair{ThemeManager::Mode::Light, "light"}}) {
        ThemeManager::instance()->setMode(mode);
        ThemeManager::instance()->apply();
        QApplication::processEvents();
        m_window->grab().save(dir + QStringLiteral("/explorer-%1.png").arg(QString::fromLatin1(name)));
        // hover + focus ring + rename + drag-over states on the tree alone
        tree()->setFocusedItem(m_vault->item("Code.md"), false);
        tree()->setLeafActive(true);
        tree()->setSelectedItems({m_vault->item("Lists.md"), m_vault->item("Math.md")});
        sendMouse(vp(), QEvent::MouseMove, rowCenter("Formatting.md"), Qt::NoButton, Qt::NoButton);
        QApplication::processEvents();
        explorer()->grab().save(dir + QStringLiteral("/explorer-states-%1.png").arg(QString::fromLatin1(name)));
        explorer()->startRename(m_vault->item("Welcome.md"));
        tree()->renameEditor()->setText("Welcome/");
        QTest::keyClick(tree()->renameEditor(), Qt::Key_Slash);
        QApplication::processEvents();
        m_window->grab().save(dir + QStringLiteral("/explorer-rename-%1.png").arg(QString::fromLatin1(name)));
        tree()->cancelRename();
        tree()->setSelectedItems({});
        // drag over a folder
        const QPoint from = rowCenter("Code.md");
        sendMouse(vp(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp(), QEvent::MouseMove, rowCenter("Projects/Alpha.md"), Qt::NoButton, Qt::LeftButton);
        QApplication::processEvents();
        m_window->grab().save(dir + QStringLiteral("/explorer-drag-%1.png").arg(QString::fromLatin1(name)));
        tree()->cancelDrag();
        sendMouse(vp(), QEvent::MouseButtonRelease, from, Qt::LeftButton, Qt::NoButton);
        // the delete confirmation
        explorer()->deleteItems({m_vault->item("Projects")});
        QApplication::processEvents();
        m_window->grab().save(dir + QStringLiteral("/explorer-delete-%1.png").arg(QString::fromLatin1(name)));
        if (Modal* m = openModal())
            m->close();
        // "Move file to..."
        openMoveModal(m_session, {m_vault->item("Code.md")});
        QApplication::processEvents();
        if (SuggestModal* sm = openSuggest()) {
            sm->setQuery("pro");
            QApplication::processEvents();
            m_window->grab().save(dir + QStringLiteral("/explorer-move-%1.png").arg(QString::fromLatin1(name)));
            sm->close();
        }
        // the context menu, rendered on its own
        QMenu menu;
        explorer()->populateContextMenu(&menu, m_vault->item("Code.md"));
        menu.adjustSize();
        menu.grab().save(dir + QStringLiteral("/explorer-menu-%1.png").arg(QString::fromLatin1(name)));
        tree()->setLeafActive(false);
        QApplication::processEvents();
    }
    ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
    ThemeManager::instance()->apply();
}
