#include "ui/views/media/media_views.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/shell/main_window.h"
#include "ui/views/media/image_view.h"
#include "ui/views/media/media_embeds.h"
#include "ui/views/media/media_view.h"
#include "ui/views/media/pdf_view.h"
#include "ui/views/media/pdf_viewer.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDir>
#include <QJsonObject>

namespace oc {

namespace media {

// Obsidian 1.13's extension lists (hb, pb, db, fb).
QStringList imageExtensions()
{
    return {QStringLiteral("bmp"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
}

QStringList audioExtensions()
{
    return {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"), QStringLiteral("3gp"),
            QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("oga"), QStringLiteral("opus")};
}

QStringList videoExtensions()
{
    return {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv"), QStringLiteral("mov"), QStringLiteral("mkv")};
}

QStringList pdfExtensions() { return {QStringLiteral("pdf")}; }

namespace {
// The session / workspace a widget belongs to: its view's, or its window's (popovers).
Workspace* workspaceOf(QWidget* w)
{
    for (QWidget* p = w; p; p = p->parentWidget()) {
        if (auto* v = qobject_cast<View*>(p); v && v->workspace())
            return v->workspace();
        if (auto* mw = qobject_cast<MainWindow*>(p); mw && mw->workspace())
            return mw->workspace();
    }
    return nullptr;
}

void openEmbedded(QWidget* from, const EmbedRequest& req, const QString& mode)
{
    Workspace* ws = workspaceOf(from);
    VaultSession* s = ws ? ws->session() : nullptr;
    if (!s || !s->vault())
        return;
    const QString path = QDir(s->root()).relativeFilePath(req.absPath);
    TFile* file = s->vault()->file(path);
    if (!file)
        return;
    QJsonObject eState;
    if (!req.subpath.isEmpty())
        eState.insert(QStringLiteral("subpath"), req.subpath);
    const Workspace::NewLeaf leaf = mode == u"tab" ? Workspace::NewLeaf::Tab : mode == u"split" ? Workspace::NewLeaf::SplitRight
                                                                                                 : Workspace::NewLeaf::No;
    ws->openFile(file, leaf, eState);
}
} // namespace

void registerMediaEmbeds()
{
    EmbedRegistry& r = EmbedRegistry::instance();
    if (!r.isExtensionRegistered(QStringLiteral("pdf")))
        r.registerExtensions(pdfExtensions(), std::make_shared<PdfEmbedProvider>());
    if (!r.isExtensionRegistered(QStringLiteral("mp3")))
        r.registerExtensions(audioExtensions(), std::make_shared<AudioEmbedProvider>());
    if (!r.isExtensionRegistered(QStringLiteral("mp4")))
        r.registerExtensions(videoExtensions(), std::make_shared<VideoEmbedProvider>());
    r.setOpener(&openEmbedded);
}

} // namespace media

void registerMediaViews(ViewRegistry& registry)
{
    using namespace media;
    registry.registerView(QStringLiteral("image"), [](WorkspaceLeaf* leaf) -> View* { return new ImageView(leaf); });
    registry.registerExtensions(imageExtensions(), QStringLiteral("image"));
    registry.registerView(QStringLiteral("audio"), [](WorkspaceLeaf* leaf) -> View* { return new MediaFileView(MediaFileView::Kind::Audio, leaf); });
    registry.registerExtensions(audioExtensions(), QStringLiteral("audio"));
    registry.registerView(QStringLiteral("video"), [](WorkspaceLeaf* leaf) -> View* { return new MediaFileView(MediaFileView::Kind::Video, leaf); });
    registry.registerExtensions(videoExtensions(), QStringLiteral("video"));
    registry.registerView(QStringLiteral("pdf"), [](WorkspaceLeaf* leaf) -> View* { return new PdfView(leaf); });
    registry.registerExtensions(pdfExtensions(), QStringLiteral("pdf"));
    registerMediaEmbeds();
}

} // namespace oc
