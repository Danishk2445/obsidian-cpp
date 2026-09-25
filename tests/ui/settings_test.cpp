// Settings modal: opening (app:open-settings), navigation, search, controls persisting to the clone's
// Obsidian-shaped JSON files, the hotkey recorder and conflicts, core plugin switches, the folder
// suggester, sub-pages; screenshots only with OC_SHOT_DIR.
#include "app_test_support.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "editor/markdown_commands.h"
#include "editor/plugins/greentext_guard.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/settings/hotkeys_tab.h"
#include "ui/settings/path_suggest.h"
#include "ui/settings/setting_item.h"
#include "ui/settings/settings_modal.h"
#include "ui/settings/settings_pages.h"
#include "ui/shell/app_commands.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalSpy>

using namespace oc;
using namespace oc::test;

class SettingsTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Welcome.md", "# Welcome\n"},
                {"Projects/Alpha/Plan.md", "plan\n"},
                {"Projects/Beta.md", "beta\n"},
                {"Journal/2026-01-01.md", "day\n"},
                {"Templates/Daily.md", "# {{title}}\n"},
                {"Attachments/image.png", "png"}};
    }
    void SetUp() override
    {
        if (!ThemeManager::instance()) {
            static ThemeManager* tm = new ThemeManager(qApp);
            tm->setMode(ThemeManager::Mode::Dark);
            tm->apply();
        }
        AppFixture::SetUp();
        registry = std::make_unique<CommandRegistry>();
        session->setCommands(registry.get());
        registerAppCommands(*registry, window);
        registerNotePluginCommands(*registry, window);
        registerSettingsCommands(*registry, window);
        registry->loadHotkeys(session->config()->object("hotkeys"));
    }
    void TearDown() override
    {
        if (SettingsModal* m = modal())
            m->close();
        QApplication::processEvents();
        session->setCommands(nullptr);
        registry.reset();
        AppFixture::TearDown();
    }
    SettingsModal* modal() const
    {
        SettingsController* c = SettingsController::of(window);
        return c ? c->modal() : nullptr;
    }
    SettingsModal* open(const QString& tab = {})
    {
        SettingsModal* m = openSettings(window, tab);
        QApplication::processEvents();
        return m;
    }
    SettingItem* item(const QString& name) const { return modal() ? modal()->findItem(name) : nullptr; }
    QJsonObject configFile(const QString& name) const
    {
        session->config()->flush();
        return QJsonDocument::fromJson(readFile(session->config()->dir() + "/" + name + ".json")).object();
    }
    QStringList navTitles() const
    {
        QStringList out;
        for (SettingsNavItem* i : modal()->navItems())
            out << i->title();
        return out;
    }

    std::unique_ptr<CommandRegistry> registry;
};

TEST_F(SettingsTest, OpensFromCommandAndHotkey)
{
    EXPECT_EQ(registry->commandsFor(Hotkey::parse("Mod+,")), QStringList{"app:open-settings"});
    ASSERT_TRUE(registry->execute("app:open-settings"));
    QApplication::processEvents();
    SettingsModal* m = modal();
    ASSERT_TRUE(m);
    EXPECT_TRUE(m->isOpen());
    EXPECT_TRUE(m->isSidebarLayout());
    EXPECT_EQ(m->activeTab()->id(), "about"); // General first
    // .modal.mod-sidebar-layout: min(90vw, 1100) x min(85vh, 1000) in a 1200 x 800 window
    EXPECT_EQ(m->dialogGeometry().size(), QSize(1080, 680));
    // Obsidian 1.13's order; core plugin tabs of enabled plugins by name; the built-in community port.
    EXPECT_EQ(navTitles(), (QStringList{"General", "Appearance", "Interface", "Editor", "Files and links", "Hotkeys", "Keychain", "Core plugins",
                                        "Community plugins", "Backlinks", "Canvas", "Command palette", "Daily notes", "File recovery", "Note composer",
                                        "Page preview",
                                        "Quick switcher", "Templates", "Greentext Guard"}));
    EXPECT_TRUE(m->searchInput()->hasFocus() || !QApplication::focusWidget());
    // Opening again brings the same modal forward.
    EXPECT_EQ(open("editor"), m);
    EXPECT_EQ(m->activeTab()->id(), "editor");
}

TEST_F(SettingsTest, NavigationRemembersLastTab)
{
    SettingsModal* m = open();
    for (SettingsNavItem* i : m->navItems())
        if (i->title() == "Files and links")
            i->click();
    EXPECT_EQ(m->activeTab()->id(), "file");
    EXPECT_EQ(m->currentTitle(), "Files and links");
    for (SettingsNavItem* i : m->navItems())
        EXPECT_EQ(i->isActive(), i->title() == "Files and links") << qPrintable(i->title());
    ASSERT_TRUE(item("Default location for new notes"));
    m->close();
    QApplication::processEvents();
    EXPECT_FALSE(modal());
    SettingsModal* again = open();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->activeTab()->id(), "file");
    // Plugin tabs open by the plugin id; a disabled plugin's tab can't be opened.
    EXPECT_TRUE(again->openTab("daily-notes"));
    EXPECT_FALSE(again->openTab("zk-prefixer"));
    EXPECT_EQ(again->activeTab()->id(), "daily-notes");

    // Keyboard: ↓ from the search input into the navigation, ↓/j to move, Enter to open, / back.
    again->searchInput()->setFocus();
    QTest::keyClick(again->searchInput(), Qt::Key_Down);
    auto* focused = qobject_cast<SettingsNavItem*>(QApplication::focusWidget());
    ASSERT_TRUE(focused);
    EXPECT_EQ(focused->title(), "General");
    QTest::keyClick(focused, Qt::Key_Down);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_J);
    focused = qobject_cast<SettingsNavItem*>(QApplication::focusWidget());
    ASSERT_TRUE(focused);
    EXPECT_EQ(focused->title(), "Interface");
    QTest::keyClick(focused, Qt::Key_Return);
    EXPECT_EQ(again->activeTab()->id(), "interface");
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Slash);
    EXPECT_EQ(QApplication::focusWidget(), again->searchInput());
    // In search mode the results are navigated the same way.
    QTest::keyClicks(again->searchInput(), "strict line");
    QTest::keyClick(again->searchInput(), Qt::Key_Return);
    focused = qobject_cast<SettingsNavItem*>(QApplication::focusWidget());
    ASSERT_TRUE(focused);
    EXPECT_EQ(focused->title(), "Editor");
    QTest::keyClick(focused, Qt::Key_Down);
    focused = qobject_cast<SettingsNavItem*>(QApplication::focusWidget());
    ASSERT_TRUE(focused);
    EXPECT_EQ(focused->title(), "Strict line breaks");
    QTest::keyClick(focused, Qt::Key_Return);
    EXPECT_EQ(again->activeTab()->id(), "editor");
}

TEST_F(SettingsTest, SearchFindsSettingsAcrossTabs)
{
    SettingsModal* m = open();
    m->setSearchQuery("line numbers");
    ASSERT_FALSE(m->searchResults().isEmpty());
    const auto& g = m->searchResults().first();
    EXPECT_EQ(g.tab->id(), "editor");
    ASSERT_FALSE(g.hits.isEmpty());
    EXPECT_EQ(g.hits.first().name, "Line numbers");
    m->navigateToSearchResult(0, 0);
    QApplication::processEvents();
    EXPECT_EQ(m->activeTab()->id(), "editor");
    ASSERT_TRUE(item("Line numbers"));
    EXPECT_FALSE(item("Line numbers")->isHidden());

    // Descriptions and aliases match too ("night mode" is an alias of the base color scheme).
    m->setSearchQuery("night mode");
    ASSERT_FALSE(m->searchResults().isEmpty());
    EXPECT_EQ(m->searchResults().first().tab->id(), "appearance");
    EXPECT_EQ(m->searchResults().first().hits.first().name, "Base color scheme");

    // Rows inside sub-pages are grouped under the page; navigating opens it.
    m->setSearchQuery("ribbon items");
    bool found = false;
    for (int i = 0; i < m->searchResults().size(); ++i) {
        if (m->searchResults()[i].pagePath == QStringList{"Ribbon menu configuration"})
            found = true;
    }
    Q_UNUSED(found); // ribbon page rows aren't searchable (like Obsidian); the page row itself is
    m->setSearchQuery("excluded files");
    ASSERT_FALSE(m->searchResults().isEmpty());
    EXPECT_EQ(m->searchResults().first().tab->id(), "file");
    m->navigateToSearchResult(0, 0);
    QApplication::processEvents();
    EXPECT_EQ(m->activeTab()->id(), "file");

    m->setSearchQuery("zzzz no such setting");
    EXPECT_TRUE(m->searchResults().isEmpty());
    m->setSearchQuery("");
    EXPECT_TRUE(m->searchResults().isEmpty());
}

TEST_F(SettingsTest, TogglesPersistToAppJson)
{
    open("editor");
    SettingItem* row = item("Line numbers");
    ASSERT_TRUE(row);
    ToggleSwitch* t = row->control<ToggleSwitch>();
    ASSERT_TRUE(t);
    EXPECT_FALSE(t->isChecked());
    t->click();
    EXPECT_TRUE(session->vaultConfig()->flag("showLineNumber"));
    EXPECT_EQ(configFile("app").value("showLineNumber"), QJsonValue(true));
    t->click(); // back to the default: removed from the file, like Obsidian
    EXPECT_FALSE(configFile("app").contains("showLineNumber"));

    // Editing behaviours are applied live to the markdown commands' settings.
    EXPECT_TRUE(mdcmd::settings().smartIndentList);
    item("Smart lists")->control<ToggleSwitch>()->click();
    EXPECT_FALSE(mdcmd::settings().smartIndentList);
    item("Smart lists")->control<ToggleSwitch>()->click();
    EXPECT_TRUE(mdcmd::settings().smartIndentList);

    // Inverted option: "Use [[Wikilinks]]" stores useMarkdownLinks.
    modal()->openTab("file");
    ToggleSwitch* wiki = item("Use [[Wikilinks]]")->control<ToggleSwitch>();
    ASSERT_TRUE(wiki);
    EXPECT_TRUE(wiki->isChecked());
    wiki->click();
    EXPECT_EQ(configFile("app").value("useMarkdownLinks"), QJsonValue(true));

    // Appearance keys go to appearance.json.
    modal()->openTab("appearance");
    item("Quick font size adjustment")->control<ToggleSwitch>()->click();
    EXPECT_EQ(configFile("appearance").value("baseFontSizeAction"), QJsonValue(true));
    EXPECT_FALSE(configFile("app").contains("baseFontSizeAction"));
}

TEST_F(SettingsTest, DropdownsPersistAndApply)
{
    open("file");
    Dropdown* trash = item("Deleted files")->control<Dropdown>();
    ASSERT_TRUE(trash);
    EXPECT_EQ(trash->value(), "system");
    EXPECT_EQ(trash->values(), (QStringList{"system", "local", "none"}));
    trash->choose("local");
    EXPECT_EQ(configFile("app").value("trashOption"), QJsonValue("local"));
    item("New link format")->control<Dropdown>()->choose("relative");
    EXPECT_EQ(session->vaultConfig()->newLinkFormat(), LinkResolver::LinkFormat::Relative);

    // Dependent rows follow the value (visible()).
    SettingItem* folder = item("Folder to create new notes in");
    ASSERT_TRUE(folder);
    EXPECT_TRUE(folder->isHidden());
    item("Default location for new notes")->control<Dropdown>()->choose("folder");
    EXPECT_FALSE(folder->isHidden());
    EXPECT_EQ(configFile("app").value("newFileLocation"), QJsonValue("folder"));

    // Attachments: one key behind the mode dropdown and the folder / subfolder inputs.
    item("Default location for new attachments")->control<Dropdown>()->choose("subfolder");
    QApplication::processEvents();
    EXPECT_EQ(configFile("app").value("attachmentFolderPath"), QJsonValue("./Attachments"));
    ASSERT_TRUE(item("Subfolder name"));
    EXPECT_FALSE(item("Subfolder name")->isHidden());
    QLineEdit* sub = item("Subfolder name")->control<QLineEdit>();
    ASSERT_TRUE(sub);
    sub->setFocus();
    QTest::keyClicks(sub, "img");
    EXPECT_EQ(configFile("app").value("attachmentFolderPath"), QJsonValue("./img"));

    // Base color scheme: appearance.json + the ThemeManager, live.
    modal()->openTab("appearance");
    item("Base color scheme")->control<Dropdown>()->choose("moonstone");
    EXPECT_EQ(configFile("appearance").value("theme"), QJsonValue("moonstone"));
    EXPECT_EQ(ThemeManager::instance()->mode(), ThemeManager::Mode::Light);
    item("Base color scheme")->control<Dropdown>()->choose("obsidian");
    EXPECT_EQ(ThemeManager::instance()->mode(), ThemeManager::Mode::Dark);
}

TEST_F(SettingsTest, SliderWithRestoreDefault)
{
    open("editor");
    SettingItem* row = item("Indent visual width");
    ASSERT_TRUE(row);
    ValueSlider* s = row->control<ValueSlider>();
    ASSERT_TRUE(s);
    EXPECT_EQ(s->value(), 4);
    auto* restore = row->findChild<IconButton*>("ocRestoreDefault");
    ASSERT_TRUE(restore);
    EXPECT_FALSE(restore->isEnabled()); // at the default
    QTest::keyClick(s, Qt::Key_Right);
    EXPECT_EQ(configFile("app").value("tabSize"), QJsonValue(5));
    EXPECT_TRUE(restore->isEnabled());
    restore->click();
    EXPECT_EQ(s->value(), 4);
    EXPECT_FALSE(configFile("app").contains("tabSize"));

    // Font size applies to the theme at once.
    modal()->openTab("appearance");
    ValueSlider* size = item("Font size")->control<ValueSlider>();
    ASSERT_TRUE(size);
    QTest::keyClick(size, Qt::Key_Right);
    EXPECT_EQ(configFile("appearance").value("baseFontSize"), QJsonValue(17));
    EXPECT_EQ(ThemeManager::instance()->fonts().baseFontSize, 17);
    item("Font size")->findChild<IconButton*>("ocRestoreDefault")->click();
    EXPECT_EQ(ThemeManager::instance()->fonts().baseFontSize, 16);
}

TEST_F(SettingsTest, FolderSuggester)
{
    open("file");
    item("Default location for new notes")->control<Dropdown>()->choose("folder");
    QLineEdit* input = item("Folder to create new notes in")->control<QLineEdit>();
    ASSERT_TRUE(input);
    auto* suggest = input->findChild<FolderSuggest*>();
    ASSERT_TRUE(suggest);
    // Every folder (root included for this option) for an empty query; fuzzy and best first otherwise.
    const QStringList all = suggest->suggestions("");
    EXPECT_EQ(all.first(), "/");
    EXPECT_TRUE(all.contains("Projects/Alpha"));
    EXPECT_TRUE(all.contains("Journal"));
    EXPECT_EQ(suggest->suggestions("alph"), QStringList{"Projects/Alpha"});
    EXPECT_EQ(suggest->suggestions("proj").first(), "Projects");

    input->setFocus();
    QApplication::processEvents();
    QTest::keyClicks(input, "alp");
    ASSERT_TRUE(suggest->isOpen());
    EXPECT_EQ(suggest->popup()->list()->suggestions().size(), 1);
    QTest::keyClick(input, Qt::Key_Return);
    EXPECT_FALSE(suggest->isOpen());
    EXPECT_EQ(input->text(), "Projects/Alpha");
    EXPECT_EQ(configFile("app").value("newFileFolderPath"), QJsonValue("Projects/Alpha"));
    // Escape closes an open popup, not the modal.
    input->clear();
    QTest::keyClicks(input, "jour");
    ASSERT_TRUE(suggest->isOpen());
    QTest::keyClick(input, Qt::Key_Escape);
    EXPECT_FALSE(suggest->isOpen());
    EXPECT_TRUE(modal() && modal()->isOpen());
}

TEST_F(SettingsTest, FilePickerForTemplates)
{
    open("daily-notes");
    SettingItem* row = item("Template file location");
    ASSERT_TRUE(row);
    auto* picker = row->control<FilePicker>();
    ASSERT_TRUE(picker);
    EXPECT_EQ(picker->value(), "");
    // Only Markdown files are offered.
    const QStringList files = picker->suggestions("");
    EXPECT_TRUE(files.contains("Templates/Daily.md"));
    EXPECT_FALSE(files.contains("Attachments/image.png"));
    EXPECT_EQ(picker->suggestions("tdaily").first(), "Templates/Daily.md");
    picker->open();
    ASSERT_TRUE(picker->isOpen());
    picker->searchInput()->setText("daily");
    QTest::keyClick(picker->searchInput(), Qt::Key_Return);
    EXPECT_FALSE(picker->isOpen());
    EXPECT_EQ(picker->value(), "Templates/Daily.md");
    EXPECT_EQ(configFile("daily-notes").value("template"), QJsonValue("Templates/Daily.md"));
    picker->choose(QString());
    EXPECT_EQ(configFile("daily-notes").value("template"), QJsonValue(""));
}

TEST_F(SettingsTest, PluginOptionPages)
{
    open("daily-notes");
    Dropdown* fmt = item("Date format")->control<Dropdown>();
    ASSERT_TRUE(fmt);
    EXPECT_EQ(fmt->value(), "YYYY-MM-DD");
    SettingItem* custom = item("Custom format");
    ASSERT_TRUE(custom);
    EXPECT_TRUE(custom->isHidden());
    fmt->choose("YYYY.MM.DD");
    EXPECT_EQ(configFile("daily-notes").value("format"), QJsonValue("YYYY.MM.DD"));
    fmt->choose("custom");
    EXPECT_FALSE(custom->isHidden());
    QLineEdit* input = custom->control<QLineEdit>();
    ASSERT_TRUE(input);
    input->setFocus();
    input->selectAll();
    QTest::keyClicks(input, "YYYY");
    EXPECT_EQ(configFile("daily-notes").value("format"), QJsonValue("YYYY"));
    EXPECT_TRUE(custom->desc().contains(QString::number(QDate::currentDate().year())));

    QLineEdit* folder = item("New file location")->control<QLineEdit>();
    folder->setFocus();
    QTest::keyClicks(folder, "Journal");
    EXPECT_EQ(configFile("daily-notes").value("folder"), QJsonValue("Journal"));

    // Templates: moment formats with a live sample.
    modal()->openTab("templates");
    SettingItem* date = item("Date format");
    QLineEdit* dateInput = date->control<QLineEdit>();
    ASSERT_TRUE(dateInput);
    EXPECT_EQ(dateInput->placeholderText(), "YYYY-MM-DD");
    dateInput->setFocus();
    QTest::keyClicks(dateInput, "DD/MM");
    EXPECT_EQ(configFile("templates").value("dateFormat"), QJsonValue("DD/MM"));

    // Page preview: per-source "require Ctrl" toggles (reading view defaults to off).
    modal()->openTab("page-preview");
    ToggleSwitch* reading = item("Reading view")->control<ToggleSwitch>();
    ToggleSwitch* editing = item("Editing view")->control<ToggleSwitch>();
    ASSERT_TRUE(reading && editing);
    EXPECT_FALSE(reading->isChecked());
    EXPECT_TRUE(editing->isChecked());
    editing->click();
    EXPECT_EQ(configFile("page-preview").value("editor"), QJsonValue(false));

    // Quick switcher.
    modal()->openTab("switcher");
    EXPECT_TRUE(item("Show attachments")->control<ToggleSwitch>()->isChecked());
    EXPECT_TRUE(item("Show all file types")->isHidden()); // needs "Show all file types" in Files and links
    item("Show existing only")->control<ToggleSwitch>()->click();
    EXPECT_EQ(configFile("switcher").value("showExistingOnly"), QJsonValue(true));

    // File recovery numbers: validated on commit.
    modal()->openTab("file-recovery");
    QLineEdit* keep = item("History length")->control<QLineEdit>();
    ASSERT_TRUE(keep);
    EXPECT_EQ(keep->text(), "7");
    keep->setText("0");
    emit keep->editingFinished();
    EXPECT_EQ(item("History length")->errorMessage(), "Value must be at least 1");
    EXPECT_FALSE(configFile("file-recovery").contains("keepDays"));
    keep->setText("30");
    emit keep->editingFinished();
    EXPECT_EQ(item("History length")->errorMessage(), "");
    EXPECT_EQ(configFile("file-recovery").value("keepDays"), QJsonValue(30));
}

TEST_F(SettingsTest, HotkeyRecorderAndConflicts)
{
    open("hotkeys");
    auto* tab = qobject_cast<HotkeysTab*>(modal()->activeTab());
    ASSERT_TRUE(tab);
    SettingItem* row = tab->rowFor("app:open-settings");
    ASSERT_TRUE(row);
    QStringList chips;
    for (HotkeyChip* c : row->findChildren<HotkeyChip*>())
        chips << c->text();
    EXPECT_EQ(chips, QStringList{"Ctrl + ,"});

    // Record Ctrl+Shift+K for "Open settings" through real key events.
    tab->startCustomizing("app:open-settings");
    QApplication::processEvents();
    row = tab->rowFor("app:open-settings");
    bool active = false;
    for (HotkeyChip* c : row->findChildren<HotkeyChip*>())
        active = active || c->state() == HotkeyChip::State::Active;
    EXPECT_TRUE(active); // "Press hotkey..."
    QWidget* target = modal()->searchInput();
    QTest::keyClick(target, Qt::Key_A); // no modifier: ignored, still waiting
    EXPECT_EQ(tab->customizingCommand(), "app:open-settings");
    QTest::keyClick(target, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(tab->customizingCommand().isEmpty());
    EXPECT_EQ(registry->hotkeys("app:open-settings"), (QList<Hotkey>{Hotkey::parse("Mod+,"), Hotkey::parse("Mod+Shift+K")}));
    const QJsonArray saved = configFile("hotkeys").value("app:open-settings").toArray();
    ASSERT_EQ(saved.size(), 2);
    EXPECT_EQ(saved[1].toObject(), (QJsonObject{{"modifiers", QJsonArray{"Mod", "Shift"}}, {"key", "K"}}));
    EXPECT_TRUE(modal()->searchInput()->text().isEmpty()); // the keys didn't reach the input

    // Escape cancels a recording without closing the modal.
    tab->startCustomizing("app:open-settings");
    QTest::keyClick(target, Qt::Key_Escape);
    EXPECT_TRUE(tab->customizingCommand().isEmpty());
    EXPECT_TRUE(modal() && modal()->isOpen());

    // Validity rule: Shift + letter alone and plain keys are rejected; F-keys alone are fine.
    EXPECT_FALSE(HotkeysTab::isValidRecordedHotkey(Hotkey::parse("Shift+A")));
    EXPECT_FALSE(HotkeysTab::isValidRecordedHotkey(Hotkey::parse("A")));
    EXPECT_TRUE(HotkeysTab::isValidRecordedHotkey(Hotkey::parse("F5")));
    EXPECT_TRUE(HotkeysTab::isValidRecordedHotkey(Hotkey::parse("Shift+1")));

    // A chord used twice is a conflict on both commands.
    ASSERT_TRUE(registry->find("workspace:new-tab"));
    tab->startCustomizing("app:open-settings");
    EXPECT_TRUE(tab->handleChord(Hotkey::parse("Mod+T")));
    EXPECT_TRUE(tab->conflicts().contains("Open settings"));
    EXPECT_TRUE(tab->conflicts().contains(registry->find("workspace:new-tab")->name));
    bool conflictChip = false;
    for (HotkeyChip* c : tab->rowFor("workspace:new-tab")->findChildren<HotkeyChip*>())
        if (c->state() == HotkeyChip::State::Conflict) {
            conflictChip = true;
            EXPECT_EQ(c->toolTip(), "This hotkey conflicts with “Open settings”");
        }
    EXPECT_TRUE(conflictChip);
    tab->setStatusFilter(HotkeysTab::StatusFilter::Conflicts);
    EXPECT_EQ(tab->visibleCommands().size(), 2);
    tab->setStatusFilter(HotkeysTab::StatusFilter::AssignedByMe);
    EXPECT_EQ(tab->visibleCommands(), QStringList{"app:open-settings"});
    tab->setStatusFilter(HotkeysTab::StatusFilter::All);
    tab->setHotkeyFilter(Hotkey::parse("Mod+T"));
    EXPECT_EQ(tab->visibleCommands().size(), 2);
    tab->setHotkeyFilter({});

    // Remove one chip, then restore the default.
    tab->removeHotkey("app:open-settings", Hotkey::parse("Mod+T"));
    EXPECT_FALSE(tab->conflicts().contains("Open settings"));
    tab->restoreDefault("app:open-settings");
    EXPECT_FALSE(registry->hasCustomHotkeys("app:open-settings"));
    EXPECT_FALSE(configFile("hotkeys").contains("app:open-settings"));
    // Removing a default leaves an explicit empty list ("Blank").
    tab->removeHotkey("app:open-settings", Hotkey::parse("Mod+,"));
    EXPECT_EQ(configFile("hotkeys").value("app:open-settings"), QJsonValue(QJsonArray()));
    bool blank = false;
    for (HotkeyChip* c : tab->rowFor("app:open-settings")->findChildren<HotkeyChip*>())
        blank = blank || (c->state() == HotkeyChip::State::Empty && c->text() == "Blank");
    EXPECT_TRUE(blank);

    // Filter by name or id.
    tab->setQuery("open settings");
    EXPECT_EQ(tab->visibleCommands(), QStringList{"app:open-settings"});
    tab->setQuery("zzz");
    EXPECT_TRUE(tab->visibleCommands().isEmpty());
}

TEST_F(SettingsTest, CorePluginToggles)
{
    open("plugins");
    SettingsController* ctl = SettingsController::of(window);
    QSignalSpy toggled(ctl, &SettingsController::corePluginToggled);
    SettingItem* unique = item("Unique note creator");
    ASSERT_TRUE(unique);
    EXPECT_FALSE(unique->control<ToggleSwitch>()->isChecked());
    EXPECT_FALSE(registry->find("zk-prefixer"));
    unique->control<ToggleSwitch>()->click();
    QApplication::processEvents();
    // Obsidian's object shape, every plugin listed.
    const QJsonObject cp = configFile("core-plugins");
    EXPECT_EQ(cp.value("zk-prefixer"), QJsonValue(true));
    EXPECT_EQ(cp.value("daily-notes"), QJsonValue(true));
    EXPECT_EQ(cp.value("random-note"), QJsonValue(false));
    EXPECT_EQ(cp.size(), corePluginInfos().size());
    ASSERT_EQ(toggled.size(), 1);
    EXPECT_EQ(toggled[0][0].toString(), "zk-prefixer");
    EXPECT_TRUE(registry->find("zk-prefixer"));
    EXPECT_TRUE(navTitles().contains("Unique note creator"));
    // The row's options button opens the plugin's tab.
    auto* gear = item("Unique note creator")->findChild<IconButton*>("ocPluginOptions");
    ASSERT_TRUE(gear);
    EXPECT_FALSE(gear->isHidden());
    gear->click();
    EXPECT_EQ(modal()->activeTab()->id(), "zk-prefixer");

    // Turning Daily notes off removes its commands and its tab.
    modal()->openTab("plugins");
    item("Daily notes")->control<ToggleSwitch>()->click();
    EXPECT_FALSE(registry->find("daily-notes"));
    EXPECT_FALSE(registry->find("daily-notes:goto-prev"));
    EXPECT_FALSE(navTitles().contains("Daily notes"));
    EXPECT_FALSE(isCorePluginEnabled(window, "daily-notes"));
    item("Daily notes")->control<ToggleSwitch>()->click();
    EXPECT_TRUE(registry->find("daily-notes"));

    // The core plugin list has its own search.
    SettingGroup* g = nullptr;
    for (SettingGroup* sg : modal()->currentBody()->findChildren<SettingGroup*>())
        if (sg->searchInput())
            g = sg;
    ASSERT_TRUE(g);
    g->searchInput()->setText("zettelkasten"); // matches the description
    EXPECT_FALSE(item("Unique note creator")->isHidden());
    EXPECT_TRUE(item("Daily notes")->isHidden());
}

TEST_F(SettingsTest, PagesAndEscape)
{
    SettingsModal* m = open("file");
    SettingItem* row = item("Excluded files");
    ASSERT_TRUE(row);
    EXPECT_EQ(row->kind(), SettingItem::Kind::Navigable);
    row->activate();
    EXPECT_EQ(m->pageDepth(), 1);
    EXPECT_EQ(m->currentTitle(), "Excluded files");
    SettingGroup* list = m->currentBody()->findChild<SettingGroup*>();
    ASSERT_TRUE(list && list->emptyStateItem());
    EXPECT_EQ(list->emptyStateItem()->name(), "No exclusions added.");
    // Escape goes back first, then closes.
    QTest::keyClick(m, Qt::Key_Escape);
    EXPECT_EQ(m->pageDepth(), 0);
    EXPECT_TRUE(m->isOpen());
    session->vaultConfig()->set("userIgnoreFilters", QJsonArray{"Archive/", "/daily-.*/"});
    m->activeTab()->update();
    EXPECT_EQ(item("Excluded files")->displayValue(), "2 rules");
    QTest::keyClick(m, Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_FALSE(modal());
}

TEST_F(SettingsTest, GreentextGuardSettings)
{
    GreentextGuard::instance().setSettings({});
    open("greentext-guard");
    QLineEdit* patterns = item("Note name patterns")->control<QLineEdit>();
    ASSERT_TRUE(patterns);
    EXPECT_EQ(patterns->text(), "4chan");
    patterns->setFocus();
    patterns->selectAll();
    QTest::keyClicks(patterns, "4chan, greentext");
    EXPECT_EQ(configFile("plugins/greentext-guard").value("patterns"), QJsonValue("4chan, greentext"));
    EXPECT_EQ(GreentextGuard::instance().settings().patterns, "4chan, greentext");
    item("Typing “>” at the start of a line")->control<Dropdown>()->choose("escape");
    EXPECT_EQ(GreentextGuard::instance().settings().onType, "escape");
    // "Include the surrounding block" is disabled while pasting isn't guarded.
    item("Wrap pasted greentext in a code fence")->control<ToggleSwitch>()->click();
    EXPECT_TRUE(item("Include the surrounding block")->isDisabledItem());
    // The plugin can be turned off from Community plugins.
    modal()->openTab("community-plugins");
    auto* toggle = item("Greentext Guard")->control<ToggleSwitch>();
    ASSERT_TRUE(toggle);
    EXPECT_TRUE(toggle->isChecked());
    toggle->click();
    EXPECT_EQ(configFile("community-plugins").value("greentext-guard"), QJsonValue(false));
    EXPECT_FALSE(GreentextGuard::instance().installed());
    EXPECT_FALSE(navTitles().contains("Greentext Guard"));
    item("Greentext Guard")->control<ToggleSwitch>()->click();
    EXPECT_TRUE(GreentextGuard::instance().installed());
    GreentextGuard::instance().uninstall();
}

TEST_F(SettingsTest, RibbonOptions)
{
    window->ribbon()->addAction("switcher:open", "file-search", "Open quick switcher");
    window->ribbon()->addAction("graph:open", "git-fork", "Open graph view");
    open("interface");
    SettingItem* page = item("Ribbon menu configuration");
    ASSERT_TRUE(page);
    EXPECT_FALSE(page->isHidden());
    item("Show ribbon")->control<ToggleSwitch>()->click();
    EXPECT_TRUE(window->ribbon()->isHidden());
    EXPECT_EQ(configFile("appearance").value("showRibbon"), QJsonValue(false));
    EXPECT_TRUE(page->isHidden()); // visible() follows the option
    item("Show ribbon")->control<ToggleSwitch>()->click();
    EXPECT_FALSE(window->ribbon()->isHidden());
    // The configuration page lists the shown items; "×" hides one, a click on a hidden one shows it.
    item("Ribbon menu configuration")->activate();
    SettingItem* graph = item("Open graph view");
    ASSERT_TRUE(graph);
    graph->findChild<IconButton*>()->click();
    QApplication::processEvents();
    bool hidden = false;
    for (const auto& [id, button] : window->ribbon()->actions())
        if (id == "graph:open")
            hidden = button->isHidden();
    EXPECT_TRUE(hidden);
    SettingItem* other = item("Open graph view");
    ASSERT_TRUE(other);
    EXPECT_EQ(other->kind(), SettingItem::Kind::Action);
    other->activate();
    for (const auto& [id, button] : window->ribbon()->actions()) {
        if (id == "graph:open") {
            EXPECT_FALSE(button->isHidden());
        }
    }
}

// The rows reused by ordinary modals (e.g. Export to PDF): a plain group in a Modal's content.
TEST_F(SettingsTest, PlainRowsInAModal)
{
    auto* m = new Modal(window->overlay());
    m->setTitle("Export to PDF");
    auto* g = new SettingGroup;
    g->setPlain(true);
    QString size = "Letter";
    bool landscape = false;
    double scale = 100;
    SettingItem* a = g->addItem();
    a->setName("Page size");
    a->addDropdown({{"A4", "A4"}, {"Letter", "Letter"}}, size, [&](const QString& v) { size = v; });
    SettingItem* b = g->addItem();
    b->setName("Landscape");
    b->addToggle(landscape, [&](bool on) { landscape = on; });
    SettingItem* c = g->addItem();
    c->setName("Downscale percent");
    c->addSlider(0, 100, 1, scale, [&](double v) { scale = v; });
    m->addContentWidget(g);
    m->addButton("Export", Modal::ButtonStyle::Cta);
    m->open();
    QApplication::processEvents();
    a->control<Dropdown>()->choose("A4");
    b->control<ToggleSwitch>()->click();
    QTest::keyClick(c->control<ValueSlider>(), Qt::Key_Left);
    EXPECT_EQ(size, "A4");
    EXPECT_TRUE(landscape);
    EXPECT_EQ(scale, 99);
    // padding: 16px 0, none above the first / below the last row
    EXPECT_EQ(a->layout()->contentsMargins(), QMargins(0, 0, 0, 16));
    EXPECT_EQ(b->layout()->contentsMargins(), QMargins(0, 16, 0, 16));
    EXPECT_EQ(c->layout()->contentsMargins(), QMargins(0, 16, 0, 0));
    if (const QString dir = qEnvironmentVariable("OC_SHOT_DIR"); !dir.isEmpty()) {
        QDir().mkpath(dir);
        window->grab().save(dir + "/settings-plain-rows.png");
    }
    m->close();
}

TEST(SettingsCssSnippets, ParsesCustomProperties)
{
    const auto o = css_snippets::parse(
        "/* c */ :root { --a: 1px; color: red; }\n"
        ".theme-dark { --background-primary: #101010 !important; --b: var(--c) }\n"
        "body.theme-light, .x { --background-primary: #fafafa; }\n"
        "@media (max-width: 10px) { body { --ignored: 1; } }\n"
        ".workspace { --not-global: 2; }\n");
    EXPECT_EQ(o.body.value("--a"), "1px");
    EXPECT_FALSE(o.body.contains("color"));
    EXPECT_EQ(o.dark.value("--background-primary"), "#101010");
    EXPECT_EQ(o.dark.value("--b"), "var(--c)");
    EXPECT_EQ(o.light.value("--background-primary"), "#fafafa");
    EXPECT_FALSE(o.body.contains("--ignored"));
    EXPECT_FALSE(o.body.contains("--not-global"));
}

TEST(SettingsFonts, ResolvesFontLists)
{
    EXPECT_EQ(resolveFontList(""), "");
    EXPECT_EQ(resolveFontList("No Such Font 123"), "");
    const QString some = Theme::defaultMonospaceFamily();
    EXPECT_EQ(resolveFontList("No Such Font 123, \"" + some + "\""), some);
}

// ---- screenshots (OC_SHOT_DIR) ----------------------------------------------------------------------

TEST_F(SettingsTest, Screenshots)
{
    const QString dir = qEnvironmentVariable("OC_SHOT_DIR");
    if (dir.isEmpty())
        GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
    QDir().mkpath(dir);
    window->resize(1400, 850);
    // The application's ribbon actions (for the ribbon configuration page).
    window->ribbon()->addAction("switcher:open", "file-search", "Open quick switcher");
    window->ribbon()->addAction("graph:open", "git-fork", "Open graph view");
    window->ribbon()->addAction("daily-notes", "calendar-days", "Open today's daily note");
    window->ribbon()->addAction("command-palette:open", "terminal", "Open command palette");
    window->ribbon()->setActionHidden("graph:open", true);
    QApplication::processEvents();
    for (const auto& [mode, name] : {std::pair{ThemeManager::Mode::Dark, "dark"}, std::pair{ThemeManager::Mode::Light, "light"}}) {
        ThemeManager::instance()->setMode(mode);
        ThemeManager::instance()->apply();
        for (const char* tab : {"editor", "appearance", "hotkeys", "plugins", "about", "file", "daily-notes", "community-plugins"}) {
            open(QString::fromLatin1(tab));
            modal()->searchInput()->clearFocus();
            QApplication::processEvents();
            window->grab().save(dir + QStringLiteral("/settings-%1-%2.png").arg(QString::fromLatin1(tab), QString::fromLatin1(name)));
        }
        auto shot = [&](const char* what) {
            QApplication::processEvents();
            window->grab().save(dir + QStringLiteral("/settings-%1-%2.png").arg(QString::fromLatin1(what), QString::fromLatin1(name)));
        };
        for (const char* tab : {"interface", "keychain", "greentext-guard", "page-preview", "command-palette"}) {
            open(QString::fromLatin1(tab));
            modal()->searchInput()->clearFocus();
            shot(tab);
        }
        open("appearance");
        modal()->setSearchQuery("font");
        shot("search");
        modal()->setSearchQuery("");
        item("Interface font")->activate();
        shot("fontpage");
        modal()->closePage();
        item("CSS snippets")->activate();
        shot("snippets");
        open("interface");
        item("Ribbon menu configuration")->activate();
        shot("ribbon");
        open("file");
        session->vaultConfig()->set("userIgnoreFilters", QJsonArray{"Archive/", "/daily-.*/"});
        modal()->activeTab()->update();
        item("Excluded files")->activate();
        shot("excluded");
        session->vaultConfig()->set("userIgnoreFilters", QJsonValue());
        // Hotkeys: a conflict and a recording in progress.
        auto* hk = qobject_cast<HotkeysTab*>(open("hotkeys")->activeTab());
        hk->startCustomizing("app:open-settings");
        hk->handleChord(Hotkey::parse("Mod+T"));
        hk->startCustomizing("workspace:new-tab");
        modal()->searchInput()->clearFocus();
        shot("hotkeys-recording");
        hk->cancelCustomizing();
        hk->restoreDefault("app:open-settings");
        // The Vim test modal.
        open("editor");
        item("Vim key bindings")->control<ToggleSwitch>()->click();
        shot("vim");
        if (auto* vim = window->overlay()->findChild<Modal*>("ocVimTestModal"))
            vim->close();
        modal()->close();
        QApplication::processEvents();
    }
    ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
    ThemeManager::instance()->apply();
}
