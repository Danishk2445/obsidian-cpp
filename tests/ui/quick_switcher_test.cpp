// Quick switcher: suggestion model (recent files, fuzzy paths, aliases, unresolved links, excluded files,
// create item, Tab completion) and the modal over a real session (open, new tab, split, create, keys).
#include "navigation_test_support.h"

#include <QJsonArray>
#include <QLineEdit>

using namespace oc;
using namespace oc::test;

namespace {

SwitcherSource fixtureSource()
{
    SwitcherSource s;
    s.files = {"Welcome.md", "Projects/Alpha.md", "Projects/Beta.md", "Projects/Notes.md", "Archive/Notes.md", "Frontmatter.md",
               "attachments/image.png", "attachments/paper.pdf", "Board.canvas", "data.json"};
    s.aliases.insert("Frontmatter.md", {"Props"});
    s.unresolved = {"Does not exist", "Notes about bees"};
    s.canOpen = [](const QString& ext) { return ext == u"md"; };
    return s;
}

QStringList texts(const QList<SwitcherItem>& items)
{
    QStringList out;
    for (const SwitcherItem& it : items)
        out << it.text;
    return out;
}

} // namespace

TEST(QuickSwitcherModel, EmptyQueryListsRecentFilesThatExist)
{
    SwitcherSource s = fixtureSource();
    s.recent = {"Projects/Beta.md", "Gone.md", "Welcome.md", "attachments/image.png", "data.json", "Archive/Notes.md"};
    const QList<SwitcherItem> items = quickSwitcherItems("  ", s);
    // missing files and types the switcher doesn't show (json without a view) are skipped
    EXPECT_EQ(texts(items), (QStringList{"Projects/Beta", "Welcome", "attachments/image.png", "Archive/Notes"}));
    for (const SwitcherItem& it : items)
        EXPECT_EQ(it.type, SwitcherItem::Type::File);

    QuickSwitcherOptions noAttachments;
    noAttachments.showAttachments = false;
    EXPECT_EQ(texts(quickSwitcherItems("", s, noAttachments)), (QStringList{"Projects/Beta", "Welcome", "Archive/Notes"}));
}

TEST(QuickSwitcherModel, FuzzyMatchesPathsWithoutMdExtension)
{
    const SwitcherSource s = fixtureSource();
    const QList<SwitcherItem> items = quickSwitcherItems("alpha", s);
    ASSERT_FALSE(items.isEmpty());
    EXPECT_EQ(items[0].type, SwitcherItem::Type::File);
    EXPECT_EQ(items[0].path, "Projects/Alpha.md");
    EXPECT_EQ(items[0].text, "Projects/Alpha");
    ASSERT_EQ(items[0].matches.size(), 1);
    EXPECT_EQ(items[0].matches[0], (MatchRange{9, 14})); // "Alpha" highlighted in the file name

    // both "Notes" files match; the file name is preferred over folder hits
    const QList<SwitcherItem> notes = quickSwitcherItems("notes", s);
    QStringList files;
    for (const SwitcherItem& it : notes)
        if (it.type == SwitcherItem::Type::File)
            files << it.text;
    EXPECT_TRUE(files.contains("Projects/Notes"));
    EXPECT_TRUE(files.contains("Archive/Notes"));
    // results are sorted by score
    for (int i = 1; i < notes.size(); ++i)
        EXPECT_GE(notes[i - 1].score, notes[i].score);

    // folders narrow the search ("proj not" -> Projects/Notes first)
    const QList<SwitcherItem> scoped = quickSwitcherItems("proj not", s);
    ASSERT_FALSE(scoped.isEmpty());
    EXPECT_EQ(scoped[0].text, "Projects/Notes");

    // non-note files keep their extension; canvases always show, pdf needs a registered view
    EXPECT_EQ(quickSwitcherItems("board", s).value(0).text, "Board.canvas");
    EXPECT_EQ(quickSwitcherItems("image", s).value(0).text, "attachments/image.png");
    for (const SwitcherItem& it : quickSwitcherItems("paper", s))
        EXPECT_NE(it.path, "attachments/paper.pdf");
    QuickSwitcherOptions all;
    all.showAllFileTypes = true;
    EXPECT_EQ(quickSwitcherItems("paper", s, all).value(0).path, "attachments/paper.pdf");
}

TEST(QuickSwitcherModel, AliasesAndUnresolvedLinks)
{
    const SwitcherSource s = fixtureSource();
    const QList<SwitcherItem> props = quickSwitcherItems("props", s);
    ASSERT_FALSE(props.isEmpty());
    EXPECT_EQ(props[0].type, SwitcherItem::Type::Alias);
    EXPECT_EQ(props[0].text, "Props");
    EXPECT_EQ(props[0].path, "Frontmatter.md");

    const QList<SwitcherItem> missing = quickSwitcherItems("does not", s);
    ASSERT_FALSE(missing.isEmpty());
    EXPECT_EQ(missing[0].type, SwitcherItem::Type::Unresolved);
    EXPECT_EQ(missing[0].text, "Does not exist");
    EXPECT_TRUE(missing[0].path.isEmpty());

    // "Show existing only" hides link targets that don't exist yet
    QuickSwitcherOptions existing;
    existing.showExistingOnly = true;
    for (const SwitcherItem& it : quickSwitcherItems("does not", s, existing))
        EXPECT_NE(it.type, SwitcherItem::Type::Unresolved);
}

TEST(QuickSwitcherModel, ExcludedFilesAreDownranked)
{
    QTemporaryDir tmp;
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    ConfigStore store(tmp.path() + "/cfg");
    VaultConfig cfg(&store);
    cfg.set("userIgnoreFilters", QJsonArray{"Archive/"});
    SwitcherSource s = fixtureSource();
    s.ignore = UserIgnoreFilters(&cfg);
    const QList<SwitcherItem> items = quickSwitcherItems("notes", s);
    int archive = -1, projects = -1;
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].path == "Archive/Notes.md")
            archive = i;
        if (items[i].path == "Projects/Notes.md")
            projects = i;
    }
    ASSERT_GE(archive, 0);
    ASSERT_GE(projects, 0);
    EXPECT_LT(projects, archive);
    EXPECT_TRUE(items[archive].downranked);
    EXPECT_FALSE(items[projects].downranked);

    // hidden from the recent list
    s.recent = {"Archive/Notes.md", "Welcome.md"};
    EXPECT_EQ(texts(quickSwitcherItems("", s)), QStringList{"Welcome"});

    // regex filters
    cfg.set("userIgnoreFilters", QJsonArray{"/^Proj.*Beta/"});
    const UserIgnoreFilters re(&cfg);
    EXPECT_TRUE(re.matches("Projects/Beta.md"));
    EXPECT_FALSE(re.matches("Projects/Alpha.md"));
}

TEST(QuickSwitcherModel, CreateItemWhenNothingMatches)
{
    const SwitcherSource s = fixtureSource();
    const QList<SwitcherItem> items = quickSwitcherItems("zzqqxx new", s);
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items[0].type, SwitcherItem::Type::Create);
    EXPECT_EQ(items[0].text, "zzqqxx new");
}

TEST(QuickSwitcherModel, TabCompletesToTheNextFolder)
{
    const SwitcherSource s = fixtureSource();
    const QList<SwitcherItem> items = quickSwitcherItems("proj", s);
    ASSERT_FALSE(items.isEmpty());
    const SwitcherItem& first = items[0];
    ASSERT_TRUE(first.text.startsWith("Projects/"));
    EXPECT_EQ(quickSwitcherCompletion(first, "proj"), "Projects/");
    // once the folder is typed, Tab completes the whole path
    const SwitcherItem inFolder = quickSwitcherItems("Projects/", s).value(0);
    EXPECT_EQ(quickSwitcherCompletion(inFolder, "Projects/"), inFolder.text);
    const SwitcherItem alpha = quickSwitcherItems("alpha", s).value(0);
    EXPECT_EQ(quickSwitcherCompletion(alpha, "alpha"), "Projects/Alpha");
    EXPECT_EQ(quickSwitcherCompletion(alpha, "Projects/Alpha"), "Projects/Alpha/");
}

// ---------------------------------------------------------------------------------------------
// the modal over a real session
// ---------------------------------------------------------------------------------------------

TEST(QuickSwitcher, ShowsObsidiansPromptAndRecentFiles)
{
    NavigationEnv env;
    env.openNote("Projects/Alpha.md");
    env.openNote("Welcome.md");
    SuggestModal* m = openQuickSwitcher(env.session, env.layer());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->input()->placeholderText(), "Find or create a note...");
    EXPECT_EQ(openQuickSwitcher(env.session, env.layer()), m); // already open
    ASSERT_GE(m->suggestions().size(), 2);
    EXPECT_EQ(m->suggestions()[0].title, "Welcome");
    EXPECT_EQ(m->suggestions()[1].title, "Projects/Alpha");
    m->close();
}

TEST(QuickSwitcher, EnterOpensModEnterOpensInNewTabModAltEnterSplits)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    const int leaves = env.rootLeafCount();

    SuggestModal* m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "alpha");
    ASSERT_FALSE(m->suggestions().isEmpty());
    EXPECT_EQ(m->suggestions()[0].title, "Projects/Alpha");
    QTest::keyClick(m->input(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), "Projects/Alpha.md");
    EXPECT_EQ(env.rootLeafCount(), leaves);
    EXPECT_FALSE(m->isOpen());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "beta");
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ControlModifier);
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), "Projects/Beta.md");
    EXPECT_EQ(env.rootLeafCount(), leaves + 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "lists");
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ControlModifier | Qt::AltModifier);
    QApplication::processEvents();
    EXPECT_EQ(env.activePath(), "Lists.md");
    EXPECT_EQ(env.rootLeafCount(), leaves + 2);
}

TEST(QuickSwitcher, ShiftEnterCreatesTheNoteAndItsFolders)
{
    NavigationEnv env;
    env.openNote("Welcome.md");
    SuggestModal* m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "Inbox/Ideas/New idea");
    ASSERT_EQ(m->suggestions().size(), 1);
    EXPECT_EQ(m->suggestions()[0].title, "Inbox/Ideas/New idea");
    EXPECT_EQ(m->suggestions()[0].auxText, "Enter to create");
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ShiftModifier);
    QApplication::processEvents();
    ASSERT_NE(env.file("Inbox/Ideas/New idea.md"), nullptr);
    EXPECT_TRUE(QFileInfo(env.root + "/Inbox/Ideas/New idea.md").isFile());
    EXPECT_EQ(readFile(env.root + "/Inbox/Ideas/New idea.md"), QByteArray());
    EXPECT_EQ(env.activePath(), "Inbox/Ideas/New idea.md");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Enter on the "Enter to create" item creates too; without a folder the note goes to the configured location
    env.session->vaultConfig()->set("newFileLocation", "folder");
    env.session->vaultConfig()->set("newFileFolderPath", "Projects");
    m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "Gamma plan");
    ASSERT_EQ(m->suggestions().size(), 1);
    QTest::keyClick(m->input(), Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_NE(env.file("Projects/Gamma plan.md"), nullptr);
    EXPECT_EQ(env.activePath(), "Projects/Gamma plan.md");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Shift+Enter with the name of an existing note opens it instead of creating a duplicate
    const int before = env.vault->fileCount();
    m = openQuickSwitcher(env.session, env.layer());
    QTest::keyClicks(m->input(), "welcome");
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ShiftModifier);
    QApplication::processEvents();
    EXPECT_EQ(env.vault->fileCount(), before);
    EXPECT_EQ(env.activePath(), "Welcome.md");

    // invalid names are refused
    QString err;
    EXPECT_EQ(openOrCreateLinkText(env.session, "bad:name", "", Workspace::NewLeaf::No, &err), nullptr);
    EXPECT_TRUE(err.contains("cannot contain"));
    EXPECT_EQ(env.vault->fileCount(), before);
}

TEST(QuickSwitcher, TabAutocompletesAndEscapeCloses)
{
    NavigationEnv env;
    SuggestModal* m = openQuickSwitcher(env.session, env.layer());
    QPointer<SuggestModal> guard(m);
    QTest::keyClicks(m->input(), "proj");
    ASSERT_FALSE(m->suggestions().isEmpty());
    ASSERT_TRUE(m->suggestions()[0].title.startsWith("Projects/"));
    QTest::keyClick(m->input(), Qt::Key_Tab);
    EXPECT_EQ(m->query(), "Projects/");
    EXPECT_TRUE(m->isOpen());
    QTest::keyClick(m->input(), Qt::Key_Escape);
    EXPECT_FALSE(m->isOpen());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(guard.isNull());
}

TEST(QuickSwitcher, UsesTheSessionsAliasesAndUnresolvedLinks)
{
    NavigationEnv env;
    const SwitcherSource src = SwitcherSource::fromSession(env.session);
    EXPECT_TRUE(src.files.contains("Projects/Alpha.md"));
    EXPECT_TRUE(src.files.contains("attachments/image.png"));
    if (src.aliases.value("Frontmatter.md").contains("Props")) {
        const QList<SwitcherItem> items = quickSwitcherItems("props", src);
        ASSERT_FALSE(items.isEmpty());
        EXPECT_EQ(items[0].type, SwitcherItem::Type::Alias);
    }
    if (src.unresolved.contains("Does not exist")) {
        SuggestModal* m = openQuickSwitcher(env.session, env.layer());
        QTest::keyClicks(m->input(), "does not exist");
        ASSERT_FALSE(m->suggestions().isEmpty());
        EXPECT_EQ(m->suggestions()[0].auxIcon, "file-plus");
        QTest::keyClick(m->input(), Qt::Key_Return); // choosing an unresolved link creates the note
        QApplication::processEvents();
        EXPECT_NE(env.file("Does not exist.md"), nullptr);
    }
}

TEST(QuickSwitcher, CommandAndHotkey)
{
    NavigationEnv env;
    const Command* c = env.commands->find("switcher:open");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "Quick switcher: Open quick switcher");
    ASSERT_EQ(env.commands->hotkeys("switcher:open").size(), 1);
    EXPECT_EQ(env.commands->hotkeys("switcher:open")[0].toString(), "Ctrl + O");
    EXPECT_TRUE(env.commands->execute("switcher:open"));
    auto* m = env.layer()->findChild<SuggestModal*>("ocQuickSwitcher");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->isOpen());
    m->close();
}
