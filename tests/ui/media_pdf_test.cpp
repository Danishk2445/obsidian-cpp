// The PDF view: registration, paging and "of N", #page subpaths through openLinkText, PDF.js zoom
// steps and presets, view state round trip, find (counts, match case, whole words, wrap), text
// selection and copy, spreads, thumbnails, "Adapt to theme", reload on modify, arrow paging.
#include "media_test_support.h"

#include "ui/views/media/pdf_file.h"
#include "ui/views/media/pdf_view.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"

#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPdfSelection>
#include <QScrollBar>

using namespace oc;
using namespace oc::media;
using namespace oc::test;

namespace {

class PdfViewTest : public MediaAppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        return {{"doc.pdf", makePdf(3)},
                {"wide.pdf", makePdf(2, true)},
                {"Note.md", "See [[doc.pdf#page=2]] and ![[doc.pdf]].\n"}};
    }
    PdfView* openPdf(const QString& path = "doc.pdf")
    {
        Workspace* ws = window->workspace();
        ws->openFile(vault->file(path), Workspace::NewLeaf::No);
        auto* v = qobject_cast<PdfView*>(ws->activeRootLeaf()->view());
        if (v)
            QApplication::processEvents();
        return v;
    }
    static bool waitSearch(PdfSearch* s)
    {
        return QTest::qWaitFor([s] { return !s->isPending(); }, 10000);
    }
};

} // namespace

TEST_F(PdfViewTest, RegistrationAndExtensions)
{
    for (const char* t : {"image", "audio", "video", "pdf"})
        EXPECT_TRUE(registry->hasView(t)) << t;
    EXPECT_EQ(registry->typeForExtension("pdf"), "pdf");
    for (const QString& e : imageExtensions())
        EXPECT_EQ(registry->typeForExtension(e), "image") << e.toStdString();
    for (const QString& e : audioExtensions())
        EXPECT_EQ(registry->typeForExtension(e), "audio") << e.toStdString();
    for (const QString& e : videoExtensions())
        EXPECT_EQ(registry->typeForExtension(e), "video") << e.toStdString();
    EXPECT_EQ(registry->typeForExtension("md"), "markdown");
    EXPECT_TRUE(registry->typeForExtension("txt").isEmpty());
    // Obsidian's lists
    EXPECT_EQ(audioExtensions(), QStringList({"mp3", "wav", "m4a", "3gp", "flac", "ogg", "oga", "opus"}));
    EXPECT_EQ(videoExtensions(), QStringList({"mp4", "webm", "ogv", "mov", "mkv"}));
    // embeds: PDF / audio / video have providers; images stay inline images
    EXPECT_TRUE(EmbedRegistry::instance().isExtensionRegistered("pdf"));
    EXPECT_TRUE(EmbedRegistry::instance().isExtensionRegistered("flac"));
    EXPECT_TRUE(EmbedRegistry::instance().isExtensionRegistered("webm"));
    EXPECT_FALSE(EmbedRegistry::instance().isExtensionRegistered("png"));
}

TEST_F(PdfViewTest, OpensWithPageCountAndNavigates)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->viewType(), "pdf");
    EXPECT_EQ(v->icon(), "file-text");
    EXPECT_EQ(v->displayText(), "doc");
    PdfViewer* pv = v->viewer();
    ASSERT_TRUE(pv->file() && pv->file()->isLoaded());
    EXPECT_EQ(pv->file()->pageCount(), 3);
    EXPECT_EQ(pv->pageNumbers()->text(), "of 3");
    EXPECT_EQ(pv->pageInput()->text(), "1");
    EXPECT_EQ(pv->zoom(), "page-width");
    // the page input
    pv->pageInput()->setFocus();
    pv->pageInput()->setText("3");
    QTest::keyClick(pv->pageInput(), Qt::Key_Return);
    EXPECT_EQ(pv->currentPage(), 2);
    EXPECT_EQ(pv->pageInput()->text(), "3");
    EXPECT_EQ(v->state().value("page").toInt(), 3);
    // out of range: stays, shows the current page again
    pv->pageInput()->setFocus();
    pv->pageInput()->setText("9");
    QTest::keyClick(pv->pageInput(), Qt::Key_Return);
    EXPECT_EQ(pv->currentPage(), 2);
    EXPECT_EQ(pv->pageInput()->text(), "3");
    // scrolling updates the current page
    PdfPagesView* pages = pv->pages();
    pages->verticalScrollBar()->setValue(0);
    QApplication::processEvents();
    EXPECT_EQ(pv->currentPage(), 0);
    EXPECT_EQ(pv->pageInput()->text(), "1");
    // arrow right turns pages while fitting the width (Obsidian's PDF view)
    pages->setFocus();
    QTest::keyClick(pages, Qt::Key_Right);
    EXPECT_EQ(pv->currentPage(), 1);
    QTest::keyClick(pages, Qt::Key_Left);
    EXPECT_EQ(pv->currentPage(), 0);
}

TEST_F(PdfViewTest, PageSubpathFromLinks)
{
    Workspace* ws = window->workspace();
    MarkdownView::openLinkText(ws, "doc.pdf#page=2", 0, "Note.md", false);
    auto* v = qobject_cast<PdfView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->viewer()->currentPage(), 1);
    PdfPagesView* pages = v->viewer()->pages();
    // FitBH: the page's top at the top of the viewport, fitting the width
    EXPECT_EQ(v->viewer()->zoom(), "page-width");
    EXPECT_NEAR(pages->scrollOffset().y(), pages->pageRect(1).top() - 4 * pages->scaleFactor(), 2);
    // the eState "page" and a new subpath on the live view
    v->setEphemeralState(QJsonObject{{"subpath", "#page=3"}});
    EXPECT_EQ(v->viewer()->currentPage(), 2);
    v->setEphemeralState(QJsonObject{{"page", 1}});
    EXPECT_EQ(v->viewer()->currentPage(), 0);
    // #page beyond the end clamps; non-page subpaths are ignored
    v->viewer()->applySubpath("#page=99");
    EXPECT_EQ(v->viewer()->currentPage(), 2);
    v->viewer()->applySubpath("#chapter-1");
    EXPECT_EQ(v->viewer()->currentPage(), 2);
    // XYZ destination with a zoom
    v->viewer()->applySubpath("#page=2&offset=0,842,150");
    EXPECT_EQ(v->viewer()->currentPage(), 1);
    EXPECT_EQ(v->viewer()->zoom(), "1.5");
}

TEST_F(PdfViewTest, ZoomStepsLikePdfJs)
{
    EXPECT_DOUBLE_EQ(PdfPagesView::increasedScale(1.0), 1.1);
    EXPECT_DOUBLE_EQ(PdfPagesView::increasedScale(1.1), 1.3);
    EXPECT_DOUBLE_EQ(PdfPagesView::increasedScale(1.3), 1.5);
    EXPECT_DOUBLE_EQ(PdfPagesView::decreasedScale(1.0), 0.9);
    EXPECT_DOUBLE_EQ(PdfPagesView::decreasedScale(0.9), 0.8);
    EXPECT_DOUBLE_EQ(PdfPagesView::increasedScale(9.8), 10.0);
    EXPECT_DOUBLE_EQ(PdfPagesView::decreasedScale(0.1), 0.1);

    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    PdfViewer* pv = v->viewer();
    PdfPagesView* pages = pv->pages();
    // page-width: the page spans the viewport minus PDF.js's 40 px padding (and the scrollbar)
    const double w = pages->pageRect(0).width();
    EXPECT_NEAR(w, pages->maximumViewportSize().width() - 40 - 12, 2);
    pv->setZoom("1");
    EXPECT_NEAR(pages->pageRect(0).width(), 595 * 96.0 / 72.0, 1); // 100% = 96/72 px per point
    QTest::mouseClick(pv->zoomInButton(), Qt::LeftButton);
    EXPECT_EQ(pv->zoom(), "1.1");
    QTest::mouseClick(pv->zoomInButton(), Qt::LeftButton);
    EXPECT_EQ(pv->zoom(), "1.3");
    QTest::mouseClick(pv->zoomOutButton(), Qt::LeftButton);
    EXPECT_EQ(pv->zoom(), "1.1");
    pv->setZoom("10");
    EXPECT_FALSE(pv->zoomInButton()->isEnabled());
    pv->setZoom("page-height");
    EXPECT_NEAR(pages->pageRect(0).height(), pages->maximumViewportSize().height() - 5, 2);
    // Ctrl+wheel zooms around the pointer
    pv->setZoom("1");
    const QPoint at(100, 100);
    QWheelEvent wheel(at, pages->viewport()->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(pages->viewport(), &wheel);
    EXPECT_EQ(pv->zoom(), "1.1");
}

TEST_F(PdfViewTest, StateRoundTrip)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    v->viewer()->setZoom("1.5");
    v->viewer()->pages()->goToPage(2);
    const QJsonObject st = v->state();
    EXPECT_EQ(st.value("file").toString(), "doc.pdf");
    EXPECT_EQ(st.value("page").toInt(), 3);
    EXPECT_EQ(st.value("zoom").toString(), "1.5");
    // a new leaf restoring that state (workspace.json / duplicate)
    WorkspaceLeaf* leaf = window->workspace()->getLeaf(Workspace::NewLeaf::Tab);
    leaf->setViewState(QJsonObject{{"type", "pdf"}, {"state", st}});
    auto* v2 = qobject_cast<PdfView*>(leaf->view());
    ASSERT_NE(v2, nullptr);
    QApplication::processEvents();
    EXPECT_EQ(v2->viewer()->zoom(), "1.5");
    EXPECT_EQ(v2->viewer()->currentPage(), 2);
    EXPECT_EQ(v2->state(), st);
    // the same file is shared between viewers
    EXPECT_EQ(v2->viewer()->file(), v->viewer()->file());
    // navigation history keeps the page
    EXPECT_EQ(v2->ephemeralState().value("page").toInt(), 3);
}

TEST_F(PdfViewTest, FindMatchesCaseAndWholeWords)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    PdfViewer* pv = v->viewer();
    PdfSearch* s = pv->search();
    s->setOptions({});
    pv->showSearch();
    EXPECT_TRUE(pv->isSearchOpen());
    EXPECT_TRUE(pv->findInput()->hasFocus() || !pv->isActiveWindow());
    QTest::keyClicks(pv->findInput(), "needle");
    QTest::keyClick(pv->findInput(), Qt::Key_Return); // runs now (else debounced 250 ms)
    ASSERT_TRUE(waitSearch(s));
    // "Needle 1", "Needle 2", "needle", "needlework", "Needle 3"
    EXPECT_EQ(s->count(), 5);
    EXPECT_EQ(s->current(), 0);
    EXPECT_EQ(pv->findCount()->text(), "1 of 5 matches");
    EXPECT_EQ(s->matches()[1].page, 1);
    QTest::keyClick(pv->findInput(), Qt::Key_Return); // next
    EXPECT_EQ(s->current(), 1);
    EXPECT_EQ(pv->currentPage(), 1); // revealed
    QTest::keyClick(pv->findInput(), Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(s->current(), 0);
    QTest::keyClick(pv->findInput(), Qt::Key_F3, Qt::ShiftModifier); // wraps
    EXPECT_EQ(s->current(), 4);
    EXPECT_EQ(s->matches()[4].page, 2);
    // match case
    PdfSearch::Options o;
    o.caseSensitive = true;
    s->setOptions(o);
    EXPECT_EQ(s->count(), 2); // "needle", "needlework"
    // whole words (case-insensitive)
    o = {};
    o.entireWord = true;
    s->setOptions(o);
    EXPECT_EQ(s->count(), 4);
    // nothing found: the input is tinted, no count
    s->setOptions({});
    pv->findInput()->setText("zebra");
    QTest::keyClick(pv->findInput(), Qt::Key_Return);
    ASSERT_TRUE(waitSearch(s));
    EXPECT_EQ(s->count(), 0);
    EXPECT_TRUE(pv->findInput()->noMatch());
    EXPECT_TRUE(pv->findCount()->text().isEmpty());
    // the search starts at the current page
    pv->pages()->goToPage(2);
    pv->findInput()->setText("Needle");
    QTest::keyClick(pv->findInput(), Qt::Key_Return);
    ASSERT_TRUE(waitSearch(s));
    ASSERT_GE(s->current(), 0);
    EXPECT_EQ(s->matches()[s->current()].page, 2);
    // Escape closes the bar and clears the highlights
    QTest::keyClick(pv->findInput(), Qt::Key_Escape);
    EXPECT_FALSE(pv->isSearchOpen());
    EXPECT_EQ(s->count(), 0);
}

TEST_F(PdfViewTest, TextSelectionAndCopy)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    PdfPagesView* pages = v->viewer()->pages();
    PdfFile* f = v->viewer()->file();
    const PdfFile::PageText& text = f->pageText(0);
    ASSERT_TRUE(text.text.startsWith("Page 1 heading"));
    pages->selectText(0, 0, 4);
    EXPECT_EQ(pages->selectedText(), "Page");
    pages->copySelection();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "Page");
    pages->clearSelection();
    EXPECT_FALSE(pages->hasSelection());

    // drag over "Page 1 heading" with the mouse
    pages->goToPage(0);
    QApplication::processEvents();
    auto glyph = [&](int index) { return f->document()->getSelectionAtIndex(0, index, 1).boundingRectangle(); };
    const QRectF first = glyph(0);
    const QRectF last = glyph(int(text.text.indexOf("heading") + 6)); // its "g"
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(last.isEmpty());
    auto toViewport = [&](QPointF pt) {
        const QRectF r = pages->pageRect(0);
        return (r.topLeft() + pt * pages->scaleFactor() - pages->scrollOffset()).toPoint();
    };
    const QPoint a = toViewport(QPointF(first.left() + 0.5, first.center().y()));
    const QPoint b = toViewport(QPointF(last.right() + 1, last.center().y()));
    QTest::mousePress(pages->viewport(), Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseMove(pages->viewport(), (a + b) / 2);
    QTest::mouseMove(pages->viewport(), b);
    QTest::mouseRelease(pages->viewport(), Qt::LeftButton, Qt::NoModifier, b);
    EXPECT_EQ(pages->selectedText(), "Page 1 heading");
    // Ctrl+C
    QGuiApplication::clipboard()->clear();
    pages->setFocus();
    QTest::keyClick(pages, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "Page 1 heading");
    // double-click selects a word
    const QRectF n = glyph(int(text.text.indexOf("Needle") + 2)); // inside "Needle"
    const QPoint w = toViewport(n.center());
    QTest::mouseDClick(pages->viewport(), Qt::LeftButton, Qt::NoModifier, w);
    EXPECT_EQ(pages->selectedText(), "Needle");
    // select all covers every page
    QTest::keyClick(pages, Qt::Key_A, Qt::ControlModifier);
    EXPECT_TRUE(pages->selectedText().contains("Page 3 heading"));
}

TEST_F(PdfViewTest, SpreadsThumbnailsAndSidebar)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    PdfViewer* pv = v->viewer();
    PdfPagesView* pages = pv->pages();
    pages->setSpreadMode(PdfPagesView::SpreadMode::Odd);
    EXPECT_NEAR(pages->pageRect(0).top(), pages->pageRect(1).top(), 1);
    EXPECT_GT(pages->pageRect(1).left(), pages->pageRect(0).right());
    EXPECT_GT(pages->pageRect(2).top(), pages->pageRect(0).bottom());
    pages->setSpreadMode(PdfPagesView::SpreadMode::Even); // page 1 alone, then 2 + 3
    EXPECT_GT(pages->pageRect(1).top(), pages->pageRect(0).bottom());
    EXPECT_NEAR(pages->pageRect(1).top(), pages->pageRect(2).top(), 1);
    pages->setSpreadMode(PdfPagesView::SpreadMode::None);

    // the sidebar: thumbnails (140 px), pages move right of it
    const int before = pages->geometry().left();
    QTest::mouseClick(pv->sidebarButton(), Qt::LeftButton);
    EXPECT_EQ(pv->sidebarView(), PdfViewer::SidebarView::Thumbnails);
    EXPECT_TRUE(pv->sidebar()->isVisible());
    EXPECT_EQ(pv->sidebar()->width(), 140);
    EXPECT_EQ(pages->geometry().left(), before + 140);
    EXPECT_TRUE(pv->sidebarButton()->isChecked());
    QApplication::processEvents();
    PdfThumbnails* thumbs = pv->thumbnails();
    const QRect t3 = thumbs->thumbnailRect(2);
    ASSERT_FALSE(t3.isNull());
    EXPECT_EQ(t3.width(), PdfThumbnails::kThumbWidth);
    if (t3.bottom() > thumbs->viewport()->height())
        thumbs->verticalScrollBar()->setValue(thumbs->verticalScrollBar()->maximum());
    QTest::mouseClick(thumbs->viewport(), Qt::LeftButton, Qt::NoModifier, thumbs->thumbnailRect(2).center());
    EXPECT_EQ(pv->currentPage(), 2);
    // no outline in this PDF: "Table of contents" is disabled, the toggle closes the sidebar
    EXPECT_FALSE(pv->hasOutline());
    QMenu* menu = pv->sidebarMenu(pv);
    ASSERT_GE(menu->actions().size(), 2);
    EXPECT_TRUE(menu->actions()[0]->isChecked());
    EXPECT_FALSE(menu->actions()[1]->isEnabled());
    delete menu;
    pv->toggleSidebar();
    EXPECT_EQ(pv->sidebarView(), PdfViewer::SidebarView::None);
    EXPECT_FALSE(pv->sidebar()->isVisible());
    // the display menu reflects the zoom preset and spread mode
    menu = pv->displayMenu(pv);
    EXPECT_TRUE(menu->actions()[0]->isChecked()); // Fit width
    delete menu;
}

TEST_F(PdfViewTest, RendersPagesAndAdaptsToTheme)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    PdfPagesView* pages = v->viewer()->pages();
    pages->viewport()->repaint();
    settle();
    const int px = int(std::lround(pages->pageRect(0).width() * pages->devicePixelRatioF()));
    const QImage plain = v->viewer()->file()->cachedImage(0, px, false);
    ASSERT_FALSE(plain.isNull());
    EXPECT_EQ(plain.pixel(5, 5), qRgb(255, 255, 255)); // white paper

    // "Adapt to theme" (dark): white paper becomes --pdf-page-background, black text light
    QImage img(3, 1, QImage::Format_RGB32);
    img.setPixel(0, 0, qRgb(255, 255, 255));
    img.setPixel(1, 0, qRgb(0, 0, 0));
    img.setPixel(2, 0, qRgb(32, 112, 208));
    themePdfImage(img, qRgb(30, 30, 30));
    EXPECT_EQ(img.pixel(0, 0), qRgb(30, 30, 30));
    EXPECT_EQ(img.pixel(1, 0), qRgb(255, 255, 255));
    const QColor blue = QColor(img.pixel(2, 0));
    EXPECT_NEAR(blue.hueF(), QColor(32, 112, 208).hueF(), 0.02); // inverted + hue-rotated back
    EXPECT_GT(blue.lightnessF(), QColor(32, 112, 208).lightnessF());

    setDarkTheme(true);
    v->viewer()->setThemed(true);
    EXPECT_TRUE(v->viewer()->isThemed());
    pages->viewport()->repaint();
    settle();
    const QColor bg = currentTheme()->color("--pdf-page-background");
    const QImage themed = v->viewer()->file()->cachedImage(0, px, true, bg.rgb());
    ASSERT_FALSE(themed.isNull());
    EXPECT_EQ(QColor(themed.pixel(5, 5)).rgb(), bg.rgb());
    v->viewer()->setThemed(false);
}

TEST_F(PdfViewTest, ReloadsWhenModified)
{
    PdfView* v = openPdf();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->viewer()->file()->pageCount(), 3);
    QTest::qWait(1100); // a new mtime
    ASSERT_TRUE(writeFile(root + "/doc.pdf", makePdf(5)));
    ASSERT_TRUE(QTest::qWaitFor([&] { return v->viewer()->file()->pageCount() == 5; }, 15000));
    EXPECT_EQ(v->viewer()->pageNumbers()->text(), "of 5");
}

TEST_F(PdfViewTest, LandscapeFitsAndPasswordlessErrors)
{
    PdfView* v = openPdf("wide.pdf");
    ASSERT_NE(v, nullptr);
    PdfPagesView* pages = v->viewer()->pages();
    EXPECT_GT(pages->pageRect(0).width(), pages->pageRect(0).height());
    // a broken PDF shows the error prompt instead of pages
    ASSERT_TRUE(writeFile(root + "/broken.pdf", "not a pdf"));
    vault->rescan();
    ASSERT_TRUE(QTest::qWaitFor([&] { return vault->file("broken.pdf") != nullptr; }, 10000));
    PdfView* b = openPdf("broken.pdf");
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(b->viewer()->file()->isLoaded());
    EXPECT_FALSE(b->viewer()->pages()->isVisible());
    EXPECT_EQ(b->viewer()->pageNumbers()->text(), "of 0");
    EXPECT_TRUE(b->state().value("page").isUndefined());
}
