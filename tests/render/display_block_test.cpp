// DisplayBlock: box (pill) padding is laid out, not painted over the neighbouring text; padding spacers keep a caret after them.
#include "render/layout/display_block.h"
#include "render/layout/doc_layout.h"
#include "render/layout/style_sheet.h"
#include "render/theme/theme.h"
#include "render/widgets/inline_widgets.h"

#include <QImage>
#include <QPainter>
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

// A hidden line (folded, or swallowed by a block in Reading view) may still carry inline widgets:
// painting and hit-testing must skip them (the 5.5 MB note crashed Reading view).
namespace {

struct Dot : InlineWidget {
    QSizeF size(const LineMetrics&) const override { return {12, 12}; }
    void paint(QPainter* p, const QRectF& r, const LineMetrics&) const override { p->fillRect(r, Qt::red); }
    quint64 signature() const override { return 7; }
};

struct HidingDecorator : LineDecorator {
    LineDecoration decorate(int line) override
    {
        LineDecoration d;
        d.replaces.push_back(Replace{0, 1, Replace::Kind::Widget, {}, StyleId::Normal, std::make_shared<Dot>()});
        d.hidden = line == 1;
        return d;
    }
    StyleId quickLineStyle(int) override { return StyleId::Normal; }
};

} // namespace

TEST(DisplayBlock, HiddenLinesWithWidgetsPaintAndHitTest)
{
    Fixture f;
    HidingDecorator deco;
    DocLayout layout;
    layout.setDecorator(&deco);
    layout.setStyleSheet(f.styles);
    layout.setContentWidth(400);
    layout.setText(Text::fromString(QStringLiteral("x one\ny two\nz three")));
    layout.ensureLaidOut(0, 1e6);
    QImage img(400, 300, QImage::Format_ARGB32);
    QPainter p(&img);
    DocLayout::PaintContext pc;
    pc.clip = QRectF(0, 0, 400, 300);
    pc.rightEdge = 400;
    layout.paint(&p, pc);
    p.end();
    for (qreal y = 0; y < layout.totalHeight(); y += 4)
        (void)layout.widgetAt(QPointF(5, y));
    EXPECT_GT(layout.totalHeight(), 0);
}

TEST(DisplayBlock, PaddingSpacerKeepsItsColumnsCaretAfterIt)
{
    // A zero-length SpacerWidget is CSS padding in front of the text at its column (Live Preview's list marker
    // padding): a caret at that column is drawn after the padding, inside the padded span; assoc < 0 keeps it before
    // (where the fold indicator goes). Other zero-length widgets keep the caret before them.
    Fixture f;
    const QString line = QStringLiteral("\t- a");
    LineDecoration deco;
    deco.replaces.push_back(Replace{1, 1, Replace::Kind::Widget, {}, StyleId::Normal, std::make_shared<SpacerWidget>(12)});
    const DisplayBlock b = DisplayBlock::build(line, deco, *f.styles);
    ASSERT_EQ(b.display().size(), line.size() + 1);
    EXPECT_EQ(b.docToDisp(1, 1), 2);
    EXPECT_EQ(b.docToDisp(1, -1), 1);
    EXPECT_EQ(b.docToDisp(0, 1), 0);
    EXPECT_EQ(b.docToDisp(2, 1), 3);
    EXPECT_EQ(b.dispToDoc(1, 1), 1);
    EXPECT_EQ(b.dispToDoc(2, 1), 1);
    // at the line start too
    LineDecoration start;
    start.replaces.push_back(Replace{0, 0, Replace::Kind::Widget, {}, StyleId::Normal, std::make_shared<SpacerWidget>(12)});
    const DisplayBlock s = DisplayBlock::build(QStringLiteral("- a"), start, *f.styles);
    EXPECT_EQ(s.docToDisp(0, 1), 1);
    EXPECT_EQ(s.docToDisp(0, -1), 0);
    // a zero-length widget that isn't padding (a fold placeholder at the line end): the caret stays before it
    LineDecoration other;
    other.replaces.push_back(Replace{3, 3, Replace::Kind::Widget, {}, StyleId::Normal, std::make_shared<BulletWidget>(Qt::red, 8, 4, 4)});
    const DisplayBlock o = DisplayBlock::build(QStringLiteral("- a"), other, *f.styles);
    EXPECT_EQ(o.docToDisp(3, 1), 3);
}
