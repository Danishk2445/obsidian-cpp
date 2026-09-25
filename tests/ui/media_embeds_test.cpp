// PDF / audio / video embeds in Live Preview, Reading view and hover previews: sizes ("page",
// "#height=", audio 42 px, video natural size after the probe), "#page=" subpaths, hosted widgets
// created near the viewport, kept across reveal / mode switches, pooled and destroyed off-screen
// (unless playing), the "Edit this block" and "Open" actions, static rendering (nested / print),
// and the image view (fit, zoom, pan, live reload).
#include "media_test_support.h"

#include "editor/live_preview_decorator.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/views/media/image_view.h"
#include "ui/views/media/media_embeds.h"
#include "ui/views/media/media_player.h"
#include "ui/views/media/pdf_view.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/widgets/icon_button.h"

#include <QPdfWriter>
#include <QScrollBar>

using namespace oc;
using namespace oc::media;
using namespace oc::test;

namespace {

const QByteArray kEmbeds = "# Embeds\n"
                           "\n"
                           "Intro paragraph.\n"
                           "\n"
                           "![[doc.pdf]]\n"
                           "\n"
                           "![[doc.pdf#page=2]]\n"
                           "\n"
                           "![[one.pdf#height=300]]\n"
                           "\n"
                           "![[a.wav]]\n"
                           "\n"
                           "![[v.mp4]]\n"
                           "\n"
                           "![](doc.pdf#page=3)\n"
                           "\n"
                           "![[missing.pdf]]\n"
                           "\n"
                           "End.\n";

// Distinct embeds (different subpaths), or all identical.
QByteArray longNote(int sections, bool identical = false)
{
    QByteArray s = "# Long\n\n";
    for (int i = 0; i < sections; ++i)
        s += "Paragraph " + QByteArray::number(i) + " with some words.\n\n![[one.pdf#height="
             + QByteArray::number(identical ? 200 : 200 + i) + "]]\n\n";
    return s;
}

class MediaEmbedTest : public MediaAppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> f{{"doc.pdf", makePdf(3)},
                                            {"one.pdf", makePdf(1)},
                                            {"media/a.wav", makeWav(1.5)},
                                            {"img.png", makePng(QSize(1600, 900), QColor(40, 90, 160))},
                                            {"small.png", makePng(QSize(200, 120), QColor(160, 60, 40))},
                                            {"Embeds.md", kEmbeds},
                                            {"Long.md", longNote(40)},
                                            {"Same.md", longNote(40, true)}};
        if (hasVideo())
            f.append({"media/v.mp4", videoBytes()});
        return f;
    }
    void SetUp() override
    {
        MediaAppFixture::SetUp();
        MediaProbe::instance().forget(root + "/media/v.mp4");
    }
    static void pump(EditorView* e)
    {
        for (int i = 0; i < 3; ++i) {
            e->viewport()->repaint();
            QApplication::processEvents();
            if (HostedEmbeds* h = HostedEmbeds::of(e))
                h->sync();
        }
    }
    template <typename W> static QList<W*> live(EditorView* e)
    {
        QList<W*> out;
        for (QWidget* w : HostedEmbeds::of(e)->liveWidgets())
            if (auto* t = qobject_cast<W*>(w))
                out << t;
        return out;
    }
    static PdfEmbedWidget* pdfWithSubpath(EditorView* e, const QString& subpath)
    {
        for (PdfEmbedWidget* w : live<PdfEmbedWidget>(e))
            if (w->request().subpath == subpath)
                return w;
        return nullptr;
    }
    MarkdownView* openEmbeds(EditorView::Mode mode = EditorView::Mode::LivePreview)
    {
        MarkdownView* mv = open("Embeds.md", mode);
        if (mv) {
            settle();
            pump(mv->editor());
        }
        return mv;
    }
    double contentWidth(EditorView* e) const { return e->contentWidth(); }
};

} // namespace

TEST_F(MediaEmbedTest, RequestSubpathParams)
{
    EmbedRequest r;
    r.subpath = "#page=3&height=400";
    EXPECT_TRUE(r.hasParam("page"));
    EXPECT_EQ(r.intParam("page"), 3);
    EXPECT_EQ(r.intParam("height"), 400);
    EXPECT_EQ(r.intParam("zoom", -7), -7);
    r.subpath = "#height=250px"; // parseInt
    EXPECT_EQ(r.intParam("height"), 250);
    r.subpath = "#page=x";
    EXPECT_EQ(r.intParam("page", 1), 1);
    const EmbedRequest a = EmbedRequest::fromLink("/v/doc.PDF", "doc.PDF", "#page=2");
    EXPECT_EQ(a.extension, "pdf");
    EXPECT_EQ(a.linktext, "doc.PDF#page=2");
    EXPECT_NE(a.key(), EmbedRequest::fromLink("/v/doc.PDF", "doc.PDF", "#page=3").key());
}

TEST_F(MediaEmbedTest, LivePreviewHostsSizedWidgets)
{
    MarkdownView* mv = openEmbeds();
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    const double cw = contentWidth(e);
    // the first PDF embed: 100% wide, "page" high (page at the embed width + toolbar + border)
    PdfEmbedWidget* first = pdfWithSubpath(e, QString());
    ASSERT_NE(first, nullptr);
    const QSizeF pts(595, 842);
    const double pageH = std::floor((cw - 2 - 12) * pts.height() / pts.width());
    EXPECT_NEAR(first->width(), cw, 1);
    EXPECT_NEAR(first->height(), pageH + PdfViewer::kToolbarHeight + 2, 1);
    EXPECT_NEAR(first->x(), e->contentLeft(), 1);
    EXPECT_EQ(first->viewer()->mode(), PdfViewer::Mode::Embed);
    EXPECT_EQ(first->viewer()->zoom(), "page-width");
    // the embedded page fills the width inside the border (and the scrollbar of a 3-page PDF)
    EXPECT_NEAR(first->viewer()->pages()->pageRect(0).width(), cw - 2 - 12, 2);
    // "#page=2" opens the viewer there
    PdfEmbedWidget* second = pdfWithSubpath(e, "#page=2");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->viewer()->currentPage(), 1);
    // scroll to the rest
    e->setScrollTop(e->documentHeight());
    pump(e);
    settle();
    pump(e);
    PdfEmbedWidget* fixed = pdfWithSubpath(e, "#height=300");
    ASSERT_NE(fixed, nullptr);
    EXPECT_EQ(fixed->height(), 300 + PdfViewer::kToolbarHeight + 2);
    // "![](doc.pdf#page=3)" works too
    PdfEmbedWidget* md = pdfWithSubpath(e, "#page=3");
    ASSERT_NE(md, nullptr);
    EXPECT_EQ(md->viewer()->currentPage(), 2);
    // audio: 100% × 42
    QList<MediaPlayerWidget*> players = live<MediaPlayerWidget>(e);
    MediaPlayerWidget* audio = nullptr;
    MediaPlayerWidget* video = nullptr;
    for (MediaPlayerWidget* p : players)
        (p->controller()->isVideo() ? video : audio) = p;
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->height(), 42);
    EXPECT_NEAR(audio->width(), cw, 1);
    audio->controller()->setMuted(true);
    if (hasVideo()) {
        ASSERT_NE(video, nullptr);
        video->controller()->setMuted(true);
        // the probe gives the natural size; the embed re-lays out to it (max-width 100%)
        ASSERT_TRUE(QTest::qWaitFor([&] {
            pump(e);
            return video->size() == QSize(320, 180);
        }, 10000)) << video->size().width() << "x" << video->size().height();
        EXPECT_TRUE(MediaProbe::instance().info(root + "/media/v.mp4", false).hasVideo);
    }
    // the unresolved embed stays a link: no widget for it
    for (PdfEmbedWidget* w : live<PdfEmbedWidget>(e))
        EXPECT_FALSE(w->request().absPath.endsWith("missing.pdf"));
}

TEST_F(MediaEmbedTest, RevealAndModesKeepWidgets)
{
    MarkdownView* mv = openEmbeds();
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    HostedEmbeds* hosted = HostedEmbeds::of(e);
    PdfEmbedWidget* w = pdfWithSubpath(e, QString());
    ASSERT_NE(w, nullptr);
    const QList<QWidget*> before = hosted->liveWidgets();
    // Live Preview: the edit action is offered
    ASSERT_NE(w->editButton(), nullptr);
    EXPECT_TRUE(w->editButton()->isVisible());
    // the cursor inside the embed reveals its markdown: the widget goes (pooled)
    const Text& t = e->document()->text();
    const qsizetype at = t.toString().indexOf("![[doc.pdf]]");
    e->setCursorPosition(at + 3, false);
    pump(e);
    EXPECT_FALSE(hosted->liveWidgets().contains(w));
    EXPECT_TRUE(w->isHidden());
    // leaving it brings the same widget back
    e->setCursorPosition(0, false);
    pump(e);
    EXPECT_TRUE(hosted->liveWidgets().contains(w));
    EXPECT_FALSE(w->isHidden());
    for (QWidget* b : before) // the same widgets, not new ones
        EXPECT_TRUE(hosted->liveWidgets().contains(b));
    // Reading view hosts them too (without the edit action); Source mode shows none
    mv->setMode(EditorView::Mode::Reading);
    pump(e);
    EXPECT_TRUE(hosted->liveWidgets().contains(w));
    EXPECT_FALSE(w->editButton()->isVisible());
    mv->setMode(EditorView::Mode::Source);
    pump(e);
    EXPECT_TRUE(hosted->liveWidgets().isEmpty());
    mv->setMode(EditorView::Mode::LivePreview);
    pump(e);
    EXPECT_TRUE(hosted->liveWidgets().contains(w));
    for (QWidget* b : before)
        EXPECT_TRUE(hosted->liveWidgets().contains(b));
    // "Edit this block" selects the embed's markdown
    QTest::mouseClick(w->editButton(), Qt::LeftButton);
    const SelRange sel = e->selection().main();
    EXPECT_EQ(sel.from(), at);
    EXPECT_EQ(sel.to(), at + 12);
}

TEST_F(MediaEmbedTest, WidgetsFollowScrollingAndArePooled)
{
    MarkdownView* mv = open("Long.md");
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    settle();
    pump(e);
    HostedEmbeds* hosted = HostedEmbeds::of(e);
    const int screen = e->viewport()->height();
    auto liveCount = [&] { return int(hosted->liveWidgets().size()); };
    const int initial = liveCount();
    EXPECT_GT(initial, 0);
    // only embeds within a screen of the viewport have widgets
    for (QWidget* w : hosted->liveWidgets()) {
        EXPECT_GT(w->geometry().bottom(), -screen - 2);
        EXPECT_LT(w->geometry().top(), 2 * screen + 2);
    }
    // widgets move with the text (synchronously on scroll)
    QWidget* w0 = hosted->liveWidgets().first();
    const QPoint before = w0->pos();
    e->setScrollTop(e->scrollTop() + 50);
    EXPECT_EQ(w0->pos(), before - QPoint(0, 50));
    // scroll through the note: the number of widgets stays bounded
    int maxLive = 0;
    for (double y = 0; y < e->documentHeight(); y += screen / 2.0) {
        e->setScrollTop(y);
        pump(e);
        maxLive = std::max(maxLive, liveCount());
    }
    EXPECT_LE(maxLive, 3 * screen / 200 + 4);
    EXPECT_GT(hosted->createdCount(), maxLive);
    // those far away are destroyed after the pool delay
    QTest::qWait(HostedEmbeds::kPoolMs + 100);
    pump(e);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(hosted->hosted().size(), hosted->liveWidgets().size());
    EXPECT_LE(e->viewport()->findChildren<PdfEmbedWidget*>(QString(), Qt::FindDirectChildrenOnly).size(), maxLive);
}

TEST_F(MediaEmbedTest, IdenticalEmbedsReusePooledWidgets)
{
    MarkdownView* mv = open("Same.md");
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    settle();
    pump(e);
    HostedEmbeds* hosted = HostedEmbeds::of(e);
    const int screen = e->viewport()->height();
    int maxLive = 0;
    for (double y = 0; y < e->documentHeight(); y += screen / 2.0) {
        e->setScrollTop(y);
        pump(e);
        maxLive = std::max(maxLive, int(hosted->liveWidgets().size()));
    }
    // widgets whose embed left the range serve the identical embeds coming in: few ever created
    EXPECT_LE(hosted->createdCount(), maxLive + 2);
    // and each embed keeps its own widget while in range
    const QList<QWidget*> liveNow = hosted->liveWidgets();
    const QSet<QWidget*> distinct(liveNow.cbegin(), liveNow.cend());
    EXPECT_EQ(distinct.size(), liveNow.size());
}

TEST_F(MediaEmbedTest, PlayingMediaSurvivesScrollingAway)
{
    MarkdownView* mv = openEmbeds();
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    e->setScrollTop(e->documentHeight());
    pump(e);
    QList<MediaPlayerWidget*> players = live<MediaPlayerWidget>(e);
    MediaPlayerWidget* audio = nullptr;
    for (MediaPlayerWidget* p : players)
        if (!p->controller()->isVideo())
            audio = p;
    ASSERT_NE(audio, nullptr);
    QPointer<MediaPlayerWidget> guard = audio;
    audio->controller()->setMuted(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return audio->controller()->state() == MediaController::State::Ready; }, 10000));
    audio->controller()->play();
    ASSERT_TRUE(audio->property(HostedEmbeds::kBusyProperty).toBool());
    // scroll far away: hidden but alive while playing
    e->setScrollTop(0);
    mv->setMode(EditorView::Mode::Source); // no embeds at all
    pump(e);
    QTest::qWait(HostedEmbeds::kPoolMs + 100);
    pump(e);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_FALSE(guard.isNull());
    EXPECT_TRUE(guard->isHidden());
    // paused: it goes
    guard->controller()->pause();
    QTest::qWait(HostedEmbeds::kPoolMs + 100);
    pump(e);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(guard.isNull());
}

TEST_F(MediaEmbedTest, OpenFromEmbedAndHoverPreview)
{
    MarkdownView* mv = openEmbeds();
    ASSERT_NE(mv, nullptr);
    EditorView* e = mv->editor();
    PdfEmbedWidget* second = pdfWithSubpath(e, "#page=2");
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second->openButton(), nullptr);
    // the "Open" menu's "Open in new tab": the PDF view at the embed's page
    Workspace* ws = window->workspace();
    EmbedRegistry::instance().open(second, second->request(), "tab");
    auto* pv = qobject_cast<PdfView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(pv, nullptr);
    EXPECT_EQ(pv->viewer()->currentPage(), 1);
    EXPECT_NE(ws->activeRootLeaf(), mv->leaf());

    // hovering a PDF link previews the PDF (450×400, no toolbar)
    PagePreview* pp = PagePreview::of(window);
    pp->setDelays(0, 300);
    HoverPopover* shown = nullptr;
    QObject::connect(pp, &PagePreview::popoverShown, [&](HoverPopover* p) { shown = p; });
    pp->onLinkHover("preview", "doc.pdf#page=3", "Embeds.md", QRect(window->mapToGlobal(QPoint(200, 200)), QSize(40, 16)), mv->editor());
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown != nullptr; }, 3000));
    auto* embed = qobject_cast<PdfEmbedWidget*>(shown->embed());
    ASSERT_NE(embed, nullptr);
    EXPECT_EQ(shown->size(), QSize(452, 402));
    EXPECT_EQ(embed->viewer()->mode(), PdfViewer::Mode::Popover);
    EXPECT_FALSE(embed->viewer()->toolbar()->isVisible());
    EXPECT_EQ(embed->viewer()->currentPage(), 2);
    pp->closeAll();
    // audio: the control bar, popover wide
    shown = nullptr;
    pp->onLinkHover("preview", "a.wav", "Embeds.md", QRect(window->mapToGlobal(QPoint(200, 260)), QSize(40, 16)), mv->editor());
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown != nullptr; }, 3000));
    ASSERT_NE(qobject_cast<MediaPlayerWidget*>(shown->embed()), nullptr);
    EXPECT_EQ(shown->size(), QSize(452, 44));
    pp->closeAll();
    // a note preview renders its embeds too (resolved from the previewed note)
    shown = nullptr;
    pp->onLinkHover("preview", "Embeds", "Long.md", QRect(window->mapToGlobal(QPoint(200, 300)), QSize(40, 16)), mv->editor());
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown != nullptr; }, 3000));
    ASSERT_NE(shown->editor(), nullptr);
    ASSERT_TRUE(shown->editor()->livePreview()->resolveAttachment);
    EXPECT_EQ(shown->editor()->livePreview()->resolveAttachment("a.wav"), root + "/media/a.wav");
    pp->closeAll();
}

TEST_F(MediaEmbedTest, StaticRenderingAndPrint)
{
    // embeds painted without hosting (callout bodies, table cells, PDF export)
    for (const QString& rel : {QStringLiteral("doc.pdf"), QStringLiteral("media/a.wav"), QStringLiteral("media/v.mp4")}) {
        if (rel.endsWith("mp4") && !hasVideo())
            continue;
        const QString abs = root + "/" + rel;
        auto w = makeEmbedWidget(abs, QFileInfo(rel).fileName(), QStringLiteral("#page=2"), -1, -1, 600);
        ASSERT_TRUE(w) << rel.toStdString();
        LineMetrics lm;
        const QSizeF s = w->size(lm);
        EXPECT_EQ(s.width(), rel.endsWith("mp4") ? 300 : 600);
        QImage img(s.toSize(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        w->paint(&p, QRectF(QPointF(0, 0), s), lm);
        p.end();
        EXPECT_NE(img.pixel(int(s.width() / 2), int(s.height() - 5)), 0u) << rel.toStdString();
    }
    // not a registered type: no embed widget (images stay ImageWidget)
    EXPECT_FALSE(makeEmbedWidget(root + "/img.png", "img.png", {}, -1, -1, 600));
    // print: only the page, rendered synchronously
    QByteArray pdf;
    QBuffer buf(&pdf);
    buf.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buf);
        QPainter p(&writer);
        auto w = makeEmbedWidget(root + "/doc.pdf", "doc.pdf", {}, -1, -1, 500);
        ASSERT_TRUE(w);
        LineMetrics lm;
        w->paint(&p, QRectF(QPointF(20, 20), w->size(lm)), lm);
    }
    EXPECT_GT(pdf.size(), 1000);
}

TEST_F(MediaEmbedTest, ImageViewFitsZoomsAndReloads)
{
    Workspace* ws = window->workspace();
    ws->openFile(vault->file("img.png"));
    auto* iv = qobject_cast<ImageView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(iv, nullptr);
    EXPECT_EQ(iv->viewType(), "image");
    EXPECT_EQ(iv->icon(), "image");
    ImageCanvas* c = iv->canvas();
    QApplication::processEvents();
    ASSERT_TRUE(c->isValid());
    EXPECT_EQ(c->naturalSize(), QSize(1600, 900));
    // max-width: 100% inside the 12 px padding, centred, at the top
    QRectF r = c->imageRect();
    EXPECT_NEAR(r.width(), c->viewport()->width() - 24, 1);
    EXPECT_NEAR(r.height(), r.width() * 900 / 1600, 1);
    EXPECT_NEAR(r.top(), 12, 1);
    // double-click zooms ×2.5 at the pointer, again back to fit
    const QPoint at = r.center().toPoint();
    QTest::mouseDClick(c->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    EXPECT_DOUBLE_EQ(c->zoom(), ImageCanvas::kDoubleClickZoom);
    EXPECT_NEAR(c->imageRect().width(), r.width() * 2.5, 2);
    // the point under the pointer stays put
    const QRectF z = c->imageRect();
    EXPECT_NEAR((at.x() - z.left()) / z.width(), (at.x() - r.left()) / r.width(), 0.02);
    // dragging pans
    const int h0 = c->horizontalScrollBar()->value();
    QTest::mousePress(c->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    QMouseEvent move(QEvent::MouseMove, QPointF(at - QPoint(60, 0)), c->viewport()->mapToGlobal(QPointF(at - QPoint(60, 0))), Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(c->viewport(), &move);
    QTest::mouseRelease(c->viewport(), Qt::LeftButton, Qt::NoModifier, at - QPoint(60, 0));
    EXPECT_EQ(c->horizontalScrollBar()->value(), h0 + 60);
    QTest::mouseDClick(c->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    EXPECT_DOUBLE_EQ(c->zoom(), 1.0);
    // Ctrl+wheel zooms (clamped to 1…10)
    QWheelEvent wheel(QPointF(at), c->viewport()->mapToGlobal(QPointF(at)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(c->viewport(), &wheel);
    EXPECT_NEAR(c->zoom(), 1 + 100.0 / 150.0, 0.01);
    c->setZoom(50);
    EXPECT_DOUBLE_EQ(c->zoom(), ImageCanvas::kMaxZoom);
    c->setZoom(0.2);
    EXPECT_DOUBLE_EQ(c->zoom(), 1.0);

    // a small image keeps its natural size, centred horizontally
    ws->openFile(vault->file("small.png"));
    ASSERT_EQ(iv, qobject_cast<ImageView*>(ws->activeRootLeaf()->view())); // the same view navigates
    QApplication::processEvents();
    r = c->imageRect();
    EXPECT_EQ(r.size(), QSizeF(200, 120));
    EXPECT_NEAR(r.center().x(), c->viewport()->width() / 2.0, 1);
    // live reload when the file changes on disk
    QTest::qWait(1100);
    ASSERT_TRUE(writeFile(root + "/small.png", makePng(QSize(300, 150), Qt::darkGreen)));
    ASSERT_TRUE(QTest::qWaitFor([&] { return c->naturalSize() == QSize(300, 150); }, 15000));
}
