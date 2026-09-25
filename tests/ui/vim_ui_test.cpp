// Vim mode in the app: the "vimMode" option toggles it live on every open markdown editor, Obsidian's
// hotkeys keep precedence over vim (like Obsidian's capture-phase keymap) while other keys reach vim,
// ":w" / ":q" run Obsidian commands, and screenshots (OC_SHOT_DIR) of the block cursor, a visual
// selection and the ":" panel in both themes.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "editor/markdown_commands.h"
#include "editor/vim/vim_controller.h"
#include "editor/vim/vim_keys.h"
#include "editor/vim/vim_panel.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>

using namespace oc;
using namespace oc::test;

namespace {

ThemeManager* themes()
{
    static ThemeManager* tm = [] {
        auto* t = new ThemeManager;
        t->setMode(ThemeManager::Mode::Dark);
        t->apply();
        return t;
    }();
    return tm;
}

// A key as the platform delivers it: ShortcutOverride then KeyPress (the app's HotkeyDispatcher
// filters both) to the focus widget.
void press(QWidget* target, const QString& key)
{
    int qtKey = 0;
    Qt::KeyboardModifiers mods;
    QString text;
    ASSERT_TRUE(vim::keyToQt(key, &qtKey, &mods, &text)) << key.toStdString();
    QKeyEvent so(QEvent::ShortcutOverride, qtKey, mods, text);
    QApplication::sendEvent(target, &so);
    QKeyEvent ev(QEvent::KeyPress, qtKey, mods, text);
    QApplication::sendEvent(target, &ev);
    QKeyEvent rel(QEvent::KeyRelease, qtKey, mods, text);
    QApplication::sendEvent(target, &rel);
}

void type(MarkdownView* mv, const QString& keys)
{
    for (const QString& k : vim::splitKeys(keys)) {
        VimController* vim = mv->vim();
        QWidget* target = vim && vim->panel()->isPromptOpen() ? static_cast<QWidget*>(vim->panel()->input()) : static_cast<QWidget*>(mv->editor());
        press(target, k);
    }
}

class VimUi : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"One.md", "# One\n\nfirst line of text\nsecond line\n"},
                {"Two.md", "alpha beta\ngamma\n"},
                {"Shots.md", "# Vim mode\n\nThe **block cursor** sits on a character in normal mode.\n- list item one\n- list item two\n\nA `code` span "
                             "and a [[link]].\n"}};
    }
    void SetUp() override
    {
        themes();
        mdcmd::settings() = mdcmd::Settings();
        VimController::resetGlobalState();
        AppFixture::SetUp();
        registry_ = std::make_unique<CommandRegistry>();
        session->setCommands(registry_.get());
    }
    void TearDown() override
    {
        AppFixture::TearDown();
        registry_.reset();
        VimController::resetGlobalState();
    }
    std::unique_ptr<CommandRegistry> registry_;
    void setVim(bool on)
    {
        session->vaultConfig()->set(QStringLiteral("vimMode"), on);
        QApplication::processEvents();
    }
};

TEST_F(VimUi, OptionTogglesVimOnEveryOpenEditor)
{
    MarkdownView* one = open(QStringLiteral("One.md"));
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->vim(), nullptr);
    EXPECT_EQ(one->editor()->cursorShape(), EditorView::CursorShape::Bar);
    Workspace* ws = window->workspace();
    ws->openFile(vault->file(QStringLiteral("Two.md")), Workspace::NewLeaf::SplitRight);
    auto* two = qobject_cast<MarkdownView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(two, nullptr);
    ASSERT_NE(one, two);

    setVim(true);
    ASSERT_NE(one->vim(), nullptr);
    ASSERT_NE(two->vim(), nullptr);
    EXPECT_EQ(VimController::of(one->editor()), one->vim());
    EXPECT_EQ(one->editor()->cursorShape(), EditorView::CursorShape::Block);
    two->editor()->setFocus();
    type(two, QStringLiteral("dw"));
    EXPECT_EQ(two->document()->text().line(0), QStringLiteral("beta"));

    // a note opened while vim is on gets it too; the document swap resets vim to normal mode
    type(two, QStringLiteral("i"));
    EXPECT_EQ(two->vim()->mode(), VimController::Mode::Insert);
    two->loadFile(vault->file(QStringLiteral("One.md")));
    EXPECT_EQ(two->vim()->mode(), VimController::Mode::Normal);

    setVim(false);
    EXPECT_EQ(one->vim(), nullptr);
    EXPECT_EQ(two->vim(), nullptr);
    EXPECT_EQ(one->editor()->cursorShape(), EditorView::CursorShape::Bar);
    EXPECT_EQ(VimController::of(one->editor()), nullptr);
    // plain typing again
    two->editor()->setFocus();
    two->editor()->setCursorPosition(0, false);
    type(two, QStringLiteral("x"));
    EXPECT_TRUE(two->document()->text().line(0).startsWith(u'x'));
}

TEST_F(VimUi, ObsidianHotkeysKeepPrecedenceOverVim)
{
    // Obsidian resolves its hotkeys in a capture-phase keydown listener, before CodeMirror-vim: a
    // bound command wins (Mod+P, and also Mod+O / Mod+E / Mod+D over vim's <C-o> <C-e> <C-d>),
    // keys bound to nothing reach vim (<C-r>, <C-v>, <C-a>, <C-x>, <C-[>, plain keys).
    int palette = 0, deleteParagraph = 0;
    CommandRegistry* commands = session->commands();
    Command p;
    p.id = QStringLiteral("command-palette:open");
    p.name = QStringLiteral("Open command palette");
    p.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+P"))};
    p.callback = [&] { ++palette; };
    commands->add(p);
    Command d;
    d.id = QStringLiteral("editor:delete-paragraph");
    d.name = QStringLiteral("Delete paragraph");
    d.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+D"))};
    d.callback = [&] { ++deleteParagraph; };
    commands->add(d);
    auto* hotkeys = new HotkeyDispatcher(commands, window);
    hotkeys->setWindow(window);

    MarkdownView* mv = open(QStringLiteral("Two.md"), EditorView::Mode::Source);
    ASSERT_NE(mv, nullptr);
    setVim(true);
    mv->editor()->setFocus();
    mv->editor()->setCursorPosition(0, false);
    const QString before = mv->document()->text().toString();

    type(mv, QStringLiteral("<C-p>"));
    EXPECT_EQ(palette, 1);
    type(mv, QStringLiteral("<C-d>")); // not vim's half-page scroll: Obsidian's Mod+D
    EXPECT_EQ(deleteParagraph, 1);
    EXPECT_EQ(mv->document()->text().toString(), before);

    type(mv, QStringLiteral("xu<C-r>"));
    EXPECT_EQ(mv->document()->text().line(0), QStringLiteral("lpha beta"));
    type(mv, QStringLiteral("<C-v>j"));
    EXPECT_EQ(mv->vim()->mode(), VimController::Mode::VisualBlock);
    type(mv, QStringLiteral("<C-[>"));
    EXPECT_EQ(mv->vim()->mode(), VimController::Mode::Normal);
    type(mv, QStringLiteral("i1<Esc><C-a>")); // (on the second line, where <C-v>j went)
    EXPECT_EQ(mv->document()->text().line(1), QStringLiteral("2gamma"));
    delete hotkeys;
}

TEST_F(VimUi, ColonWriteAndQuitRunObsidianCommands)
{
    int saves = 0, closes = 0;
    CommandRegistry* commands = session->commands();
    Command s;
    s.id = QStringLiteral("editor:save-file");
    s.name = QStringLiteral("Save current file");
    s.callback = [&] { ++saves; };
    commands->add(s);
    Command c;
    c.id = QStringLiteral("workspace:close");
    c.name = QStringLiteral("Close current tab");
    c.callback = [&] { ++closes; };
    commands->add(c);
    MarkdownView* mv = open(QStringLiteral("Two.md"), EditorView::Mode::Source);
    setVim(true);
    mv->editor()->setFocus();
    type(mv, QStringLiteral(":w<CR>"));
    QApplication::processEvents(); // commands run after the key
    EXPECT_EQ(saves, 1);
    type(mv, QStringLiteral(":wq<CR>"));
    QApplication::processEvents();
    EXPECT_EQ(saves, 2);
    EXPECT_EQ(closes, 1);
}

TEST_F(VimUi, LivePreviewRevealsUnderTheBlockCursor)
{
    MarkdownView* mv = open(QStringLiteral("Shots.md"), EditorView::Mode::LivePreview);
    setVim(true);
    EditorView* e = mv->editor();
    e->setFocus();
    const Text& t = mv->document()->text();
    const int line = 2;
    const qsizetype bold = t.lineStart(line) + t.line(line).indexOf(QStringLiteral("**block"));
    // off the bold span: its markers are hidden, the cursor box is on a visible character
    e->setCursorPosition(t.lineStart(line), false);
    const QRectF away = e->blockCursorRect(e->selection().main());
    EXPECT_GT(away.width(), 0);
    // "f*" onto the markers reveals them, like the bar cursor does
    type(mv, QStringLiteral("f*"));
    EXPECT_EQ(e->cursorPosition(), bold);
    const QRectF on = e->blockCursorRect(e->selection().main());
    EXPECT_GT(on.width(), 0);
    EXPECT_NEAR(on.left(), e->caretRectAt(bold).left(), 0.01);
    EXPECT_LT(e->caretRectAt(bold).left(), e->caretRectAt(bold + 2).left()); // "**" shown
}

// --- screenshots ---------------------------------------------------------------------------------

QString shotDir() { return QString::fromLocal8Bit(qgetenv("OC_SHOT_DIR")); }

class VimShots : public VimUi, public ::testing::WithParamInterface<bool> {
protected:
    void SetUp() override
    {
        if (shotDir().isEmpty())
            GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
        themes()->setMode(GetParam() ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        themes()->apply();
        VimUi::SetUp();
        window->resize(1100, 560);
    }
    void TearDown() override
    {
        if (!shotDir().isEmpty()) {
            VimUi::TearDown();
            themes()->setMode(ThemeManager::Mode::Dark);
            themes()->apply();
        }
    }
    bool save(const QString& name)
    {
        QApplication::processEvents();
        window->repaint();
        return window->grab().save(shotDir() + u'/' + name + u'-' + (GetParam() ? QStringLiteral("dark") : QStringLiteral("light")) + u".png");
    }
};

TEST_P(VimShots, NormalVisualAndCommandLine)
{
    MarkdownView* mv = open(QStringLiteral("Shots.md"), EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    setVim(true);
    window->activateWindow();
    (void)QTest::qWaitForWindowActive(window);
    mv->editor()->setFocus();
    type(mv, QStringLiteral("2jw"));
    EXPECT_TRUE(save(QStringLiteral("vim-normal")));
    type(mv, QStringLiteral("vjee"));
    EXPECT_TRUE(save(QStringLiteral("vim-visual")));
    type(mv, QStringLiteral("<Esc>:%s/item/entry/g"));
    EXPECT_TRUE(save(QStringLiteral("vim-command-line")));
    type(mv, QStringLiteral("<Esc>/list"));
    EXPECT_TRUE(save(QStringLiteral("vim-search")));
    type(mv, QStringLiteral("<Esc>:nosuch<CR>"));
    EXPECT_TRUE(save(QStringLiteral("vim-error")));
}

INSTANTIATE_TEST_SUITE_P(Themes, VimShots, ::testing::Values(true, false), [](const auto& info) { return info.param ? "dark" : "light"; });

} // namespace
