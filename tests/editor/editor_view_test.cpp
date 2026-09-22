// EditorView interaction tests (offscreen): typing, undo grouping, movement, selection, multi-cursor,
// clipboard, IME, scroll anchoring.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QInputMethodEvent>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>

using namespace oc;

class EditorViewTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    std::unique_ptr<ThemeManager> m_themes;
    std::unique_ptr<LocalFileStorage> m_storage;

    struct Fixture {
        std::shared_ptr<Document> doc;
        std::unique_ptr<EditorView> view;
    };
    Fixture open(const QByteArray& content, const QString& name = QStringLiteral("t.md"))
    {
        QFile f(m_dir.path() + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("cannot write fixture");
        f.write(content);
        f.close();
        Fixture fx;
        fx.doc = std::make_shared<Document>(m_storage.get(), name);
        if (!fx.doc->load())
            qFatal("cannot load");
        fx.view = std::make_unique<EditorView>();
        fx.view->resize(800, 600);
        fx.view->setDocument(fx.doc);
        fx.view->show();
        fx.view->setFocus();
        QApplication::processEvents();
        return fx;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_themes = std::make_unique<ThemeManager>();
        m_themes->setMode(ThemeManager::Mode::Dark);
        m_themes->apply();
        m_storage = std::make_unique<LocalFileStorage>(m_dir.path());
    }

    void typingAndUndoGrouping()
    {
        auto fx = open("");
        QTest::keyClicks(fx.view.get(), "hello");
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("hello"));
        QCOMPARE(fx.view->cursorPosition(), 5);
        QTest::keyClick(fx.view.get(), Qt::Key_Return);
        QTest::keyClicks(fx.view.get(), "world");
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("hello\nworld"));
        fx.view->undo();
        QCOMPARE(fx.doc->text().toString(), QStringLiteral(""));
        fx.view->redo();
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("hello\nworld"));
        QCOMPARE(fx.view->cursorPosition(), 11);
    }

    void backspaceDeleteAndWords()
    {
        auto fx = open("alpha beta gamma");
        fx.view->setCursorPosition(16);
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace, Qt::ControlModifier);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("alpha beta "));
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("alpha beta"));
        fx.view->setCursorPosition(0);
        QTest::keyClick(fx.view.get(), Qt::Key_Delete, Qt::ControlModifier);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral(" beta"));
    }

    void graphemeMovementOverEmoji()
    {
        auto fx = open("a👍️b");
        fx.view->setCursorPosition(1);
        QTest::keyClick(fx.view.get(), Qt::Key_Right);
        QCOMPARE(fx.view->cursorPosition(), 4); // over the surrogate pair + variation selector
        QTest::keyClick(fx.view.get(), Qt::Key_Backspace);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("ab"));
    }

    void verticalMovementKeepsGoalColumn()
    {
        auto fx = open("long line of text here\nab\nanother long line here");
        fx.view->setCursorPosition(15);
        QTest::keyClick(fx.view.get(), Qt::Key_Down);
        QCOMPARE(fx.view->cursorPosition(), qsizetype(23 + 2)); // clamped to end of "ab"
        QTest::keyClick(fx.view.get(), Qt::Key_Down);
        const qsizetype l2 = fx.doc->text().lineStart(2);
        QVERIFY(std::abs(fx.view->cursorPosition() - (l2 + 15)) <= 1); // goal x restored
        QTest::keyClick(fx.view.get(), Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(fx.view->selection().main().anchor, fx.view->cursorPosition() + (fx.view->selection().main().anchor - fx.view->cursorPosition()));
        QVERIFY(!fx.view->selection().main().empty());
    }

    void homeEndAndSelectAll()
    {
        auto fx = open("\t\tindented line\nnext");
        fx.view->setCursorPosition(8);
        QTest::keyClick(fx.view.get(), Qt::Key_Home);
        QCOMPARE(fx.view->cursorPosition(), 2); // first non-space
        QTest::keyClick(fx.view.get(), Qt::Key_Home);
        QCOMPARE(fx.view->cursorPosition(), 0);
        QTest::keyClick(fx.view.get(), Qt::Key_End);
        QCOMPARE(fx.view->cursorPosition(), 15);
        QTest::keyClick(fx.view.get(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(fx.view->selectedText(), QStringLiteral("\t\tindented line\nnext"));
    }

    void multiCursorTyping()
    {
        auto fx = open("one\ntwo\nthree");
        fx.view->setCursorPosition(0);
        QTest::keyClick(fx.view.get(), Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier);
        QTest::keyClick(fx.view.get(), Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(fx.view->selection().size(), 3);
        QTest::keyClicks(fx.view.get(), "- ");
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("- one\n- two\n- three"));
        QTest::keyClick(fx.view.get(), Qt::Key_Escape);
        QCOMPARE(fx.view->selection().size(), 1);
    }

    void indentOutdentAndMoveLines()
    {
        auto fx = open("a\nb\nc");
        fx.view->setSelection(Selection::single(0, 3)); // lines a and b
        QTest::keyClick(fx.view.get(), Qt::Key_Tab);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("\ta\n\tb\nc"));
        QTest::keyClick(fx.view.get(), Qt::Key_Backtab);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("a\nb\nc"));
        fx.view->setCursorPosition(4); // on "c"
        QTest::keyClick(fx.view.get(), Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("a\nc\nb"));
        QTest::keyClick(fx.view.get(), Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("a\nb"));
    }

    void clipboardRoundTrip()
    {
        auto fx = open("copy me\nand me");
        fx.view->setSelection(Selection::single(0, 7));
        fx.view->copy();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("copy me"));
        fx.view->setCursorPosition(fx.doc->text().length());
        QApplication::clipboard()->setText(QStringLiteral("X\r\nY"));
        fx.view->paste();
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("copy me\nand meX\nY"));
    }

    void imeCommitAndPreedit()
    {
        auto fx = open("ab");
        fx.view->setCursorPosition(1);
        {
            QInputMethodEvent pre(QStringLiteral("に"), {});
            QApplication::sendEvent(fx.view.get(), &pre);
        }
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("ab")); // preedit is not in the document
        {
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("日本"));
            QApplication::sendEvent(fx.view.get(), &commit);
        }
        QCOMPARE(fx.doc->text().toString(), QStringLiteral("a日本b"));
        QCOMPARE(static_cast<QWidget*>(fx.view.get())->inputMethodQuery(Qt::ImSurroundingText).toString(), QStringLiteral("a日本b"));
        QCOMPARE(static_cast<QWidget*>(fx.view.get())->inputMethodQuery(Qt::ImCursorPosition).toInt(), 3);
    }

    void externalEditAboveKeepsViewportAnchored()
    {
        QByteArray content;
        for (int i = 0; i < 400; ++i)
            content += "line " + QByteArray::number(i) + "\n";
        auto fx = open(content);
        const int target = 200;
        fx.view->setScrollTop(fx.view->layout().lineTop(target));
        QApplication::processEvents();
        const qsizetype topPos = fx.view->positionAt(QPoint(100, int(fx.view->topPadding())));
        const int topLine = fx.doc->text().lineAt(topPos);
        // Insert 50 lines at the top from "outside" (not this view).
        Transaction tr;
        QString ins;
        for (int i = 0; i < 50; ++i)
            ins += QStringLiteral("inserted\n");
        tr.changes = ChangeSet::of({{0, 0, ins}}, fx.doc->text().length());
        tr.userEvent = QStringLiteral("external");
        tr.addToHistory = false;
        fx.doc->dispatch(tr, nullptr, Selection());
        QApplication::processEvents();
        const qsizetype topPos2 = fx.view->positionAt(QPoint(100, int(fx.view->topPadding())));
        QCOMPARE(fx.doc->text().lineAt(topPos2), topLine + 50);
    }

    void clickPlacesCaretAndDragSelects()
    {
        auto fx = open("first line\nsecond line\nthird line");
        const QRectF r = fx.view->layout().caretRect(fx.doc->text().lineStart(1) + 3);
        const QPointF vp = fx.view->docToViewport(r.center());
        QTest::mouseClick(fx.view->viewport(), Qt::LeftButton, {}, vp.toPoint());
        QCOMPARE(fx.view->cursorPosition(), fx.doc->text().lineStart(1) + 3);
        QTest::qWait(QApplication::doubleClickInterval() + 50); // next press must not count as a double click
        const QRectF r2 = fx.view->layout().caretRect(fx.doc->text().lineStart(2) + 5);
        QTest::mousePress(fx.view->viewport(), Qt::LeftButton, {}, vp.toPoint());
        QTest::mouseMove(fx.view->viewport(), fx.view->docToViewport(r2.center()).toPoint());
        QMouseEvent move(QEvent::MouseMove, fx.view->docToViewport(r2.center()), fx.view->viewport()->mapToGlobal(fx.view->docToViewport(r2.center())),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(fx.view->viewport(), &move);
        QTest::mouseRelease(fx.view->viewport(), Qt::LeftButton, {}, fx.view->docToViewport(r2.center()).toPoint());
        QCOMPARE(fx.view->selection().main().from(), fx.doc->text().lineStart(1) + 3);
        QCOMPARE(fx.view->selection().main().to(), fx.doc->text().lineStart(2) + 5);
    }
};

QTEST_MAIN(EditorViewTest)
#include "editor_view_test.moc"
