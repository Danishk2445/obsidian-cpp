// Note composer: insertIntoFile, heading ranges, merge (links follow) and extract (links into the moved
// section follow; the source keeps a link), plus the commands and pickers.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/files/file_manager.h"
#include "core/plugins/note_composer.h"
#include "markdown/metadata.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/note_composer_ui.h"

#include <QCoreApplication>

using namespace oc;
using namespace oc::test;

namespace {

QString applied(const QString& note, const QString& text, bool prepend)
{
    Text t = Text::fromString(note);
    QList<ChangeSpec> specs = FileManager::insertIntoFileChanges(t, text, prepend);
    std::sort(specs.begin(), specs.end(), [](const ChangeSpec& a, const ChangeSpec& b) { return a.from > b.from; });
    for (const ChangeSpec& c : specs)
        t = t.replace(c.from, c.to, c.insert);
    return t.toString();
}

} // namespace

TEST(NoteComposer, InsertIntoFileJoinsWithABlankLine)
{
    EXPECT_EQ(applied("A", "B", false), "A\n\nB");
    EXPECT_EQ(applied("A\n", "B", false), "A\n\nB");
    EXPECT_EQ(applied("A\n\n", "B", false), "A\n\nB");
    EXPECT_EQ(applied("A", "\nB", false), "A\n\nB");
    EXPECT_EQ(applied("", "B", false), "B");
    EXPECT_EQ(applied("A", "", false), "A");
    EXPECT_EQ(applied("A", "B", true), "B\n\nA");
    // Prepending goes after the frontmatter.
    EXPECT_EQ(applied("---\nx: 1\n---\nBody", "New", true), "---\nx: 1\n---\nNew\n\nBody");
    // Inserted properties are added to the note's (existing keys win) or create its frontmatter.
    EXPECT_EQ(applied("---\nx: 1\n---\nBody", "---\ny: 2\nx: 9\n---\nMore", false), "---\nx: 1\ny: 2\n---\nBody\n\nMore");
    EXPECT_EQ(applied("Body", "---\ntags: [a]\n---\nMore", false), "---\ntags: [a]\n---\nBody\n\nMore");
}

TEST(NoteComposer, HeadingRangeStopsAtTheNextSameLevelHeading)
{
    const QString text = "# A\nintro\n## B\nb text\n### C\nc\n\n\n## D\nd\n";
    md::CachedMetadata meta;
    meta.headings = {{"A", 1, {{0, 0, 0}, {0, 3, 3}}},
                     {"B", 2, {{2, 0, 10}, {2, 4, 14}}},
                     {"C", 3, {{4, 0, 22}, {4, 5, 27}}},
                     {"D", 2, {{8, 0, 32}, {8, 4, 36}}}};
    const auto b = plugins::headingRangeAt(meta, text, 2);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->heading, "B");
    EXPECT_EQ(text.mid(b->start, b->end - b->start), "## B\nb text\n### C\nc\n");
    const auto a = plugins::headingRangeAt(meta, text, 0);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->end, text.size());
    EXPECT_FALSE(plugins::headingRangeAt(meta, text, 1));
    EXPECT_EQ(plugins::headingTextOf("## Title here").value_or("-"), "Title here");
    EXPECT_FALSE(plugins::headingTextOf("#tag"));
}

TEST(NoteComposer, TemplateVariables)
{
    const QDateTime now(QDate(2024, 5, 6), QTime(7, 8));
    EXPECT_EQ(plugins::applyComposerTemplate("From [[{{fromTitle}}]] to {{newTitle}} on {{date}}", "BODY", "Src", "Dst", now),
              "From [[Src]] to Dst on 2024-05-06\n\nBODY");
    EXPECT_EQ(plugins::applyComposerTemplate("> {{content}}", "x", "a", "b", now), "> x");
}

class NoteComposerTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {
            {"Source.md", "# Source\nKeep this.\nSee [[#Moving]] and [[Source#^blk]].\n\n## Moving\nMoved text ^blk\n\nLink to [[Other]] and [[#Moving]].\n\n## Staying\nstays\n"},
            {"Other.md", "Refs: [[Source#Moving]], [[Source#^blk]], [[Source#Staying]], [[Source]].\n"},
            {"Into.md", "Into body\n"},
            {"Sub/Deep.md", "deep [[Source]]\n"},
        };
    }
    void SetUp() override
    {
        AppFixture::SetUp();
        session->vaultConfig()->set("trashOption", "none");
        session->vaultConfig()->set("alwaysUpdateLinks", true);
    }
    QString disk(const QString& path) { return QString::fromUtf8(readFile(root + "/" + path)); }
};

TEST_F(NoteComposerTest, MergeMovesContentAndLinks)
{
    QString err;
    ASSERT_TRUE(plugins::mergeNotes(session.get(), vault->file("Source.md"), vault->file("Into.md"), false, &err)) << err.toStdString();
    EXPECT_FALSE(vault->file("Source.md"));
    EXPECT_FALSE(QFile::exists(root + "/Source.md"));
    const QString into = disk("Into.md");
    EXPECT_TRUE(into.startsWith("Into body\n\n# Source\nKeep this.")) << into.toStdString();
    // Links to the merged note follow it, subpaths included.
    EXPECT_EQ(disk("Other.md"), "Refs: [[Into#Moving]], [[Into#^blk]], [[Into#Staying]], [[Into]].\n");
    EXPECT_EQ(disk("Sub/Deep.md"), "deep [[Into]]\n");
}

TEST_F(NoteComposerTest, MergeAtTop)
{
    ASSERT_TRUE(plugins::mergeNotes(session.get(), vault->file("Other.md"), vault->file("Into.md"), true));
    EXPECT_TRUE(disk("Into.md").startsWith("Refs: [[Source#Moving]]")) << disk("Into.md").toStdString();
    EXPECT_TRUE(disk("Into.md").endsWith("\n\nInto body\n"));
}

TEST_F(NoteComposerTest, ExtractSectionKeepsLinksWorking)
{
    const QString src = disk("Source.md");
    const qsizetype from = src.indexOf("## Moving"), to = src.indexOf("## Staying") - 1; // up to the blank line
    TFile* dest = fileManager->createNewMarkdownFile(nullptr, "New");
    ASSERT_TRUE(dest);
    QString err;
    ASSERT_TRUE(plugins::extractToNote(session.get(), vault->file("Source.md"), from, to, dest, false, &err)) << err.toStdString();
    // The moved text: its links into itself stay local, other links still resolve.
    EXPECT_EQ(disk("New.md").toStdString(), "## Moving\nMoved text ^blk\n\nLink to [[Other]] and [[#Moving]].\n");
    // The source keeps a link to the new note, and its own links into the moved part follow it.
    EXPECT_EQ(disk("Source.md").toStdString(), "# Source\nKeep this.\nSee [[New#Moving]] and [[New#^blk]].\n\n[[New]]\n## Staying\nstays\n");
    // Other notes' links into the moved part follow; the rest stays.
    EXPECT_EQ(disk("Other.md").toStdString(), "Refs: [[New#Moving]], [[New#^blk]], [[Source#Staying]], [[Source]].\n");
}

TEST_F(NoteComposerTest, ExtractWithEmbedReplacementAndTemplate)
{
    writeFile(root + "/Tpl.md", "From [[{{fromTitle}}]]:\n{{content}}");
    vault->rescan();
    ASSERT_TRUE(QTest::qWaitFor([&] { return vault->file("Tpl.md") != nullptr; }, 5000));
    session->config()->setObject("note-composer", QJsonObject{{"replacementText", "embed"}, {"template", "Tpl"}});
    const QString src = disk("Into.md");
    TFile* dest = fileManager->createNewMarkdownFile(nullptr, "Piece");
    ASSERT_TRUE(plugins::extractToNote(session.get(), vault->file("Into.md"), 0, 4, dest, false));
    EXPECT_EQ(disk("Piece.md"), "From [[Into]]:\nInto");
    EXPECT_EQ(disk("Into.md"), "![[Piece]] body\n");
}

TEST_F(NoteComposerTest, ExtractThroughTheOpenEditorIsUndoable)
{
    MarkdownView* mv = open("Into.md");
    ASSERT_TRUE(mv);
    TFile* dest = fileManager->createNewMarkdownFile(nullptr, "Piece");
    ASSERT_TRUE(plugins::extractToNote(session.get(), vault->file("Into.md"), 5, 9, dest, false));
    EXPECT_EQ(mv->editor()->document()->text().toString(), "Into [[Piece]]\n");
    EXPECT_EQ(disk("Into.md"), "Into [[Piece]]\n");
    EXPECT_EQ(disk("Piece.md"), "body");
}

TEST_F(NoteComposerTest, CommandsAndMergePicker)
{
    CommandRegistry commands;
    registerNoteComposerCommands(commands, window);
    ASSERT_TRUE(commands.find("note-composer:merge-file"));
    ASSERT_TRUE(commands.find("note-composer:split-file"));
    ASSERT_TRUE(commands.find("note-composer:extract-heading"));

    MarkdownView* mv = open("Other.md");
    ASSERT_TRUE(mv);
    session->config()->setObject("note-composer", QJsonObject{{"askBeforeMerging", true}});
    SuggestModal* picker = openMergeModal(window, vault->file("Other.md"));
    ASSERT_TRUE(picker);
    picker->setQuery("Into");
    ASSERT_FALSE(picker->suggestions().isEmpty());
    EXPECT_EQ(picker->suggestions()[0].title, "Into");
    picker->chooseSelected();
    // The confirmation: "Merge" merges and opens the destination.
    QCoreApplication::processEvents();
    Modal* confirm = nullptr;
    for (Modal* m : window->overlay()->findChildren<Modal*>())
        if (m->isOpen())
            confirm = m;
    ASSERT_TRUE(confirm);
    EXPECT_EQ(confirm->title(), "Merge file");
    confirm->button(0)->click();
    EXPECT_FALSE(vault->file("Other.md"));
    EXPECT_TRUE(disk("Into.md").startsWith("Into body\n\nRefs:"));
    auto* now = qobject_cast<MarkdownView*>(window->workspace()->activeRootLeaf()->view());
    ASSERT_TRUE(now);
    EXPECT_EQ(now->file()->path(), "Into.md");
}

TEST_F(NoteComposerTest, ExtractHeadingCommandSelectsTheSection)
{
    CommandRegistry commands;
    registerNoteComposerCommands(commands, window);
    MarkdownView* mv = open("Source.md");
    ASSERT_TRUE(mv);
    commands.setEditorProvider([mv] { return EditorContext{mv->editor(), mv}; });
    const Text& t = mv->editor()->document()->text();
    mv->editor()->setSelection(Selection::cursor(t.lineStart(4))); // "## Moving"
    ASSERT_TRUE(commands.isAvailable("note-composer:extract-heading"));
    commands.execute("note-composer:extract-heading");
    SuggestModal* picker = window->overlay()->findChild<SuggestModal*>();
    ASSERT_TRUE(picker);
    EXPECT_EQ(picker->query(), "Moving");
    const SelRange sel = mv->editor()->selection().main();
    EXPECT_EQ(t.slice(sel.from(), sel.to()), "## Moving\nMoved text ^blk\n\nLink to [[Other]] and [[#Moving]].\n");
    // Mod+Enter creates a note named like the query and moves the section there.
    picker->chooseSelected(Qt::ControlModifier);
    ASSERT_TRUE(vault->file("Moving.md"));
    EXPECT_TRUE(disk("Moving.md").startsWith("## Moving\n"));
    EXPECT_TRUE(disk("Source.md").contains("[[Moving]]\n## Staying"));
}
