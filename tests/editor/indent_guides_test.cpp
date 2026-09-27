// Indentation guides (app.json "showIndentGuide") and strict line breaks ("strictLineBreaks"): Obsidian's indent
// unit walk (TAB / four spaces / spacing / quote markers), the list item owning the main selection and its active
// guide, Source vs Live Preview offsets (code blocks), .cm-indent's min-width, Reading view's nested-list guides,
// painting over the whole unit (wrapped lines), EditorView invalidation when the cursor moves or the text changes,
// toggling; strict line breaks as text runs in Reading view (soft breaks as spaces, hard breaks, lists, quotes,
// hit testing), not in Live Preview, and in nested renderings and the PDF export.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/export/pdf_exporter.h"
#include "editor/indent_guides.h"
#include "editor/live_preview_decorator.h"
#include "editor/source_decorator.h"
#include "editor/widgets/nested_renderer.h"
#include "markdown/doc_source.h"
#include "render/layout/display_block.h"
#include "render/layout/doc_layout.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPdfDocument>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <tuple>

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
};

using Unit = std::tuple<int, int, bool>;
QList<Unit> units(QStringView line)
{
    QList<Unit> out;
    indent::forEachUnit(line, [&](int from, int to, bool guide) { out.push_back({from, to, guide}); });
    return out;
}

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
    void setActive(int selectionLine)
    {
        const indent::ActiveRange r = indent::activeRange(src.text(), selectionLine);
        lp.setActiveIndent(r);
        source.setActiveIndent(r);
        layout.invalidateAll();
        layout.ensureLaidOut(0, 1e9);
    }
    void relayout()
    {
        layout.invalidateAll();
        layout.ensureLaidOut(0, 1e9);
    }
    const LineDecoration& deco(int line) { return layout.layoutFor(line)->deco; }
    QString display(int line)
    {
        QString s = layout.layoutFor(line)->block.display();
        s.replace(QChar(0x00A0), QString()); // widget placeholders (bullets)
        return s;
    }
};

bool hasLetterSpacing(const LineDecoration& d, int from, int to, qreal* spacing = nullptr)
{
    for (const Mark& m : d.marks) {
        if (m.from != from || m.to != to || m.custom == 0)
            continue;
        const QTextCharFormat& f = d.customStyles[m.custom - 1].format;
        if (f.hasProperty(QTextFormat::FontLetterSpacing)) {
            if (spacing)
                *spacing = f.fontLetterSpacing();
            return true;
        }
    }
    return false;
}

const QString kLists = QStringLiteral("- a\n"            // 0
                                      "\t- b\n"          // 1
                                      "\t  b continued\n" // 2
                                      "\t\t- c\n"        // 3
                                      "\t\t- d\n"        // 4
                                      "\t- e\n"          // 5
                                      "- f\n"            // 6
                                      "\n"               // 7
                                      "Para\n"           // 8
                                      "\tindented\n"     // 9
                                      "\tindented 2\n"   // 10
                                      "text\n");         // 11

} // namespace

// ---- Obsidian's walk ------------------------------------------------------------------------------------------

TEST(IndentGuides, Units)
{
    EXPECT_EQ(units(u"\t\t- x"), (QList<Unit>{{0, 1, true}, {1, 2, true}}));
    EXPECT_EQ(units(u"    - x"), (QList<Unit>{{0, 4, true}}));                  // four spaces: one unit
    EXPECT_EQ(units(u"        x"), (QList<Unit>{{0, 4, true}, {4, 8, true}}));
    EXPECT_EQ(units(u"  - x"), (QList<Unit>{{0, 2, false}}));                    // fewer: .cm-indent-spacing
    EXPECT_EQ(units(u"      x"), (QList<Unit>{{0, 4, true}, {4, 6, false}}));
    EXPECT_EQ(units(u"> \t- x"), (QList<Unit>{{2, 3, true}}));                  // '>' and its space skipped
    EXPECT_EQ(units(u">\t- x"), (QList<Unit>{{1, 2, true}}));
    EXPECT_EQ(units(u">     code"), (QList<Unit>{{2, 6, true}}));
    EXPECT_TRUE(units(u"> > x").isEmpty());
    EXPECT_EQ(units(u"  \t- x"), (QList<Unit>{{0, 2, false}}));                 // Obsidian skips the TAB after spacing
    EXPECT_EQ(units(u"\t\t"), (QList<Unit>{{0, 1, true}, {1, 2, true}}));        // whitespace-only lines too
    EXPECT_TRUE(units(u"x\t- y").isEmpty());
    EXPECT_TRUE(units(u"").isEmpty());
}

TEST(IndentGuides, ActiveRange)
{
    const Text t = Text::fromString(kLists);
    using R = indent::ActiveRange;
    // an item: its continuation lines (marker width) and deeper lines
    EXPECT_EQ(indent::activeRange(t, 1), (R{1, 5, 1}));
    // a continuation line walks up to its item
    EXPECT_EQ(indent::activeRange(t, 2), (R{1, 5, 1}));
    EXPECT_EQ(indent::activeRange(t, 0), (R{0, 6, 0}));
    EXPECT_TRUE(indent::activeRange(t, 3).isEmpty()); // "\t\t- c" then a sibling
    EXPECT_EQ(indent::activeRange(t, 3), (R{3, 4, 2}));
    // not a list item: the lines indented below it (Obsidian's walk with an empty prefix)
    EXPECT_EQ(indent::activeRange(t, 8), (R{8, 11, 0}));
    EXPECT_TRUE(indent::activeRange(t, 7).isEmpty());
    EXPECT_TRUE(indent::activeRange(t, 5).isEmpty());
    // quotes: the prefix includes the '>'
    const Text q = Text::fromString(QStringLiteral("> - q\n> \t- r\n> \t- s\n> t\n"));
    EXPECT_EQ(indent::activeRange(q, 0), (R{0, 3, 2}));
    EXPECT_EQ(indent::activeRange(q, 1), (R{1, 2, 3})); // prefix "> \t"
    // ordered items with a task box: the marker is "1. [ ] "
    const Text o = Text::fromString(QStringLiteral("1. [ ] task\n       continued\n\t- sub\nend\n"));
    EXPECT_EQ(indent::activeRange(o, 1), (R{0, 3, 0}));
    // out of range
    EXPECT_TRUE(indent::activeRange(t, 99).isEmpty());
}

TEST(IndentGuides, ChangedLines)
{
    using R = indent::ActiveRange;
    using L = QList<QPair<int, int>>;
    EXPECT_EQ(indent::changedLines(R{}, R{1, 5, 1}), (L{{2, 5}}));
    EXPECT_EQ(indent::changedLines(R{1, 5, 1}, R{1, 5, 1}), L{});
    EXPECT_EQ(indent::changedLines(R{1, 5, 1}, R{0, 6, 0}), (L{{2, 5}, {1, 6}}));   // other column: both
    EXPECT_EQ(indent::changedLines(R{0, 6, 0}, R{0, 3, 0}), (L{{3, 6}}));           // same column: the difference
    EXPECT_EQ(indent::changedLines(R{0, 6, 0}, R{}), (L{{1, 6}}));
}

// ---- decorations ----------------------------------------------------------------------------------------------

TEST(IndentGuides, LivePreviewAndSourceDecorations)
{
    Env env;
    const QString md = QStringLiteral("- a\n"           // 0
                                      "\t- b\n"         // 1
                                      "\t\t- c\n"       // 2
                                      "    - four\n"    // 3
                                      "\n"              // 4
                                      "text\n"          // 5
                                      "\ttabbed\n"      // 6 (a paragraph's continuation, not a list line)
                                      "```\n"           // 7
                                      "\tcode\n"        // 8
                                      "```\n"           // 9
                                      "> \tquoted\n");  // 10
    const qreal em = env.styles->textSize();
    const qreal codeEm = env.styles->line(StyleId::CodeBlock).font.pixelSize();
    {
        Note n(md, env.styles, EditorView::Mode::LivePreview);
        n.setActive(0);
        EXPECT_TRUE(n.deco(0).guideColumns.isEmpty());
        EXPECT_EQ(n.deco(1).guideColumns, QList<int>{0});
        EXPECT_EQ(n.deco(1).activeGuideColumn, 0); // the body of "- a"
        EXPECT_NEAR(n.deco(1).guideOffset, 0.85 * em + 1, 0.01); // --indentation-guide-editing-indent + the 1px box
        EXPECT_EQ(n.deco(2).guideColumns, (QList<int>{0, 1}));
        EXPECT_EQ(n.deco(2).activeGuideColumn, 0);
        EXPECT_EQ(n.deco(3).guideColumns, QList<int>{0}); // four spaces
        EXPECT_EQ(n.deco(5).guideColumns, QList<int>{});
        EXPECT_EQ(n.deco(4).guideColumns, QList<int>{});
        // non-list lines and code blocks get guides too; code blocks at offset 0
        EXPECT_EQ(n.deco(6).guideColumns, QList<int>{0});
        EXPECT_EQ(n.deco(8).guideColumns, QList<int>{0});
        EXPECT_NEAR(n.deco(8).guideOffset, 1, 0.01);
        EXPECT_EQ(n.deco(8).activeGuideColumn, -1);
        EXPECT_EQ(n.deco(10).guideColumns, QList<int>{2});
        // .cm-indent { min-width: var(--list-indent) }: four spaces are widened, a TAB outside lists gets that stop
        qreal extra = 0;
        ASSERT_TRUE(hasLetterSpacing(n.deco(3), 3, 4, &extra));
        const LineMetrics& lm = env.styles->line(StyleId::Normal);
        EXPECT_NEAR(extra, env.styles->listIndent() - 4 * lm.spaceWidth, 0.01);
        EXPECT_NEAR(n.deco(6).tabStop, env.styles->listIndent(), 0.01);
        EXPECT_FALSE(hasLetterSpacing(n.deco(8), 0, 1)); // code: the tab is already wide enough
        // the widened unit lays out --list-indent wide: "four"'s bullet at the same x as "b"'s
        const qreal bulletB = n.layout.caretRect(n.src.text().lineStart(1) + 1).x();
        const qreal bulletFour = n.layout.caretRect(n.src.text().lineStart(3) + 4).x();
        EXPECT_NEAR(bulletFour, bulletB, 1);
        // the active range follows the selection's item
        n.setActive(1);
        EXPECT_EQ(n.deco(1).activeGuideColumn, -1);
        EXPECT_EQ(n.deco(2).activeGuideColumn, 1);
    }
    {
        Note n(md, env.styles, EditorView::Mode::Source);
        n.setActive(1);
        EXPECT_EQ(n.deco(1).guideColumns, QList<int>{0});
        EXPECT_NEAR(n.deco(1).guideOffset, 0.25 * em + 1, 0.01); // --indentation-guide-source-indent
        EXPECT_EQ(n.deco(2).activeGuideColumn, 1);
        EXPECT_EQ(n.deco(6).guideColumns, QList<int>{0});
        EXPECT_EQ(n.deco(8).guideColumns, QList<int>{0});
        EXPECT_NEAR(n.deco(8).guideOffset, 0.25 * codeEm + 1, 0.01); // em of the code font
        EXPECT_TRUE(hasLetterSpacing(n.deco(3), 3, 4));
        // off: nothing at all (Obsidian doesn't install the plugin)
        n.source.setIndentGuides(false);
        n.relayout();
        for (int l = 0; l < n.src.text().lineCount(); ++l) {
            EXPECT_TRUE(n.deco(l).guideColumns.isEmpty()) << l;
            EXPECT_EQ(n.deco(l).activeGuideColumn, -1) << l;
        }
        EXPECT_FALSE(hasLetterSpacing(n.deco(3), 3, 4));
        EXPECT_EQ(n.deco(6).tabStop, 0);
    }
    {
        Note n(md, env.styles, EditorView::Mode::LivePreview);
        n.lp.setIndentGuides(false);
        n.relayout();
        EXPECT_TRUE(n.deco(2).guideColumns.isEmpty());
        EXPECT_TRUE(n.deco(2).guidePositions.isEmpty());
    }
}

TEST(IndentGuides, PaintedOverTheWholeUnit)
{
    Env env;
    // a narrow column: the grandchild wraps; guides cover every visual line and the list padding
    const QString md = QStringLiteral("- a\n\t- b\n\t\t- a long grandchild line that wraps over several visual lines here\n\t- c\n");
    Note n(md, env.styles, EditorView::Mode::LivePreview, 260);
    n.setActive(1); // "\t- b": its body (the grandchild) has the active guide at column 1
    const DocLayout::UnitLayout* ul = n.layout.layoutFor(2);
    ASSERT_TRUE(ul);
    ASSERT_GE(ul->lines.size(), 2);
    ASSERT_EQ(ul->deco.guideColumns, (QList<int>{0, 1}));
    ASSERT_EQ(ul->deco.activeGuideColumn, 1);
    QImage img(300, int(n.layout.totalHeight()) + 10, QImage::Format_ARGB32_Premultiplied);
    const QColor bg(30, 30, 30);
    img.fill(bg);
    {
        QPainter p(&img);
        DocLayout::PaintContext ctx;
        ctx.drawSelection = false;
        ctx.clip = QRectF(0, 0, 300, img.height());
        n.layout.paint(&p, ctx);
    }
    auto guideX = [&](int line, int col) {
        const DocLayout::UnitLayout* u = n.layout.layoutFor(line);
        return int(std::round(u->text->lineAt(0).cursorToX(u->block.docToDisp(col)) + u->deco.guideOffset));
    };
    const int top = int(std::ceil(n.layout.lineTop(2))) + 1, bottom = int(std::floor(n.layout.lineTop(2) + n.layout.lineHeight(2))) - 2;
    const QRgb normal = img.pixel(guideX(2, 0), top), active = img.pixel(guideX(2, 1), top);
    EXPECT_NE(normal, bg.rgb());
    EXPECT_NE(active, bg.rgb());
    EXPECT_NE(active, normal); // --indentation-guide-color-active
    EXPECT_GT(qGray(active), qGray(normal));
    for (int y = top; y <= bottom; ++y) {
        EXPECT_EQ(img.pixel(guideX(2, 0), y), normal) << y;
        EXPECT_EQ(img.pixel(guideX(2, 1), y), active) << y;
    }
    // consecutive lines connect: the first guide runs from "b" through "c" without a gap
    const int from = int(std::ceil(n.layout.lineTop(1))) + 1, to = int(std::floor(n.layout.lineTop(3) + n.layout.lineHeight(3))) - 2;
    for (int y = from; y <= to; ++y)
        EXPECT_NE(img.pixel(guideX(1, 0), y), bg.rgb()) << y;
}

TEST(IndentGuides, ReadingViewNestedLists)
{
    Env env;
    const QString md = QStringLiteral("- a\n"      // 0
                                      "\t- b\n"    // 1
                                      "\t  cont\n" // 2
                                      "\n"         // 3  (loose nested list: inside)
                                      "\t- c\n"    // 4
                                      "\t\t- d\n"  // 5
                                      "\n"         // 6  (after the nested list)
                                      "- e\n"      // 7
                                      "\n"         // 8
                                      "1. one\n"   // 9
                                      "\t- two\n"  // 10
                                      "\n"         // 11
                                      "- [ ] t\n"  // 12
                                      "\t- u\n");  // 13
    Note n(md, env.styles, EditorView::Mode::Reading);
    auto guides = [&](int l) { return n.layout.layoutFor(l)->deco.hidden ? -1 : int(n.deco(l).guidePositions.size()); };
    EXPECT_EQ(guides(0), 0);
    EXPECT_EQ(guides(1), 1);
    EXPECT_EQ(guides(2), 1);
    EXPECT_EQ(guides(3), 1); // a blank line between nested items: the guide goes through
    EXPECT_EQ(guides(4), 1);
    EXPECT_EQ(guides(5), 2);
    EXPECT_LE(guides(6), 0);
    EXPECT_EQ(guides(7), 0);
    // no editor guides in Reading view
    for (int l = 0; l < n.src.text().lineCount(); ++l)
        EXPECT_TRUE(n.deco(l).guideColumns.isEmpty()) << l;
    // under the parent's bullet: the guide's centre is the bullet widget's centre
    auto bulletCenter = [&](int line) {
        const DocLayout::UnitLayout* u = n.layout.layoutFor(line);
        return u->widgetRects.isEmpty() ? -100.0 : u->widgetRects.first().center().x();
    };
    const qreal w = env.styles->guideWidth();
    EXPECT_NEAR(n.deco(1).guidePositions[0] + w / 2, bulletCenter(0), 1);
    EXPECT_NEAR(n.deco(5).guidePositions[0] + w / 2, bulletCenter(0), 1);
    EXPECT_NEAR(n.deco(5).guidePositions[1] + w / 2, bulletCenter(4), 1);
    // an ordered parent: under its number; a task: under its checkbox
    const QFontMetricsF fm(env.styles->line(StyleId::Normal).font);
    EXPECT_NEAR(n.deco(10).guidePositions[0] + w / 2, fm.horizontalAdvance(QStringLiteral("1.")) / 2, 1);
    EXPECT_NEAR(n.deco(13).guidePositions[0] + w / 2, bulletCenter(12) - (n.layout.layoutFor(12)->widgetRects.first().width() - env.styles->textSize()) / 2, 1);
    // off: none
    n.lp.setIndentGuides(false);
    n.relayout();
    EXPECT_TRUE(n.deco(5).guidePositions.isEmpty());
}

// ---- strict line breaks -----------------------------------------------------------------------------------------

namespace {
const QString kBreaks = QStringLiteral("Soft one\n"    // 0
                                       "soft two\n"    // 1
                                       "hard  \n"      // 2  (two trailing spaces: a hard break)
                                       "after\n"       // 3
                                       "\n"            // 4
                                       "- item line\n" // 5
                                       "  item cont\n" // 6
                                       "> q1\n"        // 7
                                       "> q2\n"        // 8
                                       "\n"            // 9
                                       "back\\\n"      // 10 (a backslash: hard break)
                                       "slash\n");     // 11
}

TEST(StrictLineBreaks, ReadingViewJoinsSoftBreaks)
{
    Env env;
    Note n(kBreaks, env.styles, EditorView::Mode::Reading);
    // off (Obsidian's default, breaks: true): every line is its own line
    EXPECT_GT(n.layout.lineTop(1), n.layout.lineTop(0));
    EXPECT_EQ(n.layout.unitStart(1), 1);

    n.lp.setStrictLineBreaks(true);
    n.relayout();
    const Text& t = n.src.text();
    EXPECT_EQ(n.layout.unitStart(1), 0);
    EXPECT_EQ(n.layout.unitStart(2), 0);
    EXPECT_EQ(n.layout.lineTop(2), n.layout.lineTop(0));
    EXPECT_GT(n.layout.lineTop(3), n.layout.lineTop(0)); // after the hard break
    EXPECT_EQ(n.layout.unitStart(3), 3);
    EXPECT_TRUE(n.display(0).startsWith(QStringLiteral("Soft one soft two hard"))) << n.display(0).toStdString();
    EXPECT_EQ(n.layout.layoutFor(0)->lines.size(), 1); // one visual line
    // list item continuation and quote lines join too (their markers / indentation don't show)
    EXPECT_EQ(n.layout.unitStart(6), 5);
    EXPECT_TRUE(n.display(5).contains(QStringLiteral("item line item cont"))) << n.display(5).toStdString();
    EXPECT_EQ(n.layout.unitStart(8), 7);
    EXPECT_TRUE(n.display(7).contains(QStringLiteral("q1 q2"))) << n.display(7).toStdString();
    EXPECT_EQ(n.layout.unitStart(11), 11); // backslash hard break
    EXPECT_EQ(n.display(10), QStringLiteral("back")); // (its backslash doesn't render)
    // positions in a joined line: caret on the same visual line, hit testing back to the same offset
    const qsizetype p0 = t.lineStart(0) + 2, p1 = t.lineStart(1) + 3;
    const QRectF c0 = n.layout.caretRect(p0), c1 = n.layout.caretRect(p1);
    EXPECT_EQ(c1.top(), c0.top());
    EXPECT_GT(c1.x(), c0.x());
    EXPECT_EQ(n.layout.hitTest(c1.center()), p1);
    EXPECT_FALSE(n.layout.rangeRects(p0, p1).isEmpty());
    // the rest of the document is unchanged
    EXPECT_EQ(n.display(3), QStringLiteral("after"));
}

TEST(StrictLineBreaks, NotInLivePreviewLines)
{
    Env env;
    Note n(kBreaks, env.styles, EditorView::Mode::LivePreview);
    n.lp.setStrictLineBreaks(true);
    n.relayout();
    EXPECT_FALSE(n.lp.hasTextRuns());
    EXPECT_EQ(n.layout.unitStart(1), 1);
    EXPECT_GT(n.layout.lineTop(1), n.layout.lineTop(0));
}

TEST(StrictLineBreaks, NestedRenderingsFollowTheirParent)
{
    Env env;
    md::StaticDocSource parentSrc(Text::fromString(QStringLiteral("x")));
    LivePreviewDecorator parent(&parentSrc);
    parent.setStyleSheet(env.styles.get());
    const Text body = Text::fromString(QStringLiteral("one\ntwo\nthree\n\n- a\n\t- b\n"));
    NestedRenderer loose(body, env.styles, 1, &parent);
    const qreal broken = loose.layout(600);
    parent.setStrictLineBreaks(true);
    NestedRenderer strict(body, env.styles, 1, &parent);
    EXPECT_LT(strict.layout(600), broken); // a callout body in Live Preview joins its lines too
}

TEST(StrictLineBreaks, PdfExport)
{
    Env env;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Text text = Text::fromString(QStringLiteral("Alpha words\nbeta words\n"));
    auto pageText = [&](bool strict) {
        PdfExportSettings settings;
        settings.includeName = false;
        PdfExportContext ctx;
        ctx.strictLineBreaks = strict;
        const QString out = dir.path() + (strict ? "/strict.pdf" : "/breaks.pdf");
        EXPECT_TRUE(exportPdf(text, QStringLiteral("t"), out, settings, ctx));
        QPdfDocument pdf;
        pdf.load(out);
        return pdf.getAllText(0).text();
    };
    EXPECT_TRUE(pageText(true).contains(QStringLiteral("Alpha words beta words"))) << pageText(true).toStdString();
    const QString broken = pageText(false); // two lines
    EXPECT_TRUE(broken.contains(QStringLiteral("Alpha words")) && broken.contains(QStringLiteral("beta words")));
    EXPECT_FALSE(broken.contains(QStringLiteral("Alpha words beta words"))) << broken.toStdString();
}

// ---- EditorView ------------------------------------------------------------------------------------------------

class EditorGuidesTest : public ::testing::Test {
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
        view.reset();
        doc.reset();
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
    const LineDecoration& deco(int line) { return view->layout().layoutFor(line)->deco; }
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

TEST_F(EditorGuidesTest, ActiveGuideFollowsTheCursor)
{
    for (EditorView::Mode mode : {EditorView::Mode::Source, EditorView::Mode::LivePreview}) {
        open(kLists, mode);
        const Text& t = doc->text();
        EXPECT_TRUE(view->showIndentGuides());
        view->setCursorPosition(t.lineStart(1) + 3); // "\t- b"
        EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{1, 5, 1}));
        EXPECT_EQ(deco(3).activeGuideColumn, 1);
        EXPECT_EQ(deco(4).activeGuideColumn, 1);
        EXPECT_EQ(deco(1).activeGuideColumn, -1);
        // to "- a": lines 1..5 re-decorated with the guide at column 0
        view->setCursorPosition(t.lineStart(0) + 2);
        EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{0, 6, 0}));
        for (int l = 1; l <= 5; ++l)
            EXPECT_EQ(deco(l).activeGuideColumn, 0) << l;
        // to a line outside every list: the old range's lines lose it
        view->setCursorPosition(t.lineStart(11));
        EXPECT_TRUE(view->activeIndent().isEmpty());
        for (int l = 1; l <= 5; ++l)
            EXPECT_EQ(deco(l).activeGuideColumn, -1) << l;
        // a continuation line belongs to its item; the range uses the selection's `from`
        view->setSelection(Selection::single(t.lineStart(2) + 4, t.lineStart(0)));
        EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{0, 6, 0}));
        view->setSelection(Selection::single(t.lineStart(2) + 4, t.lineStart(2) + 6));
        EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{1, 5, 1}));
        EXPECT_EQ(deco(3).activeGuideColumn, 1);
    }
}

TEST_F(EditorGuidesTest, ActiveGuideFollowsEdits)
{
    open(kLists, EditorView::Mode::LivePreview);
    const Text& t0 = doc->text();
    view->setCursorPosition(t0.lineStart(4) + t0.lineLength(4)); // end of "\t\t- d"
    EXPECT_TRUE(view->activeIndent().isEmpty());
    // a new, deeper line under "d": d's range grows over it
    view->insertText(QStringLiteral("\n\t\t\t- e"));
    const Text& t = doc->text();
    ASSERT_EQ(t.line(5), QStringLiteral("\t\t\t- e"));
    view->setCursorPosition(t.lineStart(4) + 3);
    EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{4, 6, 2}));
    EXPECT_EQ(deco(5).activeGuideColumn, 2);
    // lines inserted above the range shift it (its lines are re-decorated where they moved)
    view->setCursorPosition(t.lineStart(0));
    view->insertText(QStringLiteral("# h\n"));
    EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{1, 8, 0}));
    for (int l = 2; l < 8; ++l)
        EXPECT_EQ(deco(l).activeGuideColumn, 0) << l;
}

TEST_F(EditorGuidesTest, OffsetsPerModeAndToggle)
{
    open(QStringLiteral("- a\n\t- b\n```\n\tcode\n```\n"), EditorView::Mode::LivePreview);
    const StyleSheetPtr ss = StyleSheet::build(*currentTheme());
    EXPECT_NEAR(deco(1).guideOffset, 0.85 * ss->textSize() + 1, 0.01);
    EXPECT_NEAR(deco(3).guideOffset, 1, 0.01); // code block: --indentation-guide-editing-indent: 0
    view->setMode(EditorView::Mode::Source);
    EXPECT_NEAR(deco(1).guideOffset, 0.25 * ss->textSize() + 1, 0.01);
    view->setMode(EditorView::Mode::Reading);
    EXPECT_TRUE(deco(1).guideColumns.isEmpty());
    EXPECT_EQ(deco(1).guidePositions.size(), 1);
    EXPECT_TRUE(view->activeIndent().isEmpty()); // no active guide in Reading view
    view->setShowIndentGuides(false);
    EXPECT_FALSE(view->livePreview()->indentGuides());
    EXPECT_TRUE(deco(1).guidePositions.isEmpty());
    view->setMode(EditorView::Mode::LivePreview);
    EXPECT_TRUE(deco(1).guideColumns.isEmpty());
    view->setCursorPosition(1);
    EXPECT_TRUE(view->activeIndent().isEmpty());
    view->setShowIndentGuides(true);
    EXPECT_EQ(deco(1).guideColumns, QList<int>{0});
    EXPECT_EQ(view->activeIndent(), (indent::ActiveRange{0, 2, 0}));
    EXPECT_EQ(deco(1).activeGuideColumn, 0);
}

TEST_F(EditorGuidesTest, StrictLineBreaksToggleInReadingView)
{
    open(QStringLiteral("one\n\ntwo\nthree\n\nfour\n"), EditorView::Mode::Reading);
    DocLayout& l = view->layout();
    EXPECT_FALSE(view->strictLineBreaks());
    EXPECT_EQ(l.unitStart(3), 3);
    const double broken = view->documentHeight();
    view->setStrictLineBreaks(true);
    EXPECT_EQ(l.unitStart(3), 2);
    EXPECT_LT(view->documentHeight(), broken);
    // an edit from another pane joining "one" with the paragraph below it: one run over all three lines (the
    // change starts on the blank line, the run on the line above it)
    const Text& t = doc->text();
    Transaction tr;
    tr.changes = ChangeSet::of({{t.lineStart(1), t.lineStart(2), QString()}}, t.length());
    doc->dispatch(tr, nullptr, Selection::cursor(0));
    QApplication::processEvents();
    ASSERT_EQ(doc->text().line(1), QStringLiteral("two"));
    EXPECT_EQ(l.unitStart(1), 0);
    EXPECT_EQ(l.unitStart(2), 0);
    EXPECT_EQ(l.unitStart(4), 4);
    view->setStrictLineBreaks(false);
    EXPECT_EQ(l.unitStart(1), 1);
    EXPECT_EQ(l.unitStart(2), 2);
    // Live Preview's own lines never join
    view->setStrictLineBreaks(true);
    view->setMode(EditorView::Mode::LivePreview);
    EXPECT_EQ(l.unitStart(1), 1);
}
