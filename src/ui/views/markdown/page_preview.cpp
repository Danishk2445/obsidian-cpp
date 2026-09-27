#include "ui/views/markdown/page_preview.h"

#include "core/config/config_store.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/subpath.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "editor/embeds/embed_registry.h"
#include "editor/live_preview_decorator.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QCursor>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QVBoxLayout>

namespace oc {

using namespace Qt::StringLiterals;

namespace {
constexpr int kWidth = 450;  // --popover-width
constexpr int kHeight = 400; // --popover-height

// Popovers show in the window of the hovered link (the main window or a pop-out; the mouse may hover
// a window that doesn't have the focus).
OverlayLayer* layerFor(MainWindow* window, const QWidget* hoverParent)
{
    if (OverlayLayer* l = OverlayLayer::of(hoverParent))
        return l;
    return window ? window->ownOverlay() : nullptr;
}

bool isImageExt(const QString& ext)
{
    static const QStringList exts = {u"bmp"_s, u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"svg"_s, u"webp"_s, u"avif"_s};
    return exts.contains(ext.toLower());
}

HoverPopover* popoverOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (auto* p = qobject_cast<HoverPopover*>(w))
            return p;
    return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// HoverPopover
// ---------------------------------------------------------------------------------------------
HoverPopover::HoverPopover(OverlayLayer* layer, VaultSession* session, TFile* file, const QString& subpath, QWidget* hoverParent)
    : QWidget(layer)
    , m_session(session)
    , m_file(file)
    , m_subpath(subpath)
    , m_hoverParent(hoverParent)
{
    setObjectName(u"ocHoverPopover"_s);
    setAttribute(Qt::WA_TranslucentBackground);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(1, 1, 1, 1);
    v->setSpacing(0);
    if (std::shared_ptr<EmbedProvider> provider = file->isMarkdown() ? nullptr : EmbedRegistry::instance().provider(file->extension())) {
        // Obsidian's popover embeds: .pdf-embed 450x400 (no toolbar), .video-embed, .audio-embed
        EmbedContext ctx;
        ctx.request = EmbedRequest::fromLink(session->vault()->absolutePath(file->path()), file->path(), subpath);
        ctx.request.display = EmbedRequest::Display::Popover;
        m_embedSize = provider->size(ctx.request, kWidth).toSize();
        m_embed = provider->createWidget(ctx, this);
        if (m_embed)
            v->addWidget(m_embed);
    } else if (isImageExt(file->extension())) {
        m_image = new QLabel(this);
        m_image->setAlignment(Qt::AlignCenter);
        QImageReader reader(session->vault()->absolutePath(file->path()));
        reader.setAutoTransform(true);
        QSize sz = reader.size();
        if (sz.isValid())
            reader.setScaledSize(sz.scaled(QSize(kWidth, kHeight).boundedTo(sz), Qt::KeepAspectRatio));
        m_image->setPixmap(QPixmap::fromImage(reader.read()));
        v->addWidget(m_image);
    } else {
        DocumentManager* dm = session->documents();
        Document* d = dm ? dm->acquire(file->path()) : nullptr;
        m_editor = new EditorView(this);
        m_editor->setObjectName(u"ocHoverPopoverEditor"_s);
        m_editor->setMode(EditorView::Mode::Reading);
        m_editor->setReadableLineLength(false);
        const ThemePtr t = currentTheme();
        const double margin = t->px(u"--file-margins"_s, -1, 32);
        m_editor->setMargins(margin, margin, margin);
        m_editor->setFocusPolicy(Qt::ClickFocus);
        if (d) {
            QPointer<DocumentManager> guard = dm;
            m_doc = std::shared_ptr<Document>(d, [guard](Document* doc) {
                if (guard)
                    guard->release(doc);
            });
            // embeds and unresolved-link styling inside the preview resolve from the previewed note
            if (LivePreviewDecorator* lp = m_editor->livePreview(); lp && session->metadata()) {
                QPointer<VaultSession> s = session;
                const QString source = file->path();
                lp->isResolved = [s, source](const QString& target) {
                    const QString path = LinkResolver::splitSubpath(target).first;
                    return !s || !s->metadata() || path.isEmpty() || !s->metadata()->resolver().resolve(path, source).isEmpty();
                };
                lp->resolveAttachment = [s, source](const QString& linkpath) -> QString {
                    if (!s || !s->metadata() || !s->vault())
                        return {};
                    const QString dest = s->metadata()->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, source);
                    return dest.isEmpty() ? QString() : s->vault()->absolutePath(dest);
                };
            }
            // a footnote ("#[^1]") shows its definition's content, a section no footnotes section
            if (LivePreviewDecorator* lp = m_editor->livePreview())
                lp->setFootnoteMode(subpath.startsWith(QLatin1String("#[^")) ? LivePreviewDecorator::FootnoteMode::Definition
                                    : subpath.isEmpty()                      ? LivePreviewDecorator::FootnoteMode::Document
                                                                             : LivePreviewDecorator::FootnoteMode::Section);
            m_editor->setDocument(m_doc);
            if (!subpath.isEmpty() && session->metadata()) {
                if (const md::CachedMetadata* meta = session->metadata()->fileCache(file->path())) {
                    if (const auto r = resolveSubpath(*meta, subpath))
                        m_editor->setVisibleLineRange(r->startLine, r->endLine);
                }
            }
        }
        v->addWidget(m_editor);
    }
    hide();
}

HoverPopover::~HoverPopover()
{
    if (m_editor)
        m_editor->setDocument(nullptr);
    m_doc.reset();
}

void HoverPopover::placeNear(const QRect& anchor)
{
    auto* layer = qobject_cast<QWidget*>(parent());
    if (!layer)
        return;
    m_anchor = anchor;
    const QRect bounds = layer->rect().adjusted(8, 8, -8, -8);
    int w = std::min(kWidth + 2, bounds.width());
    int h = kHeight + 2;
    if (m_image) {
        const QSize px = m_image->pixmap().size();
        w = std::min(std::max(px.width(), 30) + 2, bounds.width());
        h = std::max(px.height(), 30) + 2;
    } else if (m_embed) {
        w = std::min(m_embedSize.width() + 2, bounds.width());
        h = m_embedSize.height() + 2;
    }
    // measure the note at the popover's width, then fit the height to it
    setGeometry(anchor.left(), anchor.bottom() + 4, w, h);
    if (m_editor) {
        layout()->activate();
        m_editor->resize(w - 2, kHeight);
        h = std::clamp(int(std::ceil(m_editor->documentHeight())) + 2, 30, kHeight + 2);
    }
    int x = std::clamp(anchor.left(), bounds.left(), std::max(bounds.left(), bounds.right() - w));
    int y = anchor.bottom() + 4;
    if (y + h > bounds.bottom() && anchor.top() - 4 - h >= bounds.top())
        y = anchor.top() - 4 - h;
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() - h));
    setGeometry(x, y, w, h);
    show();
    raise();
    if (auto* ol = qobject_cast<OverlayLayer*>(layer))
        ol->refresh();
}

void HoverPopover::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal radius = t->px(u"--radius-m"_s, -1, 8);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.fillPath(path, t->color(u"--background-primary"_s));
    p.setPen(QPen(t->color(u"--background-modifier-border"_s), 1));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------
// PagePreview
// ---------------------------------------------------------------------------------------------
PagePreview* PagePreview::of(MainWindow* window)
{
    if (!window)
        return nullptr;
    if (auto* p = window->findChild<PagePreview*>(QString(), Qt::FindDirectChildrenOnly))
        return p;
    return new PagePreview(window);
}

PagePreview::PagePreview(MainWindow* window)
    : QObject(window)
    , m_window(window)
    , m_windowObject(window)
{
    m_showTimer.setSingleShot(true);
    m_showTimer.setInterval(300);
    connect(&m_showTimer, &QTimer::timeout, this, &PagePreview::showPending);
    m_poll.setInterval(50);
    connect(&m_poll, &QTimer::timeout, this, &PagePreview::checkMouse);
    qApp->installEventFilter(this);
}

void PagePreview::setDelays(int showMs, int hideMs)
{
    m_showTimer.setInterval(showMs);
    m_hideMs = hideMs;
}

bool PagePreview::requiresMod(const QString& source) const
{
    VaultSession* s = m_window ? m_window->session() : nullptr;
    const QJsonObject cfg = s && s->config() ? s->config()->object(u"page-preview"_s) : QJsonObject();
    if (cfg.contains(source))
        return cfg.value(source).toBool();
    return source != u"preview"; // Obsidian's defaults: only the reading view previews without Mod
}

QList<HoverPopover*> PagePreview::popovers() const
{
    QList<HoverPopover*> out;
    for (const auto& p : m_popovers)
        if (p)
            out << p;
    return out;
}

void PagePreview::attachEditor(EditorView* editor, std::function<QString()> sourcePath, std::function<QString()> source)
{
    connect(editor, &EditorView::linkHovered, this, [this, editor, sourcePath, source](const QString& target, int kind, const QRect& rect) {
        if (target.isEmpty() || kind > 2) { // only notes/files: wikilinks, embeds, markdown links
            onLinkUnhover(editor);
            return;
        }
        const QRect global(editor->viewport()->mapToGlobal(rect.topLeft()), rect.size());
        onLinkHover(source(), kind == 2 ? QUrl::fromPercentEncoding(target.toUtf8()) : target, sourcePath(), global, editor);
    });
}

void PagePreview::onLinkHover(const QString& source, const QString& linktext, const QString& sourcePath, const QRect& anchorGlobal,
                              QWidget* hoverParent)
{
    // already showing this link from this place
    for (HoverPopover* p : popovers()) {
        auto* layer = qobject_cast<QWidget*>(p->parent());
        if (p->hoverParent() == hoverParent && layer && p->anchor().intersects(QRect(layer->mapFromGlobal(anchorGlobal.topLeft()), anchorGlobal.size())))
            return;
    }
    m_pending = {source, linktext, sourcePath, anchorGlobal, hoverParent, true};
    m_showTimer.stop();
    if (!requiresMod(source) || (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier))
        m_showTimer.start();
    // else: wait for Mod to be pressed while the mouse stays on the link (eventFilter)
}

void PagePreview::onLinkUnhover(QWidget* hoverParent)
{
    if (m_pending.hoverParent == hoverParent) {
        m_pending.active = false;
        m_showTimer.stop();
    }
}

void PagePreview::showPending()
{
    OverlayLayer* layer = m_window ? layerFor(m_window, m_pending.hoverParent) : nullptr;
    if (!m_pending.active || !layer)
        return;
    m_pending.active = false;
    VaultSession* s = m_window->session();
    if (!s || !s->vault() || !s->metadata())
        return;
    auto [linkpath, subpath] = LinkResolver::splitSubpath(m_pending.linktext);
    const QString dest = linkpath.isEmpty() ? m_pending.sourcePath : s->metadata()->resolver().resolve(linkpath, m_pending.sourcePath);
    TFile* file = dest.isEmpty() ? nullptr : s->vault()->file(dest);
    if (!file || !(file->isMarkdown() || isImageExt(file->extension()) || EmbedRegistry::instance().isExtensionRegistered(file->extension())))
        return;
    // a new popover replaces those that aren't its ancestors
    HoverPopover* parentPopover = popoverOf(m_pending.hoverParent);
    const QList<HoverPopover*> list = popovers();
    if (!parentPopover) {
        closeAll();
    } else if (const qsizetype i = list.indexOf(parentPopover); i >= 0 && i + 1 < list.size()) {
        closeFrom(list[i + 1]);
    }
    auto* p = new HoverPopover(layer, s, file, subpath, m_pending.hoverParent);
    m_popovers.append(p);
    if (EditorView* e = p->editor()) {
        const QString path = file->path();
        attachEditor(e, [path] { return path; }, [] { return u"preview"_s; }); // nested previews
        connect(e, &EditorView::linkActivated, this, [this, path](const QString& target, int kind, bool newLeaf) {
            Workspace* ws = m_window ? m_window->workspace() : nullptr;
            closeAll();
            MarkdownView::openLinkText(ws, target, kind, path, newLeaf);
        });
    }
    p->placeNear(QRect(layer->mapFromGlobal(m_pending.anchorGlobal.topLeft()), m_pending.anchorGlobal.size()));
    m_outside.invalidate();
    m_poll.start();
    emit popoverShown(p);
}

bool PagePreview::isInside(HoverPopover* p, const QPoint& global) const
{
    const QRect r(p->mapToGlobal(QPoint(0, 0)), p->size());
    if (r.contains(global))
        return true;
    auto* layer = qobject_cast<QWidget*>(p->parent());
    return layer && QRect(layer->mapToGlobal(p->anchor().topLeft()), p->anchor().size()).adjusted(-2, -2, 2, 6).contains(global);
}

void PagePreview::checkMouse()
{
    const QList<HoverPopover*> list = popovers();
    if (list.isEmpty()) {
        m_poll.stop();
        return;
    }
    const QPoint g = QCursor::pos();
    // the innermost popover the mouse is on (or on its link) survives with all its ancestors
    qsizetype keep = -1;
    for (qsizetype i = list.size() - 1; i >= 0; --i) {
        if (isInside(list[i], g)) {
            keep = i;
            break;
        }
    }
    if (keep == list.size() - 1) {
        m_outside.invalidate();
        return;
    }
    if (!m_outside.isValid()) {
        m_outside.start();
        return;
    }
    if (m_outside.elapsed() >= m_hideMs) {
        m_outside.invalidate();
        closeFrom(list[keep + 1]);
    }
}

void PagePreview::closeFrom(HoverPopover* p)
{
    const qsizetype i = m_popovers.indexOf(p);
    if (i < 0)
        return;
    QList<QPointer<OverlayLayer>> layers;
    for (qsizetype j = m_popovers.size() - 1; j >= i; --j) {
        if (HoverPopover* q = m_popovers[j]) {
            if (auto* layer = qobject_cast<OverlayLayer*>(q->parent()); layer && !layers.contains(layer))
                layers.push_back(layer);
            q->hide();
            q->deleteLater();
        }
        m_popovers.removeAt(j);
    }
    for (const QPointer<OverlayLayer>& layer : std::as_const(layers))
        if (layer)
            layer->refresh();
}

void PagePreview::closeAll()
{
    m_pending.active = false;
    m_showTimer.stop();
    if (!m_popovers.isEmpty())
        closeFrom(m_popovers.first());
    m_popovers.clear();
}

bool PagePreview::eventFilter(QObject* obj, QEvent* ev)
{
    switch (ev->type()) {
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Control && m_pending.active && !m_showTimer.isActive()) {
            QTimer::singleShot(0, this, &PagePreview::showPending); // Mod pressed while resting on a link
        } else if (ke->key() == Qt::Key_Escape && !m_popovers.isEmpty()) {
            closeAll();
            return true;
        }
        break;
    }
    case QEvent::MouseButtonPress: {
        if (m_popovers.isEmpty())
            break;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || !popoverOf(w))
            closeAll(); // a click elsewhere
        break;
    }
    case QEvent::WindowDeactivate:
        if (obj == m_windowObject) // identity only: the window may be mid-destruction
            closeAll();
        break;
    default:
        break;
    }
    return false;
}

} // namespace oc
