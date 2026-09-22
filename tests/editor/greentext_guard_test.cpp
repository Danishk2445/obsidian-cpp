// Greentext Guard port: behaviour must match the user's plugin (main.js).
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "editor/plugins/greentext_guard.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>

using namespace oc;

class GreentextGuardTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    std::unique_ptr<ThemeManager> m_themes;
    std::unique_ptr<LocalFileStorage> m_storage;

    struct Fx {
        std::shared_ptr<Document> doc;
        std::unique_ptr<EditorView> view;
        QString text() const { return doc->text().toString(); }
    };
    Fx open(const QString& name, const QString& content, qsizetype cursor)
    {
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
        fx.view->setDocument(fx.doc);
        fx.view->show();
        fx.view->setFocus();
        fx.view->setCursorPosition(cursor);
        return fx;
    }
    void paste(Fx& fx, const QString& text)
    {
        QApplication::clipboard()->setText(text);
        fx.view->paste();
    }

private slots:
    void initTestCase()
    {
        m_themes = std::make_unique<ThemeManager>();
        m_themes->apply();
        m_storage = std::make_unique<LocalFileStorage>(m_dir.path());
        GreentextGuard::instance().setSettings(GreentextGuard::Settings{});
        GreentextGuard::instance().install();
    }

    void fenceWrapUsesLongerFenceThanContent()
    {
        QCOMPARE(GreentextGuard::fenceWrap("a"), QStringLiteral("```\na\n```"));
        QCOMPARE(GreentextGuard::fenceWrap("x ```` y  \n"), QStringLiteral("`````\nx ```` y\n`````"));
    }

    void typingGreaterThanAtLineStartInMatchingNote()
    {
        auto fx = open("my 4chan thread.md", "", 0);
        QTest::keyClicks(fx.view.get(), ">");
        QCOMPARE(fx.text(), QStringLiteral("`>`"));
        QCOMPARE(fx.view->cursorPosition(), 2);
        auto other = open("normal.md", "", 0);
        QTest::keyClicks(other.view.get(), ">");
        QCOMPARE(other.text(), QStringLiteral(">"));
        auto mid = open("4chan b.md", "text ", 5);
        QTest::keyClicks(mid.view.get(), ">");
        QCOMPARE(mid.text(), QStringLiteral("text >"));
    }

    void escapeModeAndInsideFence()
    {
        GreentextGuard::Settings s;
        s.onType = QStringLiteral("escape");
        GreentextGuard::instance().setSettings(s);
        auto fx = open("4CHAN.md", "\t", 1);
        QTest::keyClicks(fx.view.get(), ">");
        QCOMPARE(fx.text(), QStringLiteral("\t\\>"));
        GreentextGuard::instance().setSettings(GreentextGuard::Settings{});
        auto fenced = open("4chan c.md", "```\n\n```", 4);
        QTest::keyClicks(fenced.view.get(), ">");
        QCOMPARE(fenced.text(), QStringLiteral("```\n>\n```"));
    }

    void pasteExpandsToSurroundingBlock()
    {
        auto fx = open("4chan d.md", "before\n\nhead line\n\nafter", 17); // end of "head line"
        paste(fx, ">be me\n>paste");
        QCOMPARE(fx.text(), QStringLiteral("before\n\n```\nhead line\n>be me\n>paste\n```\n\nafter"));
        auto plain = open("4chan e.md", "", 0);
        paste(plain, "no quotes here");
        QCOMPARE(plain.text(), QStringLiteral("no quotes here"));
    }

    void pasteWithoutExpandAddsLeadingNewline()
    {
        GreentextGuard::Settings s;
        s.expandToBlock = false;
        GreentextGuard::instance().setSettings(s);
        auto fx = open("4chan f.md", "x", 1);
        paste(fx, ">green");
        QCOMPARE(fx.text(), QStringLiteral("x\n```\n>green\n```\n"));
        GreentextGuard::instance().setSettings(GreentextGuard::Settings{});
    }

    void wrapSelectionCommand()
    {
        auto fx = open("any.md", "sel", 0);
        QVERIFY(!GreentextGuard::instance().wrapSelectionInFence(*fx.view));
        fx.view->setSelection(Selection::single(0, 3));
        QVERIFY(GreentextGuard::instance().wrapSelectionInFence(*fx.view));
        QCOMPARE(fx.text(), QStringLiteral("```\nsel\n```\n"));
    }
};

QTEST_MAIN(GreentextGuardTest)
#include "greentext_guard_test.moc"
