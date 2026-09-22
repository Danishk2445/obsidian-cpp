// Daily notes / Templates / Unique note / Random note commands in a real window.
#include "app_test_support.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/time/moment_format.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/note_plugins_ui.h"

#include <QJsonObject>

using namespace oc;
using namespace oc::test;

class NotePluginsTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", "---\nstatus: done\n---\nFirst line\n"},
                {"Templates/Meeting.md", "---\nstatus: new\ntags: [meeting]\n---\n## {{title}} {{date:YYYY}}\n"},
                {"Templates/Daily.md", "# {{title}}\n"},
                {"Journal/2026-01-01.md", "old\n"},
                {"Journal/2026-01-05.md", "newer\n"},
                {"Journal/notes.md", "not a daily note\n"}};
    }
    void SetUp() override
    {
        AppFixture::SetUp();
        registry = std::make_unique<CommandRegistry>();
        session->setCommands(registry.get());
        session->config()->setObject("daily-notes", QJsonObject{{"folder", "Journal"}, {"template", "Templates/Daily"}});
        session->config()->setObject("templates", QJsonObject{{"folder", "Templates"}});
        session->config()->setObject("core-plugins", QJsonObject{{"zk-prefixer", true}, {"random-note", true}});
        registerNotePluginCommands(*registry, window);
        registry->setEditorProvider([this]() -> EditorContext {
            WorkspaceLeaf* l = window->workspace()->activeRootLeaf();
            auto* mv = l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
            return mv ? EditorContext{mv->editor(), mv} : EditorContext{};
        });
    }
    void TearDown() override
    {
        session->setCommands(nullptr);
        registry.reset();
        AppFixture::TearDown();
    }
    TFile* activeFile() const
    {
        WorkspaceLeaf* l = window->workspace()->activeRootLeaf();
        auto* fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
        return fv ? fv->file() : nullptr;
    }
    std::unique_ptr<CommandRegistry> registry;
};

TEST_F(NotePluginsTest, DailyNoteCreatedFromTemplate)
{
    ASSERT_TRUE(registry->execute("daily-notes"));
    const QString name = formatMoment(QDateTime::currentDateTime(), "YYYY-MM-DD");
    TFile* f = vault->file("Journal/" + name + ".md");
    ASSERT_TRUE(f);
    EXPECT_EQ(readFile(root + "/Journal/" + name + ".md"), ("# " + name + "\n").toUtf8());
    EXPECT_EQ(activeFile(), f);
    // running it again opens the same note
    ASSERT_TRUE(registry->execute("daily-notes"));
    EXPECT_EQ(activeFile(), f);
}

TEST_F(NotePluginsTest, PreviousAndNextDailyNote)
{
    EXPECT_FALSE(registry->isAvailable("daily-notes:goto-prev")); // no daily note active
    open("Journal/2026-01-05.md");
    ASSERT_TRUE(registry->isAvailable("daily-notes:goto-prev"));
    ASSERT_TRUE(registry->execute("daily-notes:goto-prev"));
    EXPECT_EQ(activeFile()->path(), "Journal/2026-01-01.md");
    ASSERT_TRUE(registry->execute("daily-notes:goto-next"));
    EXPECT_EQ(activeFile()->path(), "Journal/2026-01-05.md");
    ASSERT_TRUE(registry->execute("daily-notes:goto-next")); // none after: a notice, stays
    EXPECT_EQ(activeFile()->path(), "Journal/2026-01-05.md");
    open("Journal/notes.md");
    EXPECT_FALSE(registry->isAvailable("daily-notes:goto-next"));
}

TEST_F(NotePluginsTest, InsertTemplateMergesProperties)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setCursorPosition(e->document()->text().length());
    ASSERT_TRUE(registry->execute("insert-template"));
    auto* modal = window->overlay()->findChild<SuggestModal*>();
    ASSERT_TRUE(modal);
    QStringList titles;
    for (const Suggestion& s : modal->suggestions())
        titles << s.title;
    EXPECT_EQ(titles, (QStringList{"Daily", "Meeting"}));
    modal->setQuery("meet");
    ASSERT_EQ(modal->suggestions().size(), 1);
    modal->chooseSelected();
    const QString year = QString::number(QDate::currentDate().year());
    EXPECT_EQ(e->document()->text().toString(), "---\nstatus: done\ntags: [meeting]\n---\nFirst line\n## Note " + year + "\n");
}

TEST_F(NotePluginsTest, InsertDateAndTime)
{
    session->config()->setObject("templates", QJsonObject{{"folder", "Templates"}, {"dateFormat", "DD/MM"}});
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    e->setCursorPosition(e->document()->text().length());
    ASSERT_TRUE(registry->execute("insert-current-date"));
    EXPECT_EQ(lastLine(e), formatMoment(QDateTime::currentDateTime(), "DD/MM"));
}

TEST_F(NotePluginsTest, UniqueAndRandomNotes)
{
    ASSERT_TRUE(registry->execute("zk-prefixer"));
    TFile* f = activeFile();
    ASSERT_TRUE(f);
    EXPECT_TRUE(QRegularExpression("^\\d{12}\\.md$").match(f->path()).hasMatch()) << qPrintable(f->path());
    ASSERT_TRUE(registry->execute("random-note"));
    ASSERT_TRUE(activeFile());
    EXPECT_TRUE(activeFile()->isMarkdown());
}
