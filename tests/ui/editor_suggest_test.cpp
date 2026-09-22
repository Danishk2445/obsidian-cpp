// Editor suggest popups in a real window: "[[" link suggestions (typing opens, Enter / Tab / "#"
// accept, Escape closes), "#" tags, "[^" footnotes, the hotkey dispatcher leaving the keys alone.
#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}
} // namespace

class EditorSuggestTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        root = tmp.path() + "/vault";
        writeFile(root + "/Welcome.md", "Start\n");
        writeFile(root + "/Other.md", "# Head\ntext\n## Sub\n");
        writeFile(root + "/Folder/Deep note.md", "#tag and #tag/nested\n");
        writeFile(root + "/Notes.md", "Footnote[^1] here.\n\n[^1]: The first one.\n");
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = new Vault(root);
        docs = new DocumentManager(vault);
        meta = new MetadataCache;
        session->setVault(vault);
        session->setDocuments(docs);
        session->setMetadata(meta);
        bindVaultServices(session.get());
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 10000));

        window = new MainWindow(session.get(), false);
        registry = std::make_unique<ViewRegistry>();
        registerCoreViews(*registry);
        registerMarkdownView(*registry);
        session->setViewRegistry(registry.get());
        auto* ws = new Workspace(session.get(), registry.get(), window);
        session->setWorkspace(ws);
        window->setWorkspace(ws);
        window->resize(1200, 800);
        window->show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
    }
    void TearDown() override
    {
        delete window;
        delete docs;
        delete meta;
        delete vault;
        session.reset();
    }
    MarkdownView* open(const QString& path)
    {
        Workspace* ws = window->workspace();
        ws->openFile(vault->file(path), Workspace::NewLeaf::No);
        auto* mv = qobject_cast<MarkdownView*>(ws->activeRootLeaf()->view());
        if (mv) {
            mv->editorSuggest()->setRequireFocus(false);
            mv->editor()->setFocus();
        }
        return mv;
    }
    static void typeAtEnd(EditorView* e, const QString& text)
    {
        e->setCursorPosition(e->document()->text().length());
        for (const QChar c : text)
            e->insertText(QString(c));
    }
    static QString lastLine(EditorView* e)
    {
        const Text& t = e->document()->text();
        return t.line(t.lineCount() - 1);
    }
    static QStringList titles(EditorSuggest* s)
    {
        QStringList out;
        for (const Suggestion& x : s->popup()->list()->suggestions())
            out << x.title;
        return out;
    }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    Vault* vault = nullptr;
    DocumentManager* docs = nullptr;
    MetadataCache* meta = nullptr;
    std::unique_ptr<ViewRegistry> registry;
    MainWindow* window = nullptr;
};

TEST_F(EditorSuggestTest, LinkSuggestEnter)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "See [[Oth");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    ASSERT_TRUE(s->popup()->isVisible());
    ASSERT_FALSE(titles(s).isEmpty());
    EXPECT_EQ(titles(s).first(), "Other");
    EXPECT_EQ(s->popup()->list()->selectedIndex(), 0);
    // the popup sits below the typed link, inside the window
    const QRect g = s->popup()->geometry();
    EXPECT_TRUE(window->rect().contains(g));
    if (const QByteArray dir = qgetenv("OC_SHOT_DIR"); !dir.isEmpty())
        window->grab().save(QString::fromLocal8Bit(dir) + "/editor_suggest.png");
    QTest::keyClick(e, Qt::Key_Return);
    EXPECT_FALSE(s->isOpen());
    EXPECT_EQ(lastLine(e), "See [[Other]]");
    EXPECT_EQ(e->cursorPosition(), e->document()->text().length());
}

TEST_F(EditorSuggestTest, EmptyQueryHasNoSelectionAndEscapeCloses)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "[[");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    EXPECT_EQ(s->popup()->list()->selectedIndex(), -1);
    QTest::keyClick(e, Qt::Key_Down);
    EXPECT_EQ(s->popup()->list()->selectedIndex(), 0);
    QTest::keyClick(e, Qt::Key_Escape);
    EXPECT_FALSE(s->isOpen());
    EXPECT_FALSE(s->popup()->isVisible());
    EXPECT_EQ(lastLine(e), "[[");
    // moving the cursor doesn't reopen it; typing does
    e->setCursorPosition(0);
    QTest::qWait(120);
    EXPECT_FALSE(s->isOpen());
    typeAtEnd(e, "W");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    // leaving the link closes it
    e->setCursorPosition(0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !s->isOpen(); }, 2000));
}

TEST_F(EditorSuggestTest, HashContinuesWithHeadings)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "[[Oth");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    QTest::keyClick(e, '#');
    EXPECT_EQ(lastLine(e), "[[Other#]]");
    // the cursor sits before "]]" and the heading suggestions open
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    ASSERT_FALSE(titles(s).isEmpty());
    EXPECT_EQ(titles(s).first(), "Head");
    QTest::keyClick(e, Qt::Key_Down);
    QTest::keyClick(e, Qt::Key_Return);
    EXPECT_EQ(lastLine(e), "[[Other#Sub]]");
}

TEST_F(EditorSuggestTest, TabKeepsCursorInsideLink)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "[[deep");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    EXPECT_EQ(titles(s).first(), "Deep note");
    EXPECT_EQ(s->popup()->list()->suggestions().first().note, "Folder/");
    QTest::keyClick(e, Qt::Key_Tab);
    EXPECT_EQ(lastLine(e), "[[Deep note]]");
    EXPECT_EQ(e->cursorPosition(), e->document()->text().length() - 2);
}

TEST_F(EditorSuggestTest, ShiftEnterLinksTypedText)
{
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "[[Brand new");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    QTest::keyClick(e, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(lastLine(e), "[[Brand new]]");
}

TEST_F(EditorSuggestTest, Footnotes)
{
    MarkdownView* mv = open("Notes.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "\nMore[^");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    const QStringList t = titles(s);
    ASSERT_EQ(t.size(), 2);
    EXPECT_EQ(t[0], "[^2]: New footnote..."); // "create" first (score 1)
    EXPECT_EQ(t[1], "[^1]: The first one.");
    QTest::keyClick(e, Qt::Key_Down);
    QTest::keyClick(e, Qt::Key_Return);
    EXPECT_EQ(lastLine(e), "More[^1]");
}

TEST_F(EditorSuggestTest, Tags)
{
    const md::CachedMetadata* m = meta->fileCache("Folder/Deep note.md");
    ASSERT_TRUE(m && !m->tags.isEmpty());
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, " #nes");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    EXPECT_EQ(titles(s).first(), "tag/nested");
    QTest::keyClick(e, Qt::Key_Return);
    EXPECT_EQ(lastLine(e), " #tag/nested ");
}

TEST_F(EditorSuggestTest, HotkeysYieldToPopup)
{
    // A command bound to Tab/Enter must not steal the keys while the popup is open.
    auto registryOwner = std::make_unique<CommandRegistry>();
    CommandRegistry* reg = registryOwner.get();
    int runs = 0;
    Command c;
    c.id = "test:tab";
    c.name = "Tab command";
    c.callback = [&] { ++runs; };
    c.defaultHotkeys = {Hotkey::parse("Tab")};
    reg->add(c);
    HotkeyDispatcher dispatcher(reg);
    dispatcher.setWindow(window);
    MarkdownView* mv = open("Welcome.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    EditorSuggest* s = mv->editorSuggest();
    typeAtEnd(e, "[[Oth");
    ASSERT_TRUE(QTest::qWaitFor([&] { return s->isOpen(); }, 2000));
    QTest::keyClick(e, Qt::Key_Tab);
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(lastLine(e), "[[Other]]");
}
