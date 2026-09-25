// Audio / video playback: the MediaController state machine (Loading -> Ready -> Playing <-> Paused
// -> Ended -> replay, Error), the Chromium-style control bar (play, time, seek, mute, volume,
// overflow), the video surface (frames, fallback to audio, fullscreen) and the "audio" / "video"
// views. Media is muted; playback timing checks skip when the environment cannot play.
#include "media_test_support.h"

#include "ui/views/media/media_player.h"
#include "ui/views/media/media_view.h"

#include <QAudioOutput>
#include <QMediaPlayer>

using namespace oc;
using namespace oc::media;
using namespace oc::test;

namespace {

using State = MediaController::State;

class MediaPlayerTest : public MediaAppFixture {
protected:
    QList<QPair<QString, QByteArray>> files() const override
    {
        QList<QPair<QString, QByteArray>> f{{"media/a.wav", makeWav(1.5)}, {"media/bad.mp3", "not audio at all"}};
        if (hasVideo()) {
            f.append({"media/v.mp4", videoBytes()});
            f.append({"media/voice.mp4", videoBytes(true)});
        }
        return f;
    }
    QString path(const QString& p) const { return root + "/" + p; }
    static bool waitState(MediaController* c, State s, int ms = 10000)
    {
        return QTest::qWaitFor([&] { return c->state() == s; }, ms);
    }
    // Starts playback; false when this environment doesn't advance the clock (no audio sink...).
    static bool playsHere(MediaController* c)
    {
        c->play();
        return QTest::qWaitFor([&] { return c->position() > 100 || c->state() == State::Ended; }, 6000);
    }
};

} // namespace

TEST(MediaTime, FormatsLikeChromium)
{
    EXPECT_EQ(MediaControlBar::formatTime(0), "0:00");
    EXPECT_EQ(MediaControlBar::formatTime(4500), "0:04");
    EXPECT_EQ(MediaControlBar::formatTime(65000), "1:05");
    EXPECT_EQ(MediaControlBar::formatTime(600000), "10:00");
    EXPECT_EQ(MediaControlBar::formatTime(3723000), "1:02:03");
    EXPECT_EQ(MediaControlBar::formatTime(-5), "0:00");
}

TEST_F(MediaPlayerTest, AudioStateMachine)
{
    MediaController c(false);
    c.setMuted(true);
    EXPECT_EQ(c.state(), State::Empty);
    QList<State> seen;
    QObject::connect(&c, &MediaController::stateChanged, [&](State s) { seen << s; });
    c.setSource(path("media/a.wav"));
    EXPECT_EQ(c.state(), State::Loading);
    EXPECT_FALSE(Busy::idle()); // headless screenshots wait for loading media
    ASSERT_TRUE(waitState(&c, State::Ready));
    EXPECT_TRUE(Busy::idle());
    EXPECT_NEAR(c.duration(), 1500, 60);
    EXPECT_EQ(c.position(), 0);
    EXPECT_FALSE(c.hasVideo());
    // play / pause
    c.play();
    EXPECT_EQ(c.state(), State::Playing);
    c.pause();
    EXPECT_EQ(c.state(), State::Paused);
    // seeking while paused stays paused
    c.seek(1000);
    EXPECT_EQ(c.state(), State::Paused);
    EXPECT_TRUE(QTest::qWaitFor([&] { return std::abs(c.position() - 1000) < 60; }, 3000));
    c.togglePlay();
    EXPECT_EQ(c.state(), State::Playing);
    if (!QTest::qWaitFor([&] { return c.state() == State::Ended; }, 8000))
        GTEST_SKIP() << "media does not play to the end in this environment";
    EXPECT_EQ(c.position(), c.duration());
    // play after the end restarts from the beginning
    c.play();
    EXPECT_EQ(c.state(), State::Playing);
    EXPECT_LT(c.position(), 500);
    c.pause();
    EXPECT_TRUE(seen.contains(State::Ready));
    EXPECT_TRUE(seen.contains(State::Ended));
}

TEST_F(MediaPlayerTest, VolumeMuteRateAndErrors)
{
    MediaController c(false);
    QList<float> volumes;
    QObject::connect(&c, &MediaController::volumeChanged, [&](float v) { volumes << v; });
    c.setMuted(true);
    EXPECT_TRUE(c.isMuted());
    EXPECT_TRUE(c.player()->audioOutput()->isMuted());
    c.setVolume(0.25f);
    EXPECT_FLOAT_EQ(c.volume(), 0.25f);
    EXPECT_FALSE(c.isMuted()); // raising the volume unmutes, like Chromium's slider
    EXPECT_FLOAT_EQ(c.player()->audioOutput()->volume(), 0.25f);
    c.setVolume(2.0f);
    EXPECT_FLOAT_EQ(c.volume(), 1.0f);
    EXPECT_EQ(volumes, QList<float>({0.25f, 1.0f}));
    c.setPlaybackRate(1.5);
    EXPECT_DOUBLE_EQ(c.player()->playbackRate(), 1.5);
    // unplayable data
    MediaController bad(false);
    bad.setSource(path("media/bad.mp3"));
    ASSERT_TRUE(waitState(&bad, State::Error));
    EXPECT_FALSE(bad.errorString().isEmpty());
    bad.play(); // ignored
    EXPECT_EQ(bad.state(), State::Error);
    EXPECT_TRUE(Busy::idle());
}

TEST_F(MediaPlayerTest, AudioControlBar)
{
    MediaPlayerWidget w(MediaPlayerWidget::Kind::Audio);
    w.controller()->setMuted(true);
    w.resize(500, MediaPlayerWidget::kAudioHeight);
    w.show();
    w.setSource(path("media/a.wav"));
    MediaController* c = w.controller();
    ASSERT_TRUE(waitState(c, State::Ready));
    MediaControlBar* bar = w.controlBar();
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->barStyle(), MediaControlBar::Style::Audio);
    EXPECT_EQ(bar->height(), 42);
    EXPECT_EQ(bar->timeText(), "0:00 / 0:01");
    // parts laid out like Chromium: play, time, timeline, mute, overflow
    const QRect play = bar->partRect(MediaControlBar::Part::Play);
    const QRect time = bar->partRect(MediaControlBar::Part::Time);
    const QRect timeline = bar->partRect(MediaControlBar::Part::Timeline);
    const QRect mute = bar->partRect(MediaControlBar::Part::Mute);
    const QRect menu = bar->partRect(MediaControlBar::Part::Menu);
    EXPECT_LT(play.right(), time.left());
    EXPECT_LT(time.right(), timeline.left());
    EXPECT_LT(timeline.right(), mute.left());
    EXPECT_LT(mute.right(), menu.left());
    EXPECT_TRUE(bar->partRect(MediaControlBar::Part::Fullscreen).isEmpty());
    EXPECT_EQ(bar->partAt(play.center()), MediaControlBar::Part::Play);
    // play / pause by clicking
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, play.center());
    EXPECT_EQ(c->state(), State::Playing);
    EXPECT_TRUE(w.property(HostedEmbeds::kBusyProperty).toBool()); // kept alive by a hosting editor
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, play.center());
    EXPECT_EQ(c->state(), State::Paused);
    EXPECT_FALSE(w.property(HostedEmbeds::kBusyProperty).toBool());
    // seek by clicking the middle of the timeline
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->partRect(MediaControlBar::Part::Timeline).center());
    EXPECT_TRUE(QTest::qWaitFor([&] { return std::abs(c->position() - c->duration() / 2) < 100; }, 3000)) << c->position();
    EXPECT_EQ(bar->timeText(), "0:00 / 0:01");
    // hovering the speaker reveals the volume slider; the slider sets the volume
    QMouseEvent move(QEvent::MouseMove, QPointF(mute.center()), bar->mapToGlobal(QPointF(mute.center())), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(bar, &move);
    EXPECT_TRUE(bar->isVolumeExpanded());
    const QRect vol = bar->partRect(MediaControlBar::Part::Volume);
    ASSERT_FALSE(vol.isEmpty());
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(vol.left() + vol.width() / 4, vol.center().y()));
    EXPECT_NEAR(c->volume(), 0.25, 0.1);
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->partRect(MediaControlBar::Part::Mute).center());
    EXPECT_TRUE(c->isMuted());
    // leaving collapses the slider after a moment
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(bar, &leave);
    EXPECT_TRUE(QTest::qWaitFor([&] { return !bar->isVolumeExpanded(); }, 2000));
    // keyboard: space toggles, arrows seek
    w.setFocus();
    QTest::keyClick(&w, Qt::Key_Space);
    EXPECT_EQ(c->state(), State::Playing);
    QTest::keyClick(&w, Qt::Key_Space);
    EXPECT_EQ(c->state(), State::Paused);
    // narrow bars drop the time display first
    w.resize(150, 42);
    QApplication::processEvents();
    EXPECT_TRUE(bar->partRect(MediaControlBar::Part::Time).isEmpty());
    // static rendering (embed placeholders) paints without a controller
    QImage img(300, 42, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    MediaControlBar::paintStatic(&p, QRectF(0, 0, 300, 42), MediaControlBar::Style::Audio, 90000);
    p.end();
    EXPECT_NE(img.pixel(150, 20), 0u);
}

TEST_F(MediaPlayerTest, PlaysAndTracksPosition)
{
    MediaPlayerWidget w(MediaPlayerWidget::Kind::Audio);
    w.controller()->setMuted(true);
    w.resize(500, 42);
    w.show();
    w.setSource(path("media/a.wav"));
    MediaController* c = w.controller();
    ASSERT_TRUE(waitState(c, State::Ready));
    if (!playsHere(c))
        GTEST_SKIP() << "no playback clock in this environment";
    EXPECT_EQ(c->state() == State::Playing || c->state() == State::Ended, true);
    ASSERT_TRUE(waitState(c, State::Ended, 8000));
    EXPECT_EQ(w.controlBar()->timeText(), "0:01 / 0:01");
    EXPECT_FALSE(w.property(HostedEmbeds::kBusyProperty).toBool());
}

TEST_F(MediaPlayerTest, VideoSurfaceFramesAndFullscreen)
{
    if (!hasVideo())
        GTEST_SKIP() << "ffmpeg is not installed: no test video";
    MediaPlayerWidget w(MediaPlayerWidget::Kind::Video);
    w.controller()->setMuted(true);
    w.resize(640, 360);
    w.show();
    QSize natural;
    QObject::connect(&w, &MediaPlayerWidget::naturalSizeChanged, [&](QSize s) { natural = s; });
    w.setSource(path("media/v.mp4"));
    MediaController* c = w.controller();
    ASSERT_TRUE(waitState(c, State::Ready));
    EXPECT_TRUE(c->hasVideo());
    EXPECT_EQ(c->videoSize(), QSize(320, 180));
    EXPECT_EQ(natural, QSize(320, 180));
    EXPECT_EQ(w.naturalSize(), QSize(320, 180));
    EXPECT_EQ(w.heightForWidth(640), 180); // never upscaled
    EXPECT_EQ(w.heightForWidth(160), 90);
    // the overlay controls: shown while paused, with a fullscreen button
    VideoSurface* surface = w.surface();
    ASSERT_NE(surface, nullptr);
    EXPECT_TRUE(surface->controlsShown());
    MediaControlBar* bar = w.controlBar();
    EXPECT_EQ(bar->barStyle(), MediaControlBar::Style::Video);
    EXPECT_FALSE(bar->partRect(MediaControlBar::Part::Fullscreen).isEmpty());
    EXPECT_EQ(bar->geometry().bottom(), surface->height() - 1);
    if (playsHere(c)) {
        EXPECT_TRUE(QTest::qWaitFor([&] { return !c->currentFrame().isNull(); }, 5000));
    }
    c->pause();
    // fullscreen: a second surface in its own window on the same player
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->partRect(MediaControlBar::Part::Fullscreen).center());
    EXPECT_TRUE(w.isFullscreen());
    w.toggleFullscreen();
    EXPECT_TRUE(QTest::qWaitFor([&] { return !w.isFullscreen(); }, 2000));
}

TEST_F(MediaPlayerTest, VideoWithoutPictureBecomesAudio)
{
    if (!hasVideo())
        GTEST_SKIP() << "ffmpeg is not installed: no test video";
    MediaPlayerWidget w(MediaPlayerWidget::Kind::Video);
    w.controller()->setMuted(true);
    w.resize(500, 300);
    w.show();
    w.setSource(path("media/voice.mp4"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return w.kind() == MediaPlayerWidget::Kind::Audio; }, 10000));
    EXPECT_EQ(w.surface(), nullptr);
    ASSERT_NE(w.controlBar(), nullptr);
    EXPECT_EQ(w.controlBar()->barStyle(), MediaControlBar::Style::Audio);
    EXPECT_EQ(w.heightForWidth(500), 42);
}

TEST_F(MediaPlayerTest, AudioAndVideoViews)
{
    Workspace* ws = window->workspace();
    ws->openFile(vault->file("media/a.wav"));
    auto* av = qobject_cast<MediaFileView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(av, nullptr);
    EXPECT_EQ(av->viewType(), "audio");
    EXPECT_EQ(av->icon(), "file-audio");
    EXPECT_EQ(av->displayText(), "a");
    av->player()->controller()->setMuted(true);
    QApplication::processEvents();
    // centred in the padded view content, full width, 42 px
    const QRect g = av->player()->geometry();
    EXPECT_EQ(g.top(), 12);
    EXPECT_EQ(g.width(), av->width() - 24);
    EXPECT_EQ(g.height(), 42);
    ASSERT_TRUE(waitState(av->player()->controller(), State::Ready));
    // the view accepts its own extensions only
    EXPECT_TRUE(av->canAcceptExtension("flac"));
    EXPECT_FALSE(av->canAcceptExtension("mp4"));
    av->player()->controller()->play();
    av->onClose(); // closing pauses
    EXPECT_NE(av->player()->controller()->state(), State::Playing);

    if (!hasVideo())
        return;
    ws->openFile(vault->file("media/v.mp4"));
    auto* vv = qobject_cast<MediaFileView*>(ws->activeRootLeaf()->view());
    ASSERT_NE(vv, nullptr);
    EXPECT_EQ(vv->viewType(), "video");
    EXPECT_EQ(vv->icon(), "file");
    vv->player()->controller()->setMuted(true);
    ASSERT_TRUE(waitState(vv->player()->controller(), State::Ready));
    QApplication::processEvents();
    const QRect vg = vv->player()->geometry();
    EXPECT_EQ(vg.size(), QSize(320, 180)); // natural size, centred
    EXPECT_NEAR(vg.center().x(), vv->width() / 2, 1);
}
