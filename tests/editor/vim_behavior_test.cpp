// Vim behaviour beyond the oracle table: undo grouping through the Document's History, typing
// through the editor's input path (auto-pairs, list continuation) and "." repeat, the ":" / "/"
// panel, the block cursor, ex :w / :q, display-line motions, IME and read-only modes.
#include "editor/vim_test_support.h"

#include "editor/vim/vim_engine.h"

#include <QInputMethodEvent>
#include <QLabel>
#include <QScrollBar>
#include <QTest>

using namespace oc;
using namespace oc::test;

namespace {

class VimBehavior : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        static ThemeManager themes;
        themes.apply();
    }
    void SetUp() override
    {
        mdcmd::settings() = mdcmd::Settings();
        mdcmd::settings().autoPairBrackets = false;
        mdcmd::settings().autoPairMarkdown = false;
        VimController::resetGlobalState();
        h = std::make_unique<VimHarness>();
    }
    void TearDown() override
    {
        h.reset();
        mdcmd::settings() = mdcmd::Settings();
    }
    std::unique_ptr<VimHarness> h;
};

// --- undo grouping ---------------------------------------------------------------------------

TEST_F(VimBehavior, EveryCommandIsOneUndoStep)
{
    h->open(QStringLiteral("|one two three"));
    h->keys(QStringLiteral("dwdw"));
    EXPECT_EQ(h->text(), QStringLiteral("three"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("two three"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("one two three"));
    EXPECT_EQ(h->marked(), QStringLiteral("|one two three")); // Obsidian: the start of the change
    h->keys(QStringLiteral("<C-r>"));
    EXPECT_EQ(h->text(), QStringLiteral("two three"));
    h->keys(QStringLiteral("2u"));
    EXPECT_EQ(h->text(), QStringLiteral("one two three"));
    h->keys(QStringLiteral("2<C-r>"));
    EXPECT_EQ(h->text(), QStringLiteral("three"));
}

TEST_F(VimBehavior, InsertSessionIsOneUndoStepWhateverTheTiming)
{
    h->open(QStringLiteral("foo b|ar baz"));
    h->doc->history().newGroupDelayMs = 0; // plain typing would never join
    h->keys(QStringLiteral("ciwone two<Esc>"));
    EXPECT_EQ(h->text(), QStringLiteral("foo one two baz"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("foo bar baz"));
    h->keys(QStringLiteral("<C-r>"));
    EXPECT_EQ(h->text(), QStringLiteral("foo one two baz"));
}

TEST_F(VimBehavior, CursorMovesInInsertModeStartANewUndoStep)
{
    h->open(QStringLiteral("|x"));
    h->keys(QStringLiteral("ifoo<Left>bar<Esc>"));
    EXPECT_EQ(h->text(), QStringLiteral("fobarox"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("foox"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("x"));
}

TEST_F(VimBehavior, EditorUndoSharesTheHistory)
{
    h->open(QStringLiteral("|abc"));
    h->keys(QStringLiteral("xx"));
    EXPECT_TRUE(h->view->undo()); // Ctrl+Z / the app's undo
    EXPECT_EQ(h->text(), QStringLiteral("bc"));
    // like CodeMirror, the editor's undo restores the operator's selection: vim sees visual mode
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Visual);
    h->keys(QStringLiteral("<Esc>u"));
    EXPECT_EQ(h->text(), QStringLiteral("abc"));
    EXPECT_FALSE(h->doc->history().inGroup());
}

TEST_F(VimBehavior, DotRepeatAndMacrosAreSingleSteps)
{
    h->open(QStringLiteral("|a b c d"));
    h->keys(QStringLiteral("dw.."));
    EXPECT_EQ(h->text(), QStringLiteral("d"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("c d"));
    h->open(QStringLiteral("|1\n2\n3"));
    h->keys(QStringLiteral("qaA!<Esc>jq2@a"));
    EXPECT_EQ(h->text(), QStringLiteral("1!\n2!\n3!"));
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("1!\n2\n3"));
}

// --- typing goes through the editor's input path ----------------------------------------------

TEST_F(VimBehavior, AutoPairsInInsertModeAndDotRepeatsThem)
{
    mdcmd::settings().autoPairBrackets = true;
    h->open(QStringLiteral("| x"));
    h->keys(QStringLiteral("if(a<Esc>"));
    EXPECT_EQ(h->marked(), QStringLiteral("f(|a) x"));
    h->keys(QStringLiteral("$."));
    EXPECT_EQ(h->text(), QStringLiteral("f(a) f(a)x"));
}

TEST_F(VimBehavior, EnterContinuesListsInInsertMode)
{
    h->open(QStringLiteral("- a|"));
    h->keys(QStringLiteral("A<CR>b<Esc>"));
    EXPECT_EQ(h->text(), QStringLiteral("- a\n- b"));
    h->keys(QStringLiteral("."));
    EXPECT_EQ(h->text(), QStringLiteral("- a\n- b\n- b"));
    // "o" is CodeMirror's newline-and-indent: no list marker
    h->keys(QStringLiteral("ox<Esc>"));
    EXPECT_EQ(h->text(), QStringLiteral("- a\n- b\n- b\nx"));
}

TEST_F(VimBehavior, ReplaceModeThroughRealKeys)
{
    h->open(QStringLiteral("a|bcd"));
    h->keys(QStringLiteral("Rxyz!<Esc>"));
    EXPECT_EQ(h->marked(), QStringLiteral("axyz|!"));
    h->keys(QStringLiteral("0."));
    EXPECT_EQ(h->text(), QStringLiteral("xyz!!"));
}

TEST_F(VimBehavior, NormalModeSwallowsCharactersButNotEditorKeys)
{
    h->open(QStringLiteral("a|bc"));
    h->keys(QStringLiteral("QZ"));
    EXPECT_EQ(h->text(), QStringLiteral("abc"));
    // Tab is not a vim key: the editor indents like CodeMirror's keymap does
    h->keys(QStringLiteral("<Tab>"));
    EXPECT_EQ(h->text(), QStringLiteral("a\tbc"));
}

TEST_F(VimBehavior, ImeCommitsAreKeysOutsideInsertMode)
{
    h->open(QStringLiteral("|abc"));
    QInputMethodEvent ev(QString(), {});
    ev.setCommitString(QStringLiteral("x"));
    QApplication::sendEvent(h->view.get(), &ev);
    EXPECT_EQ(h->text(), QStringLiteral("bc"));
    h->keys(QStringLiteral("i"));
    QInputMethodEvent ev2(QString(), {});
    ev2.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(h->view.get(), &ev2);
    EXPECT_EQ(h->text(), QStringLiteral("ébc"));
}

TEST_F(VimBehavior, ReadingModeLeavesKeysAlone)
{
    h->open(QStringLiteral("|abc"));
    h->view->setMode(EditorView::Mode::Reading);
    h->keys(QStringLiteral("x"));
    EXPECT_EQ(h->text(), QStringLiteral("abc"));
    h->view->setMode(EditorView::Mode::Source);
    h->keys(QStringLiteral("x"));
    EXPECT_EQ(h->text(), QStringLiteral("bc"));
}

// --- modes and the block cursor -----------------------------------------------------------

TEST_F(VimBehavior, CursorShapeFollowsTheMode)
{
    h->open(QStringLiteral("a|bc\ndef"));
    EXPECT_EQ(h->view->cursorShape(), EditorView::CursorShape::Block);
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Normal);
    h->keys(QStringLiteral("i"));
    EXPECT_EQ(h->view->cursorShape(), EditorView::CursorShape::Bar);
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Insert);
    h->keys(QStringLiteral("<Esc>R"));
    EXPECT_EQ(h->view->cursorShape(), EditorView::CursorShape::Block);
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Replace);
    h->keys(QStringLiteral("<Esc>v"));
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Visual);
    h->keys(QStringLiteral("V"));
    EXPECT_EQ(h->vim->mode(), VimController::Mode::VisualLine);
    h->keys(QStringLiteral("<C-v>"));
    EXPECT_EQ(h->vim->mode(), VimController::Mode::VisualBlock);
    h->keys(QStringLiteral("<Esc>"));
    EXPECT_EQ(h->vim->mode(), VimController::Mode::Normal);
    // switching vim off restores the bar
    h->vim.reset();
    EXPECT_EQ(h->view->cursorShape(), EditorView::CursorShape::Bar);
    h->vim = std::make_unique<VimController>(h->view.get());
}

TEST_F(VimBehavior, BlockCursorCoversTheCharacter)
{
    h->open(QStringLiteral("a|bc"));
    const SelRange r = h->view->selection().main();
    const QRectF box = h->view->blockCursorRect(r);
    const QRectF c1 = h->view->caretRectAt(1), c2 = h->view->caretRectAt(2);
    EXPECT_NEAR(box.left(), c1.left(), 0.01);
    EXPECT_NEAR(box.width(), c2.left() - c1.left(), 0.01);
    EXPECT_NEAR(box.top(), c1.top(), 0.01);
    EXPECT_NEAR(box.height(), c1.height(), 0.01);
    // past the last character: a space wide
    h->keys(QStringLiteral("A"));
    h->keys(QStringLiteral("<Esc>"));
    h->view->setCursorPosition(3, false);
    const QRectF eol = h->view->blockCursorRect(h->view->selection().main());
    EXPECT_GT(eol.width(), 2);
    EXPECT_NEAR(eol.left(), h->view->caretRectAt(3).left(), 0.01);
    // a forward visual selection: on the last selected character
    h->view->setCursorPosition(0, false);
    h->keys(QStringLiteral("vl"));
    const QRectF vis = h->view->blockCursorRect(h->view->selection().main());
    EXPECT_NEAR(vis.left(), h->view->caretRectAt(1).left(), 0.01);
    // insert mode: no block
    h->keys(QStringLiteral("<Esc>i"));
    EXPECT_TRUE(h->view->blockCursorRect(h->view->selection().main()).isEmpty());
}

TEST_F(VimBehavior, BlockCursorIsPaintedInTheAccent)
{
    h->open(QStringLiteral("|abc"));
    h->view->setFocus();
    QApplication::processEvents();
    const QRectF box = h->view->blockCursorRect(h->view->selection().main());
    const QImage img = h->view->viewport()->grab().toImage();
    const QColor accent = currentTheme()->color(QStringLiteral("--interactive-accent"));
    const QPoint probe = QPoint(int(box.left()) + 1, int(box.top()) + 1);
    if (h->view->hasFocus()) { // painted only while focused (offscreen focus can be flaky)
        EXPECT_EQ(QColor(img.pixel(probe)).rgb(), accent.rgb());
    }
}

// --- the command-line panel -------------------------------------------------------------------

TEST_F(VimBehavior, ColonOpensThePanelAtTheBottom)
{
    h->open(QStringLiteral("|abc"));
    const int viewportBefore = h->view->viewport()->height();
    vim::VimPanel* panel = h->vim->panel();
    EXPECT_TRUE(panel->isHidden());
    h->keys(QStringLiteral(":s"));
    ASSERT_TRUE(panel->isPromptOpen());
    EXPECT_FALSE(panel->isHidden());
    EXPECT_EQ(panel->displayText(), QStringLiteral(":s"));
    EXPECT_EQ(panel->parentWidget(), h->view.get());
    EXPECT_EQ(panel->geometry().bottom(), h->view->height() - 1);
    EXPECT_EQ(panel->width(), h->view->width());
    EXPECT_EQ(h->view->viewport()->height(), viewportBefore - panel->height());
    EXPECT_GE(panel->height(), int(currentTheme()->textSize() * 1.3));
    h->keys(QStringLiteral("<Esc>"));
    EXPECT_FALSE(panel->isPromptOpen());
    EXPECT_TRUE(panel->isHidden());
    EXPECT_EQ(h->view->viewport()->height(), viewportBefore);
    EXPECT_EQ(h->text(), QStringLiteral("abc"));
}

TEST_F(VimBehavior, SearchPromptAndHistory)
{
    h->open(QStringLiteral("|x foo foo bar"));
    h->keys(QStringLiteral("/fo"));
    vim::VimPanel* panel = h->vim->panel();
    ASSERT_TRUE(panel->isPromptOpen());
    EXPECT_EQ(panel->displayText(), QStringLiteral("/fo"));
    EXPECT_EQ(panel->findChild<QLabel*>(QStringLiteral("ocVimPanelDesc"))->text(), QStringLiteral("(JavaScript regexp)"));
    // incremental search highlights the matches without moving the cursor
    EXPECT_TRUE(h->vim->panel()->isPromptOpen());
    EXPECT_EQ(h->view->cursorPosition(), 0);
    h->keys(QStringLiteral("o<CR>"));
    EXPECT_EQ(h->view->cursorPosition(), 2);
    h->keys(QStringLiteral("/<Up>"));
    EXPECT_EQ(panel->displayText(), QStringLiteral("/foo"));
    EXPECT_EQ(h->view->cursorPosition(), 2); // the prompt keeps <Up> (not "k")
    h->keys(QStringLiteral("<CR>"));
    EXPECT_EQ(h->view->cursorPosition(), 6);
    // <BS> on an empty prompt closes it
    h->keys(QStringLiteral("/<BS>"));
    EXPECT_FALSE(panel->isPromptOpen());
}

TEST_F(VimBehavior, ErrorsShowInThePanel)
{
    h->open(QStringLiteral("|abc"));
    h->keys(QStringLiteral(":nosuchcommand<CR>"));
    vim::VimPanel* panel = h->vim->panel();
    EXPECT_FALSE(panel->isHidden());
    EXPECT_EQ(panel->displayText(), QStringLiteral("Not an editor command \":nosuchcommand\""));
    // a long notification waits for a key; <CR> only closes it
    h->keys(QStringLiteral(":registers<CR>"));
    EXPECT_TRUE(panel->displayText().startsWith(QStringLiteral("----------Registers----------")));
    h->keys(QStringLiteral("<CR>"));
    EXPECT_TRUE(panel->isHidden());
    EXPECT_EQ(h->text(), QStringLiteral("abc"));
}

TEST_F(VimBehavior, RecordingIndicator)
{
    h->open(QStringLiteral("|abc"));
    h->keys(QStringLiteral("qa"));
    EXPECT_EQ(h->vim->panel()->displayText(), QStringLiteral("(recording)[a]"));
    h->keys(QStringLiteral("xq"));
    EXPECT_TRUE(h->vim->panel()->isHidden());
}

TEST_F(VimBehavior, SubstituteConfirm)
{
    h->open(QStringLiteral("|a a a"));
    h->keys(QStringLiteral(":s/a/b/gc<CR>"));
    vim::VimPanel* panel = h->vim->panel();
    ASSERT_TRUE(panel->isPromptOpen());
    EXPECT_EQ(panel->displayText(), QStringLiteral("replace with b (y/n/a/q/l)"));
    h->keys(QStringLiteral("yn"));
    EXPECT_EQ(h->text(), QStringLiteral("b a a"));
    h->keys(QStringLiteral("y"));
    EXPECT_EQ(h->text(), QStringLiteral("b a b"));
    EXPECT_FALSE(panel->isPromptOpen()); // no more matches
    h->keys(QStringLiteral("u"));
    EXPECT_EQ(h->text(), QStringLiteral("b a a"));
}

// --- ex commands that reach Obsidian ------------------------------------------------------------

TEST_F(VimBehavior, WriteSavesAndQuitRunsTheCommand)
{
    h->open(QStringLiteral("|abc"));
    const QString path = h->dir.path() + QStringLiteral("/note%1.md").arg(h->fileNo);
    auto disk = [&] {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    h->keys(QStringLiteral("x:w<CR>"));
    EXPECT_EQ(disk(), QStringLiteral("bc"));
    QStringList ran;
    h->vim->setCommandRunner([&](const QString& id) {
        ran << id;
        return true;
    });
    h->keys(QStringLiteral(":w<CR>:q<CR>:wq<CR>:x<CR>"));
    EXPECT_EQ(ran, (QStringList{QStringLiteral("editor:save-file"), QStringLiteral("workspace:close"), QStringLiteral("editor:save-file"),
                                QStringLiteral("workspace:close"), QStringLiteral("editor:save-file"), QStringLiteral("workspace:close")}));
}

// --- display lines ----------------------------------------------------------------------------

TEST_F(VimBehavior, GjMovesByDisplayLine)
{
    h->view->resize(260, 400);
    h->view->setReadableLineLength(false);
    QString longLine;
    for (int i = 0; i < 40; ++i)
        longLine += QStringLiteral("word ");
    h->open(u'|' + longLine + QStringLiteral("\nnext"));
    QApplication::processEvents();
    h->keys(QStringLiteral("gj"));
    const qsizetype p = h->view->cursorPosition();
    EXPECT_GT(p, 0);
    EXPECT_LT(p, longLine.size()); // still on the first logical line
    h->keys(QStringLiteral("gk"));
    EXPECT_EQ(h->view->cursorPosition(), 0);
    h->keys(QStringLiteral("j"));
    EXPECT_EQ(h->view->document()->text().lineAt(h->view->cursorPosition()), 1);
}

TEST_F(VimBehavior, JSkipsFoldedLines)
{
    h->view->setFoldOptions(FoldOptions{true, true, 4});
    h->open(QStringLiteral("|# A\nx\ny\n# B\nz"));
    ASSERT_TRUE(h->view->foldLine(0));
    h->keys(QStringLiteral("j"));
    EXPECT_EQ(h->view->document()->text().lineAt(h->view->cursorPosition()), 3);
}

// --- multiple cursors, marks following edits ------------------------------------------------

TEST_F(VimBehavior, MarksFollowEdits)
{
    h->open(QStringLiteral("a\nb|\nc"));
    h->keys(QStringLiteral("maggOnew<Esc>G`a"));
    EXPECT_EQ(h->marked(), QStringLiteral("new\na\n|b\nc"));
    h->keys(QStringLiteral("dd'a"));
    // the mark's line was deleted: the mark is gone, the cursor stays
    EXPECT_EQ(h->marked(), QStringLiteral("new\na\n|c"));
}

TEST_F(VimBehavior, ClipboardRegisterReadsForeignText)
{
    h->open(QStringLiteral("|x"));
    h->clipboard = QStringLiteral("line\n");
    h->keys(QStringLiteral("\"+p"));
    EXPECT_EQ(h->marked(), QStringLiteral("x\n|line")); // text copied elsewhere ending in a newline is linewise
    h->keys(QStringLiteral("\"+yy"));
    EXPECT_EQ(h->clipboard, QStringLiteral("line\n"));
    h->keys(QStringLiteral("yy")); // the unnamed register doesn't touch the clipboard
    h->clipboard = QStringLiteral("other");
    h->keys(QStringLiteral("p"));
    EXPECT_EQ(h->text(), QStringLiteral("x\nline\nline"));
}

} // namespace
