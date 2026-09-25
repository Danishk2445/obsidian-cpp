#include "ui/views/media/media_view.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/views/media/media_player.h"
#include "ui/views/media/media_views.h"

#include <QPainter>

namespace oc::media {

namespace {
constexpr int kPadX = 12;      // .view-content padding --size-4-3
constexpr int kPadTop = 12;
constexpr int kPadBottom = 32; // --size-4-8
} // namespace

MediaFileView::MediaFileView(Kind kind, WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_kind(kind)
    , m_player(new MediaPlayerWidget(kind == Kind::Audio ? MediaPlayerWidget::Kind::Audio : MediaPlayerWidget::Kind::Video, this))
{
    setObjectName(kind == Kind::Audio ? QStringLiteral("ocAudioView") : QStringLiteral("ocVideoView"));
    setFocusProxy(m_player);
    connect(m_player, &MediaPlayerWidget::naturalSizeChanged, this, &MediaFileView::placePlayer);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

MediaFileView::~MediaFileView() = default;

QString MediaFileView::viewType() const { return m_kind == Kind::Audio ? QStringLiteral("audio") : QStringLiteral("video"); }

// Obsidian: the audio view sets lucide-file-audio; the video view keeps the FileView default.
QString MediaFileView::icon() const { return m_kind == Kind::Audio ? QStringLiteral("file-audio") : QStringLiteral("file"); }

bool MediaFileView::canAcceptExtension(const QString& extension) const
{
    return (m_kind == Kind::Audio ? audioExtensions() : videoExtensions()).contains(extension.toLower());
}

void MediaFileView::loadFile(TFile* file)
{
    m_file = file;
    VaultSession* s = session();
    if (s && s->vault() && !m_modifiedConn) {
        m_modifiedConn = connect(s->vault(), &Vault::modified, this, [this](TFile* f, bool) {
            if (!f || f != m_file)
                return;
            MediaController* c = m_player->controller();
            const qint64 pos = c->position();
            c->setSource(c->source());
            c->seek(pos);
        });
    }
    m_player->setSource(file && s && s->vault() ? s->vault()->absolutePath(file->path()) : QString());
    placePlayer();
    emit titleChanged();
}

void MediaFileView::onRename(TFile* file, const QString& oldPath)
{
    if (VaultSession* s = session(); s && s->vault() && file == m_file) {
        MediaController* c = m_player->controller();
        const qint64 pos = c->position();
        const bool playing = c->state() == MediaController::State::Playing;
        c->setSource(s->vault()->absolutePath(file->path()));
        c->seek(pos);
        if (playing)
            c->play();
    }
    FileView::onRename(file, oldPath);
}

void MediaFileView::onClose() { m_player->controller()->pause(); }

void MediaFileView::focusContent() { m_player->setFocus(); }

void MediaFileView::placePlayer()
{
    const int availW = std::max(1, width() - 2 * kPadX);
    int w = availW;
    int h = MediaPlayerWidget::kAudioHeight;
    if (m_player->kind() == MediaPlayerWidget::Kind::Video) {
        const QSize n = m_player->naturalSize();
        w = n.isEmpty() ? std::min(availW, 300) : std::min(availW, n.width()); // video { max-width: 100% }
        h = m_player->heightForWidth(w);
        if (n.isEmpty())
            h = 150; // an unloaded <video>: 300×150
    }
    // the view content scrolls in Obsidian; here a tall video is fitted to the height instead
    const int availH = std::max(1, height() - kPadTop - kPadBottom);
    if (h > availH && m_player->kind() == MediaPlayerWidget::Kind::Video) {
        w = std::max(1, w * availH / h);
        h = availH;
    }
    m_player->setGeometry((width() - w) / 2, kPadTop, w, h);
}

void MediaFileView::resizeEvent(QResizeEvent* e)
{
    FileView::resizeEvent(e);
    placePlayer();
}

void MediaFileView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-primary")));
}

} // namespace oc::media
