// Files & links → "Default file to open" (app.json "openBehavior"), Obsidian's runOpeningBehavior(): the restored tabs,
// a new note unless the active one is empty, today's daily note, or a specific file (case-insensitive, ".md" implied).
#include "ui/navigation_test_support.h"

#include "ui/plugins/note_plugins_ui.h"
#include "ui/shell/opening_behavior.h"
#include "ui/views/file_explorer/file_explorer_view.h"

#include <QDate>
#include <QFileInfo>

using namespace oc;
using namespace oc::test;

namespace {

struct OpeningEnv : NavigationEnv {
    OpeningEnv()
    {
        registerFileExplorerCommands(*commands, session);
        registerNotePluginCommands(*commands, window);
        openNote(QStringLiteral("Welcome.md"));
    }
    void run(const QString& behavior)
    {
        runOpeningBehavior(window, behavior);
        QApplication::processEvents();
    }
};

} // namespace

TEST(OpeningBehavior, LastOpenedKeepsTheRestoredTabs)
{
    OpeningEnv env;
    env.run(QString());
    EXPECT_EQ(env.activePath(), "Welcome.md");
    EXPECT_EQ(env.rootLeafCount(), 1);
}

TEST(OpeningBehavior, SpecificFileIsCaseInsensitiveWithAnImpliedExtension)
{
    OpeningEnv env;
    env.run(QStringLiteral("file:formatting"));
    EXPECT_EQ(env.activePath(), "Formatting.md");
    EXPECT_EQ(env.rootLeafCount(), 1); // in the current tab
    env.run(QStringLiteral("file:Projects/does-not-exist"));
    EXPECT_EQ(env.activePath(), "Formatting.md");
    env.run(QStringLiteral("file:"));
    EXPECT_EQ(env.activePath(), "Formatting.md");
}

TEST(OpeningBehavior, NewNoteUnlessTheActiveNoteIsEmpty)
{
    OpeningEnv env;
    env.run(QStringLiteral("new"));
    const QString created = env.activePath();
    EXPECT_TRUE(created.startsWith("Untitled")) << qPrintable(created);
    EXPECT_TRUE(QFileInfo::exists(env.root + "/" + created));
    EXPECT_EQ(env.rootLeafCount(), 1);
    // The active note is empty now: no second note.
    env.run(QStringLiteral("new"));
    EXPECT_EQ(env.activePath(), created);
    EXPECT_FALSE(QFileInfo::exists(env.root + "/Untitled 1.md"));
}

TEST(OpeningBehavior, DailyNote)
{
    OpeningEnv env;
    env.run(QStringLiteral("daily"));
    const QString today = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".md");
    EXPECT_TRUE(QTest::qWaitFor([&] { return env.activePath().endsWith(today); }, 5000)) << qPrintable(env.activePath());
}
