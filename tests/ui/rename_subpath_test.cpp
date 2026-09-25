// "Rename this heading..." / "Rename this block ID...": the note changes and links to it follow.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "ui/overlay/text_prompt_modal.h"
#include "ui/plugins/rename_subpath.h"
#include "ui/workspace/file_menu.h"

#include <QPlainTextEdit>

using namespace oc;
using namespace oc::test;
using namespace oc::rename_subpath;

TEST(RenameSubpath, Normalisation)
{
    EXPECT_EQ(normalizeHeading("Old: heading!  (v2)"), "Old heading v2");
    EXPECT_EQ(sanitizeSubpath("a|b#c^d [[x]] 100%"), "a b c d x 100%");
    EXPECT_EQ(headingTextOf("## Some heading"), "Some heading");
    EXPECT_TRUE(headingTextOf("##NoSpace").isNull());
    EXPECT_TRUE(headingTextOf("text").isNull());
    EXPECT_EQ(validateBlockId("ok-id-2"), "");
    EXPECT_EQ(validateBlockId(""), "Block ID cannot be empty");
    EXPECT_EQ(validateBlockId("a b"), "Block ID can only contain alphanumeric characters or dash");
}

class RenameSubpathTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", "# Intro\n\nSee [[#Old heading]] here.\n\n## Old heading\n\ntext ^blk1\n\n### Lonely\n"},
                {"Other.md", "[[Note#Old heading]], [[Note#old: HEADING|shown]], [[Note#^blk1]], [[Note#Intro]]\n[md](Note.md#Old%20heading)\n"},
                {"Far.md", "no links here\n"}};
    }
    void SetUp() override
    {
        AppFixture::SetUp();
        commands = new CommandRegistry(session.get());
        session->setCommands(commands);
        registerCommands(*commands, window);
    }
    void flush()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
    CommandRegistry* commands = nullptr;
};

TEST_F(RenameSubpathTest, HeadingRenameUpdatesLinksEverywhere)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    // the command needs the cursor on a heading
    mv->editor()->setCursorPosition(0);
    commands->setEditorProvider([mv] { return EditorContext{mv->editor(), mv}; });
    EXPECT_TRUE(commands->isAvailable("editor:rename-heading"));
    mv->editor()->setCursorPosition(10);
    EXPECT_FALSE(commands->isAvailable("editor:rename-heading"));

    TextPromptModal* m = promptHeadingRename(window, mv, 4);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->title(), "Rename heading");
    EXPECT_EQ(m->input()->toPlainText(), "Old heading");
    m->input()->setPlainText("");
    EXPECT_EQ(m->errorMessage(), "Heading cannot be empty");
    m->input()->setPlainText("New | heading");
    ASSERT_TRUE(m->submit());
    flush();
    // the note: heading text and its own link, in one change
    EXPECT_EQ(mv->document()->text().toString().toStdString(),
              "# Intro\n\nSee [[#New heading]] here.\n\n## New | heading\n\ntext ^blk1\n\n### Lonely\n");
    // other notes (closed): links to the heading follow, keeping aliases; others untouched
    EXPECT_EQ(readFile(root + "/Other.md").toStdString(),
              "[[Note#New heading]], [[Note#New heading|shown]], [[Note#^blk1]], [[Note#Intro]]\n[md](Note.md#New%20heading)\n");
    EXPECT_EQ(readFile(root + "/Far.md"), "no links here\n");
}

TEST_F(RenameSubpathTest, HeadingWithoutLinksOnlyChangesTheLine)
{
    MarkdownView* mv = open("Note.md");
    const QByteArray other = readFile(root + "/Other.md");
    TextPromptModal* m = promptHeadingRename(window, mv, 8);
    ASSERT_TRUE(m);
    m->input()->setPlainText("Alone");
    ASSERT_TRUE(m->submit());
    flush();
    EXPECT_TRUE(mv->document()->text().toString().endsWith("### Alone\n"));
    // an undoable editor change
    mv->editor()->undo();
    EXPECT_TRUE(mv->document()->text().toString().endsWith("### Lonely\n"));
    EXPECT_EQ(readFile(root + "/Other.md"), other);
}

TEST_F(RenameSubpathTest, BlockIdRename)
{
    MarkdownView* mv = open("Note.md");
    const QString text = mv->document()->text().toString();
    const qsizetype caret = text.indexOf("^blk1") + 2;
    qsizetype from = 0, to = 0;
    ASSERT_TRUE(blockIdAt(mv, caret, &from, &to));
    EXPECT_EQ(text.mid(from, to - from), "^blk1");
    EXPECT_FALSE(blockIdAt(mv, 3, &from, &to));
    TextPromptModal* m = promptBlockIdRename(window, mv, text.indexOf("^blk1"), text.indexOf("^blk1") + 5);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->input()->toPlainText(), "blk1");
    m->input()->setPlainText("bad id");
    EXPECT_FALSE(m->submit());
    m->input()->setPlainText("block-2");
    ASSERT_TRUE(m->submit());
    flush();
    EXPECT_TRUE(mv->document()->text().toString().contains("text ^block-2\n"));
    EXPECT_TRUE(readFile(root + "/Other.md").contains("[[Note#^block-2]]"));
}

TEST_F(RenameSubpathTest, EditorMenuOffersTheRenames)
{
    MarkdownView* mv = open("Note.md");
    QList<MenuEntry> items;
    mv->editor()->setCursorPosition(mv->document()->text().toString().indexOf("## Old heading") + 4);
    EditorMenuHooks::collect(items, mv, mv->editor()->cursorPosition());
    bool heading = false;
    for (const MenuEntry& e : items)
        heading |= e.title == "Rename this heading...";
    EXPECT_TRUE(heading);
    items.clear();
    mv->editor()->setCursorPosition(mv->document()->text().toString().indexOf("^blk1") + 1);
    EditorMenuHooks::collect(items, mv, mv->editor()->cursorPosition());
    bool block = false;
    for (const MenuEntry& e : items)
        block |= e.title == "Rename this block ID...";
    EXPECT_TRUE(block);
}
