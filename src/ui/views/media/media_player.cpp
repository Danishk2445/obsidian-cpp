#include "ui/views/media/media_player.h"

#include "core/util/busy.h"
#include "render/theme/theme.h"

#include <QAudioOutput>
#include <QKeyEvent>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>

#include <cmath>

namespace oc::media {

// ---------------------------------------------------------------------------------------------
// MediaController
// ---------------------------------------------------------------------------------------------
MediaController::MediaController(bool video, QObject* parent)
    : QObject(parent)
    , m_video(video)
    , m_player(new QMediaPlayer(this))
    , m_audio(new QAudioOutput(this))
{
    m_player->setAudioOutput(m_audio);
    if (video) {
        m_sink = new QVideoSink(this);
        m_player->setVideoSink(m_sink);
        connect(m_sink, &QVideoSink::videoFrameChanged, this, &MediaController::onFrame);
    }
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
            const bool first = !m_hasVideo && m_videoSize.isEmpty();
            m_hasVideo = m_player->hasVideo();
            const QSize res = m_player->metaData().value(QMediaMetaData::Resolution).toSize();
            if (res.isValid())
                m_videoSize = res;
            if (first && status == QMediaPlayer::LoadedMedia) {
                emit metadataLoaded();
                // like <video preload=metadata> at "#t=0.001": show the first frame, paused
                if (m_video && m_hasVideo && !m_started)
                    m_player->pause();
            }
        }
        if (status == QMediaPlayer::EndOfMedia)
            m_ended = true;
        updateState();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] { updateState(); });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        if (d == m_duration)
            return;
        m_duration = d;
        emit durationChanged(d);
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (m_state == State::Playing || m_state == State::Paused)
            emit positionChanged(ms);
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString& message) {
        if (error == QMediaPlayer::NoError)
            return;
        m_error = message.isEmpty() ? tr("Playback error") : message;
        updateState();
    });
}

MediaController::~MediaController()
{
    if (m_state == State::Loading)
        Busy::end();
    m_player->stop();
}

void MediaController::setSource(const QString& absPath)
{
    m_source = absPath;
    m_error.clear();
    m_started = false;
    m_ended = false;
    m_hasVideo = false;
    m_videoSize = QSize();
    m_frame = QImage();
    m_duration = 0;
    m_player->setSource(absPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(absPath));
    updateState();
}

qint64 MediaController::position() const { return m_state == State::Ended ? m_duration : m_player->position(); }

void MediaController::setState(State s)
{
    if (s == m_state)
        return;
    // headless screenshots wait while media loads
    if (s == State::Loading)
        Busy::begin();
    else if (m_state == State::Loading)
        Busy::end();
    m_state = s;
    emit stateChanged(s);
    emit positionChanged(position());
}

void MediaController::updateState()
{
    if (!m_error.isEmpty()) {
        setState(State::Error);
        return;
    }
    const QMediaPlayer::PlaybackState ps = m_player->playbackState();
    switch (m_player->mediaStatus()) {
    case QMediaPlayer::NoMedia:
        setState(m_source.isEmpty() ? State::Empty : State::Loading);
        return;
    case QMediaPlayer::LoadingMedia:
        setState(ps == QMediaPlayer::PlayingState && m_started ? State::Playing : State::Loading);
        return;
    case QMediaPlayer::InvalidMedia:
        m_error = m_player->errorString().isEmpty() ? tr("Unsupported media") : m_player->errorString();
        setState(State::Error);
        return;
    case QMediaPlayer::EndOfMedia:
        setState(ps == QMediaPlayer::PlayingState ? State::Playing : State::Ended);
        return;
    default:
        break;
    }
    if (ps == QMediaPlayer::PlayingState)
        setState(State::Playing);
    else if (ps == QMediaPlayer::PausedState)
        setState(m_started ? State::Paused : State::Ready);
    else
        setState(m_ended ? State::Ended : State::Ready);
}

void MediaController::play()
{
    if (m_state == State::Error || m_state == State::Empty)
        return;
    if (m_state == State::Ended || m_ended) {
        m_ended = false;
        m_player->setPosition(0);
    }
    m_started = true;
    m_player->play();
    updateState();
}

void MediaController::pause()
{
    if (m_state != State::Playing)
        return;
    m_player->pause();
    updateState();
}

void MediaController::togglePlay()
{
    if (m_state == State::Playing)
        pause();
    else
        play();
}

void MediaController::seek(qint64 ms)
{
    if (m_duration <= 0)
        return;
    ms = std::clamp<qint64>(ms, 0, m_duration);
    if (m_ended && ms < m_duration) {
        m_ended = false;
        if (!m_started)
            m_started = true;
        m_player->pause(); // Chromium: seeking an ended element leaves it paused there
    }
    m_player->setPosition(ms);
    updateState();
    emit positionChanged(ms);
}

void MediaController::setVolume(float volume)
{
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (!qFuzzyCompare(volume, m_volume)) {
        m_volume = volume;
        m_audio->setVolume(volume);
        emit volumeChanged(volume);
    }
    if (volume > 0 && m_muted)
        setMuted(false);
}

void MediaController::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    m_audio->setMuted(muted);
    emit mutedChanged(muted);
}

void MediaController::setPlaybackRate(double rate)
{
    if (qFuzzyCompare(rate, m_rate))
        return;
    m_rate = rate;
    m_player->setPlaybackRate(rate);
    emit playbackRateChanged(rate);
}

void MediaController::onFrame(const QVideoFrame& frame)
{
    if (!frame.isValid())
        return;
    m_frame = frame.toImage();
    if (!m_frame.isNull() && m_videoSize.isEmpty()) {
        m_videoSize = m_frame.size();
        emit metadataLoaded();
    }
    emit frameChanged();
}

// ---------------------------------------------------------------------------------------------
// Control bar rendering (shared by the widget and the static placeholder)
// ---------------------------------------------------------------------------------------------
namespace {
constexpr int kButton = 32;      // Chromium's control buttons (32×32 hit area)
constexpr int kVolumeSlider = 56;
constexpr int kVideoBarHeight = 56;

struct BarModel {
    MediaControlBar::Style style = MediaControlBar::Style::Audio;
    bool playing = false;
    qint64 position = 0;
    qint64 duration = 0;
    float volume = 1;
    bool muted = false;
    bool fullscreenAvailable = false;
    bool fullscreen = false;
    bool volumeExpanded = false;
    bool error = false;
    MediaControlBar::Part hover = MediaControlBar::Part::None;
};

struct BarGeometry {
    QRectF frame, play, time, timeline, mute, volume, fullscreen, menu;
};

QFont timeFont()
{
    QFont f = currentTheme()->interfaceFont(13);
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

QString timeString(const BarModel& m)
{
    return MediaControlBar::formatTime(m.position) + QStringLiteral(" / ") + MediaControlBar::formatTime(m.duration);
}

BarGeometry layoutBar(const QRectF& r, const BarModel& m)
{
    BarGeometry g;
    g.frame = r;
    const QFontMetricsF fm(timeFont());
    const double timeW = fm.horizontalAdvance(timeString(m)) + 2;
    if (m.style == MediaControlBar::Style::Audio) {
        const double cy = r.center().y();
        double x = r.left() + 6;
        g.play = QRectF(x, cy - kButton / 2.0, kButton, kButton);
        x += kButton + 4;
        const bool roomForTime = r.width() >= 200;
        if (roomForTime) {
            g.time = QRectF(x, cy - 10, timeW, 20);
            x += timeW + 12;
        }
        double right = r.right() - 6;
        g.menu = QRectF(right - kButton, cy - kButton / 2.0, kButton, kButton);
        right -= kButton;
        g.mute = QRectF(right - kButton, cy - kButton / 2.0, kButton, kButton);
        right = g.mute.left() - 4;
        if (m.volumeExpanded) {
            g.volume = QRectF(right - kVolumeSlider, cy - 8, kVolumeSlider, 16);
            right = g.volume.left() - 12;
        }
        g.timeline = QRectF(x, cy - 8, std::max(0.0, right - x), 16);
    } else {
        // overlay: timeline row above a row of buttons
        const double bottom = r.bottom() - 4;
        const double cy = bottom - kButton / 2.0;
        g.timeline = QRectF(r.left() + 16, bottom - kButton - 18, std::max(0.0, r.width() - 32), 16);
        double x = r.left() + 8;
        g.play = QRectF(x, cy - kButton / 2.0, kButton, kButton);
        x += kButton;
        g.mute = QRectF(x, cy - kButton / 2.0, kButton, kButton);
        x += kButton + 4;
        if (m.volumeExpanded) {
            g.volume = QRectF(x, cy - 8, kVolumeSlider, 16);
            x += kVolumeSlider + 8;
        }
        double right = r.right() - 8;
        g.menu = QRectF(right - kButton, cy - kButton / 2.0, kButton, kButton);
        right -= kButton;
        if (m.fullscreenAvailable) {
            g.fullscreen = QRectF(right - kButton, cy - kButton / 2.0, kButton, kButton);
            right -= kButton;
        }
        if (right - x >= timeW + 4)
            g.time = QRectF(x + 4, cy - 10, timeW, 20);
    }
    return g;
}

void drawPlay(QPainter* p, const QRectF& r, const QColor& c, bool playing)
{
    const QPointF c0 = r.center();
    p->setPen(Qt::NoPen);
    p->setBrush(c);
    if (playing) {
        const double w = 3.5, h = 13, gap = 4;
        p->drawRoundedRect(QRectF(c0.x() - gap / 2 - w, c0.y() - h / 2, w, h), 1, 1);
        p->drawRoundedRect(QRectF(c0.x() + gap / 2, c0.y() - h / 2, w, h), 1, 1);
    } else {
        QPainterPath tri;
        tri.moveTo(c0.x() - 4.5, c0.y() - 7.5);
        tri.lineTo(c0.x() + 7.5, c0.y());
        tri.lineTo(c0.x() - 4.5, c0.y() + 7.5);
        tri.closeSubpath();
        p->drawPath(tri);
    }
}

void drawSpeaker(QPainter* p, const QRectF& r, const QColor& c, float volume, bool muted)
{
    const QPointF o = r.center() - QPointF(3, 0);
    p->setPen(Qt::NoPen);
    p->setBrush(c);
    QPainterPath body;
    body.moveTo(o.x() - 6, o.y() - 3);
    body.lineTo(o.x() - 3, o.y() - 3);
    body.lineTo(o.x() + 1, o.y() - 7);
    body.lineTo(o.x() + 1, o.y() + 7);
    body.lineTo(o.x() - 3, o.y() + 3);
    body.lineTo(o.x() - 6, o.y() + 3);
    body.closeSubpath();
    p->drawPath(body);
    QPen pen(c, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    if (muted || volume <= 0) {
        p->drawLine(QPointF(o.x() + 4.5, o.y() - 3.5), QPointF(o.x() + 11.5, o.y() + 3.5));
        p->drawLine(QPointF(o.x() + 11.5, o.y() - 3.5), QPointF(o.x() + 4.5, o.y() + 3.5));
        return;
    }
    p->drawArc(QRectF(o.x() - 1, o.y() - 4, 8, 8), -60 * 16, 120 * 16);
    if (volume > 0.5)
        p->drawArc(QRectF(o.x() - 3, o.y() - 8, 16, 16), -60 * 16, 120 * 16);
}

void drawFullscreen(QPainter* p, const QRectF& r, const QColor& c, bool exit)
{
    QPen pen(c, 2);
    pen.setCapStyle(Qt::SquareCap);
    pen.setJoinStyle(Qt::MiterJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    const QRectF b = QRectF(r.center() - QPointF(7, 6), QSizeF(14, 12));
    const double l = 4;
    auto corner = [&](QPointF pt, double dx, double dy) {
        if (exit) {
            pt += QPointF(dx * l, dy * l);
            dx = -dx;
            dy = -dy;
        }
        QPainterPath path;
        path.moveTo(pt + QPointF(0, dy * l));
        path.lineTo(pt);
        path.lineTo(pt + QPointF(dx * l, 0));
        p->drawPath(path);
    };
    corner(b.topLeft(), 1, 1);
    corner(b.topRight(), -1, 1);
    corner(b.bottomLeft(), 1, -1);
    corner(b.bottomRight(), -1, -1);
}

void drawDots(QPainter* p, const QRectF& r, const QColor& c)
{
    p->setPen(Qt::NoPen);
    p->setBrush(c);
    for (int i = -1; i <= 1; ++i)
        p->drawEllipse(QPointF(r.center().x(), r.center().y() + i * 5.5), 1.8, 1.8);
}

void drawSlider(QPainter* p, const QRectF& r, double fraction, const QColor& track, const QColor& fill, bool thumb)
{
    fraction = std::clamp(fraction, 0.0, 1.0);
    const double h = 4, y = r.center().y() - h / 2;
    p->setPen(Qt::NoPen);
    p->setBrush(track);
    p->drawRoundedRect(QRectF(r.left(), y, r.width(), h), h / 2, h / 2);
    p->setBrush(fill);
    p->drawRoundedRect(QRectF(r.left(), y, std::max(h, r.width() * fraction), h), h / 2, h / 2);
    if (thumb) {
        p->drawEllipse(QPointF(r.left() + r.width() * fraction, r.center().y()), 6, 6);
    }
}

void paintBar(QPainter* p, const BarModel& m, const BarGeometry& g)
{
    const ThemePtr t = currentTheme();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QColor fg, track, fill, hoverBg;
    if (m.style == MediaControlBar::Style::Audio) {
        // audio::-webkit-media-controls-enclosure
        const double radius = std::max(0.0, t->px(QStringLiteral("--radius-m"), -1, 8) - 1);
        QPainterPath frame;
        frame.addRoundedRect(g.frame.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        p->fillPath(frame, t->color(QStringLiteral("--background-primary-alt")));
        p->setPen(QPen(t->color(QStringLiteral("--background-modifier-border")), 1));
        p->drawPath(frame);
        fg = t->color(QStringLiteral("--text-normal"));
        track = t->color(QStringLiteral("--background-modifier-border-hover"), t->color(QStringLiteral("--background-modifier-border")));
        fill = t->color(QStringLiteral("--text-muted"));
        hoverBg = t->color(QStringLiteral("--background-modifier-hover"));
    } else {
        QLinearGradient grad(g.frame.topLeft(), g.frame.bottomLeft());
        grad.setColorAt(0, QColor(0, 0, 0, 0));
        grad.setColorAt(1, QColor(0, 0, 0, 150));
        p->fillRect(g.frame, grad);
        fg = QColor(255, 255, 255);
        track = QColor(255, 255, 255, 77);
        fill = QColor(255, 255, 255);
        hoverBg = QColor(255, 255, 255, 38);
    }
    auto hovered = [&](MediaControlBar::Part part, const QRectF& r) {
        if (m.hover == part && !r.isEmpty()) {
            p->setPen(Qt::NoPen);
            p->setBrush(hoverBg);
            p->drawEllipse(r.adjusted(2, 2, -2, -2));
        }
    };
    const QColor iconColor = m.error ? t->color(QStringLiteral("--text-faint")) : fg;
    hovered(MediaControlBar::Part::Play, g.play);
    drawPlay(p, g.play, iconColor, m.playing);
    if (!g.time.isEmpty()) {
        p->setFont(timeFont());
        p->setPen(iconColor);
        p->drawText(g.time, Qt::AlignLeft | Qt::AlignVCenter, timeString(m));
    }
    if (!g.timeline.isEmpty() && g.timeline.width() > 8) {
        const double frac = m.duration > 0 ? double(m.position) / double(m.duration) : 0;
        drawSlider(p, g.timeline, frac, track, fill, m.hover == MediaControlBar::Part::Timeline);
    }
    hovered(MediaControlBar::Part::Mute, g.mute);
    drawSpeaker(p, g.mute, iconColor, m.volume, m.muted);
    if (!g.volume.isEmpty())
        drawSlider(p, g.volume, m.muted ? 0 : m.volume, track, fill, true);
    if (!g.fullscreen.isEmpty()) {
        hovered(MediaControlBar::Part::Fullscreen, g.fullscreen);
        drawFullscreen(p, g.fullscreen, iconColor, m.fullscreen);
    }
    hovered(MediaControlBar::Part::Menu, g.menu);
    drawDots(p, g.menu, iconColor);
    p->restore();
}
} // namespace

struct MediaControlBar::Geometry : BarGeometry {
    BarModel model;
};

// ---------------------------------------------------------------------------------------------
// MediaControlBar
// ---------------------------------------------------------------------------------------------
MediaControlBar::MediaControlBar(MediaController* controller, Style style, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_style(style)
{
    setObjectName(style == Style::Audio ? QStringLiteral("ocAudioControls") : QStringLiteral("ocVideoControls"));
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::ArrowCursor);
    if (style == Style::Video)
        setAttribute(Qt::WA_TranslucentBackground);
    m_collapse.setSingleShot(true);
    m_collapse.setInterval(300);
    connect(&m_collapse, &QTimer::timeout, this, [this] {
        if (m_drag != Part::Volume && m_hover != Part::Mute && m_hover != Part::Volume) {
            m_volumeExpanded = false;
            update();
        }
    });
    auto repaint = [this] { update(); };
    connect(controller, &MediaController::stateChanged, this, repaint);
    connect(controller, &MediaController::positionChanged, this, repaint);
    connect(controller, &MediaController::durationChanged, this, repaint);
    connect(controller, &MediaController::volumeChanged, this, repaint);
    connect(controller, &MediaController::mutedChanged, this, repaint);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, repaint);
}

void MediaControlBar::setFullscreenAvailable(bool on)
{
    m_fullscreen = on;
    update();
}

QSize MediaControlBar::sizeHint() const { return QSize(300, m_style == Style::Audio ? MediaPlayerWidget::kAudioHeight : kVideoBarHeight); }

QString MediaControlBar::formatTime(qint64 ms)
{
    const qint64 total = std::max<qint64>(0, ms / 1000);
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString MediaControlBar::timeText() const
{
    return formatTime(m_controller->position()) + QStringLiteral(" / ") + formatTime(m_controller->duration());
}

MediaControlBar::Geometry MediaControlBar::geometry() const
{
    BarModel m;
    m.style = m_style;
    m.playing = m_controller->state() == MediaController::State::Playing;
    m.position = m_controller->position();
    m.duration = m_controller->duration();
    m.volume = m_controller->volume();
    m.muted = m_controller->isMuted();
    m.error = m_controller->state() == MediaController::State::Error;
    m.fullscreenAvailable = m_style == Style::Video;
    m.fullscreen = m_fullscreen;
    m.volumeExpanded = m_volumeExpanded;
    m.hover = m_hover;
    Geometry g;
    static_cast<BarGeometry&>(g) = layoutBar(QRectF(rect()), m);
    g.model = m;
    return g;
}

QRect MediaControlBar::partRect(Part part) const
{
    const Geometry g = geometry();
    switch (part) {
    case Part::Play: return g.play.toAlignedRect();
    case Part::Time: return g.time.toAlignedRect();
    case Part::Timeline: return g.timeline.toAlignedRect();
    case Part::Mute: return g.mute.toAlignedRect();
    case Part::Volume: return g.volume.toAlignedRect();
    case Part::Fullscreen: return g.fullscreen.toAlignedRect();
    case Part::Menu: return g.menu.toAlignedRect();
    case Part::None: break;
    }
    return {};
}

MediaControlBar::Part MediaControlBar::partAt(const QPoint& pos) const
{
    const Geometry g = geometry();
    const QPointF pt(pos);
    if (g.play.contains(pt))
        return Part::Play;
    if (g.mute.contains(pt))
        return Part::Mute;
    if (!g.volume.isEmpty() && g.volume.adjusted(-4, -8, 4, 8).contains(pt))
        return Part::Volume;
    if (!g.fullscreen.isEmpty() && g.fullscreen.contains(pt))
        return Part::Fullscreen;
    if (g.menu.contains(pt))
        return Part::Menu;
    if (!g.timeline.isEmpty() && g.timeline.adjusted(-6, -8, 6, 8).contains(pt))
        return Part::Timeline;
    if (!g.time.isEmpty() && g.time.contains(pt))
        return Part::Time;
    return Part::None;
}

void MediaControlBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const Geometry g = geometry();
    paintBar(&p, g.model, g);
}

void MediaControlBar::paintStatic(QPainter* p, const QRectF& rect, Style style, qint64 durationMs)
{
    BarModel m;
    m.style = style;
    m.duration = durationMs;
    m.fullscreenAvailable = style == Style::Video;
    paintBar(p, m, layoutBar(rect, m));
}

void MediaControlBar::seekTo(int x)
{
    const QRectF tl = geometry().timeline;
    if (tl.width() <= 0 || m_controller->duration() <= 0)
        return;
    const double frac = std::clamp((x - tl.left()) / tl.width(), 0.0, 1.0);
    m_controller->seek(qint64(std::llround(frac * double(m_controller->duration()))));
}

void MediaControlBar::setVolumeAt(int x)
{
    const QRectF vr = geometry().volume;
    if (vr.width() <= 0)
        return;
    m_controller->setVolume(float(std::clamp((x - vr.left()) / vr.width(), 0.0, 1.0)));
}

void MediaControlBar::showMenu()
{
    QMenu menu(this);
    QMenu* speed = menu.addMenu(tr("Playback speed"));
    const double rates[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    for (double r : rates) {
        QAction* a = speed->addAction(qFuzzyCompare(r, 1.0) ? tr("Normal") : QString::number(r), this,
                                      [this, r] { m_controller->setPlaybackRate(r); });
        a->setCheckable(true);
        a->setChecked(qFuzzyCompare(r, m_controller->playbackRate()));
    }
    const QRect mr = partRect(Part::Menu);
    menu.exec(mapToGlobal(QPoint(mr.left(), m_style == Style::Audio ? mr.bottom() : mr.top() - menu.sizeHint().height())));
}

void MediaControlBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const Part part = partAt(e->position().toPoint());
    switch (part) {
    case Part::Play:
        m_controller->togglePlay();
        break;
    case Part::Timeline:
        m_drag = Part::Timeline;
        seekTo(e->position().toPoint().x());
        break;
    case Part::Mute:
        m_controller->setMuted(!m_controller->isMuted());
        break;
    case Part::Volume:
        m_drag = Part::Volume;
        setVolumeAt(e->position().toPoint().x());
        break;
    case Part::Fullscreen:
        emit fullscreenRequested();
        break;
    case Part::Menu:
        showMenu();
        break;
    default:
        break;
    }
    e->accept();
}

void MediaControlBar::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag == Part::Timeline) {
        seekTo(pos.x());
        return;
    }
    if (m_drag == Part::Volume) {
        setVolumeAt(pos.x());
        return;
    }
    const Part part = partAt(pos);
    if (part != m_hover) {
        m_hover = part;
        if (part == Part::Mute || part == Part::Volume) {
            m_volumeExpanded = true;
            m_collapse.stop();
        } else if (m_volumeExpanded) {
            m_collapse.start();
        }
        update();
    }
}

void MediaControlBar::mouseReleaseEvent(QMouseEvent* e)
{
    Q_UNUSED(e);
    m_drag = Part::None;
    if (m_hover != Part::Mute && m_hover != Part::Volume && m_volumeExpanded)
        m_collapse.start();
}

void MediaControlBar::enterEvent(QEnterEvent* e) { QWidget::enterEvent(e); }

void MediaControlBar::leaveEvent(QEvent* e)
{
    m_hover = Part::None;
    if (m_volumeExpanded)
        m_collapse.start();
    update();
    QWidget::leaveEvent(e);
}

void MediaControlBar::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); }

// ---------------------------------------------------------------------------------------------
// VideoSurface
// ---------------------------------------------------------------------------------------------
VideoSurface::VideoSurface(MediaController* controller, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_controls(new MediaControlBar(controller, MediaControlBar::Style::Video, this))
{
    setObjectName(QStringLiteral("ocVideoSurface"));
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(m_controls, &MediaControlBar::fullscreenRequested, this, &VideoSurface::fullscreenRequested);
    connect(controller, &MediaController::frameChanged, this, qOverload<>(&QWidget::update));
    connect(controller, &MediaController::stateChanged, this, &VideoSurface::updateControlsVisibility);
    m_idle.setSingleShot(true);
    m_idle.setInterval(2500);
    connect(&m_idle, &QTimer::timeout, this, &VideoSurface::updateControlsVisibility);
    m_click.setSingleShot(true);
    m_click.setInterval(QApplication::doubleClickInterval());
    connect(&m_click, &QTimer::timeout, m_controller, &MediaController::togglePlay);
}

void VideoSurface::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    const QImage frame = m_controller->currentFrame();
    if (frame.isNull())
        return;
    const QSizeF s = QSizeF(frame.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
    const QRectF r(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, frame);
}

void VideoSurface::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const int h = std::min(height(), kVideoBarHeight);
    m_controls->setGeometry(0, height() - h, width(), h);
}

void VideoSurface::revealControls()
{
    m_controls->show();
    m_controls->raise();
    m_idle.start();
    unsetCursor();
}

void VideoSurface::updateControlsVisibility()
{
    const bool playing = m_controller->state() == MediaController::State::Playing;
    const bool overControls = m_controls->underMouse();
    const bool show = !playing || m_idle.isActive() || overControls;
    m_controls->setVisible(show);
    if (!show && window()->isFullScreen())
        setCursor(Qt::BlankCursor);
}

void VideoSurface::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        m_click.start(); // Chromium: a click on the video toggles playback
    revealControls();
}

void VideoSurface::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_click.stop();
    emit fullscreenRequested();
}

void VideoSurface::mouseMoveEvent(QMouseEvent* e)
{
    Q_UNUSED(e);
    revealControls();
}

void VideoSurface::leaveEvent(QEvent* e)
{
    m_idle.stop();
    updateControlsVisibility();
    QWidget::leaveEvent(e);
}

// ---------------------------------------------------------------------------------------------
// MediaPlayerWidget
// ---------------------------------------------------------------------------------------------
MediaPlayerWidget::MediaPlayerWidget(Kind kind, QWidget* parent)
    : QWidget(parent)
    , m_kind(kind)
    , m_controller(new MediaController(kind == Kind::Video, this))
{
    setObjectName(kind == Kind::Audio ? QStringLiteral("ocAudioPlayer") : QStringLiteral("ocVideoPlayer"));
    setFocusPolicy(Qt::ClickFocus);
    if (kind == Kind::Audio) {
        m_audioBar = new MediaControlBar(m_controller, MediaControlBar::Style::Audio, this);
    } else {
        m_surface = new VideoSurface(m_controller, this);
        connect(m_surface, &VideoSurface::fullscreenRequested, this, &MediaPlayerWidget::toggleFullscreen);
        connect(m_controller, &MediaController::metadataLoaded, this, [this] {
            if (m_controller->state() != MediaController::State::Error && !m_controller->hasVideo() && m_controller->videoSize().isEmpty()) {
                becomeAudio(); // Obsidian: a "video" without a picture plays in an <audio> element
                return;
            }
            updateGeometry();
            emit naturalSizeChanged(naturalSize());
        });
    }
    // playing media is "busy": a hosting editor keeps it alive when scrolled away
    connect(m_controller, &MediaController::stateChanged, this, [this](MediaController::State s) {
        setProperty("ocEmbedBusy", s == MediaController::State::Playing);
    });
}

MediaPlayerWidget::~MediaPlayerWidget()
{
    if (m_fullscreenWindow)
        delete m_fullscreenWindow.data();
}

void MediaPlayerWidget::setSource(const QString& absPath) { m_controller->setSource(absPath); }

MediaControlBar* MediaPlayerWidget::controlBar() const { return m_audioBar ? m_audioBar : (m_surface ? m_surface->controls() : nullptr); }

void MediaPlayerWidget::becomeAudio()
{
    if (m_kind == Kind::Audio)
        return;
    m_kind = Kind::Audio;
    delete m_surface;
    m_surface = nullptr;
    m_audioBar = new MediaControlBar(m_controller, MediaControlBar::Style::Audio, this);
    m_audioBar->setGeometry(0, 0, width(), kAudioHeight);
    m_audioBar->show();
    updateGeometry();
    emit naturalSizeChanged(naturalSize());
}

QSize MediaPlayerWidget::naturalSize() const
{
    if (m_kind == Kind::Audio)
        return QSize(0, kAudioHeight);
    return m_controller->videoSize();
}

int MediaPlayerWidget::heightForWidth(int w) const
{
    if (m_kind == Kind::Audio)
        return kAudioHeight;
    const QSize n = m_controller->videoSize();
    if (n.isEmpty())
        return w * 9 / 16;
    return std::max(1, int(std::lround(double(std::min(w, n.width())) * n.height() / n.width())));
}

QSize MediaPlayerWidget::sizeHint() const
{
    if (m_kind == Kind::Audio)
        return QSize(300, kAudioHeight);
    const QSize n = m_controller->videoSize();
    return n.isEmpty() ? QSize(300, 150) : n; // <video> without metadata: 300×150
}

void MediaPlayerWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (m_audioBar)
        m_audioBar->setGeometry(0, (height() - kAudioHeight) / 2, width(), kAudioHeight);
    if (m_surface)
        m_surface->setGeometry(rect());
}

void MediaPlayerWidget::keyPressEvent(QKeyEvent* e)
{
    MediaController* c = m_controller;
    switch (e->key()) {
    case Qt::Key_Space:
    case Qt::Key_K:
        c->togglePlay();
        return;
    case Qt::Key_Left:
        c->seek(c->position() - 5000);
        return;
    case Qt::Key_Right:
        c->seek(c->position() + 5000);
        return;
    case Qt::Key_Up:
        c->setVolume(c->volume() + 0.05f);
        return;
    case Qt::Key_Down:
        c->setVolume(c->volume() - 0.05f);
        return;
    case Qt::Key_M:
        c->setMuted(!c->isMuted());
        return;
    case Qt::Key_F:
        if (m_kind == Kind::Video) {
            toggleFullscreen();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

void MediaPlayerWidget::toggleFullscreen()
{
    if (m_kind != Kind::Video)
        return;
    if (m_fullscreenWindow) {
        m_fullscreenWindow->close();
        return;
    }
    // a second surface on the same player, in its own full-screen window
    class FullscreenWindow : public QWidget {
    public:
        FullscreenWindow(MediaController* c)
            : QWidget(nullptr, Qt::Window)
            , surface(new VideoSurface(c, this))
        {
            setAttribute(Qt::WA_DeleteOnClose);
            setObjectName(QStringLiteral("ocVideoFullscreen"));
            auto* l = new QVBoxLayout(this);
            l->setContentsMargins(0, 0, 0, 0);
            l->addWidget(surface);
            surface->controls()->setFullscreenAvailable(true);
            connect(surface, &VideoSurface::fullscreenRequested, this, &QWidget::close);
            player = c;
        }
        VideoSurface* surface;
        MediaController* player;

    protected:
        void keyPressEvent(QKeyEvent* e) override
        {
            if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_F) {
                close();
                return;
            }
            if (e->key() == Qt::Key_Space || e->key() == Qt::Key_K) {
                player->togglePlay();
                return;
            }
            QWidget::keyPressEvent(e);
        }
    };
    auto* w = new FullscreenWindow(m_controller);
    m_fullscreenWindow = w;
    w->setWindowTitle(QFileInfo(m_controller->source()).fileName());
    w->showFullScreen();
    w->activateWindow();
}

} // namespace oc::media
