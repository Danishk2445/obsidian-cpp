// Modal / ConfirmModal (Obsidian's .modal): layout, buttons and results, keyboard (Enter, Escape,
// Tab), background clicks, the checkbox, exec() and the async helper.
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/widgets/icon_button.h"

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>

using namespace oc;

namespace {
struct Window {
    Window()
    {
        themes.setMode(ThemeManager::Mode::Dark);
        themes.apply();
        w.resize(1000, 700);
        layer = new OverlayLayer(&w);
        w.show();
        w.activateWindow();
        (void)QTest::qWaitForWindowActive(&w, 2000);
        QApplication::processEvents();
    }
    ThemeManager themes;
    QWidget w;
    OverlayLayer* layer;
};
} // namespace

TEST(Modal, LayoutFollowsObsidian)
{
    Window win;
    auto* m = new Modal(win.layer);
    m->setTitle("Delete file");
    m->addParagraph("Are you sure you want to delete “Note.md”?");
    m->addParagraph("It will be moved to your system trash.");
    m->addParagraph("There are currently 3 links pointing to this file.", true);
    m->setCheckbox("Don't ask again");
    m->addButton("Cancel");
    m->addButton("Delete", Modal::ButtonStyle::Warning);
    m->open();
    QApplication::processEvents();
    EXPECT_TRUE(m->isOpen());
    EXPECT_TRUE(win.layer->isVisible());
    const QRect r = m->dialogGeometry();
    EXPECT_EQ(r.width(), 560); // --dialog-width
    EXPECT_EQ(r.left(), (1000 - 560) / 2);
    EXPECT_NEAR(r.center().y(), 350, 1);
    EXPECT_EQ(m->paragraphs().size(), 3);
    // buttons right-aligned, in order, 30 px high; the checkbox on the left
    QPushButton* cancel = m->button(0);
    QPushButton* del = m->button(1);
    EXPECT_EQ(cancel->height(), 30);
    EXPECT_LT(cancel->mapTo(&win.w, QPoint()).x(), del->mapTo(&win.w, QPoint()).x());
    EXPECT_EQ(del->mapTo(&win.w, QPoint(del->width(), 0)).x(), r.right() + 1 - 16);
    EXPECT_EQ(del->mapTo(&win.w, QPoint()).x() - cancel->mapTo(&win.w, QPoint(cancel->width(), 0)).x(), 8);
    EXPECT_LT(m->checkbox()->mapTo(&win.w, QPoint()).x(), cancel->mapTo(&win.w, QPoint()).x());
    // close button: top right
    EXPECT_EQ(m->closeButton()->mapTo(&win.w, QPoint()).y(), r.top() + 6);
    EXPECT_EQ(m->closeButton()->mapTo(&win.w, QPoint(m->closeButton()->width(), 0)).x(), r.right() + 1 - 12);
    // the CTA has the initial focus
    EXPECT_EQ(m->defaultButton(), 1);
    EXPECT_TRUE(del->hasFocus());
    m->close();
}

TEST(Modal, ButtonsKeysAndBackground)
{
    Window win;
    {
        auto* m = new Modal(win.layer);
        m->addButton("A");
        m->addButton("B", Modal::ButtonStyle::Cta);
        QSignalSpy closed(m, &Modal::closed);
        QSignalSpy clicked(m, &Modal::buttonClicked);
        m->open();
        m->button(0)->click();
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), 0);
        EXPECT_EQ(clicked.at(0).at(0).toInt(), 0);
        EXPECT_FALSE(m->isOpen());
    }
    {
        auto* m = new Modal(win.layer);
        m->addButton("A");
        m->addButton("B", Modal::ButtonStyle::Cta);
        QSignalSpy closed(m, &Modal::closed);
        m->open();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return); // the CTA
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), 1);
    }
    {
        auto* m = new Modal(win.layer);
        m->addButton("A");
        m->addButton("B", Modal::ButtonStyle::Cta);
        QSignalSpy closed(m, &Modal::closed);
        m->open();
        // Tab moves the focus to the other button (and stays inside the modal); Enter clicks it
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        EXPECT_EQ(QApplication::focusWidget(), m->button(0));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        EXPECT_EQ(QApplication::focusWidget(), m->button(1));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Backtab);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Enter);
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), 0);
    }
    {
        auto* m = new Modal(win.layer);
        m->addButton("OK", Modal::ButtonStyle::Cta);
        QSignalSpy closed(m, &Modal::closed);
        m->open();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), -1);
    }
    {
        auto* m = new Modal(win.layer);
        m->addButton("OK");
        QSignalSpy closed(m, &Modal::closed);
        m->open();
        QTest::mouseClick(m, Qt::LeftButton, {}, QPoint(10, 10)); // the dimmed background
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), -1);
    }
    {
        auto* m = new Modal(win.layer);
        m->addButton("OK");
        QSignalSpy closed(m, &Modal::closed);
        m->open();
        m->closeButton()->click();
        ASSERT_EQ(closed.count(), 1);
        EXPECT_EQ(closed.at(0).at(0).toInt(), -1);
    }
    {
        // a button that doesn't close
        auto* m = new Modal(win.layer);
        const int keep = m->addButton("Apply", Modal::ButtonStyle::Normal, false);
        QSignalSpy clicked(m, &Modal::buttonClicked);
        m->open();
        m->button(keep)->click();
        EXPECT_EQ(clicked.count(), 1);
        EXPECT_TRUE(m->isOpen());
        m->close();
    }
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(win.layer->findChildren<Modal*>().isEmpty()); // closed modals delete themselves
}

TEST(Modal, ContentWidgetsAndExec)
{
    Window win;
    auto* m = new Modal(win.layer);
    m->setTitle("Rename");
    auto* input = new QLineEdit;
    m->addContentWidget(input);
    m->addButton("Cancel");
    m->addButton("Save", Modal::ButtonStyle::Cta);
    m->setDefaultButton(1);
    QPointer<Modal> guard(m);
    QTimer::singleShot(0, [&] {
        input->setText("x");
        QTest::keyClick(input, Qt::Key_Return); // Enter in a single-line input: the default button
    });
    EXPECT_EQ(m->exec(), 1);
    EXPECT_EQ(input->text(), "x");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(guard.isNull());
}

TEST(Modal, ConfirmModalHelpers)
{
    Window win;
    int button = -2;
    bool checked = false;
    Modal* m = ConfirmModal::ask(win.layer, "Update links", "First paragraph.\n\nSecond paragraph.",
                                 {{"Always update"}, {"Just once"}, {"Do not update"}}, "Remember", [&](int b, bool c) {
                                     button = b;
                                     checked = c;
                                 });
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->paragraphs(), (QStringList{"First paragraph.", "Second paragraph."}));
    EXPECT_EQ(m->defaultButton(), -1); // no CTA
    m->checkbox()->click();
    m->button(2)->click();
    EXPECT_EQ(button, 2);
    EXPECT_TRUE(checked);

    QTimer::singleShot(0, [&] {
        for (Modal* x : win.layer->findChildren<Modal*>())
            if (x->isOpen())
                x->button(0)->click();
    });
    bool c2 = true;
    EXPECT_EQ(ConfirmModal::exec(win.layer, "T", "Text", {{"Yes", Modal::ButtonStyle::Cta}, {"No"}}, "Box", &c2), 0);
    EXPECT_FALSE(c2);
}
