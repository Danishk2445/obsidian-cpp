// A note's "More options" / tab header menu entries, the rename prompt and the Export to PDF modal.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "ui/overlay/text_prompt_modal.h"
#include "ui/plugins/core_file_menu.h"
#include "ui/plugins/pdf_export_ui.h"
#include "ui/settings/setting_item.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/sidebar/sidebar_registration.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/workspace/file_menu.h"

#include <QJsonObject>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>

using namespace oc;
using namespace oc::test;

namespace {
QStringList titles(const QList<MenuEntry>& items, const QString& section)
{
    QStringList out;
    for (const MenuEntry& e : items)
        if (e.section == section)
            out << e.title;
    return out;
}

const MenuEntry* entry(const QList<MenuEntry>& items, const QString& title)
{
    for (const MenuEntry& e : items)
        if (e.title == title)
            return &e;
    return nullptr;
}

void flushDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
} // namespace

class PaneMenuTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"Note.md", "# Title\n\nSome text with a [[Other]] link.\n"}, {"Other.md", "other\n"}};
    }
    void SetUp() override
    {
        AppFixture::SetUp();
        registerSidebarViews(*registry);
        installCoreFileMenu();
        desktopHandlers().openUrl = [this](const QUrl& u) { opened << u; };
        desktopHandlers().showInFolder = [](const QString&) {};
    }
    QList<QUrl> opened;
};

TEST_F(PaneMenuTest, MoreOptionsFollowObsidiansSections)
{
    MarkdownView* mv = open("Note.md");
    ASSERT_TRUE(mv);
    QList<MenuEntry> items;
    mv->paneMenuEntries(items, "more-options");
    EXPECT_EQ(titles(items, "open"), QStringList({"Split right", "Split down"}));
    EXPECT_EQ(titles(items, "pane"), QStringList({"Reading view", "Source mode"}));
    EXPECT_EQ(titles(items, "find"), QStringList({"Find...", "Replace..."}));
    const QStringList actions = titles(items, "action");
    ASSERT_TRUE(actions.contains("Rename..."));
    EXPECT_LT(actions.indexOf("Rename..."), actions.indexOf("Export to PDF..."));
    EXPECT_TRUE(actions.contains("Move file to..."));
    EXPECT_EQ(titles(items, "danger"), QStringList({"Delete file"}));
    EXPECT_TRUE(entry(items, "Delete file")->warning);
    EXPECT_EQ(titles(items, "info.copy"), QStringList({"as Obsidian URL", "from vault folder", "from system root"}));
    EXPECT_TRUE(titles(items, "view.linked").contains("Open backlinks"));
    EXPECT_TRUE(titles(items, "view.linked").contains("Open outline"));
    const MenuEntry* reading = entry(items, "Reading view");
    ASSERT_TRUE(reading);
    EXPECT_TRUE(reading->checkable);
    EXPECT_FALSE(reading->checked);

    // Sections become separators; "info.copy" folds into a "Copy path" submenu.
    QMenu menu;
    buildSectionedMenu(&menu, items, paneMenuLayout());
    QMenu* copyPath = nullptr;
    for (QAction* a : menu.actions())
        if (a->menu() && a->text() == "Copy path")
            copyPath = a->menu();
    ASSERT_TRUE(copyPath);
    EXPECT_EQ(copyPath->actions().size(), 3);

    // Reading view: no Source mode, Replace disabled.
    mv->setMode(EditorView::Mode::Reading);
    items.clear();
    mv->paneMenuEntries(items, "more-options");
    EXPECT_EQ(titles(items, "pane"), QStringList({"Reading view"}));
    EXPECT_TRUE(entry(items, "Reading view")->checked);
    EXPECT_FALSE(entry(items, "Replace...")->enabled);
}

TEST_F(PaneMenuTest, LinkedViewOpensNextToTheNote)
{
    MarkdownView* mv = open("Note.md");
    QList<MenuEntry> items;
    mv->paneMenuEntries(items, "more-options");
    const MenuEntry* outline = entry(items, "Open outline");
    ASSERT_TRUE(outline);
    outline->run();
    const auto leaves = window->workspace()->leavesOfType("outline");
    ASSERT_FALSE(leaves.isEmpty());
    WorkspaceLeaf* l = leaves.last();
    EXPECT_FALSE(l->isInSidedock());
    EXPECT_EQ(l->viewState().value("state").toObject().value("file").toString(), "Note.md");
}

TEST_F(PaneMenuTest, RenamePromptValidatesAndRenames)
{
    MarkdownView* mv = open("Note.md");
    TextPromptModal* m = promptForFileRename(window, vault->file("Note.md"));
    ASSERT_TRUE(m);
    QCoreApplication::processEvents();
    EXPECT_EQ(m->title(), "Note title");
    EXPECT_EQ(m->input()->toPlainText(), "Note");
    m->input()->setPlainText("Other");
    EXPECT_EQ(m->errorMessage(), "There's already a file with the same name");
    EXPECT_FALSE(m->submit());
    EXPECT_TRUE(m->isOpen());
    m->input()->setPlainText("a:b");
    EXPECT_FALSE(m->errorMessage().isEmpty());
    m->input()->setPlainText("  Renamed  ");
    EXPECT_TRUE(m->errorMessage().isEmpty());
    QTest::keyClick(m->input(), Qt::Key_Return);
    flushDeletes();
    EXPECT_TRUE(vault->file("Renamed.md"));
    EXPECT_FALSE(vault->file("Note.md"));
    EXPECT_EQ(mv->file()->path(), "Renamed.md");
}

TEST_F(PaneMenuTest, ExportPdfModalSavesSettingsAndWritesThePdf)
{
    MarkdownView* mv = open("Note.md");
    QStringList asked;
    QString answer; // cancelled first
    pdfSavePathProvider() = [&](MainWindow*, const QString& suggested) {
        asked << suggested;
        return answer;
    };
    Modal* m = openExportPdfModal(window, mv);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->title(), "Export to PDF");
    auto* group = m->findChild<SettingGroup*>();
    ASSERT_TRUE(group);
    ASSERT_EQ(group->items().size(), 5);
    // Landscape on, A4
    group->items()[2]->control<ToggleSwitch>()->click();
    group->items()[1]->control<Dropdown>()->choose("A4");

    m->button(0)->click(); // "Export to PDF", dialog cancelled: the modal stays
    EXPECT_EQ(asked, QStringList({"Note.pdf"}));
    EXPECT_TRUE(m->isOpen());
    const QJsonObject saved = session->vaultConfig()->get("pdfExportSettings").toObject();
    EXPECT_EQ(saved.value("pageSize").toString(), "A4");
    EXPECT_TRUE(saved.value("landscape").toBool());

    answer = tmp.path() + "/out/Note.pdf";
    QDir().mkpath(tmp.path() + "/out");
    m->button(0)->click();
    EXPECT_FALSE(m->isOpen());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.isEmpty(); }, 10000));
    EXPECT_EQ(opened.first(), QUrl::fromLocalFile(answer));
    EXPECT_TRUE(readFile(answer).startsWith("%PDF-"));
    // Nothing was written into the vault.
    EXPECT_FALSE(QFile::exists(root + "/Note.pdf"));
    pdfSavePathProvider() = nullptr;
}
