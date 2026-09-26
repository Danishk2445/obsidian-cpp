// Footnotes: editor styling (Live Preview / Source), Reading view numbering ("[1]", "[1-1]", inline footnotes,
// missing definitions), definitions leaving the flow, the footnotes section after the last line, click
// navigation between references and back-links, PDF export.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/export/pdf_exporter.h"
#include "editor/footnotes/footnotes.h"
#include "editor/live_preview_decorator.h"
#include "editor/source_decorator.h"
#include "markdown/doc_source.h"
#include "render/layout/display_block.h"
#include "render/layout/doc_layout.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

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

struct Note {
    md::StaticDocSource src;
    LivePreviewDecorator deco;
    const StyleSheet& ss;
    Note(const QString& markdown, const StyleSheet& styles, bool reading, qsizetype cursor = -1)
        : src(Text::fromString(markdown))
        , deco(&src)
        , ss(styles)
    {
        deco.setStyleSheet(&styles);
        deco.setReading(reading);
        deco.setSelection(Selection::cursor(cursor < 0 ? src.text().length() : cursor));
    }
    LineDecoration line(int l) { return deco.decorate(l); }
    QString display(int l)
    {
        const LineDecoration d = deco.decorate(l);
        QString s = DisplayBlock::build(src.text().lineView(l), d, ss).display();
        if (s.startsWith(QChar(0x00A0)))
            s.remove(0, 1);
        return s;
    }
};

bool hasMark(const LineDecoration& d, int from, int to, StyleId s)
{
    for (const Mark& m : d.marks)
        if (m.from == from && m.to == to && m.style == s)
            return true;
    return false;
}

bool hasHide(const LineDecoration& d, int from, int to)
{
    for (const Replace& r : d.replaces)
        if (r.kind == Replace::Kind::Hide && r.from == from && r.to == to)
            return true;
    return false;
}

const QString kNote = QStringLiteral("Named[^note], numeric[^1], inline^[Inline *text*.] and again[^note] and[^Missing].\n" // 0
                                     "\n"                                                                           // 1
                                     "Second paragraph[^2] and[^1].\n"                                              // 2
                                     "\n"                                                                           // 3
                                     "[^1]: One.\n"                                                                 // 4
                                     "[^note]: The named one.\n"                                                    // 5
                                     "    Continued.\n"                                                             // 6
                                     "\n"                                                                           // 7
                                     "[^2]: Two.\n"                                                                 // 8
                                     "[^unused]: Nobody refers to me.\n");                                          // 9

} // namespace

// ---- editor styling ----------------------------------------------------------------------------------

TEST(FootnotesEditor, ReferenceStyles)
{
    Env env;
    Note n(kNote, *env.styles, false, Text::fromString(kNote).lineStart(2));
    const LineDecoration d = n.line(0);
    // "[^note]" at 5..12: small and raised (.cm-footref), "[^" and "]" faint
    EXPECT_TRUE(hasMark(d, 5, 12, StyleId::FootnoteRef));
    EXPECT_TRUE(hasMark(d, 5, 7, StyleId::Formatting));
    EXPECT_TRUE(hasMark(d, 11, 12, StyleId::Formatting));
    // the inline footnote: raised, not muted
    const int inl = int(kNote.indexOf(QStringLiteral("^[")));
    const int inlEnd = int(kNote.indexOf(QStringLiteral(".]"))) + 2;
    EXPECT_TRUE(hasMark(d, inl, inlEnd, StyleId::FootnoteInline));
    // the source stays (no substitution / hiding of references in the editor)
    QString source = kNote.section(u'\n', 0, 0);
    source.remove(u'*'); // (the emphasis inside the inline footnote renders)
    EXPECT_EQ(n.display(0), source);
    const QTextCharFormat& ref = env.styles->spec(StyleId::FootnoteRef).format;
    EXPECT_EQ(ref.verticalAlignment(), QTextCharFormat::AlignSuperScript);
    EXPECT_GT(ref.superScriptBaseline(), 0);
}

TEST(FootnotesEditor, DefinitionLines)
{
    Env env;
    const Text t = Text::fromString(kNote);
    {
        Note n(kNote, *env.styles, false, t.lineStart(0));
        const LineDecoration d = n.line(4);
        EXPECT_EQ(d.lineStyle, StyleId::FootnoteLine);
        EXPECT_TRUE(hasMark(d, 0, 5, StyleId::FootnoteMarker)); // "[^1]:"
        EXPECT_TRUE(hasHide(d, 0, 2));                          // "[^" and "]:" hidden while the cursor is away
        EXPECT_TRUE(hasHide(d, 3, 5));
        EXPECT_EQ(n.display(4), QStringLiteral("1 One."));
        EXPECT_EQ(n.line(6).lineStyle, StyleId::FootnoteLine); // continuation line
        EXPECT_EQ(n.deco.quickLineStyle(6), StyleId::FootnoteLine);
        EXPECT_EQ(n.deco.quickLineStyle(0), StyleId::Normal);
    }
    {
        Note n(kNote, *env.styles, false, t.lineStart(4) + 2); // on the marker
        const LineDecoration d = n.line(4);
        EXPECT_FALSE(hasHide(d, 0, 2));
        EXPECT_TRUE(hasMark(d, 0, 2, StyleId::Formatting));
        EXPECT_TRUE(hasMark(d, 3, 5, StyleId::Formatting));
        EXPECT_EQ(n.display(4), QStringLiteral("[^1]: One."));
    }
    const QFont& fn = env.styles->line(StyleId::FootnoteLine).font;
    EXPECT_EQ(fn.pixelSize(), int(std::lround(env.styles->textSize() * 0.875)));
}

TEST(FootnotesEditor, SourceModeStyles)
{
    Env env;
    md::StaticDocSource src(Text::fromString(kNote));
    SourceDecorator deco(&src);
    deco.setStyleSheet(env.styles.get());
    const LineDecoration d = deco.decorate(4);
    EXPECT_EQ(d.lineStyle, StyleId::FootnoteLine);
    EXPECT_TRUE(hasMark(d, 0, 5, StyleId::FootnoteMarker));
    EXPECT_TRUE(hasMark(d, 0, 2, StyleId::Formatting));
    EXPECT_TRUE(d.replaces.isEmpty());
}

// ---- Reading view ------------------------------------------------------------------------------------

TEST(FootnotesReading, NumberingInFirstReferenceOrder)
{
    Env env;
    Note n(kNote, *env.styles, true);
    // note -> 1, 1 -> 2, inline -> 3, note again -> 1-1, Missing -> its lowercased label
    EXPECT_EQ(n.display(0), QStringLiteral("Named[1], numeric[2], inline[3] and again[1-1] andmissing."));
    EXPECT_EQ(n.display(2), QStringLiteral("Second paragraph[4] and[2-1]."));
    const auto idx = n.deco.footnoteIndex();
    ASSERT_TRUE(idx);
    ASSERT_EQ(idx->notes().size(), 4);
    EXPECT_EQ(idx->notes()[0].id, QStringLiteral("note"));
    EXPECT_EQ(idx->notes()[0].refs.size(), 2);
    EXPECT_TRUE(idx->notes()[2].isInline);
    EXPECT_EQ(idx->notes()[2].inlineText, QStringLiteral("Inline *text*."));
    EXPECT_EQ(idx->notes()[3].id, QStringLiteral("2"));
    // "[1]": superscript link colour, no underline
    const LineDecoration d = n.line(0);
    bool found = false;
    for (const Replace& r : d.replaces)
        if (r.kind == Replace::Kind::Text && r.text == QStringLiteral("[1]")) {
            found = true;
            EXPECT_EQ(r.style, StyleId::FootnoteLink);
        }
    EXPECT_TRUE(found);
    const QTextCharFormat& link = env.styles->spec(StyleId::FootnoteLink).format;
    EXPECT_EQ(link.verticalAlignment(), QTextCharFormat::AlignSuperScript);
    EXPECT_FALSE(link.fontUnderline());
}

TEST(FootnotesReading, DefinitionsLeaveTheFlow)
{
    Env env;
    Note n(kNote, *env.styles, true);
    for (int l : {4, 5, 6, 8, 9})
        EXPECT_TRUE(n.line(l).hidden) << l;
    // the blank line between two hidden definitions adds no second gap
    EXPECT_FALSE(n.line(3).hidden);
    EXPECT_TRUE(n.line(7).hidden);
    EXPECT_TRUE(n.line(10).hidden); // the empty last line after them
    EXPECT_FALSE(n.line(0).hidden);
    // Live Preview keeps them
    Note lp(kNote, *env.styles, false);
    EXPECT_FALSE(lp.line(5).hidden);
}

TEST(FootnotesReading, SectionAfterTheLastLine)
{
    Env env;
    Note n(kNote, *env.styles, true);
    DocLayout layout;
    layout.setDecorator(&n.deco);
    layout.setStyleSheet(env.styles);
    layout.setContentWidth(600);
    layout.setText(n.src.text());
    layout.ensureLaidOut(0, 1e9);
    const auto* section = dynamic_cast<const footnotes::SectionWidget*>(layout.trailer().get());
    ASSERT_NE(section, nullptr);
    EXPECT_GT(layout.trailerHeight(), 100);
    EXPECT_DOUBLE_EQ(layout.totalHeight(), layout.trailerTop() + layout.trailerHeight());
    const QString list = section->listText().toString();
    const QString back = QStringLiteral("↩︎");
    EXPECT_EQ(list, QStringLiteral("1. The named one.\n   Continued. ") + back + QLatin1Char(' ') + back
                        + QStringLiteral("\n2. One. ") + back + QLatin1Char(' ') + back + QStringLiteral("\n3. Inline *text*. ") + back
                        + QStringLiteral("\n4. Two. ") + back);
    EXPECT_FALSE(list.contains(QStringLiteral("Nobody"))); // unreferenced definitions disappear
    // hit testing below the text: the section, mapped to the end of the note
    const QPointF below(10, layout.trailerTop() + 5);
    EXPECT_EQ(layout.hitTest(below), n.src.text().length());
    ASSERT_TRUE(layout.blockAt(below).has_value());
    EXPECT_EQ(layout.blockAt(below)->fromLine, layout.lineCount());
    // anchors of the items grow down the list
    EXPECT_GT(section->anchorY(QStringLiteral("fn-1")), 0);
    EXPECT_GT(section->anchorY(QStringLiteral("fn-4")), section->anchorY(QStringLiteral("fn-1")));
    EXPECT_LT(section->anchorY(QStringLiteral("fn-9")), 0);
    // no section in the editor, nor without footnotes
    Note lp(kNote, *env.styles, false);
    EXPECT_EQ(lp.deco.trailer(), nullptr);
    Note plain(QStringLiteral("no footnotes here\n\n[^x]: unused"), *env.styles, true);
    EXPECT_EQ(plain.deco.trailer(), nullptr);
}

TEST(FootnotesReading, DefinitionModeShowsTheDefinition)
{
    Env env;
    Note n(kNote, *env.styles, true);
    n.deco.setFootnoteMode(LivePreviewDecorator::FootnoteMode::Definition);
    EXPECT_FALSE(n.line(5).hidden);
    EXPECT_EQ(n.display(5), QStringLiteral("The named one."));
    EXPECT_EQ(n.deco.trailer(), nullptr);
}

TEST(FootnotesReading, CalloutBodiesShareTheNumbering)
{
    Env env;
    const QString md = QStringLiteral("First[^a].\n\n> [!note] Title\n> In a callout[^b].\n\n[^a]: A.\n[^b]: B.\n");
    Note n(md, *env.styles, true);
    const auto idx = n.deco.footnoteIndex();
    ASSERT_TRUE(idx);
    EXPECT_EQ(idx->noteForId(QStringLiteral("b")), 1);
    // the callout body is a nested document: its reference resolves by label
    md::StaticDocSource body(Text::fromString(QStringLiteral("In a callout[^b].")));
    LivePreviewDecorator nested(&body);
    nested.setStyleSheet(env.styles.get());
    nested.setReading(true);
    nested.setNestingDepth(1);
    nested.setSharedFootnotes(idx);
    const LineDecoration d = nested.decorate(0);
    EXPECT_EQ(DisplayBlock::build(body.text().lineView(0), d, *env.styles).display(), QStringLiteral("In a callout[2]."));
    const auto hit = nested.linkAt(13);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->anchor, QStringLiteral("fn-2"));
}

TEST(FootnotesReading, LinkHits)
{
    Env env;
    Note reading(kNote, *env.styles, true);
    const auto hit = reading.deco.linkAt(6);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->target, QStringLiteral("#[^note]"));
    EXPECT_EQ(hit->anchor, QStringLiteral("fn-1"));
    EXPECT_FALSE(reading.deco.linkAt(kNote.indexOf(QStringLiteral("[^Missing]")) + 2).has_value());
    EXPECT_EQ(reading.deco.footnoteRefPosition(QStringLiteral("fnref-1-1")), kNote.indexOf(QStringLiteral("[^note]"), 10));
    EXPECT_EQ(reading.deco.footnoteRefPosition(QStringLiteral("fnref-2")), kNote.indexOf(QStringLiteral("[^1]")));
    EXPECT_FALSE(reading.deco.footnoteRefPosition(QStringLiteral("fnref-7")).has_value());
    // the editor: hover target, never followed
    Note lp(kNote, *env.styles, false);
    const auto lpHit = lp.deco.linkAt(6);
    ASSERT_TRUE(lpHit.has_value());
    EXPECT_EQ(lpHit->target, QStringLiteral("#[^note]"));
    EXPECT_EQ(lpHit->anchor, QStringLiteral("fn"));
}

// ---- EditorView: click navigation ---------------------------------------------------------------------

TEST(FootnotesView, ClickScrollsBetweenReferenceAndFootnote)
{
    Env env;
    QTemporaryDir dir;
    QString md;
    md += QStringLiteral("Top reference[^far].\n\n");
    for (int i = 0; i < 200; ++i)
        md += QStringLiteral("Filler paragraph %1 to push the footnotes section far below.\n\n").arg(i);
    md += QStringLiteral("[^far]: The far footnote.\n");
    {
        QFile f(dir.path() + "/n.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(md.toUtf8());
    }
    LocalFileStorage storage(dir.path());
    auto doc = std::make_shared<Document>(&storage, QStringLiteral("n.md"));
    ASSERT_TRUE(doc->load());
    EditorView view;
    view.resize(800, 600);
    view.setMode(EditorView::Mode::Reading);
    view.setDocument(doc);
    view.show();
    QApplication::processEvents();
    ASSERT_NE(view.layout().trailer(), nullptr);
    const double top0 = view.scrollTop(); // the top margin above the text

    // click the "[1]" of the first line
    const QRectF ref = view.caretRectAt(md.indexOf(QStringLiteral("[^far]")) + 1);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, ref.center().toPoint());
    QApplication::processEvents();
    const double itemY = view.layout().trailerTop() + view.layout().trailer()->anchorY(QStringLiteral("fn-1"));
    EXPECT_GT(view.scrollTop(), 1000);
    EXPECT_NEAR(view.scrollTop() + view.viewport()->height() / 2.0, itemY, 2);

    // and back: the "↩︎" link at the end of the footnote
    EXPECT_TRUE(view.scrollToAnchor(QStringLiteral("fnref-1")));
    EXPECT_EQ(view.scrollTop(), top0); // clamped at the top
    EXPECT_FALSE(view.scrollToAnchor(QStringLiteral("fn-5")));
    // a click on the back-link itself
    view.scrollToAnchor(QStringLiteral("fn-1"));
    QApplication::processEvents();
    const auto* section = dynamic_cast<const footnotes::SectionWidget*>(view.layout().trailer().get());
    ASSERT_NE(section, nullptr);
    // find the back-link by probing the section's links along the item's line
    const double itemTop = section->anchorY(QStringLiteral("fn-1"));
    QPointF local(-1, -1);
    for (qreal x = 0; x < 600 && local.x() < 0; x += 2)
        for (qreal dy = 2; dy < 20 && local.x() < 0; dy += 3)
            if (section->linkAt(QPointF(x, itemTop + dy)) == QStringLiteral("#fnref-1"))
                local = QPointF(x, itemTop + dy);
    ASSERT_GE(local.x(), 0);
    const QPointF docPt = local + QPointF(0, view.layout().trailerTop());
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, view.docToViewport(docPt).toPoint());
    QApplication::processEvents();
    EXPECT_EQ(view.scrollTop(), top0);
}

// ---- PDF export --------------------------------------------------------------------------------------------

TEST(FootnotesPdf, ExportRendersReferencesAndSection)
{
    Env env;
    QTemporaryDir dir;
    const QString out = dir.path() + "/fn.pdf";
    PdfExportSettings s;
    s.includeName = false;
    QString err;
    ASSERT_TRUE(exportPdf(Text::fromString(kNote), "Footnotes", out, s, {}, &err)) << err.toStdString();
    if (qEnvironmentVariableIsSet("OC_KEEP_PDF")) // for looking at it
        QFile::copy(out, qEnvironmentVariable("OC_KEEP_PDF"));
    QPdfDocument pdf;
    ASSERT_EQ(pdf.load(out), QPdfDocument::Error::None);
    QString text;
    for (int p = 0; p < pdf.pageCount(); ++p)
        text += pdf.getAllText(p).text();
    // (the brackets of "[1]" don't come back from the PDF's text layer)
    EXPECT_TRUE(text.contains(QRegularExpression(QStringLiteral("Named\\W*1\\W*, numeric")))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("andmissing"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("The named one."))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Two."))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("[^note]"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("Nobody"))) << text.toStdString();
}
