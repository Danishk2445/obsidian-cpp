// DisplayBlock: box (pill) padding is laid out, not painted over the neighbouring text.
#include "render/layout/display_block.h"
#include "render/layout/style_sheet.h"
#include "render/theme/theme.h"

#include <QTextLayout>
#include <gtest/gtest.h>

using namespace oc;

namespace {

struct Fixture {
    ThemeManager themes;
    StyleSheetPtr styles;
    Fixture()
    {
        themes.setMode(ThemeManager::Mode::Dark);
        styles = StyleSheet::build(*currentTheme());
    }
    qreal spacingAt(const DisplayBlock& b, int pos) const
    {
        qreal total = 0;
        for (const QTextLayout::FormatRange& fr : b.formats())
            if (fr.start <= pos && pos < fr.start + fr.length && fr.format.hasProperty(QTextFormat::FontLetterSpacing))
                total = fr.format.fontLetterSpacing(); // later ranges win, like QTextLayout's merge
        return total;
    }
};

} // namespace

TEST(DisplayBlock, TagPaddingIsReservedAroundThePill)
{
    Fixture f;
    const QString line = QStringLiteral("a #tag b");
    LineDecoration deco;
    deco.marks.push_back(Mark{2, 6, StyleId::Tag});
    const DisplayBlock b = DisplayBlock::build(line, deco, *f.styles);
    const qreal pad = f.styles->spec(StyleId::Tag).box.padX;
    ASSERT_GT(pad, 0);
    EXPECT_EQ(b.display(), line); // no spacer needed: the space before the tag takes the left padding
    ASSERT_EQ(b.boxes().size(), 1);
    EXPECT_TRUE(b.boxes()[0].reserved);
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 1), pad); // the space before '#'
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 5), pad); // the tag's last character
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 3), 0);
}

TEST(DisplayBlock, BoxAtLineStartGetsALeadSpacer)
{
    Fixture f;
    const QString line = QStringLiteral("#tag x");
    LineDecoration deco;
    deco.marks.push_back(Mark{0, 4, StyleId::Tag});
    const DisplayBlock b = DisplayBlock::build(line, deco, *f.styles);
    EXPECT_EQ(b.display(), QChar(0x00A0) + line);
    ASSERT_EQ(b.boxes().size(), 1);
    EXPECT_EQ(b.boxes()[0].dispFrom, 1);
    EXPECT_EQ(b.boxes()[0].dispTo, 5);
    // The caret before the tag sits before the spacer (outside the pill); columns map past it.
    EXPECT_EQ(b.docToDisp(0), 0);
    EXPECT_EQ(b.docToDisp(1), 2);
    EXPECT_EQ(b.docToDisp(6), 7);
    EXPECT_EQ(b.dispToDoc(0), 0);
    EXPECT_EQ(b.dispToDoc(1), 0);
    EXPECT_EQ(b.dispToDoc(2), 1);
    // The spacer is exactly the padding wide.
    QTextLayout tl(b.display(), f.styles->line(StyleId::Normal).font);
    tl.setFormats(b.formats());
    tl.beginLayout();
    QTextLine ql = tl.createLine();
    ql.setLineWidth(1000);
    tl.endLayout();
    EXPECT_NEAR(ql.cursorToX(1) - ql.cursorToX(0), f.styles->spec(StyleId::Tag).box.padX, 0.6);
}

TEST(DisplayBlock, AdjacentBoxesAddTheirPaddings)
{
    Fixture f;
    const QString line = QStringLiteral("x`a`#t");
    LineDecoration deco;
    deco.replaces.push_back(Replace{1, 2, Replace::Kind::Hide});
    deco.replaces.push_back(Replace{3, 4, Replace::Kind::Hide});
    deco.marks.push_back(Mark{2, 3, StyleId::InlineCode});
    deco.marks.push_back(Mark{4, 6, StyleId::Tag});
    const DisplayBlock b = DisplayBlock::build(line, deco, *f.styles);
    EXPECT_EQ(b.display(), QStringLiteral("xa#t"));
    const qreal code = f.styles->spec(StyleId::InlineCode).box.padX, tag = f.styles->spec(StyleId::Tag).box.padX;
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 0), code);       // before the code span
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 1), code + tag); // end of code + start of tag
    EXPECT_DOUBLE_EQ(f.spacingAt(b, 3), tag);
}
