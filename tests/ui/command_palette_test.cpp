// Command palette: availability, recent-first ordering, pinned commands, fuzzy matching with prefixes and
// hotkey chips, execution through the registry, recent command persistence.
#include "navigation_test_support.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "editor/editor_view.h"

#include <QJsonArray>
#include <QLineEdit>

using namespace oc;
using namespace oc::test;

namespace {

Command simple(const QString& id, const QString& name, std::function<void()> fn = [] {})
{
    Command c;
    c.id = id;
    c.name = name;
    c.callback = std::move(fn);
    return c;
}

QStringList ids(const QList<PaletteItem>& items)
{
    QStringList out;
    for (const PaletteItem& it : items)
        out << it.id;
    return out;
}

} // namespace

TEST(CommandPaletteModel, OnlyAvailableNamedCommandsAlphabetically)
{
    CommandRegistry reg;
    reg.add(simple("b:cmd", "Bravo"));
    reg.add(simple("a:cmd", "Alpha"));
    reg.add(simple("hidden:cmd", "")); // no name: never listed
    Command unavailable;
    unavailable.id = "c:cmd";
    unavailable.name = "Charlie";
    unavailable.checkCallback = [](bool) { return false; };
    reg.add(unavailable);
    Command editorOnly;
    editorOnly.id = "editor:thing";
    editorOnly.name = "Editor thing";
    editorOnly.editorCallback = [](EditorContext&) {};
    reg.add(editorOnly);
    EXPECT_EQ(ids(commandPaletteItems("", reg)), (QStringList{"a:cmd", "b:cmd"}));
}

TEST(CommandPaletteModel, PinnedThenRecentThenTheRest)
{
    CommandRegistry reg;
    for (const char* n : {"Alpha", "Bravo", "Charlie", "Delta", "Echo"})
        reg.add(simple(QString("x:") + QString(n).toLower(), n));
    EXPECT_TRUE(reg.execute("x:delta"));
    EXPECT_TRUE(reg.execute("x:bravo")); // most recent first
    CommandPaletteOptions opts;
    opts.pinned = {"x:echo", "x:missing", "x:bravo"};
    const QList<PaletteItem> items = commandPaletteItems("", reg, opts);
    EXPECT_EQ(ids(items), (QStringList{"x:echo", "x:bravo", "x:delta", "x:alpha", "x:charlie"}));
    EXPECT_TRUE(items[0].pinned);
    EXPECT_TRUE(items[1].pinned);
    EXPECT_FALSE(items[2].pinned);
}

TEST(CommandPaletteModel, FuzzyMatchesTheFullName)
{
    CommandRegistry reg;
    reg.add(simple("editor:toggle-bold", "Toggle bold"));
    reg.add(simple("editor:toggle-blockquote", "Toggle blockquote"));
    reg.add(simple("switcher:open", "Quick switcher: Open quick switcher"));
    const QList<PaletteItem> bold = commandPaletteItems("tgl bold", reg);
    ASSERT_FALSE(bold.isEmpty());
    EXPECT_EQ(bold[0].id, "editor:toggle-bold");
    EXPECT_FALSE(bold[0].matches.isEmpty());
    // the plugin prefix is part of the searchable name
    const QList<PaletteItem> qs = commandPaletteItems("quick open", reg);
    ASSERT_EQ(qs.size(), 1);
    EXPECT_EQ(qs[0].id, "switcher:open");
    EXPECT_TRUE(commandPaletteItems("zzz", reg).isEmpty());
}

TEST(CommandPaletteOptions, JsonRoundTrip)
{
    const CommandPaletteOptions o = CommandPaletteOptions::fromJson(QJsonObject{{"pinned", QJsonArray{"a", "b"}}});
    EXPECT_EQ(o.pinned, (QStringList{"a", "b"}));
    EXPECT_EQ(CommandPaletteOptions::fromJson(o.toJson()).pinned, o.pinned);
}

// ---------------------------------------------------------------------------------------------
// the modal over a real session
// ---------------------------------------------------------------------------------------------

TEST(CommandPalette, ListsAvailableCommandsWithPrefixesAndHotkeys)
{
    NavigationEnv env;
    SuggestModal* m = openCommandPalette(env.session, env.layer());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->input()->placeholderText(), "Select a command...");
    EXPECT_EQ(openCommandPalette(env.session, env.layer()), m);
    // no markdown view: editor commands are unavailable
    for (const Suggestion& s : m->suggestions())
        EXPECT_FALSE(s.data.toString().startsWith("editor:")) << qPrintable(s.data.toString());
    QTest::keyClicks(m->input(), "open quick switcher");
    ASSERT_FALSE(m->suggestions().isEmpty());
    const Suggestion& s = m->suggestions()[0];
    EXPECT_EQ(s.data.toString(), "switcher:open");
    EXPECT_EQ(s.prefix, "Quick switcher");
    EXPECT_EQ(s.title, "Open quick switcher");
    EXPECT_EQ(s.hotkeys, QStringList{"Ctrl + O"});
    // highlights are relative to the title (the list shifts them by the prefix again): "Quick switcher: " hits
    // come out negative, "Open quick switcher" hits inside the title
    bool inTitle = false;
    for (const auto& [a, b] : s.titleMatches)
        inTitle = inTitle || (a >= 0 && b <= s.title.size());
    EXPECT_TRUE(inTitle);
    m->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // with a note open, the editor commands appear
    env.openNote("Welcome.md");
    m = openCommandPalette(env.session, env.layer());
    QTest::keyClicks(m->input(), "toggle bold");
    ASSERT_FALSE(m->suggestions().isEmpty());
    EXPECT_EQ(m->suggestions()[0].data.toString(), "editor:toggle-bold");
    EXPECT_EQ(m->suggestions()[0].hotkeys, QStringList{"Ctrl + B"});
    m->close();
}

TEST(CommandPalette, ExecutesTheChosenCommandAndListsItFirstNextTime)
{
    NavigationEnv env;
    int runs = 0;
    env.commands->add(simple("test:count", "Count things", [&runs] { ++runs; }));
    SuggestModal* m = openCommandPalette(env.session, env.layer());
    QTest::keyClicks(m->input(), "count things");
    ASSERT_FALSE(m->suggestions().isEmpty());
    EXPECT_EQ(m->suggestions()[0].data.toString(), "test:count");
    QTest::keyClick(m->input(), Qt::Key_Return);
    EXPECT_EQ(runs, 1);
    EXPECT_FALSE(m->isOpen());
    EXPECT_EQ(env.commands->recent().value(0), "test:count");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    m = openCommandPalette(env.session, env.layer());
    ASSERT_FALSE(m->suggestions().isEmpty());
    EXPECT_EQ(m->suggestions()[0].data.toString(), "test:count"); // recent first on an empty query
    m->close();
}

TEST(CommandPalette, PinnedCommandsFromTheConfigComeFirstWithAPin)
{
    NavigationEnv env;
    env.session->config()->setObject("command-palette", QJsonObject{{"pinned", QJsonArray{"workspace:new-tab", "app:toggle-left-sidebar"}}});
    SuggestModal* m = openCommandPalette(env.session, env.layer());
    ASSERT_GE(m->suggestions().size(), 2);
    EXPECT_EQ(m->suggestions()[0].data.toString(), "workspace:new-tab");
    EXPECT_EQ(m->suggestions()[0].auxIcon, "pin");
    EXPECT_EQ(m->suggestions()[1].data.toString(), "app:toggle-left-sidebar");
    EXPECT_EQ(m->suggestions()[1].auxIcon, "pin");
    EXPECT_TRUE(m->suggestions()[2].auxIcon.isEmpty());
    m->close();
}

TEST(CommandPalette, EditorCommandsRunOnTheEditorThatHadFocus)
{
    NavigationEnv env;
    env.openNote("Lists.md");
    MarkdownView* mv = env.activeMarkdown();
    ASSERT_NE(mv, nullptr);
    mv->editor()->setFocus();
    mv->editor()->setSelection(Selection::single(0, 0), false);
    const QString before = mv->document()->text().toString();
    SuggestModal* m = openCommandPalette(env.session, env.layer());
    QTest::keyClicks(m->input(), "toggle bold");
    ASSERT_EQ(m->suggestions().value(0).data.toString(), "editor:toggle-bold");
    QTest::keyClick(m->input(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_NE(mv->document()->text().toString(), before); // "****" inserted at the cursor
    EXPECT_TRUE(mv->editor()->hasFocus() || !QApplication::focusWidget());
}

TEST(CommandPalette, RecentCommandsPersistInTheCloneDataDir)
{
    QString file;
    {
        NavigationEnv env;
        env.commands->add(simple("test:persist", "Persist me"));
        ASSERT_TRUE(env.commands->execute("test:persist"));
        file = ClonePaths::vaultDataDir(env.session->id()) + "/recent-commands.json";
        // saved when the store goes away with the session at the latest (debounced otherwise)
        auto* store = env.session->findChild<QObject*>("ocRecentCommands");
        ASSERT_NE(store, nullptr);
        delete store;
        EXPECT_EQ(JsonFile::read(file).value("recent").toArray().first().toString(), "test:persist");
        EXPECT_FALSE(file.startsWith(env.root)); // never inside the vault
        // a new registry picks them up
        CommandRegistry fresh;
        persistRecentCommands(fresh, env.session);
        EXPECT_EQ(fresh.recent().value(0), "test:persist");
    }
}

TEST(CommandPalette, CommandAndHotkey)
{
    NavigationEnv env;
    const Command* c = env.commands->find("command-palette:open");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "Command palette: Open command palette");
    EXPECT_EQ(env.commands->hotkeys("command-palette:open").value(0).toString(), "Ctrl + P");
    EXPECT_TRUE(env.commands->execute("command-palette:open"));
    auto* m = env.layer()->findChild<SuggestModal*>("ocCommandPalette");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->isOpen());
    // the palette lists itself as a recent command next time
    m->close();
}
