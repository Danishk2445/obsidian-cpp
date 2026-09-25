// Raw HTML rendering: inline elements as decorations (tags hidden, element formats, <br> line breaks),
// HTML blocks as QTextDocument block widgets, Live Preview reveal, Reading view and PDF export.
#include "editor/export/pdf_exporter.h"
#include "editor/html/html_block.h"
#include "editor/html/html_inline.h"
#include "editor/html/html_parse.h"
#include "editor/live_preview_decorator.h"
#include "markdown/doc_source.h"
#include "render/layout/display_block.h"
#include "render/layout/doc_layout.h"
#include "render/theme/theme.h"

#include <QPdfDocument>
#include <QPdfSelection>
#include <QTemporaryDir>
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

// A parsed note with a Live Preview / Reading decorator. The cursor defaults to the end of the note.
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
    DisplayBlock block(int l)
    {
        const LineDecoration d = deco.decorate(l);
        return DisplayBlock::build(src.text().lineView(l), d, ss);
    }
    // The displayed text, without the spacer DisplayBlock puts in front of a pill that starts the line.
    QString display(int l)
    {
        QString s = block(l).display();
        if (s.startsWith(QChar(0x00A0)))
            s.remove(0, 1);
        return s;
    }
};

bool hasHide(const LineDecoration& d, int from, int to)
{
    for (const Replace& r : d.replaces)
        if (r.kind == Replace::Kind::Hide && r.from == from && r.to == to)
            return true;
    return false;
}

bool hasMark(const LineDecoration& d, int from, int to, StyleId s)
{
    for (const Mark& m : d.marks)
        if (m.from == from && m.to == to && m.style == s && m.custom == 0)
            return true;
    return false;
}

int hideCount(const LineDecoration& d)
{
    int n = 0;
    for (const Replace& r : d.replaces)
        n += r.kind == Replace::Kind::Hide;
    return n;
}

// Merged character format at a display position (later ranges win, like QTextLayout).
QTextCharFormat formatAt(const DisplayBlock& b, int pos)
{
    QTextCharFormat f;
    for (const QTextLayout::FormatRange& fr : b.formats())
        if (fr.start <= pos && pos < fr.start + fr.length)
            f.merge(fr.format);
    return f;
}

int visualLines(DocLayout& layout, int line)
{
    const DocLayout::UnitLayout* ul = layout.layoutFor(line);
    return ul ? int(ul->lines.size()) : -1;
}

} // namespace

// ---- parsing helpers -----------------------------------------------------------------------------

TEST(HtmlParse, TagsAndAttributes)
{
    html::Tag t;
    ASSERT_TRUE(html::parseTag(u"<a HREF=\"https://x.y/?a=1&amp;b=2\" title='t' data-x=raw hidden>", &t));
    EXPECT_EQ(t.name, "a");
    EXPECT_FALSE(t.closing);
    EXPECT_EQ(t.attr(u"href"), "https://x.y/?a=1&b=2");
    EXPECT_EQ(t.attr(u"title"), "t");
    EXPECT_EQ(t.attr(u"data-x"), "raw");
    EXPECT_TRUE(t.hasAttr(u"hidden"));
    ASSERT_TRUE(html::parseTag(u"<br />", &t));
    EXPECT_EQ(t.name, "br");
    EXPECT_TRUE(t.selfClosing);
    ASSERT_TRUE(html::parseTag(u"</KBD>", &t));
    EXPECT_EQ(t.name, "kbd");
    EXPECT_TRUE(t.closing);
    ASSERT_TRUE(html::parseTag(u"<!-- note -->", &t));
    EXPECT_TRUE(t.comment);
    EXPECT_FALSE(html::parseTag(u"< notatag>", &t));
}

TEST(HtmlParse, ElementClasses)
{
    for (const char* inl : {"kbd", "sub", "sup", "span", "a", "img", "br", "mark", "font", "custom"})
        EXPECT_TRUE(html::isInlineElement(QString::fromLatin1(inl))) << inl;
    for (const char* blk : {"p", "div", "details", "table", "h1", "li", "center", "my-element"})
        EXPECT_FALSE(html::isInlineElement(QString::fromLatin1(blk))) << blk;
    for (const char* ex : {"script", "style", "link", "meta", "object", "embed", "webview"})
        EXPECT_TRUE(html::isExcludedElement(QString::fromLatin1(ex))) << ex;
    EXPECT_FALSE(html::isExcludedElement(u"iframe"));
}

TEST(HtmlParse, InlineStyleSubset)
{
    const html::InlineStyle s = html::parseInlineStyle(u"color: red; background-color: rgba(0, 0, 255, 0.5); font-weight: bold; "
                                                       u"font-style: italic; text-decoration: underline line-through; font-size: 20px");
    EXPECT_EQ(s.color, QColor(255, 0, 0));
    EXPECT_EQ(s.background.blue(), 255);
    EXPECT_NEAR(s.background.alphaF(), 0.5, 0.01);
    EXPECT_EQ(s.weight, int(QFont::Bold));
    EXPECT_EQ(s.italic, 1);
    EXPECT_EQ(s.underline, 1);
    EXPECT_EQ(s.strike, 1);
    EXPECT_EQ(s.sizePx, 20);
    EXPECT_EQ(html::parseCssColor(u"#0f0"), QColor(0, 255, 0));
    EXPECT_EQ(html::parseCssColor(u"darkorange"), QColor(255, 140, 0));
    EXPECT_FALSE(html::parseCssColor(u"var(--text-accent)").isValid());
    EXPECT_TRUE(html::parseInlineStyle(u"display: none").isEmpty());
}

// ---- inline elements ------------------------------------------------------------------------------

TEST(HtmlInline, PairsTagsHidesThemAndFormatsContent)
{
    Env env;
    Note n(QStringLiteral("<kbd>Ctrl</kbd> + <kbd>O</kbd>, H<sub>2</sub>O and <b>bold</b>"), *env.styles, true);
    const LineDecoration d = n.line(0);
    EXPECT_TRUE(hasHide(d, 0, 5));   // <kbd>
    EXPECT_TRUE(hasHide(d, 9, 15));  // </kbd>
    EXPECT_TRUE(hasMark(d, 5, 9, StyleId::Kbd));
    EXPECT_TRUE(hasMark(d, 38, 39, StyleId::Subscript));
    EXPECT_EQ(n.display(0), QStringLiteral("Ctrl + O, H2O and bold"));
}

TEST(HtmlInline, KbdSubSupFormats)
{
    Env env;
    Note n(QStringLiteral("<kbd>K</kbd> H<sub>2</sub>O x<sup>2</sup>"), *env.styles, true);
    const DisplayBlock b = n.block(0);
    ASSERT_EQ(b.display(), QChar(0x00A0) + QStringLiteral("K H2O x2")); // lead spacer: the pill starts the line
    // kbd: a code-coloured pill with its padding laid out
    ASSERT_FALSE(b.boxes().isEmpty());
    const DisplayBlock::Box& box = b.boxes().first();
    EXPECT_EQ(b.display().mid(box.dispFrom, box.dispTo - box.dispFrom), QStringLiteral("K"));
    EXPECT_EQ(box.style.background, env.styles->codeBackground());
    EXPECT_TRUE(box.reserved);
    EXPECT_TRUE(formatAt(b, b.display().indexOf(u'K')).fontFamilies().toStringList().contains(currentTheme()->monospaceFamily()));
    EXPECT_EQ(formatAt(b, 4).verticalAlignment(), QTextCharFormat::AlignSubScript);
    EXPECT_EQ(formatAt(b, 8).verticalAlignment(), QTextCharFormat::AlignSuperScript);
    EXPECT_EQ(formatAt(b, 3).verticalAlignment(), QTextCharFormat::AlignNormal);
}

TEST(HtmlInline, AdjacentKbdPillsStaySeparate)
{
    Env env;
    Note n(QStringLiteral("x <kbd>A</kbd><kbd>B</kbd> y"), *env.styles, true);
    const DisplayBlock b = n.block(0);
    ASSERT_EQ(b.display(), QStringLiteral("x AB y"));
    EXPECT_EQ(b.boxes().size(), 2);
}

TEST(HtmlInline, SpanColorsAndFontAttributes)
{
    Env env;
    Note n(QStringLiteral("<span style=\"color: #ff0000; background-color: #00ff00\">red</span> <font color=\"blue\">blue</font> "
                          "<u>u</u> <s>s</s> <mark>m</mark>"),
           *env.styles, true);
    const DisplayBlock b = n.block(0);
    ASSERT_EQ(b.display(), QStringLiteral("red blue u s m"));
    EXPECT_EQ(formatAt(b, 0).foreground().color(), QColor(255, 0, 0));
    EXPECT_EQ(formatAt(b, 0).background().color(), QColor(0, 255, 0));
    EXPECT_EQ(formatAt(b, 4).foreground().color(), QColor(0, 0, 255));
    EXPECT_TRUE(formatAt(b, 9).fontUnderline());
    EXPECT_TRUE(formatAt(b, 11).fontStrikeOut());
    EXPECT_EQ(formatAt(b, 13).background().color(), env.styles->spec(StyleId::Highlight).format.background().color());
}

TEST(HtmlInline, BreakMakesTwoVisualLines)
{
    Env env;
    for (const bool reading : {true, false}) {
        Note n(QStringLiteral("line<br>break and <br/>more\ntrailing<br>\nlast<br />\n\nend"), *env.styles, reading);
        EXPECT_EQ(n.display(0), QStringLiteral("line") + QChar(QChar::LineSeparator) + QStringLiteral("break and ") + QChar(QChar::LineSeparator)
                                    + QStringLiteral("more"));
        DocLayout layout;
        layout.setDecorator(&n.deco);
        layout.setStyleSheet(env.styles);
        layout.setContentWidth(600);
        layout.setText(n.src.text());
        layout.ensureLaidOut(0, 1e6);
        EXPECT_EQ(visualLines(layout, 0), 3) << reading;
        // A <br> ending a line adds no line in Live Preview (the next line is its own block); Reading view
        // renders the soft break after it as another break (Obsidian: "trailing<br><br>last").
        EXPECT_EQ(visualLines(layout, 1), reading ? 2 : 1) << reading;
        // ...but the paragraph's last <br> is dropped by the browser.
        EXPECT_EQ(visualLines(layout, 2), 1) << reading;
        EXPECT_EQ(n.display(2), QStringLiteral("last"));
    }
}

TEST(HtmlInline, RevealOnCursorInLivePreview)
{
    Env env;
    const QString md = QStringLiteral("x <kbd>Ctrl</kbd> y <b>b</b>\n\n   <kbd>K</kbd>\n\nend");
    {
        Note n(md, *env.styles, false);
        const LineDecoration d = n.line(0);
        EXPECT_TRUE(hasHide(d, 2, 7));
        EXPECT_TRUE(hasHide(d, 11, 17));
        EXPECT_EQ(n.display(0), QStringLiteral("x Ctrl y b"));
    }
    {
        Note n(md, *env.styles, false, 8); // inside "Ctrl"
        const LineDecoration d = n.line(0);
        EXPECT_FALSE(hasHide(d, 2, 7));
        EXPECT_TRUE(hasMark(d, 2, 7, StyleId::Html));
        EXPECT_TRUE(hasMark(d, 11, 17, StyleId::Html));
        EXPECT_FALSE(hasMark(d, 7, 11, StyleId::Kbd));
        EXPECT_TRUE(hasHide(d, 20, 23)); // the other element stays rendered
        EXPECT_EQ(n.display(0), QStringLiteral("x <kbd>Ctrl</kbd> y b"));
    }
    {
        // An element alone on its line reveals when the cursor is anywhere on the line (Obsidian widens the
        // widget range to whole lines).
        const qsizetype line2 = Text::fromString(md).lineStart(2);
        Note n(md, *env.styles, false, line2);
        EXPECT_EQ(hideCount(n.line(2)), 0);
        EXPECT_EQ(n.display(2), QStringLiteral("   <kbd>K</kbd>"));
    }
}

TEST(HtmlInline, ReadingViewAlwaysRenders)
{
    Env env;
    Note n(QStringLiteral("x <kbd>Ctrl</kbd> y"), *env.styles, true, 8);
    EXPECT_EQ(n.display(0), QStringLiteral("x Ctrl y"));
}

TEST(HtmlInline, ExcludedTagsUntouched)
{
    Env env;
    const QString md = QStringLiteral("a <script>alert(1)</script> b <style>p{}</style> c\n\nend");
    {
        Note n(md, *env.styles, false);
        const LineDecoration d = n.line(0);
        EXPECT_EQ(hideCount(d), 0);
        EXPECT_TRUE(hasMark(d, 2, 10, StyleId::Html));
        EXPECT_TRUE(hasMark(d, 18, 27, StyleId::Html));
        EXPECT_EQ(n.display(0), md.section(u'\n', 0, 0));
    }
    {
        // Reading view sanitizes them away with their content.
        Note n(md, *env.styles, true);
        EXPECT_EQ(n.display(0), QStringLiteral("a  b  c"));
    }
}

TEST(HtmlInline, UnclosedAndStrayTags)
{
    Env env;
    const QString md = QStringLiteral("press <Enter> to <kbd>go</kbd> now</b>\n\nend");
    {
        // Live preview: an element that never closes (and what follows it) stays source.
        Note n(md, *env.styles, false);
        EXPECT_EQ(hideCount(n.line(0)), 0);
    }
    {
        // Reading view: the browser drops unknown / stray tags.
        Note n(md, *env.styles, true);
        EXPECT_EQ(n.display(0), QStringLiteral("press  to go now"));
    }
}

TEST(HtmlInline, CommentsHiddenInReading)
{
    Env env;
    const QString md = QStringLiteral("a <!-- note --> b\n\nend");
    Note lp(md, *env.styles, false);
    EXPECT_TRUE(hasMark(lp.line(0), 2, 15, StyleId::Comment));
    Note rd(md, *env.styles, true);
    EXPECT_EQ(rd.display(0), QStringLiteral("a  b"));
}

TEST(HtmlInline, AnchorsAreLinks)
{
    Env env;
    const QString md = QStringLiteral("see <a href=\"https://obsidian.md\">site</a> or <a href=\"Welcome.md\">home</a>\n\nend");
    Note n(md, *env.styles, true);
    const LineDecoration d = n.line(0);
    EXPECT_TRUE(hasMark(d, 34, 38, StyleId::ExternalLink));
    const auto ext = n.deco.linkAt(35);
    ASSERT_TRUE(ext.has_value());
    EXPECT_EQ(ext->kind, LivePreviewDecorator::LinkHit::Kind::Url);
    EXPECT_EQ(ext->target, QStringLiteral("https://obsidian.md"));
    const qsizetype home = md.indexOf(QStringLiteral("home"));
    const auto internal = n.deco.linkAt(home + 1);
    ASSERT_TRUE(internal.has_value());
    EXPECT_EQ(internal->kind, LivePreviewDecorator::LinkHit::Kind::Link);
    EXPECT_EQ(internal->target, QStringLiteral("Welcome.md"));
    EXPECT_FALSE(n.deco.linkAt(1).has_value());

    // The hit range is what Live Preview reveals with the anchor: its outermost element, or whole lines when it
    // is alone on them (a click there while the source shows places the cursor instead of following the link).
    const QString nested = QStringLiteral("a <b>x <a href=\"y\">link</a></b> z\n\n  <a href=\"w\">alone</a>  \n\nend");
    Note m(nested, *env.styles, false);
    const auto inB = m.deco.linkAt(nested.indexOf(QStringLiteral("link")) + 1);
    ASSERT_TRUE(inB.has_value());
    EXPECT_EQ(inB->from, nested.indexOf(QStringLiteral("<b>")));
    EXPECT_EQ(inB->to, nested.indexOf(QStringLiteral(" z")));
    const Text t = Text::fromString(nested);
    const auto alone = m.deco.linkAt(nested.indexOf(QStringLiteral("alone")) + 1);
    ASSERT_TRUE(alone.has_value());
    EXPECT_EQ(alone->from, t.lineStart(2));
    EXPECT_EQ(alone->to, t.lineEnd(2));
}

TEST(HtmlInline, TableCellsRenderHtml)
{
    Env env;
    Note n(QStringLiteral("| a | b |\n| - | - |\n| x<br>y | <kbd>K</kbd> |\n\nend"), *env.styles, false);
    QList<BlockRange> blocks;
    n.deco.blockRanges(0, 5, blocks);
    ASSERT_EQ(blocks.size(), 1);
    // x<br>y takes two lines: the table is taller than one with plain cells
    Note plain(QStringLiteral("| a | b |\n| - | - |\n| xy | K |\n\nend"), *env.styles, false);
    QList<BlockRange> plainBlocks;
    plain.deco.blockRanges(0, 5, plainBlocks);
    ASSERT_EQ(plainBlocks.size(), 1);
    EXPECT_GT(blocks[0].widget->layout(600), plainBlocks[0].widget->layout(600));
}

TEST(HtmlInline, NestedHtmlBlockRendersInline)
{
    Env env;
    // An HTML block inside a list item isn't a block widget: its tags render with decorations.
    Note n(QStringLiteral("- item\n\n  <div>nested <b>bold</b></div>\n\nend"), *env.styles, true);
    ASSERT_EQ(n.src.syntax().role(2), md::LineRole::Html);
    const LineDecoration d = n.line(2);
    EXPECT_EQ(d.lineStyle, StyleId::Normal);
    EXPECT_EQ(n.display(2), QStringLiteral("  nested bold"));
    // Live preview reveals its elements one by one, like a paragraph's.
    const QString md = QStringLiteral("- item\n\n  <span>one</span> <b>two</b>\n\nend");
    Note lp(md, *env.styles, false, md.indexOf(QStringLiteral("two")));
    EXPECT_EQ(lp.display(2), QStringLiteral("  one <b>two</b>"));
}

// ---- HTML blocks ------------------------------------------------------------------------------------

TEST(HtmlBlock, WidgetHeight)
{
    Env env;
    const StyleSheet& ss = *env.styles;
    const qreal line = ss.line(StyleId::Normal).lineBox;
    HtmlBlockWidget one(QStringLiteral("<p align=\"center\">one line</p>"), ss, {});
    EXPECT_NEAR(one.layout(600), line, 2);
    HtmlBlockWidget two(QStringLiteral("<p>first<br>second</p>"), ss, {});
    EXPECT_NEAR(two.layout(600), 2 * line, 3);
    HtmlBlockWidget br(QStringLiteral("<br>"), ss, {});
    EXPECT_FALSE(br.isEmpty());
    EXPECT_NEAR(br.layout(600), line, 2); // a lone <br> is one empty line, not two
    HtmlBlockWidget wrap(QStringLiteral("<div>") + QStringLiteral("word ").repeated(200) + QStringLiteral("</div>"), ss, {});
    EXPECT_GT(wrap.layout(300), wrap.layout(600)); // laid out to the content width
    HtmlBlockWidget table(QStringLiteral("<table><tr><th>a</th></tr><tr><td>b</td></tr><tr><td>c</td></tr></table>"), ss, {});
    EXPECT_GT(table.layout(600), 3 * ss.textSize());
    EXPECT_EQ(table.plainText().simplified(), QStringLiteral("a b c"));
}

TEST(HtmlBlock, EmptyAndSanitized)
{
    Env env;
    HtmlBlockWidget open(QStringLiteral("<div align=\"center\">"), *env.styles, {});
    EXPECT_TRUE(open.isEmpty());
    EXPECT_EQ(open.layout(600), 0);
    HtmlBlockWidget comment(QStringLiteral("<!-- hidden -->"), *env.styles, {});
    EXPECT_TRUE(comment.isEmpty());
    HtmlBlockWidget script(QStringLiteral("<script>alert('x')</script>\n<style>p { color: red }</style>"), *env.styles, {});
    EXPECT_TRUE(script.isEmpty());
    EXPECT_FALSE(script.qtHtml().contains(QStringLiteral("alert")));
    HtmlBlockWidget mixed(QStringLiteral("<div onclick=\"x()\">Text<script>bad()</script> <foo>kept</foo></div>"), *env.styles, {});
    EXPECT_EQ(mixed.plainText(), QStringLiteral("Text kept"));
    EXPECT_FALSE(mixed.qtHtml().contains(QStringLiteral("onclick")));
}

TEST(HtmlBlock, DetailsToggle)
{
    Env env;
    HtmlBlockWidget w(QStringLiteral("<details>\n<summary>Title</summary>\nBody text\n</details>"), *env.styles, {});
    EXPECT_TRUE(w.plainText().contains(QStringLiteral("Title")));
    EXPECT_FALSE(w.plainText().contains(QStringLiteral("Body")));
    const qreal closed = w.layout(600);
    EXPECT_TRUE(w.click(QPointF(10, closed / 2), Qt::NoModifier));
    EXPECT_TRUE(w.plainText().contains(QStringLiteral("Body")));
    EXPECT_GT(w.layout(600), closed);
    EXPECT_TRUE(w.click(QPointF(10, 5), Qt::NoModifier));
    EXPECT_FALSE(w.plainText().contains(QStringLiteral("Body")));
    HtmlBlockWidget open(QStringLiteral("<details open><summary>T</summary>Shown</details>"), *env.styles, {});
    EXPECT_TRUE(open.plainText().contains(QStringLiteral("Shown")));
}

TEST(HtmlBlock, LinksAndMarkup)
{
    Env env;
    HtmlBlockWidget w(QStringLiteral("<p><a href=\"https://obsidian.md\">Obsidian</a> <mark>m</mark> <kbd>K</kbd> <del>d</del> <ins>i</ins></p>"),
                      *env.styles, {});
    w.layout(600);
    EXPECT_EQ(w.linkAt(QPointF(10, 10)), QStringLiteral("https://obsidian.md"));
    EXPECT_TRUE(w.qtHtml().contains(QStringLiteral("<s>")));
    EXPECT_TRUE(w.qtHtml().contains(QStringLiteral("<u>")));
    EXPECT_FALSE(w.qtHtml().contains(QStringLiteral("<mark")));
}

TEST(HtmlBlock, LivePreviewRevealsSourceReadingNever)
{
    Env env;
    const QString md = QStringLiteral("<p>html</p>\n\n<div align=\"center\">\n\nend");
    {
        Note n(md, *env.styles, false);
        QList<BlockRange> out;
        n.deco.blockRanges(0, n.src.text().lineCount(), out);
        ASSERT_EQ(out.size(), 1); // the empty <div> keeps its source in Live Preview
        EXPECT_EQ(out[0].fromLine, 0);
        EXPECT_EQ(out[0].toLine, 1);
    }
    {
        Note n(md, *env.styles, false, 3); // cursor inside <p>html</p>
        QList<BlockRange> out;
        n.deco.blockRanges(0, n.src.text().lineCount(), out);
        EXPECT_TRUE(out.isEmpty());
    }
    {
        Note n(md, *env.styles, true, 3);
        QList<BlockRange> out;
        n.deco.blockRanges(0, n.src.text().lineCount(), out);
        ASSERT_EQ(out.size(), 2);
        EXPECT_EQ(out[1].widget->layout(600), 0); // renders nothing...
        EXPECT_EQ(out[1].fromLine, 2);
        EXPECT_EQ(out[1].toLine, 4); // ...and takes the blank line after it along (no double paragraph gap)
    }
}

TEST(HtmlBlock, ReadingLayoutUsesTheWidget)
{
    Env env;
    Note n(QStringLiteral("before\n\n<table>\n<tr><td>a</td><td>b</td></tr>\n</table>\n\nafter"), *env.styles, true);
    DocLayout layout;
    layout.setDecorator(&n.deco);
    layout.setStyleSheet(env.styles);
    layout.setContentWidth(600);
    layout.setText(n.src.text());
    layout.ensureLaidOut(0, 1e6);
    EXPECT_TRUE(layout.isInBlock(2));
    EXPECT_TRUE(layout.isInBlock(4));
    EXPECT_FALSE(layout.isInBlock(6));
}

// ---- PDF export ---------------------------------------------------------------------------------------

TEST(HtmlPdf, ExportRendersHtml)
{
    Env env;
    const QString md = QStringLiteral("Keys <kbd>Ctrl</kbd> + <kbd>O</kbd>, H<sub>2</sub>O, line<br>break.\n\n"
                                      "<p align=\"center\">Centered <b>block</b> html</p>\n\n<script>alert(1)</script>\n");
    QTemporaryDir dir;
    const QString out = dir.path() + "/html.pdf";
    PdfExportSettings s;
    s.includeName = false;
    QString err;
    ASSERT_TRUE(exportPdf(Text::fromString(md), "HTML", out, s, {}, &err)) << err.toStdString();
    QPdfDocument pdf;
    ASSERT_EQ(pdf.load(out), QPdfDocument::Error::None);
    ASSERT_GE(pdf.pageCount(), 1);
    const QString text = pdf.getAllText(0).text();
    EXPECT_TRUE(text.contains(QStringLiteral("Ctrl"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Centered"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("break"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("<kbd>"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("<p"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("alert"))) << text.toStdString();
}
