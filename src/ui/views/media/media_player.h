#pragma once

// Audio / video playback for the "audio" and "video" views and their embeds. Obsidian uses the
// native <audio controls> / <video controls> elements, so the controls mimic Chromium's media
// controls, themed like Obsidian's (audio: 42 px tall, --background-primary-alt, 1px
// --background-modifier-border, radius --radius-m - 1; video: an overlay bar on a dark gradient).
//
// MediaController is the state machine over QMediaPlayer (+ QAudioOutput, + QVideoSink for video):
//   Loading -> Ready (metadata known, at 0) -> Playing <-> Paused -> Ended (at the end; play
//   restarts from 0); any failure -> Error. MediaControlBar renders and drives it: play / pause,
//   "current / total" time, a seek timeline (click or drag), mute + volume (the slider appears while
//   hovering the speaker), the overflow menu (playback speed) and, for video, fullscreen.

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QAudioOutput;
class QMediaPlayer;
class QVideoFrame;
class QVideoSink;

namespace oc::media {

class MediaController : public QObject {
    Q_OBJECT
public:
    enum class State { Empty, Loading, Ready, Playing, Paused, Ended, Error };
    Q_ENUM(State)

    explicit MediaController(bool video, QObject* parent = nullptr);
    ~MediaController() override;

    void setSource(const QString& absPath);
    QString source() const { return m_source; }
    bool isVideo() const { return m_video; }

    State state() const { return m_state; }
    qint64 position() const; // ms
    qint64 duration() const { return m_duration; }
    float volume() const { return m_volume; }
    bool isMuted() const { return m_muted; }
    double playbackRate() const { return m_rate; }
    bool hasVideo() const { return m_hasVideo; }
    QSize videoSize() const { return m_videoSize; }
    QImage currentFrame() const { return m_frame; }
    QString errorString() const { return m_error; }

    void play();
    void pause();
    void togglePlay();
    void seek(qint64 ms);
    void setVolume(float volume); // 0..1 (unmutes when > 0)
    void setMuted(bool muted);
    void setPlaybackRate(double rate);

    QMediaPlayer* player() const { return m_player; }

signals:
    void stateChanged(oc::media::MediaController::State state);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void volumeChanged(float volume);
    void mutedChanged(bool muted);
    void playbackRateChanged(double rate);
    void frameChanged();
    void metadataLoaded(); // hasVideo() / videoSize() / duration() known

private:
    void updateState();
    void setState(State s);
    void onFrame(const QVideoFrame& frame);

    bool m_video;
    QString m_source;
    QMediaPlayer* m_player;
    QAudioOutput* m_audio;
    QVideoSink* m_sink = nullptr;
    State m_state = State::Empty;
    qint64 m_duration = 0;
    float m_volume = 1.0f;
    bool m_muted = false;
    double m_rate = 1.0;
    bool m_hasVideo = false;
    bool m_started = false; // play() was called since the source loaded
    bool m_ended = false;
    QSize m_videoSize;
    QImage m_frame;
    QString m_error;
};

// Chromium-style controls. Audio: a framed pill; video: an overlay at the bottom of the surface.
class MediaControlBar : public QWidget {
    Q_OBJECT
public:
    enum class Style { Audio, Video };
    enum class Part { None, Play, Time, Timeline, Mute, Volume, Fullscreen, Menu };

    MediaControlBar(MediaController* controller, Style style, QWidget* parent = nullptr);

    Style barStyle() const { return m_style; }
    MediaController* controller() const { return m_controller; }
    void setFullscreenAvailable(bool on);
    QString timeText() const; // "0:03 / 0:05"
    QRect partRect(Part part) const;
    Part partAt(const QPoint& pos) const;
    bool isVolumeExpanded() const { return m_volumeExpanded; }
    static QString formatTime(qint64 ms); // "m:ss" / "h:mm:ss" like Chromium

    // Static rendering (the embed placeholder): a paused bar at 0 showing `durationMs`.
    static void paintStatic(QPainter* p, const QRectF& rect, Style style, qint64 durationMs);

    QSize sizeHint() const override;

signals:
    void fullscreenRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    struct Geometry;
    Geometry layoutParts() const;
    void seekTo(int x);
    void setVolumeAt(int x);
    void showMenu();

    MediaController* m_controller;
    Style m_style;
    bool m_fullscreen = false;
    Part m_hover = Part::None;
    Part m_drag = Part::None;
    bool m_volumeExpanded = false;
    QTimer m_collapse;
};

// The video picture: the current frame letterboxed on black; a click toggles play, a double-click
// fullscreen. Controls overlay the bottom and hide while playing without mouse movement.
class VideoSurface : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(MediaController* controller, QWidget* parent = nullptr);
    MediaControlBar* controls() const { return m_controls; }
    bool controlsShown() const { return !m_controls->isHidden(); }
    void revealControls(); // show them and restart the idle timer

signals:
    void fullscreenRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void updateControlsVisibility();
    MediaController* m_controller;
    MediaControlBar* m_controls;
    QTimer m_idle;
    QTimer m_click; // a single click toggles play unless a double-click follows
};

// The player used by the views and the embeds: an audio bar, or a video surface with its overlay
// controls. Obsidian's video element falls back to audio when the file has no video track.
class MediaPlayerWidget : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Audio, Video };
    static constexpr int kAudioHeight = 42; // audio { height: 42px }

    MediaPlayerWidget(Kind kind, QWidget* parent = nullptr);
    ~MediaPlayerWidget() override;

    void setSource(const QString& absPath);
    Kind kind() const { return m_kind; }
    MediaController* controller() const { return m_controller; }
    MediaControlBar* controlBar() const;
    VideoSurface* surface() const { return m_surface; }
    QSize naturalSize() const; // video: its size once known; audio: (0, 42)

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

    void toggleFullscreen();
    bool isFullscreen() const { return m_fullscreenWindow != nullptr; }

signals:
    void naturalSizeChanged(QSize size);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void becomeAudio();
    Kind m_kind;
    MediaController* m_controller;
    MediaControlBar* m_audioBar = nullptr;
    VideoSurface* m_surface = nullptr;
    QPointer<QWidget> m_fullscreenWindow;
};

} // namespace oc::media
