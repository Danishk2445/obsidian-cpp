#include "ui/graph/graph_view.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/util/owned_menu.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "graph/graph_settings.h"
#include "render/theme/icons.h"
#include "ui/graph/graph_canvas.h"
#include "ui/graph/graph_vault.h"
#include "ui/graph/graph_widget.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

namespace oc {

namespace {

GraphViewHooks& hooks()
{
    static GraphViewHooks h;
    return h;
}

const QString kGraphConfig = QStringLiteral("graph");

void fillLayout(QWidget* view, QWidget* content)
{
    auto* l = new QVBoxLayout(view);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(content);
}

// The file shown by a leaf, if it shows one (local graphs excluded: they are not "the active file").
TFile* leafFile(WorkspaceLeaf* leaf)
{
    if (!leaf)
        return nullptr;
    auto* fv = qobject_cast<FileView*>(leaf->view());
    if (!fv || qobject_cast<LocalGraphView*>(fv))
        return nullptr;
    return fv->file();
}

} // namespace

void setGraphViewHooks(GraphViewHooks h) { hooks() = std::move(h); }

// ---------------------------------------------------------------------------------------------
// GraphViewController: Obsidian's graph "data engine" glue shared by both views — vault graph
// rebuilds, search, focused (active) file, node clicks and the context menu.
// ---------------------------------------------------------------------------------------------
class GraphViewController : public QObject {
public:
    GraphViewController(View* view, GraphWidget* widget)
        : QObject(view)
        , m_view(view)
        , m_widget(widget)
    {
        m_rebuild.setSingleShot(true);
        m_rebuild.setInterval(100);
        connect(&m_rebuild, &QTimer::timeout, this, &GraphViewController::refresh);
        m_requery.setSingleShot(true);
        m_requery.setInterval(30);
        connect(&m_requery, &QTimer::timeout, m_widget, &GraphWidget::refreshQueries);
        connect(widget, &GraphWidget::openRequested, this, [this](const QString& id, bool newTab) {
            // Deferred: opening in the same leaf replaces (destroys) this view.
            QTimer::singleShot(0, this, [this, id, newTab] { open(id, newTab); });
        });
        connect(widget, &GraphWidget::contextMenuRequested, this, [this](const QString& id, const QPoint& pos) {
            QTimer::singleShot(0, this, [this, id, pos] { contextMenu(id, pos); });
        });
        connect(widget->canvas(), &GraphCanvas::hoverChanged, this, &GraphViewController::hover);
    }

    // Obsidian's onNodeHover: notes, attachments and the focused file get a page preview (hover
    // source "graph", Mod by default); tags and unresolved nodes don't.
    void hover(const QString& id)
    {
        Workspace* ws = m_view->workspace();
        MainWindow* window = ws ? ws->window() : nullptr;
        if (!window)
            return;
        PagePreview* preview = PagePreview::of(window);
        preview->onLinkUnhover(m_view);
        const graph::NodeKind kind = id.isEmpty() ? graph::NodeKind::Tag : kindOf(id);
        if (kind != graph::NodeKind::File && kind != graph::NodeKind::Attachment)
            return;
        GraphCanvas* c = m_widget->canvas();
        const int i = c->indexOf(id);
        if (i < 0)
            return;
        const QPointF center = c->worldToWidget(c->nodePosition(i));
        const double r = c->nodeRadius(i) * c->scale() / c->devicePixelRatioF();
        const QRect anchor = QRectF(center - QPointF(r, r), QSizeF(2 * r, 2 * r)).toAlignedRect();
        preview->onLinkHover(QStringLiteral("graph"), id, QString(), QRect(c->mapToGlobal(anchor.topLeft()), anchor.size()), m_view);
    }

    VaultSession* session() const { return m_view->session(); }

    void attach()
    {
        VaultSession* s = session();
        if (!s || m_attached)
            return;
        m_attached = true;
        m_search = new GraphVaultSearch(s, this);
        QPointer<GraphVaultSearch> search = m_search;
        m_widget->setQueryCompiler([search](const QString& q) { return search ? search->compile(q) : graph::NodePredicate{}; });
        connect(m_search, &GraphVaultSearch::updated, &m_requery, qOverload<>(&QTimer::start));
        connect(m_search, &GraphVaultSearch::searchingChanged, m_widget->controls(), &GraphControls::setSearching);
        if (MetadataCache* mc = s->metadata())
            connect(mc, &MetadataCache::resolved, &m_rebuild, qOverload<>(&QTimer::start));
        if (Vault* v = s->vault()) {
            // Attachments come and go without metadata changes.
            connect(v, &Vault::created, &m_rebuild, qOverload<>(&QTimer::start));
            connect(v, &Vault::deleted, &m_rebuild, qOverload<>(&QTimer::start));
            connect(v, &Vault::renamed, &m_rebuild, qOverload<>(&QTimer::start));
        }
        if (Workspace* ws = m_view->workspace()) {
            // Obsidian's "file-open": the focused node follows the active file.
            connect(ws, &Workspace::fileOpened, this, [this](TFile* file) { m_widget->setActiveNode(file ? file->path() : QString()); });
            connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* leaf) {
                if (TFile* f = leafFile(leaf))
                    m_widget->setActiveNode(f->path());
            });
            TFile* active = leafFile(ws->activeRootLeaf());
            if (active)
                m_widget->setActiveNode(active->path());
            else if (const QStringList recent = ws->lastOpenFiles(); !recent.isEmpty())
                m_widget->setActiveNode(recent.first());
        }
        refresh();
    }

    void refresh()
    {
        m_rebuild.stop();
        if (VaultSession* s = session())
            m_widget->setGraph(std::make_shared<graph::GraphData>(graphDataFromSession(*s)));
    }

    graph::NodeKind kindOf(const QString& id) const
    {
        for (const auto& g : {m_widget->shownGraph(), m_widget->fullGraph()}) {
            const int i = g ? g->indexOf(id) : -1;
            if (i >= 0)
                return g->node(i).kind;
        }
        return id.startsWith(QLatin1Char('#')) ? graph::NodeKind::Tag : graph::NodeKind::File;
    }

    NoticeManager* notices() const
    {
        Workspace* ws = m_view->workspace();
        MainWindow* w = ws ? ws->window() : nullptr;
        return w ? w->notices() : nullptr;
    }

    // Obsidian's onNodeClick: tags open the global search, everything else openLinkText(id) —
    // which creates the note of an unresolved link.
    void open(const QString& id, bool newTab)
    {
        VaultSession* s = session();
        Workspace* ws = m_view->workspace();
        const graph::NodeKind kind = kindOf(id);
        if (kind == graph::NodeKind::Tag) {
            if (hooks().openSearch)
                hooks().openSearch(ws, QStringLiteral("tag:") + id);
            return;
        }
        if (!s || !ws || !s->vault())
            return;
        TFile* file = s->vault()->file(id);
        if (!file && kind == graph::NodeKind::Unresolved) {
            QString path = id;
            if (!path.section(QLatin1Char('/'), -1).contains(QLatin1Char('.')))
                path += QStringLiteral(".md");
            if (!path.contains(QLatin1Char('/')))
                if (FileManager* fm = s->fileManager())
                    if (TFolder* parent = fm->newFileParent(QString()); parent && !parent->isRoot())
                        path = parent->path() + QLatin1Char('/') + path;
            QString err;
            file = s->vault()->file(path);
            if (!file)
                file = s->vault()->createFile(path, QByteArray(), &err);
            if (!file) {
                if (NoticeManager* n = notices())
                    n->show(QObject::tr("Failed to create \"%1\": %2").arg(path, err));
                return;
            }
        }
        if (file)
            ws->openFile(file, newTab ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No);
    }

    // Obsidian's onNodeRightClick: a file menu for existing files, the tag search for tags.
    void contextMenu(const QString& id, const QPoint& globalPos)
    {
        VaultSession* s = session();
        Workspace* ws = m_view->workspace();
        if (kindOf(id) == graph::NodeKind::Tag) {
            if (hooks().openSearch)
                hooks().openSearch(ws, QStringLiteral("tag:") + id);
            return;
        }
        TFile* file = s && s->vault() ? s->vault()->file(id) : nullptr;
        if (!file || !ws)
            return;
        QMenu& menu = *ownedMenu(m_view);
        QAction* title = menu.addAction(file->isMarkdown() ? file->basename() : file->name());
        title->setEnabled(false);
        menu.addSeparator();
        const QPointer<Workspace> w = ws;
        const QString path = file->path();
        auto openIn = [w, s, path](Workspace::NewLeaf mode) {
            if (w && s->vault())
                if (TFile* f = s->vault()->file(path))
                    w->openFile(f, mode);
        };
        menu.addAction(IconProvider::instance().icon(QStringLiteral("file-plus"), 16), QObject::tr("Open in new tab"),
                       [openIn] { openIn(Workspace::NewLeaf::Tab); });
        menu.addAction(IconProvider::instance().icon(QStringLiteral("separator-vertical"), 16), QObject::tr("Open to the right"),
                       [openIn] { openIn(Workspace::NewLeaf::SplitRight); });
        if (hooks().fileMenu)
            hooks().fileMenu(&menu, file, m_view->leaf());
        if (hooks().deleteFile) {
            menu.addSeparator();
            menu.addAction(IconProvider::instance().icon(QStringLiteral("trash-2"), 16), QObject::tr("Delete file"), [s, path] {
                if (s->vault())
                    if (TFile* f = s->vault()->file(path))
                        hooks().deleteFile(s, f);
            });
        }
        menu.exec(globalPos);
    }

    void copyScreenshot()
    {
        if (QClipboard* cb = QGuiApplication::clipboard())
            cb->setImage(m_widget->canvas()->grabFrame(true));
        if (NoticeManager* n = notices())
            n->show(QObject::tr("Screenshot copied to the clipboard."));
    }

private:
    View* m_view;
    GraphWidget* m_widget;
    GraphVaultSearch* m_search = nullptr;
    QTimer m_rebuild;
    QTimer m_requery;
    bool m_attached = false;
};

// ---------------------------------------------------------------------------------------------
// GraphView
// ---------------------------------------------------------------------------------------------
GraphView::GraphView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
    , m_widget(new GraphWidget(GraphWidget::Mode::Global, this))
{
    fillLayout(this, m_widget);
    m_controller = new GraphViewController(this, m_widget);
}

GraphView::~GraphView() = default;

QString GraphView::displayText() const { return tr("Graph view"); }

void GraphView::onOpen()
{
    VaultSession* s = session();
    if (s && s->config()) {
        // graph.json lives in the clone's per-vault config (never the vault's .obsidian).
        m_widget->setSettings(graph::GraphSettings::fromJson(s->config()->object(kGraphConfig)));
        QPointer<VaultSession> session = s;
        connect(m_widget, &GraphWidget::settingsChanged, this, [this, session] {
            if (session && session->config())
                session->config()->setObject(kGraphConfig, m_widget->settings().toJson(graph::GraphSettings::Variant::Global));
        });
    }
    m_controller->attach();
}

void GraphView::onClose() { m_widget->runner()->setPaused(true); }

QJsonObject GraphView::options() const { return m_widget->settings().toJson(graph::GraphSettings::Variant::Global); }

void GraphView::setOptions(const QJsonObject& options)
{
    m_widget->setSettings(graph::GraphSettings::fromJson(options));
    if (VaultSession* s = session(); s && s->config())
        s->config()->setObject(kGraphConfig, m_widget->settings().toJson(graph::GraphSettings::Variant::Global));
}

void GraphView::populatePaneMenu(QMenu* menu)
{
    menu->addAction(IconProvider::instance().icon(QStringLiteral("image"), 16), tr("Copy screenshot"), this, [this] { m_controller->copyScreenshot(); });
}

void GraphView::focusContent() { m_widget->canvas()->setFocus(); }
void GraphView::startTimelapse() { m_widget->startTimelapse(); }
void GraphView::refresh() { m_controller->refresh(); }

// ---------------------------------------------------------------------------------------------
// LocalGraphView
// ---------------------------------------------------------------------------------------------
LocalGraphView::LocalGraphView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_widget(new GraphWidget(GraphWidget::Mode::Local, this))
{
    fillLayout(this, m_widget);
    m_controller = new GraphViewController(this, m_widget);
    // Local options are part of the view state (workspace.json), like Obsidian's.
    connect(m_widget, &GraphWidget::settingsChanged, this, &View::stateChanged);
}

LocalGraphView::~LocalGraphView() = default;

QString LocalGraphView::displayText() const
{
    if (m_file)
        return tr("Graph of %1").arg(m_file->basename());
    return tr("Local graph");
}

void LocalGraphView::loadFile(TFile* file)
{
    m_file = file;
    m_widget->setLocalCenter(file ? file->path() : QString());
    emit titleChanged();
}

void LocalGraphView::onRename(TFile* file, const QString& oldPath)
{
    Q_UNUSED(oldPath);
    if (file == m_file) {
        m_controller->refresh();
        m_widget->setLocalCenter(file->path());
    }
    emit titleChanged();
    emit stateChanged();
}

void LocalGraphView::onDelete(TFile* file)
{
    if (file != m_file)
        return;
    m_file = nullptr;
    m_widget->setLocalCenter(QString());
    emit titleChanged();
}

QJsonObject LocalGraphView::state() const
{
    QJsonObject o = FileView::state();
    o.insert(QStringLiteral("options"), m_widget->settings().toJson(graph::GraphSettings::Variant::Local));
    return o;
}

void LocalGraphView::setState(const QJsonObject& state)
{
    const QJsonValue options = state.value(QStringLiteral("options"));
    if (options.isObject())
        m_widget->setSettings(graph::GraphSettings::fromJson(options.toObject()));
    FileView::setState(state);
}

void LocalGraphView::onOpen()
{
    if (WorkspaceLeaf* l = leaf())
        m_widget->setInSidebar(l->isInSidedock());
    m_controller->attach();
    if (Workspace* ws = workspace()) {
        connect(ws, &Workspace::activeLeafChanged, this, &LocalGraphView::followActiveLeaf);
        connect(ws, &Workspace::fileOpened, this, [this] {
            if (Workspace* w = workspace())
                followActiveLeaf(w->activeLeaf());
        });
        if (!m_file && property("ocFilePath").toString().isEmpty())
            followActiveLeaf(ws->activeRootLeaf());
    }
}

void LocalGraphView::onClose() { m_widget->runner()->setPaused(true); }

void LocalGraphView::followActiveLeaf(WorkspaceLeaf* leaf)
{
    // Obsidian's linked local graph shows the active note (never itself).
    if (!leaf || leaf == this->leaf())
        return;
    TFile* file = leafFile(leaf);
    if (!file || file == m_file)
        return;
    loadFile(file);
    emit stateChanged();
}

void LocalGraphView::populatePaneMenu(QMenu* menu)
{
    menu->addAction(IconProvider::instance().icon(QStringLiteral("image"), 16), tr("Copy screenshot"), this, [this] { m_controller->copyScreenshot(); });
}

void LocalGraphView::focusContent() { m_widget->canvas()->setFocus(); }
void LocalGraphView::refresh() { m_controller->refresh(); }

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------
void registerGraphViews(ViewRegistry& registry)
{
    registry.registerView(QStringLiteral("graph"), [](WorkspaceLeaf* leaf) -> View* { return new GraphView(leaf); });
    registry.registerView(QStringLiteral("localgraph"), [](WorkspaceLeaf* leaf) -> View* { return new LocalGraphView(leaf); });
}

void registerGraphCommands(CommandRegistry& registry, Workspace* workspace)
{
    const QPointer<Workspace> ws = workspace;

    Command open;
    open.id = QStringLiteral("graph:open");
    open.name = QObject::tr("Open graph view");
    open.icon = QStringLiteral("git-fork");
    open.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+G"))};
    open.callback = [ws] {
        if (!ws)
            return;
        WorkspaceLeaf* leaf = ws->getLeaf(Workspace::NewLeaf::No);
        if (!leaf)
            return;
        leaf->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("graph")}, {QStringLiteral("state"), QJsonObject{}}});
        ws->setActiveLeaf(leaf);
    };
    registry.add(std::move(open));

    Command local;
    local.id = QStringLiteral("graph:open-local");
    local.name = QObject::tr("Open local graph");
    local.icon = QStringLiteral("git-fork");
    local.checkCallback = [ws](bool checking) {
        if (!ws)
            return false;
        WorkspaceLeaf* active = ws->activeLeaf();
        auto* fv = active ? qobject_cast<FileView*>(active->view()) : nullptr;
        TFile* file = fv ? fv->file() : nullptr;
        if (!file)
            return false;
        if (!checking) {
            // Obsidian: splitActiveLeaf("vertical") — a new pane to the right.
            if (WorkspaceLeaf* leaf = ws->splitLeaf(active, Qt::Horizontal)) {
                leaf->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("localgraph")},
                                               {QStringLiteral("state"), QJsonObject{{QStringLiteral("file"), file->path()}}}});
                ws->setActiveLeaf(leaf);
            }
        }
        return true;
    };
    registry.add(std::move(local));

    Command animate;
    animate.id = QStringLiteral("graph:animate");
    animate.name = QObject::tr("Start graph timelapse animation");
    animate.icon = QStringLiteral("wand");
    animate.checkCallback = [ws](bool checking) {
        GraphView* view = ws ? ws->activeViewOfType<GraphView>() : nullptr;
        if (!view)
            return false;
        if (!checking)
            view->startTimelapse();
        return true;
    };
    registry.add(std::move(animate));
}

} // namespace oc
