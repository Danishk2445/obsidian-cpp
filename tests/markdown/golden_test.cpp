// Golden dumps of the block parser for Obsidian-flavoured cases (tab lists, greentext, frontmatter
// variants, $$ / %% blocks, callouts, tables, setext vs HR, fences in containers, footnotes,
// refdefs...). Each case's SyntaxTree::dump() plus its leaf / table-cell contents is compared with
// tests/markdown/golden/<case>.dump; run with OC_UPDATE_GOLDEN=1 to (re)write the files, then review
// the diff. Every case is also typed in character by character and deleted again with incremental
// updates, which must agree with a full parse after every keystroke.
#include "markdown/syntax_tree.h"
#include "text/changeset.h"
#include "text/text.h"

#include <QDir>
#include <QFile>

#include <gtest/gtest.h>

using namespace oc;
using namespace oc::md;

namespace {

struct Case {
    const char* name;
    const char* markdown;
};

// clang-format off
const Case kCases[] = {
    {"tab_lists",
     "- level 1\n"
     "\t- level 2\n"
     "\t\t- level 3\n"
     "\t\t\t- level 4\n"
     "\t\t\t\t- level 5\n"
     "\t- back to 2\n"
     "  continuation of 2\n"
     "- [ ] task\n"
     "\t- [x] done\n"
     "\t\t- [>] forwarded\n"
     "\t\t1. ordered\n"
     "\t\t2) other delimiter\n"
     "- [] not a task\n"
     "- [ ]\n"
     "-\n"
     "  late content\n"},
    {"tab_list_top_level_indent",
     "\t- a tab-indented first item is indented code\n"
     "para\n"
     "\t- lazy? no: paragraph continuation\n"},
    {"loose_and_tight_lists",
     "- a\n"
     "- b\n"
     "\n"
     "- c\n"
     "\n"
     "1. x\n"
     "\n"
     "   y\n"
     "2. z\n"
     "\n"
     "* p\n"
     "  * q\n"
     "\n"
     "  * r\n"},
    {"greentext",
     ">greentext\n"
     ">more\n"
     "> spaced\n"
     ">\ttab\n"
     "not a quote\n"
     "\n"
     ">>nested\n"
     ">> > deeper\n"
     ">\n"
     "   > three spaces\n"
     "    > four spaces is code\n"},
    {"frontmatter_valid",
     "---\n"
     "title: x\n"
     "tags: [a, b]\n"
     "---\n"
     "# Body\n"},
    {"frontmatter_trailing_spaces",
     "---  \n"
     "k: v\n"
     "--- \t\n"
     "text\n"},
    {"frontmatter_unclosed",
     "---\n"
     "k: v\n"
     "\n"
     "text\n"},
    {"frontmatter_not_first_line",
     "\n"
     "---\n"
     "k: v\n"
     "---\n"},
    {"frontmatter_empty",
     "---\n"
     "---\n"
     "after\n"},
    {"frontmatter_indented_or_long",
     " ---\n"
     "k: v\n"
     "---\n"
     "----\n"},
    {"setext_vs_hr",
     "Heading\n"
     "===\n"
     "\n"
     "Para\n"
     "---\n"
     "\n"
     "---\n"
     "\n"
     "- - -\n"
     "text\n"
     "***\n"
     "two\n"
     "lines\n"
     "  ---  \n"
     "- item\n"
     "---\n"},
    {"atx_headings",
     "# H1\n"
     "## H2 ##\n"
     "###### H6 #\n"
     "####### seven\n"
     "#no space\n"
     "#\n"
     "# trailing #s ###   \n"
     "   ### indented\n"
     "## a#\n"
     "## \\#\n"},
    {"math_blocks",
     "$$\n"
     "x^2\n"
     "$$\n"
     "$$ \\sum one line $$\n"
     "$$x$$ more text\n"
     "para\n"
     "$$\n"
     "interrupts the paragraph\n"
     "text before $$ closes\n"
     "$$a\n"
     "b$$\n"
     "   $$ indented 3\n"
     "$$\n"
     "    $$ indented 4 is code\n"},
    {"math_in_containers",
     "- $$\n"
     "  a\n"
     "  $$\n"
     "> $$\n"
     "> b\n"
     "> $$\n"
     "- $$\n"
     "  unclosed, ends with the item\n"
     "after\n"},
    {"math_unclosed",
     "text\n"
     "$$\n"
     "runs to the end\n"
     "\n"
     "# not a heading\n"},
    {"comment_blocks",
     "%%\n"
     "hidden\n"
     "%%\n"
     "%% inline comment %%\n"
     "para\n"
     "%% opens\n"
     "still hidden\n"
     "closes %% here\n"
     "> %%\n"
     "> quoted comment\n"
     "> %%\n"
     "%%\n"
     "unclosed\n"},
    {"callouts",
     "> [!note] Title\n"
     "> body\n"
     "\n"
     "> [!TIP]- Folded *title*\n"
     "> - list in callout\n"
     "\n"
     "> [!warning|meta data]+\n"
     "> no title, metadata\n"
     "\n"
     "> [!info]\n"
     "> > [!question] Nested\n"
     "> > text\n"
     "\n"
     ">[!note] greentext-style callout\n"
     "\n"
     "> [!note]x is not a callout\n"
     "\n"
     "> text first\n"
     "> [!note] not a header either\n"
     "\n"
     "- > [!faq] In a list item\n"
     "  > answer\n"},
    {"callout_continuations",
     "> [!note] Title\n"
     "lazy line after the header\n"
     "\n"
     "> [!note] Title\n"
     "> body\n"
     "lazy body line\n"
     "\n"
     "> [!note]\n"
     ">\n"
     "> after an empty line\n"
     "> ```\n"
     "> code in a callout\n"
     "> ```\n"},
    {"tables",
     "para line\n"
     "| a | b \\| c |\n"
     "|:--|--:|\n"
     "| `x \\| y` | [[link\\|alias]] |\n"
     "row without pipes\n"
     "\n"
     "a | b\n"
     "- | :-:\n"
     "1 | 2\n"
     "> quote ends the table\n"
     "\n"
     "| one |\n"
     "| --- | --- |\n"
     "\n"
     "|x|\n"
     "|-|\n"
     "|\n"},
    {"tables_in_containers",
     "- | h1 | h2 |\n"
     "  | --- | --- |\n"
     "  | c1 | c2 |\n"
     "> | q |\n"
     "> |---|\n"
     "> | r |\n"
     "lazy text is not a row\n"},
    {"fences_in_containers",
     "- item\n"
     "  ```js title=x\n"
     "  code\n"
     "  ```\n"
     "- item 2\n"
     "  ~~~\n"
     "  unclosed in item\n"
     "- item 3\n"
     "\n"
     "> ```\n"
     "> quoted code\n"
     "> ```\n"
     "\n"
     "````Python extra\n"
     "```\n"
     "not closed by a shorter fence\n"
     "`````\n"
     " ```\n"
     "   indented fence\n"
     "  ```\n"},
    {"footnotes",
     "Text[^1].\n"
     "\n"
     "[^1]: Footnote text\n"
     "    continued\n"
     "lazy line\n"
     "\n"
     "[^note]: Second\n"
     "\n"
     "    Paragraph two in the footnote.\n"
     "\n"
     "Outside again.\n"
     "[^x]:\n"
     "[^ bad]: not a footnote\n"},
    {"refdefs",
     "[a]: /url \"title\"\n"
     "[b]:\n"
     "  /url2\n"
     "  'title2'\n"
     "Para after refdefs [a].\n"
     "\n"
     "[c]: /x\n"
     "===\n"
     "\n"
     "[d]: /y\n"
     "Setext after a refdef\n"
     "---\n"
     "\n"
     "> [e]: /in-quote\n"},
    {"html_blocks",
     "<div>\n"
     "*x*\n"
     "</div>\n"
     "\n"
     "<!-- comment\n"
     "-->\n"
     "after\n"
     "<script>\n"
     "\n"
     "</script>\n"
     "<span class=\"k7\">\n"
     "kind 7 runs to a blank line\n"
     "\n"
     "<span>inline, not a block</span>\n"
     "<span>\n"
     "(kind 7 cannot interrupt a paragraph)\n"},
    {"indented_code",
     "    code\n"
     "\n"
     "    more\n"
     "\n"
     "\n"
     "para\n"
     "    continuation\n"
     "\t\ttabbed code\n"},
    {"nested_containers",
     "> - a\n"
     ">   > b\n"
     ">   c\n"
     "\n"
     "1. > quote in item\n"
     "   > more\n"
     "\n"
     "   para in item\n"
     "2. [^f]: footnote in item\n"
     "       continued\n"},
    {"blank_and_whitespace",
     "\n"
     "  \n"
     "\t\n"
     "text  \n"
     "  more text\t\n"
     "\n"},
};
// clang-format on

QString escaped(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('\n'), QLatin1String("\\n")).replace(QLatin1Char('\t'), QLatin1String("\\t"));
    return out;
}

// dump() plus the inline content of every leaf and table cell.
QString render(const SyntaxTree& t, const Text& text)
{
    QString out = t.dump(text);
    out += QStringLiteral("-- content\n");
    for (int id = 1; id < t.nodeCount(); ++id) {
        const BlockNode& n = t.node(id);
        if (n.type != NodeType::Paragraph && n.type != NodeType::AtxHeading && n.type != NodeType::SetextHeading)
            continue;
        const LeafContent c = t.leafContent(id, text);
        QStringList segs;
        for (const LeafContent::Segment& s : c.segments)
            segs << QStringLiteral("%1:%2+%3@%4").arg(s.line).arg(s.col).arg(s.length).arg(s.contentFrom);
        out += QStringLiteral("#%1 %2 \"%3\" [%4]\n").arg(id).arg(QLatin1StringView(nodeTypeName(n.type)), escaped(c.text), segs.join(QLatin1Char(' ')));
    }
    for (int l = 0; l < t.lineCount(); ++l) {
        const QList<TableCell> cells = t.tableCells(l, text);
        if (cells.isEmpty())
            continue;
        QStringList cs;
        for (int i = 0; i < cells.size(); ++i)
            cs << QStringLiteral("%1-%2 \"%3\"").arg(cells[i].from).arg(cells[i].to).arg(escaped(t.tableCellContent(l, i, text).text));
        out += QStringLiteral("cells %1: %2\n").arg(l).arg(cs.join(QStringLiteral(" | ")));
    }
    return out;
}

QString goldenPath(const char* name)
{
    return QStringLiteral(OC_SOURCE_DIR "/tests/markdown/golden/%1.dump").arg(QLatin1StringView(name));
}

class Golden : public ::testing::TestWithParam<Case> {};

TEST_P(Golden, Dump)
{
    const Case& c = GetParam();
    const Text text = Text::fromString(QString::fromUtf8(c.markdown));
    SyntaxTree tree;
    tree.parse(text);
    const QString actual = QStringLiteral("=== input\n") + QString::fromUtf8(c.markdown) + QStringLiteral("=== tree\n") + render(tree, text);
    QFile f(goldenPath(c.name));
    if (qEnvironmentVariableIsSet("OC_UPDATE_GOLDEN")) {
        QDir().mkpath(QFileInfo(f).absolutePath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(actual.toUtf8());
        return;
    }
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "missing " << qPrintable(f.fileName()) << " (run with OC_UPDATE_GOLDEN=1)";
    const QString expected = QString::fromUtf8(f.readAll());
    EXPECT_EQ(expected.toStdString(), actual.toStdString());
}

// Types the case in one character at a time, then deletes it from the front, with incremental
// updates (default budget, and a zero budget + resume); the tree must equal a full parse throughout.
TEST_P(Golden, TypedIncrementally)
{
    const Case& c = GetParam();
    const QString doc = QString::fromUtf8(c.markdown);
    for (const bool zeroBudget : {false, true}) {
        Text text;
        SyntaxTree tree;
        tree.parse(text);
        auto step = [&](const ChangeSet& cs, const char* what, int i) {
            text = cs.apply(text);
            TreeUpdate u = tree.update(text, cs.desc(), zeroBudget ? ParseBudget{-1, 0} : ParseBudget{});
            while (!u.complete)
                u.complete = !tree.resume(text, 0, &u);
            SyntaxTree full;
            full.parse(text);
            QString why;
            ASSERT_TRUE(SyntaxTree::equivalent(tree, full, &why)) << c.name << ' ' << what << ' ' << i << ": " << qPrintable(why);
        };
        for (qsizetype i = 0; i < doc.size(); ++i) {
            step(ChangeSet::of(ChangeSpec{i, i, doc.mid(i, 1)}, text.length()), "typing", int(i));
            if (HasFatalFailure())
                return;
        }
        for (qsizetype i = 0; i < doc.size(); ++i) {
            step(ChangeSet::of(ChangeSpec{0, 1, QString()}, text.length()), "deleting", int(i));
            if (HasFatalFailure())
                return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Obsidian, Golden, ::testing::ValuesIn(kCases), [](const ::testing::TestParamInfo<Case>& i) { return std::string(i.param.name); });

} // namespace
