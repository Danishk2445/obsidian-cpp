#include "render/theme/css_value.h"
#include "render/theme/theme.h"

#include <QHash>
#include <gtest/gtest.h>

using namespace oc;

namespace {
CssEvaluator makeEval(const QHash<QString, QString>& vars)
{
    return CssEvaluator([vars](const QString& n, bool* found) {
        *found = vars.contains(n);
        return vars.value(n);
    });
}
} // namespace

TEST(CssValue, HexColors)
{
    auto e = makeEval({});
    EXPECT_EQ(e.evaluate("#1C1C1C").toColor(), QColor(0x1c, 0x1c, 0x1c));
    EXPECT_EQ(e.evaluate("#fff").toColor(), QColor(255, 255, 255));
    EXPECT_EQ(e.evaluate("#11223344").toColor(), QColor(0x11, 0x22, 0x33, 0x44));
    EXPECT_EQ(e.evaluate("white").toColor(), QColor(255, 255, 255));
    EXPECT_EQ(e.evaluate("transparent").toColor().alpha(), 0);
}

TEST(CssValue, HslAccentMatchesObsidianDefault)
{
    auto e = makeEval({{"--accent-h", "258"}, {"--accent-s", "88%"}, {"--accent-l", "66%"}});
    const QColor c = e.evaluate("hsl(var(--accent-h), var(--accent-s), var(--accent-l))").toColor();
    // Obsidian's default accent is #8a5cf5 (rounding of hsl(258, 88%, 66%)).
    EXPECT_NEAR(c.red(), 0x8a, 1);
    EXPECT_NEAR(c.green(), 0x5c, 1);
    EXPECT_NEAR(c.blue(), 0xf5, 1);
}

TEST(CssValue, CalcInsideHsl)
{
    auto e = makeEval({{"--accent-h", "258"}, {"--accent-s", "88%"}, {"--accent-l", "66%"}});
    const QColor c = e.evaluate("hsl(calc(var(--accent-h) - 3), calc(var(--accent-s) * 1.02), calc(var(--accent-l) * 1.15))").toColor();
    // hsl(255, 89.76%, 75.9%) ~= #a68af9 (dark theme accent-1)
    EXPECT_NEAR(c.red(), 0xa6, 2);
    EXPECT_NEAR(c.green(), 0x8a, 2);
    EXPECT_NEAR(c.blue(), 0xf9, 2);
}

TEST(CssValue, RgbaWithVarTriplet)
{
    auto e = makeEval({{"--mono-rgb-100", "255, 255, 255"}});
    const QColor c = e.evaluate("rgba(var(--mono-rgb-100), 0.067)").toColor();
    EXPECT_EQ(c.red(), 255);
    EXPECT_NEAR(c.alphaF(), 0.067, 0.005);
}

TEST(CssValue, ColorMixWithTransparentKeepsColorScalesAlpha)
{
    auto e = makeEval({{"--interactive-accent", "#8a5cf5"}});
    const QColor c = e.evaluate("color-mix(in oklch, var(--interactive-accent) 20%, transparent)").toColor();
    EXPECT_NEAR(c.alphaF(), 0.2, 0.01);
    EXPECT_NEAR(c.red(), 0x8a, 2);
    EXPECT_NEAR(c.green(), 0x5c, 2);
    EXPECT_NEAR(c.blue(), 0xf5, 2);
}

TEST(CssValue, ColorMixSrgbTwoOpaque)
{
    auto e = makeEval({});
    const QColor c = e.evaluate("color-mix(in srgb, #000000 50%, #ffffff)").toColor();
    EXPECT_NEAR(c.red(), 128, 2);
    EXPECT_EQ(c.alpha(), 255);
}

TEST(CssValue, LengthsAndCalc)
{
    auto e = makeEval({{"--indent-unit", "0.5625em"}, {"--indent-size", "4"}, {"--font-text-size", "16px"}, {"--size-4-2", "8px"}});
    const CssValue li = e.evaluate("calc(var(--indent-unit) * var(--indent-size))");
    ASSERT_EQ(li.kind, CssValue::Length);
    EXPECT_DOUBLE_EQ(li.len.em, 2.25);
    EXPECT_DOUBLE_EQ(li.toPx(16), 36);
    EXPECT_DOUBLE_EQ(e.evaluate("calc(var(--font-text-size) * 0.9)").toPx(16), 14.4);
    EXPECT_DOUBLE_EQ(e.evaluate("calc(var(--size-4-2) + 1em)").toPx(10), 18);
    EXPECT_DOUBLE_EQ(e.evaluate("-0.015em").toPx(20), -0.3);
}

TEST(CssValue, VarFallbacks)
{
    auto e = makeEval({{"--a", "var(--missing, #ff0000)"}, {"--q", "'??'"}});
    EXPECT_EQ(e.evaluate("var(--a)").toColor(), QColor(255, 0, 0));
    EXPECT_EQ(e.evaluate("var(--q, 12px)").toPx(16), 12);
    EXPECT_EQ(e.evaluate("var(--nope, var(--also-nope, 3px))").toPx(16), 3);
}

TEST(CssValue, CycleIsSafe)
{
    auto e = makeEval({{"--a", "var(--b)"}, {"--b", "var(--a)"}});
    EXPECT_FALSE(e.evaluate("var(--a)").isValid());
}

TEST(ThemeTokens, DarkDefaultsMatchObsidian)
{
    ThemeManager tm;
    tm.setMode(ThemeManager::Mode::Dark);
    tm.apply();
    const ThemePtr t = tm.theme();
    ASSERT_TRUE(t);
    EXPECT_TRUE(t->isDark());
    EXPECT_EQ(t->color("--background-primary").name(), "#1c1c1c");
    EXPECT_EQ(t->color("--background-secondary").name(), "#282828");
    EXPECT_EQ(t->color("--text-normal").name(), "#dadada");
    EXPECT_EQ(t->color("--text-muted").name(), "#b3b3b3");
    EXPECT_EQ(t->color("--text-faint").name(), "#666666");
    const QColor ia = t->color("--interactive-accent");
    EXPECT_NEAR(ia.red(), 0x8a, 1);
    EXPECT_NEAR(ia.blue(), 0xf5, 1);
    EXPECT_DOUBLE_EQ(t->px("--h1-size"), 16 * 1.618);
    EXPECT_DOUBLE_EQ(t->number("--line-height-normal"), 1.5);
    EXPECT_DOUBLE_EQ(t->px("--file-line-width"), 700);
    EXPECT_DOUBLE_EQ(t->px("--ribbon-width"), 44);
    EXPECT_DOUBLE_EQ(t->px("--list-indent"), 36);
    EXPECT_TRUE(t->color("--tag-background").isValid());
    EXPECT_NEAR(t->color("--tag-background").alphaF(), 0.10, 0.01);
    EXPECT_EQ(t->color("--callout-quote").name(), "#9e9e9e");
}

TEST(ThemeTokens, LightDefaultsMatchObsidian)
{
    ThemeManager tm;
    tm.setMode(ThemeManager::Mode::Light);
    tm.apply();
    const ThemePtr t = tm.theme();
    EXPECT_FALSE(t->isDark());
    EXPECT_EQ(t->color("--background-primary").name(), "#ffffff");
    EXPECT_EQ(t->color("--text-normal").name(), "#222222");
    EXPECT_EQ(t->color("--background-secondary-alt").name(), "#fcfcfc");
}

TEST(ThemeTokens, AccentOverrideDerivesHsl)
{
    ThemeManager tm;
    tm.setMode(ThemeManager::Mode::Dark);
    tm.setAccent(QColor("#027aff"));
    const ThemePtr t = tm.theme();
    const QColor a = t->color("--color-accent");
    EXPECT_NEAR(a.red(), 0x02, 2);
    EXPECT_NEAR(a.green(), 0x7a, 2);
    EXPECT_NEAR(a.blue(), 0xff, 2);
}
