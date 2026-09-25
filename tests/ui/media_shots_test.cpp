// Screenshots of the media views and embeds in both themes (skipped unless OC_SHOT_DIR is set):
// media-pdf-*.png (view with the thumbnails sidebar and the find bar), media-image-*.png,
// media-audio-*.png, media-video-*.png, media-embeds-lp-*.png, media-embeds-reading-*.png,
// media-popover-*.png.
#include "media_test_support.h"

#include "ui/views/markdown/page_preview.h"
#include "ui/views/media/media_player.h"
#include "ui/views/media/media_view.h"
#include "ui/views/media/pdf_view.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/widgets/search_input.h"

using namespace oc;
using namespace oc::media;
using namespace oc::test;

namespace {

const QByteArray kNote = "# Media embeds\n"
                         "\n"
                         "A PDF at its second page, 360 px high:\n"
                         "\n"
                         "![[doc.pdf#page=2&height=360]]\n"
                         "\n"
                         "An audio clip:\n"
                         "\n"
                         "![[a.wav]]\n"
                         "\n"
                         "A video:\n"
                         "\n"
                         "![[v.mp4]]\n"
                         "\n"
                         "The end.\n";

class MediaShots : public MediaAppFixture, public ::testing::WithParamInterface<bool> {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> f{{"doc.pdf", makePdf(4)},
                                            {"media/a.wav", makeWav(4.0)},
                                            {"photo.png", makePng(QSize(900, 600), QColor(60, 110, 170))},
                                            {"Media embeds.md", kNote}};
        if (hasVideo())
            f.append({"media/v.mp4", videoBytes()});
        return f;
    }
    void SetUp() override
    {
        if (shotDir().isEmpty())
            GTEST_SKIP() << "set OC_SHOT_DIR to write screenshots";
        setDarkTheme(GetParam());
        MediaAppFixture::SetUp();
        window->resize(1400, 850);
        QApplication::processEvents();
    }
    void TearDown() override
    {
        MediaAppFixture::TearDown();
        setDarkTheme(true);
    }
    QString suffix() const { return GetParam() ? "dark" : "light"; }
    View* openFile(const QString& path)
    {
        window->workspace()->openFile(vault->file(path));
        View* v = window->workspace()->activeRootLeaf()->view();
        QApplication::processEvents();
        return v;
    }
    void settleAndPump(EditorView* e = nullptr)
    {
        for (int i = 0; i < 4; ++i) {
            settle();
            if (e) {
                e->viewport()->repaint();
                QApplication::processEvents();
                HostedEmbeds::of(e)->sync();
            }
        }
    }
};

} // namespace

TEST_P(MediaShots, PdfView)
{
    auto* v = qobject_cast<PdfView*>(openFile("doc.pdf"));
    ASSERT_NE(v, nullptr);
    v->viewer()->setSidebarView(PdfViewer::SidebarView::Thumbnails);
    settle();
    v->viewer()->repaint();
    settle();
    EXPECT_TRUE(shot("media-pdf-" + suffix() + ".png"));
    v->viewer()->showSearch();
    v->viewer()->findInput()->setText("needle");
    QTest::keyClick(v->viewer()->findInput(), Qt::Key_Return);
    QTest::qWaitFor([&] { return !v->viewer()->search()->isPending(); }, 5000);
    settle();
    EXPECT_TRUE(shot("media-pdf-find-" + suffix() + ".png"));
}

TEST_P(MediaShots, ImageAudioVideoViews)
{
    ASSERT_NE(openFile("photo.png"), nullptr);
    settle();
    EXPECT_TRUE(shot("media-image-" + suffix() + ".png"));
    auto* av = qobject_cast<MediaFileView*>(openFile("media/a.wav"));
    ASSERT_NE(av, nullptr);
    av->player()->controller()->setMuted(true);
    settle();
    EXPECT_TRUE(shot("media-audio-" + suffix() + ".png"));
    if (!hasVideo())
        return;
    auto* vv = qobject_cast<MediaFileView*>(openFile("media/v.mp4"));
    ASSERT_NE(vv, nullptr);
    vv->player()->controller()->setMuted(true);
    settle();
    // the first frame, paused at 0.5 s
    vv->player()->controller()->play();
    QTest::qWaitFor([&] { return vv->player()->controller()->position() > 500; }, 4000);
    vv->player()->controller()->pause();
    settle();
    EXPECT_TRUE(shot("media-video-" + suffix() + ".png"));
}

TEST_P(MediaShots, EmbedsInLivePreviewAndReading)
{
    MarkdownView* mv = open("Media embeds.md", EditorView::Mode::LivePreview);
    ASSERT_NE(mv, nullptr);
    mv->editor()->setCursorPosition(mv->document()->text().length(), false);
    settleAndPump(mv->editor());
    for (QWidget* w : HostedEmbeds::of(mv->editor())->liveWidgets())
        if (auto* p = qobject_cast<MediaPlayerWidget*>(w))
            p->controller()->setMuted(true);
    mv->editor()->setScrollTop(0);
    settleAndPump(mv->editor());
    EXPECT_TRUE(shot("media-embeds-lp-" + suffix() + ".png"));
    mv->editor()->setScrollTop(420);
    settleAndPump(mv->editor());
    EXPECT_TRUE(shot("media-embeds-lp-scrolled-" + suffix() + ".png"));
    mv->setMode(EditorView::Mode::Reading);
    mv->editor()->setScrollTop(0);
    settleAndPump(mv->editor());
    EXPECT_TRUE(shot("media-embeds-reading-" + suffix() + ".png"));
    // a hover preview of the PDF
    PagePreview* pp = PagePreview::of(window);
    pp->setDelays(0, 300);
    HoverPopover* shown = nullptr;
    QObject::connect(pp, &PagePreview::popoverShown, [&](HoverPopover* p) { shown = p; });
    pp->onLinkHover("preview", "doc.pdf#page=1", "Media embeds.md", QRect(window->mapToGlobal(QPoint(420, 160)), QSize(60, 18)), mv->editor());
    ASSERT_TRUE(QTest::qWaitFor([&] { return shown != nullptr; }, 3000));
    settle();
    shown->repaint();
    settle();
    EXPECT_TRUE(shot("media-popover-" + suffix() + ".png"));
    pp->closeAll();
}

INSTANTIATE_TEST_SUITE_P(Themes, MediaShots, ::testing::Values(true, false), [](const auto& info) { return info.param ? "Dark" : "Light"; });
