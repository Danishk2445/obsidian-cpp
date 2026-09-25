// Core plugin switches: a disabled plugin's commands, ribbon action, status bar item and views go away
// and come back when it is enabled again.
#include "app_test_support.h"

#include "core/commands/command_registry.h"
#include "ui/plugins/core_plugins.h"
#include "ui/settings/settings_modal.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/views/sidebar/sidebar_registration.h"
#include "ui/widgets/icon_button.h"

using namespace oc;
using namespace oc::test;

namespace {
Command simple(const char* id)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = c.id;
    c.callback = [] {};
    return c;
}
} // namespace

class CorePluginsTest : public AppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override { return {{"Note.md", "# Title\n\ntext\n"}}; }
    void SetUp() override
    {
        AppFixture::SetUp();
        commands = new CommandRegistry(session.get());
        session->setCommands(commands);
        commands->add(simple("graph:open"));
        commands->add(simple("graph:open-local"));
        commands->add(simple("switcher:open"));
        commands->add(simple("editor:toggle-bold"));
        registerSidebarViews(*registry);
        graphButton = window->ribbon()->addAction("graph:open", "git-fork", "Open graph view");
        window->statusBar()->addItem("word-count", "3 words");
        window->statusBar()->setItemVisible("word-count", true);
        window->workspace()->ensureSideLeaf("outline", Workspace::Side::Right);
        QCoreApplication::processEvents(); // widgets added to a shown window appear from the event loop
        installCorePluginSwitches(window);
    }
    SettingsController* settings() { return SettingsController::of(window); }

    CommandRegistry* commands = nullptr;
    IconButton* graphButton = nullptr;
};

TEST_F(CorePluginsTest, DisablingHidesCommandsRibbonAndViews)
{
    ASSERT_TRUE(commands->isAvailable("graph:open"));
    ASSERT_FALSE(graphButton->isHidden());
    settings()->setCorePluginEnabled("graph", false);
    EXPECT_FALSE(commands->isAvailable("graph:open"));
    EXPECT_FALSE(commands->isAvailable("graph:open-local"));
    EXPECT_FALSE(commands->execute("graph:open"));
    EXPECT_TRUE(commands->isAvailable("switcher:open"));
    EXPECT_TRUE(commands->isAvailable("editor:toggle-bold"));
    EXPECT_TRUE(graphButton->isHidden());
    settings()->setCorePluginEnabled("graph", true);
    EXPECT_TRUE(commands->isAvailable("graph:open"));
    EXPECT_FALSE(graphButton->isHidden());

    // The user's own ribbon choice survives the plugin coming back.
    window->ribbon()->setActionHidden("graph:open", true);
    settings()->setCorePluginEnabled("graph", false);
    settings()->setCorePluginEnabled("graph", true);
    EXPECT_TRUE(graphButton->isHidden());
}

TEST_F(CorePluginsTest, SidebarViewsCloseAndReturn)
{
    Workspace* ws = window->workspace();
    ASSERT_EQ(ws->leavesOfType("outline").size(), 1);
    settings()->setCorePluginEnabled("outline", false);
    EXPECT_TRUE(ws->leavesOfType("outline").isEmpty());
    settings()->setCorePluginEnabled("outline", true);
    ASSERT_EQ(ws->leavesOfType("outline").size(), 1);
    EXPECT_TRUE(ws->leavesOfType("outline").first()->isInSidedock());
}

TEST_F(CorePluginsTest, StatusItemsFollowThePlugin)
{
    QLabel* item = window->statusBar()->item("word-count");
    ASSERT_TRUE(item->isVisible());
    settings()->setCorePluginEnabled("word-count", false);
    EXPECT_FALSE(item->isVisible());
    window->statusBar()->setItemVisible("word-count", true); // the plugin's updates can't show it
    EXPECT_FALSE(item->isVisible());
    settings()->setCorePluginEnabled("word-count", true);
    EXPECT_TRUE(item->isVisible());
}

TEST_F(CorePluginsTest, StateOnStartupIsApplied)
{
    // Plugins already off when the window is built start without their parts.
    settings()->setCorePluginEnabled("switcher", false);
    auto* other = new MainWindow(session.get(), false);
    auto* ws2 = new Workspace(session.get(), registry.get(), other);
    other->setWorkspace(ws2);
    IconButton* b = other->ribbon()->addAction("switcher:open", "file-search", "Open quick switcher");
    installCorePluginSwitches(other);
    EXPECT_TRUE(b->isHidden());
    delete other;
}
