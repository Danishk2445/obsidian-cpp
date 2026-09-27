// Markdown editing behaviours (Obsidian parity): list continuation/exit, list indent, markup
// backspace, auto-pairing, formatting toggles.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/markdown_commands.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace oc;

class MarkdownCommandsTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    std::unique_ptr<ThemeManager> m_themes;
    std::unique_ptr<LocalFileStorage> m_storage;
    int m_n = 0;

    struct Fx {
        std::shared_ptr<Document> doc;
        std::unique_ptr<EditorView> view;
        QString text() const { return doc->text().toString(); }
    };
    Fx open(const QString& content, qsizetype cursor)
    {
        const QString name = QStringLiteral("n%1.md").arg(++m_n);
        QFile f(m_dir.path() + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("fixture");
        f.write(content.toUtf8());
        f.close();
        Fx fx;
        fx.doc = std::make_shared<Document>(m_storage.get(), name);
        if (!fx.doc->load())
            qFatal("load");
        fx.view = std::make_unique<EditorView>();
        fx.view->resize(700, 400);
        fx.view->setDocument(fx.doc);
        fx.view->show();
        fx.view->setFocus();
        fx.view->setCursorPosition(cursor);
        return fx;
    }

private slots:
    void initTestCase()
    {
        m_themes = std::make_unique<ThemeManager>();
        m_themes->apply();
        m_storage = std::make_unique<LocalFileStorage>(m_dir.path());
    }

    void enterContinuesBulletList()
    {
        auto fx = open("- first", 7);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("- first\n- "));
        QTest::keyClicks(fx.view.get(), "second");
        QCOMPARE(fx.text(), QStringLiteral("- first\n- second"));
    }

    void enterContinuesTabIndentedTaskAndOrdered()
    {
        // (A tab-indented item needs a parent item: a top-level "\t- x" line is an indented code
        // block in CommonMark, where Enter must not continue list markup.)
        auto fx = open("- a\n\t- [x] done", 15);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("- a\n\t- [x] done\n\t- [ ] "));
        auto fx2 = open("9. nine", 7);
        QTest::keyClick(fx2.view.get(), Qt::Key_Return);
        QCOMPARE(fx2.text(), QStringLiteral("9. nine\n10. "));
    }

    void enterOnEmptyItemOutdentsThenExits()
    {
        auto fx = open("- a\n\t- ", 7);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("- a\n- "));
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("- a\n"));
    }

    void enterContinuesQuoteAndExitsEmptyQuote()
    {
        auto fx = open("> quoted", 8);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("> quoted\n> "));
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("> quoted\n"));
    }

    void tabIndentsListItemAndBackspaceRemovesMarker()
    {
        auto fx = open("- a\n- b", 7);
        QTest::keyClick(fx.view.get(), Qt::Key_Tab);
        QCOMPARE(fx.text(), QStringLiteral("- a\n\t- b"));
        QTest::keyClick(fx.view.get(), Qt::Key_Backtab);
        QCOMPARE(fx.text(), QStringLiteral("- a\n- b"));
        fx.view->setCursorPosition(6); // right after "- " on line 2
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx.text(), QStringLiteral("- a\nb"));
    }

    void autoPairBracketsAndWrapSelection()
    {
        auto fx = open("x ", 2);
        QTest::keyClicks(fx.view.get(), "(");
        QCOMPARE(fx.text(), QStringLiteral("x ()"));
        QTest::keyClicks(fx.view.get(), "a)");
        QCOMPARE(fx.text(), QStringLiteral("x (a)")); // typed over the closer
        auto fx2 = open("word", 0);
        fx2.view->setSelection(Selection::single(0, 4));
        QTest::keyClicks(fx2.view.get(), "*");
        QCOMPARE(fx2.text(), QStringLiteral("*word*"));
        QCOMPARE(fx2.view->selectedText(), QStringLiteral("word"));
        auto fx3 = open("don", 3);
        QTest::keyClicks(fx3.view.get(), "'t");
        QCOMPARE(fx3.text(), QStringLiteral("don't")); // no pairing inside words
        auto fx4 = open("", 0);
        QTest::keyClicks(fx4.view.get(), "[[");
        QCOMPARE(fx4.text(), QStringLiteral("[[]]"));
        QCOMPARE(fx4.view->cursorPosition(), 2);
    }

    void backticksPairAndCloseCodeBlocks()
    {
        // ``` on an empty line: "`|`", then over the closer, then the closing fence on the next line
        auto fx = open("", 0);
        QTest::keyClicks(fx.view.get(), "`");
        QCOMPARE(fx.text(), QStringLiteral("``"));
        QCOMPARE(fx.view->cursorPosition(), 1);
        QTest::keyClicks(fx.view.get(), "``");
        QCOMPARE(fx.text(), QStringLiteral("```\n```"));
        QCOMPARE(fx.view->cursorPosition(), 3); // on the opening fence, ready for the language
        QTest::keyClicks(fx.view.get(), "js");
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QTest::keyClicks(fx.view.get(), "x");
        QCOMPARE(fx.text(), QStringLiteral("```js\nx\n```"));
        // inline code types over its own closer
        auto fx2 = open("see ", 4);
        QTest::keyClicks(fx2.view.get(), "`code`.");
        QCOMPARE(fx2.text(), QStringLiteral("see `code`."));
        // ...but not over a backtick that was already there, and not after a word
        auto fx3 = open("a `b`", 4);
        QTest::keyClicks(fx3.view.get(), "`");
        QCOMPARE(fx3.text(), QStringLiteral("a `b``"));
        auto fx4 = open("word", 4);
        QTest::keyClicks(fx4.view.get(), "`");
        QCOMPARE(fx4.text(), QStringLiteral("word`"));
        // the closer is forgotten once the cursor leaves its line
        auto fx5 = open("x\n", 2);
        QTest::keyClicks(fx5.view.get(), "`");
        fx5.view->setCursorPosition(0);
        fx5.view->setCursorPosition(3);
        QTest::keyClicks(fx5.view.get(), "`");
        QCOMPARE(fx5.text(), QStringLiteral("x\n```"));
        // list items and quotes: the closing fence lines up with the content
        auto fx6 = open("- ", 2);
        QTest::keyClicks(fx6.view.get(), "```");
        QCOMPARE(fx6.text(), QStringLiteral("- ```\n  ```"));
        auto fx7 = open("- a\n\t1. ", 8);
        QTest::keyClicks(fx7.view.get(), "```");
        QCOMPARE(fx7.text(), QStringLiteral("- a\n\t1. ```\n\t   ```"));
        auto fx8 = open("> ", 2);
        QTest::keyClicks(fx8.view.get(), "```");
        QCOMPARE(fx8.text(), QStringLiteral("> ```\n> ```"));
        // typing the closing fence of an open code block adds nothing
        auto fx9 = open("```\nx\n", 6);
        QTest::keyClicks(fx9.view.get(), "```");
        QCOMPARE(fx9.text(), QStringLiteral("```\nx\n```"));
        // mid-line ``` is inline code, not a block
        auto fx10 = open("run ", 4);
        QTest::keyClicks(fx10.view.get(), "```");
        QCOMPARE(fx10.text(), QStringLiteral("run ```"));
        // off: nothing pairs
        mdcmd::settings().autoPairMarkdown = false;
        auto fx11 = open("", 0);
        QTest::keyClicks(fx11.view.get(), "```");
        QCOMPARE(fx11.text(), QStringLiteral("```"));
        mdcmd::settings().autoPairMarkdown = true;
    }

    void backspaceDeletesAnEmptyPair()
    {
        auto fx = open("x ", 2);
        QTest::keyClicks(fx.view.get(), "`");
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx.text(), QStringLiteral("x "));
        QTest::keyClicks(fx.view.get(), "(");
        QCOMPARE(fx.text(), QStringLiteral("x ()"));
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx.text(), QStringLiteral("x "));
        auto fx2 = open("(a)", 2);
        QTest::keyClick(fx2.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx2.text(), QStringLiteral("()")); // only right between the two
        QTest::keyClick(fx2.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx2.text(), QString());
    }

    void formattingToggles()
    {
        auto fx = open("bold me", 0);
        fx.view->setSelection(Selection::single(0, 4));
        mdcmd::toggleBold(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("**bold** me"));
        mdcmd::toggleBold(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("bold me"));
        mdcmd::setHeading(*fx.view, 2);
        QCOMPARE(fx.text(), QStringLiteral("## bold me"));
        mdcmd::setHeading(*fx.view, 0);
        QCOMPARE(fx.text(), QStringLiteral("bold me"));
        mdcmd::toggleChecklist(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("- [ ] bold me"));
        mdcmd::toggleChecklist(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("- [x] bold me"));
        mdcmd::toggleBlockquote(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("> - [x] bold me"));
        auto fx2 = open("see", 3);
        fx2.view->setSelection(Selection::single(0, 3));
        mdcmd::insertLink(*fx2.view);
        QCOMPARE(fx2.text(), QStringLiteral("[see]()"));
        QCOMPARE(fx2.view->cursorPosition(), 6);
    }

    void checklistFollowsObsidian()
    {
        // Mod+L on numbered items keeps the number; the cursor stays on the same text
        auto fx = open("1. one", 6);
        mdcmd::toggleChecklist(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("1. [ ] one"));
        QCOMPARE(fx.view->cursorPosition(), 10);
        mdcmd::toggleChecklist(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("1. [x] one"));
        mdcmd::toggleChecklist(*fx.view);
        QCOMPARE(fx.text(), QStringLiteral("1. [ ] one"));
        // cycle: text -> bullet -> [ ] -> [x] -> bullet
        auto c = open("\tnote", 1);
        mdcmd::toggleChecklist(*c.view, true);
        QCOMPARE(c.text(), QStringLiteral("\t- note"));
        mdcmd::toggleChecklist(*c.view, true);
        QCOMPARE(c.text(), QStringLiteral("\t- [ ] note"));
        mdcmd::toggleChecklist(*c.view, true);
        QCOMPARE(c.text(), QStringLiteral("\t- [x] note"));
        mdcmd::toggleChecklist(*c.view, true);
        QCOMPARE(c.text(), QStringLiteral("\t- note"));
        // several lines take one state from the least-done line; quotes keep their prefix
        auto m = open("> - [x] a\n> - [ ] b\n> - [x] c", 0);
        m.view->setSelection(Selection::single(0, m.doc->text().length()));
        mdcmd::toggleChecklist(*m.view);
        QCOMPARE(m.text(), QStringLiteral("> - [x] a\n> - [x] b\n> - [x] c"));
        mdcmd::toggleChecklist(*m.view);
        QCOMPARE(m.text(), QStringLiteral("> - [ ] a\n> - [ ] b\n> - [ ] c"));
    }

    void insertFootnoteAndTag()
    {
        auto fx = open("Text[^1] here\n\n[^1]: first\n[^note]: named\n", 4);
        QCOMPARE(mdcmd::nextFootnoteNumber(fx.doc->text()), 2);
        QCOMPARE(mdcmd::insertFootnote(*fx.view), QStringLiteral("[^2]"));
        QCOMPARE(fx.text(), QStringLiteral("Text[^2][^1] here\n\n[^1]: first\n[^note]: named\n\n[^2]: \n"));
        QCOMPARE(fx.view->cursorPosition(), 8);
        auto bare = open("no newline", 2);
        mdcmd::insertFootnote(*bare.view);
        QCOMPARE(bare.text(), QStringLiteral("no[^1] newline\n\n[^1]: \n"));
        auto tag = open("ab", 1);
        tag.view->setSelection(Selection::single(1, 2));
        mdcmd::insertTag(*tag.view);
        QCOMPARE(tag.text(), QStringLiteral("a#b"));
        QCOMPARE(tag.view->cursorPosition(), 2);
    }
};

QTEST_MAIN(MarkdownCommandsTest)
#include "markdown_commands_test.moc"
