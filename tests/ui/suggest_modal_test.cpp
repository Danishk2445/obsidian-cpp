// SuggestModal (Obsidian's prompt): filtering, keyboard navigation, choosing, closing.
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"

#include <QApplication>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;

namespace {
QList<Suggestion> filter(const QString& q)
{
    static const QStringList all{"Welcome", "Formatting", "Lists", "Code", "Tables", "Quotes and callouts", "Math"};
    QList<Suggestion> out;
    for (const QString& s : all) {
        const qsizetype i = s.indexOf(q, 0, Qt::CaseInsensitive);
        if (q.isEmpty() || i >= 0) {
            Suggestion sg;
            sg.title = s;
            if (!q.isEmpty())
                sg.titleMatches.append({int(i), int(i + q.size())});
            sg.note = QStringLiteral("folder/");
            sg.data = s;
            out.append(sg);
        }
    }
    return out;
}

struct Window {
    Window()
    {
        themes.setMode(ThemeManager::Mode::Dark);
        themes.apply();
        w.resize(1000, 700);
        layer = new OverlayLayer(&w);
        w.show();
        QApplication::processEvents();
    }
    ThemeManager themes;
    QWidget w;
    OverlayLayer* layer;
};
} // namespace

TEST(SuggestModal, FiltersNavigatesAndChooses)
{
    Window win;
    auto* m = new SuggestModal(win.layer);
    m->setPlaceholder("Find or create a note...");
    m->setInstructions({{"↑↓", "to navigate"}, {"↵", "to open"}, {"ctrl ↵", "to open in new tab"}, {"esc", "to dismiss"}});
    m->setProvider(filter);
    QPointer<SuggestModal> guard(m);
    QSignalSpy chosen(m, &SuggestModal::chosen);
    QSignalSpy closed(m, &SuggestModal::closed);
    m->open();
    QApplication::processEvents();
    EXPECT_TRUE(m->isOpen());
    EXPECT_EQ(m->suggestions().size(), 7);
    EXPECT_EQ(m->selectedIndex(), 0);
    // prompt geometry: 700 wide, 80 px from the top, centered
    EXPECT_EQ(m->promptGeometry().top(), 80);
    EXPECT_EQ(m->promptGeometry().width(), 700);
    EXPECT_EQ(m->promptGeometry().left(), 150);

    QTest::keyClicks(m->input(), "t");
    EXPECT_EQ(m->suggestions().size(), 5); // Formatting, Lists, Tables, Quotes and callouts, Math
    QTest::keyClick(m->input(), Qt::Key_Up); // wraps to the last
    EXPECT_EQ(m->selectedIndex(), int(m->suggestions().size()) - 1);
    QTest::keyClick(m->input(), Qt::Key_Down); // wraps to the first
    EXPECT_EQ(m->selectedIndex(), 0);
    QTest::keyClick(m->input(), Qt::Key_N, Qt::ControlModifier);
    EXPECT_EQ(m->selectedIndex(), 1);
    QTest::keyClick(m->input(), Qt::Key_End);
    EXPECT_EQ(m->selectedIndex(), int(m->suggestions().size()) - 1);
    QTest::keyClick(m->input(), Qt::Key_Home);
    EXPECT_EQ(m->selectedIndex(), 0);

    const QString expected = m->suggestions().at(0).title;
    QTest::keyClick(m->input(), Qt::Key_Return, Qt::ControlModifier);
    ASSERT_EQ(chosen.count(), 1);
    EXPECT_EQ(chosen.at(0).at(0).value<Suggestion>().title, expected);
    EXPECT_EQ(chosen.at(0).at(1).value<Qt::KeyboardModifiers>(), Qt::ControlModifier);
    EXPECT_EQ(closed.count(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(guard.isNull());
}

TEST(SuggestModal, EmptyStateEscapeAndBackgroundClick)
{
    Window win;
    auto* m = new SuggestModal(win.layer);
    m->setProvider(filter);
    QSignalSpy closed(m, &SuggestModal::closed);
    m->open();
    QTest::keyClicks(m->input(), "zzz");
    EXPECT_TRUE(m->suggestions().isEmpty());
    QTest::keyClick(m->input(), Qt::Key_Return); // nothing to choose: stays open
    EXPECT_TRUE(m->isOpen());
    QTest::keyClick(m->input(), Qt::Key_Escape);
    EXPECT_EQ(closed.count(), 1);

    auto* m2 = new SuggestModal(win.layer);
    m2->setProvider(filter);
    QSignalSpy closed2(m2, &SuggestModal::closed);
    m2->open();
    QApplication::processEvents();
    QTest::mouseClick(m2, Qt::LeftButton, Qt::NoModifier, QPoint(20, 650)); // outside the prompt
    EXPECT_EQ(closed2.count(), 1);
}

TEST(SuggestModal, MouseHoverSelectsAndClickChooses)
{
    Window win;
    auto* m = new SuggestModal(win.layer);
    m->setProvider(filter);
    QSignalSpy chosen(m, &SuggestModal::chosen);
    m->open();
    QApplication::processEvents();
    auto* list = m->findChild<SuggestionList*>();
    ASSERT_NE(list, nullptr);
    const QRect r2 = list->rowRect(2);
    QTest::mouseMove(list->viewport(), r2.center());
    QApplication::processEvents();
    EXPECT_EQ(m->selectedIndex(), 2);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, r2.center());
    ASSERT_EQ(chosen.count(), 1);
    EXPECT_EQ(chosen.at(0).at(0).value<Suggestion>().title, "Lists");
}

TEST(SuggestModal, Screenshot)
{
    const QByteArray out = qgetenv("OC_SHOT_DIR");
    if (out.isEmpty())
        GTEST_SKIP() << "set OC_SHOT_DIR to write a screenshot";
    Window win;
    auto* m = new SuggestModal(win.layer);
    m->setPlaceholder("Find or create a note...");
    m->setInstructions({{"↑↓", "to navigate"}, {"↵", "to open"}, {"ctrl ↵", "to open in new tab"}, {"shift ↵", "to create"}, {"esc", "to dismiss"}});
    m->setProvider([](const QString& q) {
        QList<Suggestion> l = filter(q);
        if (!l.isEmpty())
            l[0].hotkeys = QStringList{"Ctrl", "O"};
        if (l.size() > 1)
            l[1].auxIcon = QStringLiteral("forward");
        return l;
    });
    m->open();
    QTest::keyClicks(m->input(), "o");
    QApplication::processEvents();
    win.w.grab().save(QString::fromLocal8Bit(out) + "/suggest_modal.png");
}
