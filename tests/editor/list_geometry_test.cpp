// List geometry against Obsidian's CSS (app.css 1.13). Live Preview and Source keep the source indentation (CM6
// lines): the marker token has padding-inline-start --list-indent-editing (0.75em, Live Preview) / 0 (Source),
// list lines tab-size --list-indent, guide units min-width --list-indent, a continuation line's last spacing unit
// --list-indent-editing + --list-marker-space, wrapped lines hang under the list prefix (floored). Reading view nests
// by depth: li margin-inline-start 3ch (--list-indent in a list of the same type), bullets floated -0.8em, numbers as
// outside ::markers, checkboxes at -1.5 * --checkbox-size, blockquotes 2px + 24px, --list-spacing around items, no
// gap in loose lists, --p-spacing around a list next to another block; its guides at -0.85em from a nested list.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/live_preview_decorator.h"
#include "editor/source_decorator.h"
#include "markdown/doc_source.h"
#include "render/layout/display_block.h"
#include "render/layout/doc_layout.h"
#include "render/theme/theme.h"
#include "render/widgets/inline_widgets.h"

#include <QApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <cmath>

using namespace oc;

namespace {

struct Env {
    ThemeManager themes;
    StyleSheetPtr styles;
    Env()
    {
        themes.setMode(ThemeManager::Mode::Dark);
        themes.apply();
        styles = StyleSheet::build(*currentTheme());
    }
    qreal em() const { return styles->textSize(); }
    const LineMetrics& lm() const { return styles->line(StyleId::Normal); }
    // widths in the text font; `tnum`: font-variant-numeric: tabular-nums (.cm-formatting-list-*, ::marker)
    qreal w(const QString& s, bool tnum = false) const
    {
        QFont f = lm().font;
        if (tnum)
            f.setFeature(QFont::Tag("tnum"), 1);
        return QFontMetricsF(f).horizontalAdvance(s);
    }
    qreal mono(const QString& s) const
    {
        QFont f = lm().font;
        f.setFamilies(styles->spec(StyleId::Monospace).format.fontFamilies().toStringList());
        f.setPixelSize(int(std::lround(em())));
        return QFontMetricsF(f).horizontalAdvance(s);
    }
};

// A note decorated like the editor (Live Preview / Reading) or Source mode, laid out with a DocLayout.
struct Note {
    md::StaticDocSource src;
    LivePreviewDecorator lp;
    SourceDecorator source;
    DocLayout layout;
    Note(const QString& markdown, const StyleSheetPtr& styles, EditorView::Mode mode, qreal width = 700)
        : src(Text::fromString(markdown))
        , lp(&src)
        , source(&src)
    {
        lp.setStyleSheet(styles.get());
        lp.setReading(mode == EditorView::Mode::Reading);
        lp.setSelection(Selection::cursor(src.text().length()));
        lp.setContentWidth(width);
        source.setStyleSheet(styles.get());
        layout.setDecorator(mode == EditorView::Mode::Source ? static_cast<LineDecorator*>(&source) : &lp);
        layout.setStyleSheet(styles);
        layout.setContentWidth(width);
        layout.setText(src.text());
        layout.ensureLaidOut(0, 1e9);
    }
    void relayout()
    {
        layout.invalidateAll();
        layout.ensureLaidOut(0, 1e9);
    }
    void select(qsizetype pos)
    {
        lp.setSelection(Selection::cursor(pos));
        relayout();
    }
    const DocLayout::UnitLayout& ul(int line) { return *layout.layoutFor(line); }
    const LineDecoration& deco(int line) { return ul(line).deco; }
    // x of a source column on the line's first visual line
    qreal colX(int line, int col) { return ul(line).text->lineAt(0).cursorToX(ul(line).block.docToDisp(col)); }
    // x where the line's visible text starts (its first non-empty text segment)
    qreal textX(int line)
    {
        for (const DisplayBlock::Segment& s : ul(line).block.segments())
            if (s.kind == DisplayBlock::Segment::Kind::Text && s.docTo > s.docFrom)
                return ul(line).text->lineAt(0).cursorToX(s.dispFrom);
        return -1000;
    }
    qreal wrapX(int line) { return ul(line).lines.size() > 1 ? ul(line).lines[1].x : -1000; }
    template <class W>
    const W* widget(int line, QRectF* rect = nullptr)
    {
        for (int i = 0; i < ul(line).block.widgets().size(); ++i)
            if (auto* w = dynamic_cast<const W*>(ul(line).block.widgets()[i].widget.get())) {
                if (rect)
                    *rect = ul(line).widgetRects[i];
                return w;
            }
        return nullptr;
    }
    template <class W>
    const W* floating(int line, QRectF* rect = nullptr)
    {
        for (const FloatWidget& f : deco(line).floatWidgets)
            if (auto* w = dynamic_cast<const W*>(f.widget.get())) {
                if (rect) {
                    const LineMetrics& lm = layout.styleSheet()->line(deco(line).lineStyle);
                    const QSizeF s = w->size(lm);
                    *rect = QRectF(f.x, ul(line).lines.first().baseline - w->ascent(lm), s.width(), s.height());
                }
                return w;
            }
        return nullptr;
    }
    qreal lineTop(int line) { return layout.lineTop(line); }
};

const QString kLong = QStringLiteral("a long item that wraps across the narrow column so that the hanging indent shows where it goes");

} // namespace

TEST(ListGeometry, ThemeTokens)
{
    // docs/obsidian-reference.md: 16px text, --list-indent 2.25em, --list-bullet-size 0.3em, ...
    Env env;
    const qreal em = env.em();
    EXPECT_NEAR(env.styles->listIndent(), 2.25 * em, 0.01);
    EXPECT_NEAR(env.styles->listIndentEditing(), 0.75 * em, 0.01);
    EXPECT_NEAR(env.styles->listBulletSize(), 0.3 * em, 0.01);
    EXPECT_NEAR(env.styles->listSpacing(), 0.075 * em, 0.01);
    EXPECT_NEAR(env.styles->checkboxSize(), em, 0.01);
    EXPECT_NEAR(env.styles->checkboxMarginStart(), 0.85 * em, 0.01);
    EXPECT_NEAR(env.styles->ch(), env.w(QStringLiteral("0")), 0.01);
    EXPECT_NEAR(env.styles->quoteBarWidth(), 2, 0.01);
    EXPECT_NEAR(env.styles->quoteIndent(), 24, 0.01);
}

// ---- Live Preview -------------------------------------------------------------------------------------------------

TEST(ListGeometry, LivePreviewBulletsAndTextPerLevel)
{
    Env env;
    const qreal em = env.em(), P = 0.75 * em, unit = 2.25 * em, dash = env.w(QStringLiteral("-"), true);
    const qreal text0 = P + env.w(QStringLiteral("- "), true);
    const QString md = QStringLiteral("- a\n"      // 0
                                      "\t- b\n"    // 1  tab
                                      "\t\t- c\n"  // 2
                                      "    - d\n"  // 3  four spaces: a .cm-indent, min-width --list-indent
                                      "  - e\n");  // 4  two spaces: .cm-indent-spacing, natural width
    Note n(md, env.styles, EditorView::Mode::LivePreview);
    const qreal indentOf[] = {0, unit, 2 * unit, unit, 2 * env.lm().spaceWidth};
    for (int l = 0; l < 5; ++l) {
        QRectF r;
        const BulletWidget* b = n.widget<BulletWidget>(l, &r);
        ASSERT_TRUE(b) << l;
        // .list-bullet: the "-" (transparent) after the marker padding, the disc centred in it
        EXPECT_NEAR(r.left(), indentOf[l] + P, 0.6) << l;
        EXPECT_NEAR(r.width(), dash, 0.01) << l;
        EXPECT_NEAR(b->dotCenter(r, env.lm()).x(), indentOf[l] + P + dash / 2, 0.6) << l;
        EXPECT_NEAR(b->dotSize(), 0.3 * em, 0.01);
        // centred in the line box: (ascent - descent) / 2 above the baseline
        EXPECT_NEAR(b->dotCenter(r, env.lm()).y(), n.ul(l).lines[0].baseline - (env.lm().ascent - env.lm().descent) / 2, 0.01);
        EXPECT_NEAR(n.colX(l, int(n.src.text().lineLength(l)) - 1), indentOf[l] + text0, 0.6) << l;
    }
    // --list-spacing above and below a list line
    EXPECT_NEAR(n.deco(1).paddingTop, 0.075 * em, 0.01);
    EXPECT_NEAR(n.deco(1).paddingBottom, 0.075 * em, 0.01);
    // guides off: no .cm-indent min-width, four spaces keep their natural width (tabs still use tab-size)
    n.lp.setIndentGuides(false);
    n.relayout();
    EXPECT_NEAR(n.colX(3, 6), 4 * env.lm().spaceWidth + text0, 0.6);
    EXPECT_NEAR(n.colX(1, 3), unit + text0, 0.6);
}

TEST(ListGeometry, LivePreviewGuidesThroughTheParentBullet)
{
    Env env;
    const qreal em = env.em(), P = 0.75 * em, dash = env.w(QStringLiteral("-"), true), dot = 0.3 * em;
    Note n(QStringLiteral("- a\n\t- b\n\t\t- c\n"), env.styles, EditorView::Mode::LivePreview);
    // .cm-indent::before { margin-inline-start: var(--indentation-guide-editing-indent) } + its 1px content box
    const qreal guide = n.colX(2, 1) + n.deco(2).guideOffset;
    EXPECT_NEAR(n.deco(2).guideOffset, 0.85 * em + 1, 0.01);
    EXPECT_NEAR(n.colX(2, 0) + n.deco(2).guideOffset, 0.85 * em + 1, 0.6);
    // ...which runs through the parent's disc (P + dash / 2 +- dot / 2) at every level
    const qreal parentDot = 2.25 * em + P + dash / 2;
    EXPECT_GT(guide + 0.5, parentDot - dot / 2);
    EXPECT_LT(guide + 0.5, parentDot + dot / 2);
}

TEST(ListGeometry, LivePreviewWrapIndent)
{
    Env env;
    const qreal em = env.em(), P = 0.75 * em, unit = 2.25 * em, sp = env.lm().spaceWidth;
    const QString md = QStringLiteral("- ") + kLong + QStringLiteral("\n"           // 0
                       "\t- ") + kLong + QStringLiteral("\n"                        // 1
                       "\t  ") + kLong + QStringLiteral("\n"                        // 2  continuation
                       "- [ ] ") + kLong + QStringLiteral("\n"                      // 3
                       "1. ") + kLong + QStringLiteral("\n"                         // 4
                       "> ") + kLong + QStringLiteral("\n");                        // 5  (quote prefix)
    Note n(md, env.styles, EditorView::Mode::LivePreview, 320);
    const qreal text0 = P + env.w(QStringLiteral("- "), true);
    // text-indent: -Kpx; padding-inline-start: Kpx, K = floor(x after the list prefix)
    EXPECT_EQ(n.wrapX(0), std::floor(text0));
    EXPECT_EQ(n.wrapX(1), std::floor(unit + text0));
    // .HyperMD-list-line-nobullet .cm-indent-spacing:last-child { padding-inline-start: 0.75em + 0.25em }
    EXPECT_NEAR(n.colX(2, 3), unit + em + 2 * sp, 0.6);
    EXPECT_EQ(n.wrapX(2), std::floor(n.colX(2, 3) + 1e-3));
    EXPECT_EQ(n.deco(2).paddingTop, 0); // .HyperMD-list-line-nobullet { padding-top: initial }
    EXPECT_NEAR(n.deco(2).paddingBottom, 0.075 * em, 0.01);
    EXPECT_EQ(n.wrapX(3), std::floor(n.colX(3, 6) + 1e-3));
    EXPECT_EQ(n.wrapX(4), std::floor(P + env.w(QStringLiteral("1. "), true) + 1e-3));
    EXPECT_EQ(n.wrapX(5), std::floor(n.colX(5, 2) + 1e-3));
    // without guides there are no .cm-indent-spacing spans: no padding
    n.lp.setIndentGuides(false);
    n.relayout();
    EXPECT_NEAR(n.colX(2, 3), unit + 2 * sp, 0.6);
}

TEST(ListGeometry, LivePreviewOrderedTasksAndReveal)
{
    Env env;
    const qreal em = env.em(), P = 0.75 * em, sp = env.lm().spaceWidth;
    const QString md = QStringLiteral("1. one\n"     // 0
                                      "- [ ] todo\n" // 1
                                      "- [x] done\n" // 2
                                      "- [-] gone\n" // 3
                                      "1. [ ] ord\n" // 4  (a "3." couldn't interrupt the paragraph above)
                                      "- plain\n");  // 5
    Note n(md, env.styles, EditorView::Mode::LivePreview);
    const Text& t = n.src.text();
    // ordered: padded, "1." in tabular figures, text after the token
    EXPECT_NEAR(n.colX(0, 3), P + env.w(QStringLiteral("1. "), true), 0.6);
    // a task: "- " replaced (no padded span), the label's -0.25em + the checkbox's 0.85em, 1em box, 0.25em after
    QRectF r;
    const CheckboxWidget* cb = n.widget<CheckboxWidget>(1, &r);
    ASSERT_TRUE(cb);
    const QRectF box = cb->boxRect(r, env.lm());
    EXPECT_NEAR(box.left(), 0.6 * em, 0.6);
    EXPECT_NEAR(box.width(), em, 0.01);
    EXPECT_NEAR(n.colX(1, 6), 0.6 * em + em + 0.25 * em + sp, 0.6);
    // vertical-align: middle (x-height / 2 above the baseline) and top: -0.1em
    EXPECT_NEAR(box.center().y(), n.ul(1).lines[0].baseline - QFontMetricsF(env.lm().font).xHeight() / 2 - 0.1 * em, 0.01);
    // only x / X strike the task through (.HyperMD-task-line[data-task="x"])
    auto struck = [&](int line) {
        for (const Mark& m : n.deco(line).marks)
            if (m.style == StyleId::TaskDone)
                return true;
        return false;
    };
    EXPECT_FALSE(struck(1));
    EXPECT_TRUE(struck(2));
    EXPECT_FALSE(struck(3));
    EXPECT_TRUE(n.widget<CheckboxWidget>(3)->checked());
    // an ordered task keeps its number
    QRectF r4;
    ASSERT_TRUE(n.widget<CheckboxWidget>(4, &r4));
    EXPECT_NEAR(r4.left(), P + env.w(QStringLiteral("1. "), true), 0.6);
    // the selection on the marker token reveals the "-" (same width, so the text doesn't move)
    const qreal plainText = n.colX(5, 2);
    n.select(t.lineStart(5) + 1);
    EXPECT_FALSE(n.widget<BulletWidget>(5));
    EXPECT_NEAR(n.colX(5, 2), plainText, 0.01);
    n.select(t.lineStart(5) + 4); // in the text: the bullet again
    EXPECT_TRUE(n.widget<BulletWidget>(5));
    // ... and a task's "- [ ]": padded marker, the box in monospace
    n.select(t.lineStart(1) + 3);
    EXPECT_FALSE(n.widget<CheckboxWidget>(1));
    EXPECT_NEAR(n.colX(1, 6), P + env.w(QStringLiteral("- "), true) + env.mono(QStringLiteral("[ ]")) + sp, 0.6);
}

// ---- Source mode --------------------------------------------------------------------------------------------------

TEST(ListGeometry, SourceModeMarkers)
{
    Env env;
    const qreal em = env.em(), unit = 2.25 * em, sp = env.lm().spaceWidth;
    const QString md = QStringLiteral("- a\n"            // 0
                                      "\t- b\n"          // 1
                                      "\t  cont\n"       // 2
                                      "- [ ] task\n"     // 3
                                      "- ") + kLong + QStringLiteral("\n"); // 4
    Note n(md, env.styles, EditorView::Mode::Source, 320);
    const qreal text0 = env.w(QStringLiteral("- "), true); // --list-indent-source: 0
    EXPECT_NEAR(n.colX(0, 2), text0, 0.6);
    EXPECT_NEAR(n.colX(1, 3), unit + text0, 0.6);
    // continuation: --list-indent-editing + --list-marker-space (0 in Source mode)
    EXPECT_NEAR(n.colX(2, 3), unit + 0.75 * em + 2 * sp, 0.6);
    EXPECT_NEAR(n.colX(3, 6), text0 + env.mono(QStringLiteral("[ ]")) + sp, 0.6);
    EXPECT_EQ(n.wrapX(4), std::floor(text0 + 1e-3));
    n.source.setIndentGuides(false);
    n.relayout();
    EXPECT_NEAR(n.colX(2, 3), unit + 2 * sp, 0.6);
}

// ---- Reading view -------------------------------------------------------------------------------------------------

TEST(ListGeometry, ReadingNestsByDepthNotSourceColumns)
{
    Env env;
    const qreal em = env.em(), ch3 = 3 * env.styles->ch(), unit = 2.25 * em, dot = 0.3 * em;
    const QString md = QStringLiteral("- tab\n"          // 0
                                      "\t- child\n"      // 1
                                      "\t  cont\n"       // 2  continuation of "child"
                                      "- two\n"          // 3
                                      "  - child\n"      // 4
                                      "- four\n"         // 5
                                      "    - child\n"    // 6
                                      "\t\t- grand\n"    // 7
                                      "lazy line\n");    // 8  lazy continuation of "grand"
    Note n(md, env.styles, EditorView::Mode::Reading);
    for (int l : {0, 3, 5})
        EXPECT_NEAR(n.textX(l), ch3, 0.01) << l;
    for (int l : {1, 2, 4, 6})
        EXPECT_NEAR(n.textX(l), ch3 + unit, 0.01) << l;
    EXPECT_NEAR(n.textX(7), ch3 + 2 * unit, 0.01);
    EXPECT_NEAR(n.textX(8), ch3 + 2 * unit, 0.01);
    // bullets: floated -0.8em, the disc centred there
    for (int l : {0, 1, 4, 6, 7}) {
        QRectF r;
        const BulletWidget* b = n.floating<BulletWidget>(l, &r);
        ASSERT_TRUE(b) << l;
        EXPECT_NEAR(b->dotCenter(r, env.lm()).x(), n.textX(l) - 0.8 * em, 0.01) << l;
        EXPECT_NEAR(b->dotSize(), dot, 0.01);
    }
    EXPECT_TRUE(n.deco(2).floatWidgets.isEmpty());
    // nothing of the source prefix shows
    EXPECT_EQ(n.ul(1).block.display(), QStringLiteral("child"));
    EXPECT_EQ(n.ul(2).block.display(), QStringLiteral("cont"));
    // guides: li > ul::before at --indentation-guide-reading-indent (-0.85em) from the nested list, i.e. through the
    // parent's bullet
    ASSERT_EQ(n.deco(7).guidePositions.size(), 2);
    EXPECT_NEAR(n.deco(7).guidePositions[0], ch3 - 0.85 * em, 0.01);
    EXPECT_NEAR(n.deco(7).guidePositions[1], ch3 + unit - 0.85 * em, 0.01);
    const qreal g = n.deco(7).guidePositions[1] + env.styles->guideWidth() / 2;
    QRectF pr;
    const BulletWidget* parent = n.floating<BulletWidget>(6, &pr);
    EXPECT_LT(std::abs(g - parent->dotCenter(pr, env.lm()).x()), dot / 2);
    // wrapped lines start at the item's content edge
    Note w(QStringLiteral("- ") + kLong + QStringLiteral("\n\t- ") + kLong + QStringLiteral("\n"), env.styles, EditorView::Mode::Reading, 320);
    EXPECT_NEAR(w.wrapX(0), ch3, 0.01);
    EXPECT_NEAR(w.wrapX(1), ch3 + unit, 0.01);
}

TEST(ListGeometry, ReadingOrderedMarkers)
{
    Env env;
    const qreal em = env.em(), ch3 = 3 * env.styles->ch(), unit = 2.25 * em;
    const QString md = QStringLiteral("1. a\n"          // 0
                                      "1. b\n"          // 1
                                      "7. c\n"          // 2
                                      "\t1. nested\n"   // 3  ol ol > li: --list-indent
                                      "\t\t- mixed\n"   // 4  ol ol ul > li: 3ch (no ul above)
                                      "\n"              // 5
                                      "5) five\n"       // 6
                                      "- b\n"           // 7
                                      "\t1. in ul\n");  // 8  ul ol > li: 3ch
    Note n(md, env.styles, EditorView::Mode::Reading);
    auto marker = [&](int line, QRectF* r) { return n.floating<MarkerTextWidget>(line, r); };
    const char* expect[] = {"1. ", "2. ", "3. "};
    for (int l = 0; l < 3; ++l) {
        QRectF r;
        const MarkerTextWidget* m = marker(l, &r);
        ASSERT_TRUE(m) << l;
        EXPECT_EQ(m->text(), QString::fromLatin1(expect[l])); // counted from the list's start
        // outside the item, its end (the trailing space included) at the content start; tabular figures
        EXPECT_NEAR(r.right(), ch3, 0.01) << l;
        EXPECT_NEAR(r.width(), env.w(m->text(), true), 0.01) << l;
        EXPECT_NEAR(n.textX(l), ch3, 0.01) << l;
    }
    QRectF r3;
    ASSERT_TRUE(marker(3, &r3));
    EXPECT_NEAR(r3.right(), ch3 + unit, 0.01);
    EXPECT_NEAR(n.textX(4), ch3 + unit + ch3, 0.01);
    QRectF r6;
    ASSERT_TRUE(marker(6, &r6));
    EXPECT_EQ(marker(6, &r6)->text(), QStringLiteral("5. ")); // <ol start="5">, rendered with "."
    EXPECT_NEAR(n.textX(8), ch3 + ch3, 0.01);
}

TEST(ListGeometry, ReadingTasks)
{
    Env env;
    const qreal em = env.em(), ch3 = 3 * env.styles->ch(), unit = 2.25 * em, box = em;
    const QString md = QStringLiteral("- [ ] todo\n"                          // 0
                                      "\t- [x] ") + kLong + QStringLiteral("\n" // 1
                                      "\t- [-] cancelled\n"                   // 2
                                      "\n"                                    // 3
                                      "1. [ ] ordered\n");                    // 4
    Note n(md, env.styles, EditorView::Mode::Reading, 320);
    // ul > li.task-list-item > .task-list-item-checkbox { margin-inline-start: calc(var(--checkbox-size) * -1.5) },
    // margin-inline-end: 0.6em; no bullet
    for (int l : {0, 1}) {
        const qreal x = l == 0 ? ch3 : ch3 + unit;
        QRectF r;
        const CheckboxWidget* cb = n.floating<CheckboxWidget>(l, &r);
        ASSERT_TRUE(cb) << l;
        EXPECT_FALSE(n.floating<BulletWidget>(l));
        const QRectF b = cb->boxRect(r, env.lm());
        EXPECT_NEAR(b.left(), x - 1.5 * box, 0.01) << l;
        EXPECT_NEAR(b.width(), box, 0.01);
        // on the baseline, moved down by top: 0.2em
        EXPECT_NEAR(b.bottom(), n.ul(l).lines[0].baseline + 0.2 * em, 0.01);
        EXPECT_NEAR(n.textX(l), x - 1.5 * box + box + 0.6 * em, 0.01) << l;
    }
    EXPECT_NEAR(n.wrapX(1), ch3 + unit, 0.01); // wrapped lines at the li's content edge
    EXPECT_EQ(n.deco(0).floatWidgets.first().from, 2); // a click toggles the "[ ]" at column 2
    auto struck = [&](int line) {
        for (const Mark& m : n.deco(line).marks)
            if (m.style == StyleId::TaskDone)
                return true;
        return false;
    };
    EXPECT_TRUE(struck(1));
    EXPECT_FALSE(struck(2)); // [data-task="x"] / "X" only
    // an ordered task: its number stays, the checkbox is inline at the content start (the -1.5 margin is ul-only)
    QRectF r4;
    ASSERT_TRUE(n.floating<MarkerTextWidget>(4));
    const CheckboxWidget* cb4 = n.widget<CheckboxWidget>(4, &r4);
    ASSERT_TRUE(cb4);
    EXPECT_NEAR(cb4->boxRect(r4, env.lm()).left(), ch3, 0.01);
    EXPECT_NEAR(n.textX(4), ch3 + box + 0.6 * em, 0.01);
}

TEST(ListGeometry, ReadingSpacing)
{
    Env env;
    const qreal em = env.em(), s = 0.075 * em, p = env.styles->pSpacing();
    // tight and loose lists look the same: li p:first-of-type / :last-of-type lose their margins
    Note tight(QStringLiteral("- a\n- b\n- c\n"), env.styles, EditorView::Mode::Reading);
    Note loose(QStringLiteral("- a\n\n- b\n\n\n- c\n"), env.styles, EditorView::Mode::Reading);
    EXPECT_NEAR(loose.lineTop(2) - loose.lineTop(0), tight.lineTop(1) - tight.lineTop(0), 0.01);
    EXPECT_NEAR(loose.lineTop(5) - loose.lineTop(0), tight.lineTop(2) - tight.lineTop(0), 0.01);
    EXPECT_TRUE(loose.deco(1).hidden);
    // li { padding-block: var(--list-spacing) } around each item, nested ones inside their parent's
    Note nested(QStringLiteral("- a\n\t- b\n\t- c\n- d\n"), env.styles, EditorView::Mode::Reading);
    EXPECT_NEAR(nested.deco(0).paddingTop, s, 0.01);
    EXPECT_NEAR(nested.deco(0).paddingBottom, 0, 0.01);
    EXPECT_NEAR(nested.deco(1).paddingTop, s, 0.01);
    EXPECT_NEAR(nested.deco(1).paddingBottom, s, 0.01);
    EXPECT_NEAR(nested.deco(2).paddingBottom, 2 * s, 0.01); // "c" and "a" end here
    // a second paragraph in an item keeps the paragraph gap; a blank line before a nested list doesn't
    Note paras(QStringLiteral("- a\n\n  second\n\n  - nested\n- b\n"), env.styles, EditorView::Mode::Reading);
    EXPECT_FALSE(paras.deco(1).hidden);
    EXPECT_NEAR(paras.layout.lineHeight(1), p, 0.01);
    EXPECT_TRUE(paras.deco(3).hidden);
    EXPECT_NEAR(paras.textX(2), 3 * env.styles->ch(), 0.01);
    // ul, ol { margin-block: var(--p-spacing) }: a list right after a paragraph keeps a gap; nested lists have none
    Note after(QStringLiteral("Things:\n- a\n\t- b\n"), env.styles, EditorView::Mode::Reading);
    EXPECT_NEAR(after.deco(1).paddingTop, p + s, 0.01);
    EXPECT_NEAR(after.deco(2).paddingTop, s, 0.01);
    // two adjacent lists: one gap (the margins collapse)
    Note two(QStringLiteral("- a\n1. b\n"), env.styles, EditorView::Mode::Reading);
    EXPECT_NEAR(two.deco(0).paddingBottom + two.deco(1).paddingTop, p + 2 * s, 0.01);
}

TEST(ListGeometry, ReadingQuotesAndNestedContainers)
{
    Env env;
    const qreal em = env.em(), ch3 = 3 * env.styles->ch(), unit = 2.25 * em, q = 2 + 24;
    const QString md = QStringLiteral("> quote\n"         // 0
                                      "> \tquote tab\n"   // 1  (the tab is stripped)
                                      "> - a\n"           // 2
                                      "> \t- b\n"         // 3
                                      "\n"                // 4
                                      "- item\n"          // 5
                                      "\t> in item\n"     // 6
                                      "\t> - deep\n");    // 7
    Note n(md, env.styles, EditorView::Mode::Reading);
    // blockquote: border-inline-start 2px + padding-inline-start var(--size-4-6)
    EXPECT_EQ(n.deco(0).quoteBars, QList<qreal>{0});
    EXPECT_NEAR(n.textX(0), q, 0.01);
    EXPECT_NEAR(n.textX(1), q, 0.01);
    EXPECT_NEAR(n.textX(2), q + ch3, 0.01);
    EXPECT_NEAR(n.textX(3), q + ch3 + unit, 0.01);
    EXPECT_EQ(n.deco(3).quoteBars, QList<qreal>{0});
    EXPECT_NEAR(n.deco(3).guidePositions.value(0), q + ch3 - 0.85 * em, 0.01);
    QRectF r;
    const BulletWidget* b = n.floating<BulletWidget>(2, &r);
    ASSERT_TRUE(b);
    EXPECT_NEAR(b->dotCenter(r, env.lm()).x(), q + ch3 - 0.8 * em, 0.01);
    // a quote inside an item: its bar at the item's content edge
    EXPECT_EQ(n.deco(6).quoteBars, QList<qreal>{ch3});
    EXPECT_NEAR(n.textX(6), ch3 + q, 0.01);
    EXPECT_NEAR(n.textX(7), ch3 + q + unit, 0.01); // ul ul > li: the outer list counts through the quote
    EXPECT_TRUE(n.deco(0).quoteColumns.isEmpty()); // (Live Preview's bars at the '>' columns)
}

// ---- EditorView ---------------------------------------------------------------------------------------------------

class ListGeometryViewTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        s_themes = new ThemeManager();
        s_themes->setMode(ThemeManager::Mode::Dark);
        s_themes->apply();
    }
    static void TearDownTestSuite() { delete s_themes; }
    void open(const QString& content, EditorView::Mode mode)
    {
        ASSERT_TRUE(dir.isValid());
        storage = std::make_unique<LocalFileStorage>(dir.path());
        QFile f(dir.path() + "/t.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(content.toUtf8());
        f.close();
        doc = std::make_shared<Document>(storage.get(), QStringLiteral("t.md"));
        ASSERT_TRUE(doc->load());
        view = std::make_unique<EditorView>();
        view->resize(800, 600);
        view->setDocument(doc);
        view->setMode(mode);
        view->show();
        QApplication::processEvents();
    }
    void TearDown() override
    {
        view.reset();
        doc.reset();
    }

    static inline ThemeManager* s_themes = nullptr;
    QTemporaryDir dir;
    std::unique_ptr<LocalFileStorage> storage;
    std::shared_ptr<Document> doc;
    std::unique_ptr<EditorView> view;
};

TEST_F(ListGeometryViewTest, ReadingCheckboxClickToggles)
{
    open(QStringLiteral("- [ ] todo\n\t- plain\n"), EditorView::Mode::Reading);
    DocLayout& l = view->layout();
    const DocLayout::UnitLayout* ul = l.layoutFor(0);
    ASSERT_TRUE(ul);
    ASSERT_EQ(ul->deco.floatWidgets.size(), 1);
    const FloatWidget& f = ul->deco.floatWidgets.first();
    const LineMetrics& lm = l.styleSheet()->line(StyleId::Normal);
    const QRectF rect(f.x, l.lineTop(0) + ul->lines.first().baseline - f.widget->ascent(lm), f.widget->size(lm).width(), f.widget->size(lm).height());
    const QRectF box = static_cast<const CheckboxWidget*>(f.widget.get())->boxRect(rect, lm);
    const auto hit = l.widgetAt(box.center());
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->docFrom, 2);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->docToViewport(box.center()).toPoint());
    QApplication::processEvents();
    EXPECT_EQ(doc->text().line(0), QStringLiteral("- [x] todo"));
    // a bullet isn't a click target
    const DocLayout::UnitLayout* ul1 = l.layoutFor(1);
    ASSERT_EQ(ul1->deco.floatWidgets.size(), 1);
    EXPECT_FALSE(dynamic_cast<const CheckboxWidget*>(ul1->deco.floatWidgets.first().widget.get()));
}
