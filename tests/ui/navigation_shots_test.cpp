// Screenshots of the quick switcher, the command palette and the search view in both themes (skipped unless
// OC_SHOT_DIR is set): nav-switcher-*.png, nav-switcher-recent-*.png, nav-palette-*.png, nav-search-*.png,
// nav-search-settings-*.png.
#include "navigation_test_support.h"

#include <QJsonArray>

using namespace oc;
using namespace oc::test;

namespace {

QString shotDir()
{
    return QString::fromLocal8Bit(qgetenv("OC_SHOT_DIR"));
}

void setTheme(bool dark)
{
    ThemeManager* tm = testThemes();
    tm->setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
    tm->apply();
    QApplication::processEvents();
}

bool save(NavigationEnv& env, const QString& name)
{
    QCursor::setPos(-10000, -10000); // nothing hovered
    QApplication::processEvents();
    env.window->repaint();
    return env.window->grab().save(shotDir() + "/" + name);
}

class NavigationShots : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override
    {
        if (shotDir().isEmpty())
            GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
        setTheme(GetParam());
    }
    void TearDown() override { setTheme(true); }
    QString suffix() const { return GetParam() ? "dark" : "light"; }
};

std::unique_ptr<NavigationEnv> makeEnv()
{
    auto env = std::make_unique<NavigationEnv>();
    env->window->resize(1400, 850);
    env->openNote("Formatting.md");
    env->openNote("Projects/Alpha.md");
    env->openNote("Math.md");
    env->openNote("Welcome.md");
    QApplication::processEvents();
    return env;
}

} // namespace

TEST_P(NavigationShots, QuickSwitcher)
{
    auto env = makeEnv();
    SuggestModal* m = openQuickSwitcher(env->session, env->layer());
    ASSERT_NE(m, nullptr);
    QApplication::processEvents();
    EXPECT_TRUE(save(*env, "nav-switcher-recent-" + suffix() + ".png"));
    QTest::keyClicks(m->input(), "no");
    QApplication::processEvents();
    EXPECT_TRUE(save(*env, "nav-switcher-" + suffix() + ".png"));
    m->close();
}

TEST_P(NavigationShots, CommandPalette)
{
    auto env = makeEnv();
    env->session->config()->setObject("command-palette", QJsonObject{{"pinned", QJsonArray{"switcher:open"}}});
    SuggestModal* m = openCommandPalette(env->session, env->layer());
    ASSERT_NE(m, nullptr);
    QTest::keyClicks(m->input(), "toggle");
    QApplication::processEvents();
    EXPECT_TRUE(save(*env, "nav-palette-" + suffix() + ".png"));
    m->close();
}

TEST_P(NavigationShots, SearchView)
{
    auto env = makeEnv();
    SearchView* v = openGlobalSearch(env->session, "welcome");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(env->waitIdle(v));
    env->openNote("Welcome.md"); // focus back to the editor (the input would show a caret)
    QApplication::processEvents();
    EXPECT_TRUE(save(*env, "nav-search-" + suffix() + ".png"));
    v->setSettingsVisible(true);
    v->setExplainSearch(true);
    v->setQuery("callout OR tag:#fixture");
    ASSERT_TRUE(env->waitIdle(v));
    env->openNote("Welcome.md");
    QApplication::processEvents();
    EXPECT_TRUE(save(*env, "nav-search-settings-" + suffix() + ".png"));
}

INSTANTIATE_TEST_SUITE_P(Themes, NavigationShots, ::testing::Values(true, false), [](const auto& info) { return info.param ? "Dark" : "Light"; });
