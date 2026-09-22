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
        auto fx = open("\t- [x] done", 11);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QCOMPARE(fx.text(), QStringLiteral("\t- [x] done\n\t- [ ] "));
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
};

QTEST_MAIN(MarkdownCommandsTest)
#include "markdown_commands_test.moc"
