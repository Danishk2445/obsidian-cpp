#include "ui/views/markdown/note_embed_view.h"

#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/config/vault_config.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "render/theme/theme.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

#include <cmath>

namespace oc {

namespace {
constexpr int kClickSlop = 4; // px the mouse may move between press and release of a click

// ---------------------------------------------------------------------------------------------
// A session as a NoteEmbedHost
// ---------------------------------------------------------------------------------------------
class SessionEmbedHost : public NoteEmbedHost {
public:
    explicit SessionEmbedHost(VaultSession* s)
        : m_vault(s->vault())
        , m_docs(s->documents())
        , m_meta(s->metadata())
        , m_config(s->vaultConfig())
    {
    }
    std::optional<QString> vaultPath(const QString& absPath) const override
    {
        return m_vault && !absPath.isEmpty() ? m_vault->vaultPath(absPath) : std::nullopt;
    }
    QString absolutePath(const QString& vaultPath) const override { return m_vault ? m_vault->absolutePath(vaultPath) : QString(); }
    const Document* openDocument(const QString& vaultPath) const override
    {
        const Document* d = m_docs ? m_docs->find(vaultPath) : nullptr;
        return d && d->isLoaded() ? d : nullptr;
    }
    QString resolveLink(const QString& linkpath, const QString& sourcePath) const override
    {
        return m_meta ? m_meta->resolver().resolve(linkpath, sourcePath) : QString();
    }
    bool showIndentGuide() const override { return !m_config || m_config->flag(QStringLiteral("showIndentGuide")); }
    bool strictLineBreaks() const override { return m_config && m_config->flag(QStringLiteral("strictLineBreaks")); }

private:
    QPointer<Vault> m_vault;
    QPointer<DocumentManager> m_docs;
    QPointer<MetadataCache> m_meta;
    QPointer<VaultConfig> m_config;
};

// Registers the session's host while alive and turns its events into NoteEmbeds notifications.
class NoteEmbedBinder : public QObject {
public:
    explicit NoteEmbedBinder(VaultSession* s)
        : QObject(s)
        , m_host(std::make_shared<SessionEmbedHost>(s))
        , m_vault(s->vault())
    {
        setObjectName(QStringLiteral("ocNoteEmbedBinder"));
        NoteEmbeds::instance().addHost(m_host);
        m_timer.setSingleShot(true);
        m_timer.setInterval(kNoteEmbedChangeDelayMs);
        connect(&m_timer, &QTimer::timeout, this, &NoteEmbedBinder::flush);
        if (Vault* v = s->vault()) {
            connect(v, &Vault::modified, this, [this](TFile* f, bool) { schedule(f->path()); });
            connect(v, &Vault::created, this, [this](TAbstractFile* f, bool) {
                NoteEmbeds::instance().linksChanged();
                schedule(f->path());
            });
            connect(v, &Vault::deleted, this, [this](TAbstractFile* f, bool) {
                NoteEmbeds::instance().linksChanged();
                schedule(f->path());
            });
            connect(v, &Vault::renamed, this, [this](TAbstractFile* f, const QString& oldPath, bool) {
                NoteEmbeds::instance().linksChanged();
                schedule(oldPath);
                schedule(f->path());
            });
        }
        if (DocumentManager* dm = s->documents()) {
            connect(dm, &DocumentManager::documentOpened, this, [this](Document* d) {
                watch(d);
                schedule(d->path());
            });
            connect(dm, &DocumentManager::documentClosed, this, [this](const QString& path) { schedule(path); });
            connect(dm, &DocumentManager::documentRenamed, this, [this](Document* d, const QString& oldPath) {
                schedule(oldPath);
                schedule(d->path());
            });
            for (Document* d : dm->documents())
                watch(d);
        }
        if (MetadataCache* mc = s->metadata())
            connect(mc, &MetadataCache::resolved, this, [] { NoteEmbeds::instance().linksChanged(); });
        if (VaultConfig* cfg = s->vaultConfig()) {
            // rendering options: every cached layout is rebuilt (like a link change) and the live embeds repaint
            connect(cfg, &VaultConfig::configChanged, this, [](const QString& key) {
                if (key == u"showIndentGuide" || key == u"strictLineBreaks")
                    NoteEmbeds::instance().linksChanged();
            });
        }
    }
    ~NoteEmbedBinder() override { NoteEmbeds::instance().removeHost(m_host.get()); }

private:
    void watch(Document* d)
    {
        // unsaved edits show in the embeds of the note (after a short pause)
        if (!d || m_watched.contains(d))
            return;
        m_watched.insert(d);
        connect(d, &Document::changed, this, [this, d] { schedule(d->path()); });
        connect(d, &QObject::destroyed, this, [this, d] { m_watched.remove(d); });
    }
    void schedule(const QString& vaultPath)
    {
        if (!m_vault || vaultPath.isEmpty())
            return;
        m_pending.insert(m_vault->absolutePath(vaultPath));
        m_timer.start();
    }
    void flush()
    {
        const QSet<QString> paths = std::exchange(m_pending, {});
        for (const QString& p : paths)
            NoteEmbeds::instance().noteChanged(p);
    }

    std::shared_ptr<SessionEmbedHost> m_host;
    QPointer<Vault> m_vault;
    QSet<QString> m_pending;
    QSet<Document*> m_watched;
    QTimer m_timer;
};

HoverPopover* popoverOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (auto* p = qobject_cast<HoverPopover*>(w))
            return p;
    return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// NoteEmbedWidget
// ---------------------------------------------------------------------------------------------
NoteEmbedWidget::NoteEmbedWidget(const EmbedContext& context, QWidget* parent)
    : QWidget(parent)
    , m_ctx(context)
{
    setObjectName(QStringLiteral("ocNoteEmbed"));
    setAttribute(Qt::WA_OpaquePaintEvent); // (the note's background: the static rendering under it is skipped)
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    // .markdown-embed-link: --icon-s in a 24px box at top 4px / end 4px, radius-s
    m_open = new IconButton(QStringLiteral("maximize-2"), tr("Open link"), this);
    m_open->setObjectName(QStringLiteral("ocNoteEmbedOpen"));
    m_open->setIconSize(16);
    m_open->setStrokeWidth(2);
    m_open->setButtonSize(QSize(24, 24));
    m_open->resize(24, 24);
    m_open->setRadius(currentTheme() ? currentTheme()->px(QStringLiteral("--radius-s"), -1, 4) : 4);
    m_open->setCursor(Qt::PointingHandCursor);
    m_open->hide();
    connect(m_open, &QAbstractButton::clicked, this, [this] {
        NoteEmbedLayout::Hit h;
        h.type = NoteEmbedLayout::Hit::Type::OpenIcon;
        const std::shared_ptr<NoteEmbedLayout> l = embedLayout();
        h.linktext = l->vaultPath().isEmpty() ? QString() : l->vaultPath() + request().subpath;
        activate(h, QGuiApplication::keyboardModifiers());
    });
    m_bar = new QScrollBar(Qt::Vertical, this);
    m_bar->setObjectName(QStringLiteral("ocNoteEmbedScrollBar"));
    m_bar->setCursor(Qt::ArrowCursor);
    m_bar->hide();
    connect(m_bar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncingBar)
            setScroll(v);
    });
    connect(&NoteEmbeds::instance(), &NoteEmbeds::changed, this, [this](const QString& absPath) {
        if (absPath.isEmpty() || absPath == request().absPath || embedLayout()->dependsOn(absPath)) {
            updateChrome();
            update();
        }
    });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateChrome();
            update();
        });
    updateChrome();
}

NoteEmbedWidget::~NoteEmbedWidget()
{
    if (!m_hoverLink.isEmpty())
        if (PagePreview* pp = PagePreview::of(MainWindow::of(window())))
            pp->onLinkUnhover(this);
}

std::shared_ptr<NoteEmbedLayout> NoteEmbedWidget::embedLayout() const
{
    return NoteEmbeds::instance().layout(m_ctx.request, std::max(1, width()));
}

NoteEmbedLayout::Hit NoteEmbedWidget::hitAt(const QPoint& pos) const { return embedLayout()->hitTest(QPointF(pos), m_scroll); }

void NoteEmbedWidget::setScroll(qreal y)
{
    const std::shared_ptr<NoteEmbedLayout> l = embedLayout();
    l->ensureLaidOut(y);
    y = std::clamp<qreal>(std::round(y), 0, l->maxScroll());
    if (y == m_scroll)
        return;
    m_scroll = y;
    updateChrome();
    update();
}

void NoteEmbedWidget::updateChrome()
{
    const std::shared_ptr<NoteEmbedLayout> l = embedLayout();
    const bool note = l->kind() == NoteEmbedLayout::Kind::Note && !l->iconRect().isEmpty();
    m_open->setVisible(note);
    if (note) {
        m_open->move(l->iconRect().topLeft().toPoint());
        m_open->raise();
    }
    m_scroll = std::clamp<qreal>(m_scroll, 0, l->maxScroll());
    if (l->scrollable()) {
        const QRectF cr = l->contentRect();
        const int w = std::max(8, m_bar->sizeHint().width());
        m_syncingBar = true;
        m_bar->setGeometry(int(std::round(cr.right())) - w, int(std::round(cr.top())), w, int(std::round(cr.height())));
        m_bar->setRange(0, int(std::ceil(l->maxScroll())));
        m_bar->setPageStep(int(cr.height()));
        m_bar->setSingleStep(24);
        m_bar->setValue(int(m_scroll));
        m_syncingBar = false;
        m_bar->show();
        m_open->raise();
    } else {
        m_bar->hide();
    }
}

Workspace* NoteEmbedWidget::workspace() const
{
    for (QWidget* p = const_cast<NoteEmbedWidget*>(this); p; p = p->parentWidget()) {
        if (auto* v = qobject_cast<View*>(p); v && v->workspace())
            return v->workspace();
        if (auto* mw = p->isWindow() ? MainWindow::of(p) : nullptr; mw && mw->workspace())
            return mw->workspace();
    }
    return nullptr;
}

void NoteEmbedWidget::activate(const NoteEmbedLayout::Hit& hit, Qt::KeyboardModifiers mods, Qt::MouseButton button)
{
    using Type = NoteEmbedLayout::Hit::Type;
    const bool newTab = (mods & (Qt::ControlModifier | Qt::MetaModifier)) || button == Qt::MiddleButton;
    const Workspace::NewLeaf leaf = newTab ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No;
    auto follow = [&](const QString& linktext, int kind, const QString& sourcePath) {
        Workspace* ws = workspace();
        if (!ws || linktext.isEmpty())
            return;
        // a link followed from a hover preview closes the previews (like the preview's own links)
        if (popoverOf(this))
            if (PagePreview* pp = PagePreview::of(MainWindow::of(window())))
                pp->closeAll();
        MarkdownView::openLinkText(ws, linktext, kind, sourcePath, leaf);
    };
    // the note showing this embed (the source of the embed's own link)
    auto hostPath = [this] {
        auto* v = qobject_cast<EditorView*>(m_ctx.view.data());
        return v && v->document() ? v->document()->path() : QString();
    };
    switch (hit.type) {
    case Type::Link:
        if (hit.linkKind == 4) { // #tag: the note's tag handler (search)
            if (auto* v = qobject_cast<EditorView*>(m_ctx.view.data()))
                emit v->linkActivated(hit.linktext, 4, newTab);
            return;
        }
        follow(hit.linktext, hit.linkKind, hit.sourcePath);
        return;
    case Type::OpenIcon:
        follow(hit.linktext, 0, hit.linktext.section(u'#', 0, 0));
        return;
    case Type::Box: // generic box: open the note; "not created yet": create it
        follow(hit.linktext, 0, hit.sourcePath.isEmpty() ? hostPath() : hit.sourcePath);
        return;
    case Type::Content:
        // Live Preview: a click on the embed selects its markdown (Obsidian's selectElement)
        if (property("ocEmbedEditable").toBool() && m_ctx.revealSource)
            m_ctx.revealSource();
        return;
    case Type::None:
        return;
    }
}

void NoteEmbedWidget::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    const ThemePtr t = currentTheme();
    p.fillRect(e->rect(), t ? t->color(QStringLiteral("--background-primary")) : palette().color(QPalette::Base));
    p.setRenderHint(QPainter::TextAntialiasing);
    const std::shared_ptr<NoteEmbedLayout> l = embedLayout();
    if (m_scroll > l->maxScroll())
        m_scroll = l->maxScroll();
    l->paint(&p, QPointF(0, 0), m_scroll, QRectF(e->rect()), false, m_boxHover);
}

void NoteEmbedWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    updateChrome();
}

void NoteEmbedWidget::updateHover(const QPoint& pos)
{
    using Type = NoteEmbedLayout::Hit::Type;
    const NoteEmbedLayout::Hit h = pos.x() >= 0 ? hitAt(pos) : NoteEmbedLayout::Hit{};
    setCursor(h.type == Type::Link || h.type == Type::Box ? Qt::PointingHandCursor : Qt::ArrowCursor);
    const bool boxHover = h.type == Type::Box && embedLayout()->kind() != NoteEmbedLayout::Kind::Note; // (the embed's own box)
    if (boxHover != m_boxHover) {
        m_boxHover = boxHover;
        update();
    }
    const QString key = h.type == Type::Link ? QString::number(h.linkKind) + u':' + h.sourcePath + u':' + h.linktext : QString();
    if (key == m_hoverLink)
        return;
    m_hoverLink = key;
    PagePreview* pp = PagePreview::of(MainWindow::of(window()));
    if (!pp)
        return;
    if (key.isEmpty() || h.linkKind > 2) {
        pp->onLinkUnhover(this);
        return;
    }
    // Obsidian's embeds are their own hover source "preview": no Mod needed
    const QRect r = h.rect.toAlignedRect();
    const QString linktext = h.linkKind == 2 ? QUrl::fromPercentEncoding(h.linktext.toUtf8()) : h.linktext;
    pp->onLinkHover(QStringLiteral("preview"), linktext, h.sourcePath, QRect(mapToGlobal(r.topLeft()), r.size()), this);
}

void NoteEmbedWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() == Qt::NoButton)
        updateHover(e->position().toPoint());
    e->accept(); // (the note's view under it would report its own link: the embed's markdown)
}

void NoteEmbedWidget::mousePressEvent(QMouseEvent* e)
{
    m_pressPos = e->position().toPoint();
    m_pressButton = e->button();
    e->accept();
}

void NoteEmbedWidget::mouseReleaseEvent(QMouseEvent* e)
{
    const bool click = e->button() == m_pressButton && (e->position().toPoint() - m_pressPos).manhattanLength() <= kClickSlop;
    m_pressButton = Qt::NoButton;
    if (!click || (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton)) {
        e->accept();
        return;
    }
    const NoteEmbedLayout::Hit h = hitAt(e->position().toPoint());
    if (e->button() == Qt::MiddleButton && h.type == NoteEmbedLayout::Hit::Type::Content)
        return;
    activate(h, e->modifiers(), e->button());
    e->accept();
}

void NoteEmbedWidget::leaveEvent(QEvent* e)
{
    updateHover(QPoint(-1, -1));
    QWidget::leaveEvent(e);
}

void NoteEmbedWidget::wheelEvent(QWheelEvent* e)
{
    const std::shared_ptr<NoteEmbedLayout> l = embedLayout();
    if (!l->scrollable()) {
        e->ignore(); // the note scrolls
        return;
    }
    qreal dy = 0;
    if (!e->pixelDelta().isNull())
        dy = -e->pixelDelta().y();
    else
        dy = -e->angleDelta().y() / 120.0 * QApplication::wheelScrollLines() * 24;
    const qreal next = std::clamp<qreal>(m_scroll + dy, 0, l->maxScroll());
    if (std::abs(next - m_scroll) < 0.5) {
        e->ignore(); // at its end: scroll the note (scroll chaining)
        return;
    }
    setScroll(next);
    updateHover(mapFromGlobal(QCursor::pos()));
    e->accept();
}

bool NoteEmbedWidget::event(QEvent* e)
{
    if (e->type() == QEvent::DynamicPropertyChange)
        update();
    return QWidget::event(e);
}

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------
QWidget* NoteEmbedViewProvider::createWidget(const EmbedContext& context, QWidget* parent) const { return new NoteEmbedWidget(context, parent); }

void registerNoteEmbeds(VaultSession* session)
{
    EmbedRegistry& r = EmbedRegistry::instance();
    if (!dynamic_cast<NoteEmbedViewProvider*>(r.provider(QStringLiteral("md")).get()))
        r.registerExtensions({QStringLiteral("md")}, std::make_shared<NoteEmbedViewProvider>());
    if (session && session->vault() && !session->findChild<QObject*>(QStringLiteral("ocNoteEmbedBinder"), Qt::FindDirectChildrenOnly))
        new NoteEmbedBinder(session);
}

} // namespace oc
