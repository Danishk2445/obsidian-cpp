#include "ui/views/media/media_embeds.h"

#include "core/util/busy.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/views/media/media_player.h"
#include "ui/views/media/pdf_file.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/widgets/icon_button.h"

#include <QBoxLayout>
#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenu>
#include <QPagedPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocumentRenderOptions>
#include <QTimer>
#include <QVideoSink>

#include <cmath>

namespace oc::media {

namespace {
constexpr int kPopoverWidth = 450;     // --popover-width / --popover-pdf-width
constexpr int kPopoverHeight = 400;    // --popover-height / --popover-pdf-height
constexpr int kPdfCssDefaultHeight = 800; // .pdf-embed { height: 800px } (unreadable files)
constexpr int kScrollbar = 12;
constexpr int kProbeTimeoutMs = 5000;

qint64 mtimeOf(const QString& path) { return QFileInfo(path).lastModified().toMSecsSinceEpoch(); }

double themeRadiusS() { return currentTheme()->px(QStringLiteral("--radius-s"), -1, 4); }

QSize fitVideo(QSize natural, double maxW, double maxH)
{
    QSizeF s = natural;
    if (s.width() > maxW)
        s = QSizeF(maxW, s.height() * maxW / s.width());
    if (maxH > 0 && s.height() > maxH)
        s = QSizeF(s.width() * maxH / s.height(), maxH);
    return QSize(std::max(1, int(std::lround(s.width()))), std::max(1, int(std::lround(s.height()))));
}
} // namespace

// ---------------------------------------------------------------------------------------------
// MediaProbe
// ---------------------------------------------------------------------------------------------
MediaProbe& MediaProbe::instance()
{
    static MediaProbe p;
    return p;
}

MediaInfo MediaProbe::info(const QString& absPath, bool start)
{
    const qint64 mtime = mtimeOf(absPath);
    if (auto it = m_cache.constFind(absPath); it != m_cache.cend() && it->mtime == mtime)
        return it->info;
    if (start && !m_queue.contains(absPath)) {
        m_cache.remove(absPath);
        m_queue.append(absPath);
        Busy::begin();
        pump();
    }
    return {};
}

void MediaProbe::forget(const QString& absPath) { m_cache.remove(absPath); }

void MediaProbe::pump()
{
    while (m_running < 2 && !m_queue.isEmpty()) {
        const QString path = m_queue.takeFirst();
        ++m_running;
        auto* player = new QMediaPlayer(this);
        auto* sink = new QVideoSink(player); // with a video output the backend reports the picture size
        player->setVideoSink(sink);
        auto* timeout = new QTimer(player);
        timeout->setSingleShot(true);
        auto done = std::make_shared<bool>(false);
        auto finishWith = [this, player, path, done](const MediaInfo& info) {
            if (*done)
                return;
            *done = true;
            player->deleteLater();
            finish(path, info);
        };
        connect(player, &QMediaPlayer::mediaStatusChanged, this, [player, finishWith](QMediaPlayer::MediaStatus st) {
            if (st == QMediaPlayer::LoadedMedia) {
                MediaInfo info;
                info.known = info.valid = true;
                info.hasVideo = player->hasVideo();
                info.size = player->metaData().value(QMediaMetaData::Resolution).toSize();
                info.duration = player->duration();
                if (info.hasVideo && info.size.isEmpty())
                    info.size = QSize(300, 150);
                finishWith(info);
            } else if (st == QMediaPlayer::InvalidMedia) {
                MediaInfo info;
                info.known = true;
                finishWith(info);
            }
        });
        connect(player, &QMediaPlayer::errorOccurred, this, [finishWith](QMediaPlayer::Error e, const QString&) {
            if (e == QMediaPlayer::NoError)
                return;
            MediaInfo info;
            info.known = true;
            finishWith(info);
        });
        connect(timeout, &QTimer::timeout, this, [finishWith] {
            MediaInfo info;
            info.known = true;
            finishWith(info);
        });
        timeout->start(kProbeTimeoutMs);
        player->setSource(QUrl::fromLocalFile(path));
    }
}

void MediaProbe::finish(const QString& absPath, const MediaInfo& info)
{
    --m_running;
    m_cache.insert(absPath, Entry{mtimeOf(absPath), info});
    Busy::end();
    emit probed(absPath);
    EmbedRegistry::instance().notifyChanged(absPath); // embeds re-lay out at the real size
    pump();
}

// ---------------------------------------------------------------------------------------------
// EmbedFileWatcher
// ---------------------------------------------------------------------------------------------
EmbedFileWatcher& EmbedFileWatcher::instance()
{
    static EmbedFileWatcher w;
    return w;
}

EmbedFileWatcher::EmbedFileWatcher()
    : m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        // atomic saves replace the file (and drop the watch): watch the new one
        if (!m_watcher->files().contains(path) && QFileInfo::exists(path))
            m_watcher->addPath(path);
        MediaProbe::instance().forget(path);
        if (std::shared_ptr<PdfFile> f = PdfFile::find(path))
            f->reload();
        emit fileChanged(path);
        EmbedRegistry::instance().notifyChanged(path);
    });
}

void EmbedFileWatcher::watch(const QString& absPath, QObject* owner)
{
    if (absPath.isEmpty() || !owner)
        return;
    if (m_refs[absPath]++ == 0)
        m_watcher->addPath(absPath);
    connect(owner, &QObject::destroyed, this, [this, absPath] {
        if (--m_refs[absPath] <= 0) {
            m_refs.remove(absPath);
            m_watcher->removePath(absPath);
        }
    });
}

// ---------------------------------------------------------------------------------------------
// PdfEmbedWidget
// ---------------------------------------------------------------------------------------------
PdfEmbedWidget::PdfEmbedWidget(const EmbedContext& context, QWidget* parent)
    : QWidget(parent)
    , m_ctx(context)
    , m_viewer(new PdfViewer(context.request.display == EmbedRequest::Display::Popover ? PdfViewer::Mode::Popover : PdfViewer::Mode::Embed,
                             this))
{
    setObjectName(QStringLiteral("ocPdfEmbed"));
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(1, 1, 1, 1); // border: var(--border-width) solid var(--background-modifier-border)
    l->addWidget(m_viewer);
    if (context.request.display == EmbedRequest::Display::Inline) {
        auto* right = m_viewer->toolbarRight()->layout();
        m_more = new IconButton(QStringLiteral("more-vertical"), tr("Open"), m_viewer->toolbarRight());
        m_more->setButtonSize(QSize(30, 26));
        right->addWidget(m_more);
        m_editSpacer = new QWidget(m_viewer->toolbarRight());
        m_editSpacer->setFixedWidth(36 - 12); // .embed-actions + .pdf-toolbar { padding-inline-end: --size-4-9 }
        right->addWidget(m_editSpacer);
        connect(m_more, &QAbstractButton::clicked, this, [this] {
            // Obsidian's link context menu
            QMenu menu(this);
            const EmbedRequest req = m_ctx.request;
            QPointer<QWidget> self = this;
            menu.addAction(IconProvider::instance().icon(QStringLiteral("file-plus"), 16, 2.0), tr("Open in new tab"), this,
                           [self, req] { EmbedRegistry::instance().open(self, req, QStringLiteral("tab")); });
            menu.addAction(IconProvider::instance().icon(QStringLiteral("separator-vertical"), 16, 2.0), tr("Open to the right"), this,
                           [self, req] { EmbedRegistry::instance().open(self, req, QStringLiteral("split")); });
            menu.exec(m_more->mapToGlobal(QPoint(m_more->width() - menu.sizeHint().width(), m_more->height())));
        });
        // .embed-actions (Live Preview): "Edit this block" at top 6px, inline-end 4px
        m_edit = new IconButton(QStringLiteral("code-2"), tr("Edit this block"), this);
        m_edit->setObjectName(QStringLiteral("ocEmbedEdit"));
        m_edit->setButtonSize(QSize(30, 26));
        m_edit->resize(m_edit->sizeHint()); // (not in a layout)
        connect(m_edit, &QAbstractButton::clicked, this, [this] {
            if (m_ctx.revealSource)
                m_ctx.revealSource();
        });
    }
    m_viewer->load(context.request.absPath);
    m_viewer->applySubpath(context.request.subpath);
    EmbedFileWatcher::instance().watch(context.request.absPath, this);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    updateActions();
}

void PdfEmbedWidget::updateActions()
{
    // the host sets "ocEmbedEditable" while the note is in (editable) Live Preview
    const bool show = m_edit && property("ocEmbedEditable").toBool();
    if (m_edit) {
        m_edit->setVisible(show);
        m_edit->move(width() - 4 - m_edit->width(), 6);
        m_edit->raise();
    }
    if (m_editSpacer)
        m_editSpacer->setVisible(show);
}

bool PdfEmbedWidget::event(QEvent* e)
{
    if (e->type() == QEvent::DynamicPropertyChange)
        updateActions();
    return QWidget::event(e);
}

void PdfEmbedWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // round the viewer's corners inside the 1px border (radius-s)
    const double r = std::max(0.0, themeRadiusS() - 1);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, m_viewer->width(), m_viewer->height()), r, r);
    m_viewer->setMask(QRegion(path.toFillPolygon().toPolygon()));
    updateActions();
}

void PdfEmbedWidget::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = themeRadiusS();
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), r, r);
    p.fillPath(path, t->color(QStringLiteral("--pdf-background"), t->color(QStringLiteral("--background-primary"))));
    p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border")), 1));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------
// PdfEmbedProvider
// ---------------------------------------------------------------------------------------------
QSizeF PdfEmbedProvider::size(const EmbedRequest& req, qreal maxWidth) const
{
    if (req.display == EmbedRequest::Display::Popover)
        return QSizeF(std::min<qreal>(kPopoverWidth, maxWidth), kPopoverHeight);
    const double w = std::max<qreal>(80, maxWidth); // width: 100%
    const int height = req.intParam(QStringLiteral("height"));
    if (height > 0)
        return QSizeF(w, height + PdfViewer::kToolbarHeight + 2);
    // "page": the (subpath's) page at page-width in the embed, plus the toolbar
    const QList<QSizeF> pages = PdfFile::pageSizes(req.absPath);
    if (pages.isEmpty())
        return QSizeF(w, kPdfCssDefaultHeight);
    const int page = std::clamp(req.intParam(QStringLiteral("page"), 1), 1, int(pages.size())) - 1;
    const QSizeF pts = pages[page];
    const double inner = w - 2 - (pages.size() > 1 ? kScrollbar : 0);
    const double pageH = std::floor(inner * pts.height() / std::max(1.0, pts.width()));
    return QSizeF(w, pageH + PdfViewer::kToolbarHeight + 2);
}

quint64 PdfEmbedProvider::signature(const EmbedRequest& req) const { return quint64(mtimeOf(req.absPath)); }

void PdfEmbedProvider::paint(QPainter* p, const QRectF& rect, const EmbedRequest& req) const
{
    const ThemePtr t = currentTheme();
    const QList<QSizeF> pages = PdfFile::pageSizes(req.absPath);
    const int page = pages.isEmpty() ? 0 : std::clamp(req.intParam(QStringLiteral("page"), 1), 1, int(pages.size())) - 1;
    p->setRenderHint(QPainter::Antialiasing);
    const double radius = themeRadiusS();
    if (dynamic_cast<QPagedPaintDevice*>(p->device()) && !pages.isEmpty()) {
        // @media print: only the page, bordered and rounded (no toolbar)
        std::shared_ptr<PdfFile> f = PdfFile::open(req.absPath);
        const QSizeF pts = pages[page];
        QSizeF s(rect.width(), rect.width() * pts.height() / pts.width());
        if (s.height() > rect.height())
            s = QSizeF(rect.height() * pts.width() / pts.height(), rect.height());
        const QRectF r(rect.topLeft(), s);
        const QImage img = f->document()->render(page, (s * 2).toSize());
        QPainterPath clip;
        clip.addRoundedRect(r, radius, radius);
        p->save();
        p->setClipPath(clip);
        p->fillRect(r, Qt::white);
        p->drawImage(r, img);
        p->restore();
        p->setPen(QPen(t->color(QStringLiteral("--background-modifier-border")), 1));
        p->setBrush(Qt::NoBrush);
        p->drawPath(clip);
        return;
    }
    QPainterPath frame;
    frame.addRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p->fillPath(frame, t->color(QStringLiteral("--pdf-background"), t->color(QStringLiteral("--background-primary"))));
    double top = rect.top() + 1;
    if (req.display == EmbedRequest::Display::Inline) {
        const QRectF bar(rect.left() + 1, top, rect.width() - 2, PdfViewer::kToolbarHeight);
        p->fillRect(bar, t->color(QStringLiteral("--background-primary")));
        p->fillRect(QRectF(bar.left(), bar.bottom() - 1, bar.width(), 1), t->color(QStringLiteral("--background-modifier-border")));
        top = bar.bottom();
    }
    if (!pages.isEmpty()) {
        const QSizeF pts = pages[page];
        const double inner = rect.width() - 2 - (pages.size() > 1 ? kScrollbar : 0);
        const QRectF pr(rect.left() + 1, top, inner, inner * pts.height() / pts.width());
        p->save();
        p->setClipRect(QRectF(rect.left() + 1, top, rect.width() - 2, rect.bottom() - 1 - top));
        p->fillRect(pr, Qt::white);
        if (std::shared_ptr<PdfFile> f = PdfFile::find(req.absPath)) {
            const QImage img = f->anyImage(page, false);
            if (!img.isNull())
                p->drawImage(pr, img);
        }
        p->restore();
    }
    p->setPen(QPen(t->color(QStringLiteral("--background-modifier-border")), 1));
    p->setBrush(Qt::NoBrush);
    p->drawPath(frame);
}

QWidget* PdfEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const
{
    return new PdfEmbedWidget(context, parent);
}

// ---------------------------------------------------------------------------------------------
// Audio / video
// ---------------------------------------------------------------------------------------------
namespace {
MediaPlayerWidget* makePlayer(MediaPlayerWidget::Kind kind, const EmbedContext& context, QWidget* parent)
{
    auto* w = new MediaPlayerWidget(kind, parent);
    w->setObjectName(kind == MediaPlayerWidget::Kind::Audio ? QStringLiteral("ocAudioEmbed") : QStringLiteral("ocVideoEmbed"));
    w->setSource(context.request.absPath);
    const QString path = context.request.absPath;
    EmbedFileWatcher::instance().watch(path, w);
    QObject::connect(&EmbedFileWatcher::instance(), &EmbedFileWatcher::fileChanged, w, [w, path](const QString& changed) {
        if (changed != path)
            return;
        MediaController* c = w->controller();
        const qint64 pos = c->position();
        c->setSource(path);
        c->seek(pos);
    });
    // its own metadata may disagree with the probe (e.g. the file changed): re-lay out the embed
    QObject::connect(w, &MediaPlayerWidget::naturalSizeChanged, w, [path](QSize s) {
        const MediaInfo info = MediaProbe::instance().info(path, false);
        if (info.known && info.size != s && !s.isEmpty()) {
            MediaProbe::instance().forget(path);
            MediaProbe::instance().info(path);
        }
    });
    return w;
}
} // namespace

QSizeF AudioEmbedProvider::size(const EmbedRequest& req, qreal maxWidth) const
{
    const double w = req.display == EmbedRequest::Display::Popover ? std::min<qreal>(kPopoverWidth, maxWidth) : maxWidth;
    return QSizeF(std::max(80.0, w), MediaPlayerWidget::kAudioHeight); // audio { width: 100%; height: 42px }
}

void AudioEmbedProvider::paint(QPainter* p, const QRectF& rect, const EmbedRequest& req) const
{
    const MediaInfo info = MediaProbe::instance().info(req.absPath, false);
    MediaControlBar::paintStatic(p, rect, MediaControlBar::Style::Audio, info.duration);
}

QWidget* AudioEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const
{
    return makePlayer(MediaPlayerWidget::Kind::Audio, context, parent);
}

QSizeF VideoEmbedProvider::size(const EmbedRequest& req, qreal maxWidth) const
{
    const bool popover = req.display == EmbedRequest::Display::Popover;
    const double maxW = popover ? std::min<qreal>(kPopoverWidth, maxWidth) : maxWidth;
    const MediaInfo info = MediaProbe::instance().info(req.absPath);
    if (info.known && info.valid && !info.hasVideo)
        return QSizeF(maxW, MediaPlayerWidget::kAudioHeight); // an audio-only file: <audio>
    if (!info.known || !info.valid || info.size.isEmpty())
        return fitVideo(QSize(300, 150), maxW, popover ? kPopoverHeight : 0); // <video> before metadata
    return fitVideo(info.size, maxW, popover ? kPopoverHeight : 0); // max-width: 100%
}

quint64 VideoEmbedProvider::signature(const EmbedRequest& req) const
{
    const MediaInfo info = MediaProbe::instance().info(req.absPath, false);
    return qHashMulti(0, info.known, info.hasVideo, info.size.width(), info.size.height(), mtimeOf(req.absPath));
}

void VideoEmbedProvider::paint(QPainter* p, const QRectF& rect, const EmbedRequest& req) const
{
    const MediaInfo info = MediaProbe::instance().info(req.absPath, false);
    if (info.known && info.valid && !info.hasVideo) {
        MediaControlBar::paintStatic(p, rect, MediaControlBar::Style::Audio, info.duration);
        return;
    }
    p->fillRect(rect, Qt::black);
    MediaControlBar::paintStatic(p, QRectF(rect.left(), rect.bottom() - std::min(rect.height(), 56.0), rect.width(), std::min(rect.height(), 56.0)),
                                 MediaControlBar::Style::Video, info.duration);
}

QWidget* VideoEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const
{
    return makePlayer(MediaPlayerWidget::Kind::Video, context, parent);
}

} // namespace oc::media
