#include "ui/views/canvas/canvas_widget_p.h"

#include "core/config/vault_config.h"
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
#include "render/services/image_cache.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "text/codec.h"
#include "ui/shell/main_window.h"
#include "ui/views/canvas/canvas_controls.h"
#include "ui/views/canvas/canvas_items.h"
#include "ui/views/canvas/canvas_render.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QClipboard>
#include <QFontMetricsF>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QUrl>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>
#include <QTimerEvent>

#include <cmath>

namespace oc {

using namespace Qt::StringLiterals;
using namespace oc::canvas;
using canvasui::CanvasItem;
using canvasui::Palette;

namespace {

bool g_animationsByDefault = true;

// The root of the scene: carries the canvas transform, paints nothing.
class RootItem : public QGraphicsItem {
public:
    RootItem() { setFlag(ItemHasNoContents); }
    QRectF boundingRect() const override { return {}; }
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}
};

// The text of a card being edited lives in memory (the canvas file is saved by the view).
class MemoryStorage : public DocumentStorage {
public:
    explicit MemoryStorage(QByteArray bytes) : m_bytes(std::move(bytes)) {}
    std::optional<QByteArray> read(const QString&, DiskState* state, QString*) override
    {
        if (state)
            *state = stateOf();
        return m_bytes;
    }
    WriteResult write(const QString&, const QByteArray& bytes, const DiskState&, DiskState* newState, QString*) override
    {
        m_bytes = bytes;
        if (newState)
            *newState = stateOf();
        return WriteResult::Ok;
    }
    DiskState stat(const QString&) override { return stateOf(); }

private:
    DiskState stateOf() const
    {
        DiskState s;
        s.size = m_bytes.size();
        s.mtimeNs = 1;
        return s;
    }
    QByteArray m_bytes;
};

// Owns the storage of an in-memory document.
struct MemoryDocument {
    std::unique_ptr<MemoryStorage> storage;
    std::unique_ptr<Document> doc;
};

QString fileShortName(const QString& path)
{
    const QString name = path.section(u'/', -1);
    return name.endsWith(u".md", Qt::CaseInsensitive) ? name.chopped(3) : name;
}

bool isImagePath(const QString& path)
{
    static const QStringList exts = {u"bmp"_s, u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"svg"_s, u"webp"_s, u"avif"_s};
    return exts.contains(path.section(u'.', -1).toLower());
}

// Obsidian keeps image and canvas embeds mounted when zoomed out (only notes become placeholders).
bool keepsContentZoomedOut(const Node& n)
{
    return isImagePath(n.file) || n.file.endsWith(u".canvas", Qt::CaseInsensitive);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------

CanvasWidget::Options CanvasWidget::Options::fromJson(const QJsonObject& json)
{
    Options o;
    o.snapToGrid = json.value(u"snapToGrid").toBool(true);
    o.snapToObjects = json.value(u"snapToObjects").toBool(true);
    o.defaultWheelBehavior = json.value(u"defaultWheelBehavior").toString(u"pan"_s);
    o.defaultModDragBehavior = json.value(u"defaultModDragBehavior").toString(u"menu"_s);
    o.cardLabelVisibility = json.value(u"cardLabelVisibility").toString(u"always"_s);
    o.zoomBreakpoint = json.value(u"zoomBreakpoint").toDouble(0);
    o.newFileLocation = json.value(u"newFileLocation").toString(u"root"_s);
    o.newFileFolderPath = json.value(u"newFileFolderPath").toString();
    return o;
}

void CanvasWidget::Options::writeTo(QJsonObject& json) const
{
    json.insert(u"snapToGrid"_s, snapToGrid);
    json.insert(u"snapToObjects"_s, snapToObjects);
    if (defaultWheelBehavior != u"pan" || json.contains(u"defaultWheelBehavior"))
        json.insert(u"defaultWheelBehavior"_s, defaultWheelBehavior);
    if (defaultModDragBehavior != u"menu" || json.contains(u"defaultModDragBehavior"))
        json.insert(u"defaultModDragBehavior"_s, defaultModDragBehavior);
    if (cardLabelVisibility != u"always" || json.contains(u"cardLabelVisibility"))
        json.insert(u"cardLabelVisibility"_s, cardLabelVisibility);
    if (zoomBreakpoint != 0 || json.contains(u"zoomBreakpoint"))
        json.insert(u"zoomBreakpoint"_s, zoomBreakpoint);
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

void CanvasWidget::setAnimationsEnabledByDefault(bool on) { g_animationsByDefault = on; }

CanvasWidget::CanvasWidget(QWidget* parent)
    : QGraphicsView(parent)
    , m_animations(g_animationsByDefault && qEnvironmentVariableIsEmpty("OC_TESTING"))
{
    setObjectName(u"ocCanvas"_s);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setDragMode(QGraphicsView::NoDrag);

    auto* scene = new QGraphicsScene(this);
    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    setScene(scene);
    m_root = new RootItem;
    scene->addItem(m_root);
    m_overlay = new CanvasItem(this, CanvasItem::Kind::Overlay, QString(), m_root);
    m_overlay->setZValue(4e6);

    m_controls = new canvasui::CanvasControls(viewport());
    m_cardMenu = new canvasui::CardMenu(viewport());
    m_selectionMenu = new canvasui::SelectionMenu(viewport());
    m_selectionMenu->hide();

    connect(m_controls, &canvasui::CanvasControls::triggered, this, [this](const QString& name, const QPoint& global) {
        if (name == u"settings") {
            if (m_readOnly) {
                setReadOnly(false);
                return;
            }
            QMenu* menu = newMenu();
            fillSettingsMenu(menu);
            popupMenu(menu, global);
        } else if (name == u"zoom-in") {
            zoomBy(kButtonZoomStep);
        } else if (name == u"zoom-out") {
            zoomBy(-kButtonZoomStep);
        } else if (name == u"reset-zoom") {
            resetZoom();
        } else if (name == u"zoom-to-fit") {
            zoomToFit();
        } else if (name == u"undo") {
            undo();
        } else if (name == u"redo") {
            redo();
        } else if (name == u"help") {
            QMenu* menu = newMenu();
            const QStringList lines = {
                tr("Pan: Space + Drag, Scroll"),
                tr("Pan horizontally: Shift + Scroll"),
                tr("Zoom: Ctrl + Scroll, Space + Scroll"),
                tr("Zoom to fit: Shift + 1"),
                tr("Zoom to selection: Shift + 2"),
                tr("Select all: Ctrl + A"),
                tr("Add to / remove from selection: Shift + Click, Shift + Drag to select"),
                tr("Create a card with specific size: Ctrl + Drag to select"),
                tr("Clone card: Ctrl + Drag"),
                tr("Constrain card movement to axis: Shift + Drag"),
                tr("Disable snapping while dragging: Alt"),
                tr("Remove card: Backspace, Delete"),
                tr("Undo: Ctrl + Z"),
                tr("Redo: Ctrl + Y, Ctrl + Shift + Z"),
            };
            for (const QString& l : lines)
                menu->addAction(l)->setEnabled(false);
            popupMenu(menu, global);
        }
    });
    connect(m_cardMenu, &canvasui::CardMenu::clicked, this, [this](const QString& kind) {
        if (m_readOnly)
            return;
        const QPointF center = m_vp.toCanvas(QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
        if (kind == u"card") {
            createTextNode(center, Anchor::Center);
        } else {
            pickFileFor(kind == u"media", [this, center, kind](const QString& path) {
                createFileNode(path, center, kind == u"media" ? Anchor::Center : Anchor::TopLeft);
            });
        }
    });
    connect(m_cardMenu, &canvasui::CardMenu::dragMoved, this, [this](const QString& kind, const QPoint& global) {
        if (m_readOnly)
            return;
        const QPoint vp = viewport()->mapFromGlobal(global);
        if (!m_drag) {
            m_drag = std::make_unique<Drag>();
            m_drag->kind = Drag::Kind::NewCard;
            m_drag->started = true;
            m_drag->cardKind = kind;
            m_drag->cardSize = kind == u"card" ? QSizeF(kDefaultTextWidth, kDefaultTextHeight) : QSizeF(kDefaultFileWidth, kDefaultFileHeight);
        }
        dragMove(vp, QApplication::keyboardModifiers());
    });
    connect(m_cardMenu, &canvasui::CardMenu::dragFinished, this, [this](const QString&, const QPoint& global) {
        if (m_drag && m_drag->kind == Drag::Kind::NewCard)
            dragEnd(viewport()->mapFromGlobal(global), QApplication::keyboardModifiers(), false);
    });

    m_textCommit.setSingleShot(true);
    m_textCommit.setInterval(250); // Obsidian's requestPushHistory debounce
    connect(&m_textCommit, &QTimer::timeout, this, [this] { commit(); });

    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            m_contents.clear();
            m_syncedRevision = ~0ull;
            syncItems();
            viewport()->update();
            m_controls->update();
            m_cardMenu->update();
        });
    connect(&ImageCache::instance(), &ImageCache::imageReady, this, [this](const QString&) { viewport()->update(); });
    m_vp.size = m_target.size = QSizeF(viewport()->size());
    updateSceneTransform();
    updateOverlays();
}

CanvasWidget::~CanvasWidget()
{
    stopEditing();
    m_drag.reset();
    m_media.clear();
}

void CanvasWidget::setSession(VaultSession* session)
{
    m_session = session;
    m_contents.clear();
    m_syncedRevision = ~0ull;
    syncItems();
}

VaultSession* CanvasWidget::session() const { return m_session; }

void CanvasWidget::setFilePath(const QString& path)
{
    m_filePath = path;
    m_contents.clear(); // links resolve relative to it
    m_syncedRevision = ~0ull;
    syncItems();
}

void CanvasWidget::setOptions(const Options& options)
{
    m_options = options;
    m_syncedRevision = ~0ull;
    syncItems();
    viewport()->update();
}

MainWindow* CanvasWidget::mainWindow() const
{
    Workspace* ws = m_session ? m_session->workspace() : nullptr;
    return ws ? ws->window() : nullptr;
}

LivePreviewDecorator* CanvasWidget::resolver()
{
    if (!m_resolver) {
        m_resolver = std::make_unique<LivePreviewDecorator>(nullptr);
        m_resolver->isResolved = [this](const QString& target) {
            const QString path = LinkResolver::splitSubpath(target).first;
            if (path.isEmpty() || !m_session || !m_session->metadata())
                return true;
            return !m_session->metadata()->resolver().resolve(path, m_filePath).isEmpty();
        };
        m_resolver->resolveAttachment = [this](const QString& linkpath) -> QString {
            const QString dest = resolveLinkPath(linkpath);
            return dest.isEmpty() || !m_session || !m_session->vault() ? QString() : m_session->vault()->absolutePath(dest);
        };
    }
    return m_resolver.get();
}

QString CanvasWidget::resolveLinkPath(const QString& linkpath) const
{
    if (!m_session || !m_session->metadata())
        return {};
    return m_session->metadata()->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, m_filePath);
}

// ---------------------------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------------------------

void CanvasWidget::setData(CanvasData data, bool fit)
{
    stopEditing();
    finishLabelEdit(false);
    m_drag.reset();
    m_doc.reset(std::move(data));
    m_selection.clear();
    m_scroll.clear();
    m_contents.clear();
    m_error.clear();
    m_syncedRevision = ~0ull;
    syncItems();
    m_autoFit = false;
    if (fit) {
        m_fitQueued = true;
        if (viewport()->width() > 0 && viewport()->height() > 0 && isVisible())
            autoFit();
    }
    updateOverlays();
    emit selectionChanged();
}

void CanvasWidget::reloadData(CanvasData data)
{
    const QString editing = m_editingId;
    const bool wasEditingText = !editing.isEmpty() && m_doc.data().node(editing) && m_doc.data().node(editing)->type == NodeType::Text;
    if (wasEditingText)
        stopEditing();
    m_doc.reset(std::move(data));
    QSet<QString> keep;
    for (const QString& id : std::as_const(m_selection))
        if (m_doc.data().node(id) || m_doc.data().edge(id))
            keep.insert(id);
    m_selection = keep;
    m_contents.clear();
    m_syncedRevision = ~0ull;
    syncItems();
    updateOverlays();
    emit selectionChanged();
}

void CanvasWidget::setErrorMessage(const QString& message)
{
    m_error = message;
    viewport()->update();
}

void CanvasWidget::markChanged()
{
    syncItems();
    updateOverlays();
}

void CanvasWidget::commit()
{
    m_textCommit.stop();
    syncItems();
    if (m_doc.commit())
        emit edited();
    updateOverlays();
}

bool CanvasWidget::canUndo() const { return m_doc.canUndo() || m_doc.hasUncommittedChanges(); }
bool CanvasWidget::canRedo() const { return m_doc.canRedo(); }

void CanvasWidget::undo()
{
    if (m_readOnly)
        return;
    stopEditing();
    if (m_doc.undo()) {
        QSet<QString> keep;
        for (const QString& id : std::as_const(m_selection))
            if (m_doc.data().node(id) || m_doc.data().edge(id))
                keep.insert(id);
        setSelectionInternal(keep);
        markChanged();
        emit edited();
    }
}

void CanvasWidget::redo()
{
    if (m_readOnly)
        return;
    stopEditing();
    if (m_doc.redo()) {
        QSet<QString> keep;
        for (const QString& id : std::as_const(m_selection))
            if (m_doc.data().node(id) || m_doc.data().edge(id))
                keep.insert(id);
        setSelectionInternal(keep);
        markChanged();
        emit edited();
    }
}

// ---------------------------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------------------------

Viewport CanvasWidget::canvasViewport() const { return m_vp; }
Viewport CanvasWidget::targetViewport() const { return m_target; }

QPointF CanvasWidget::mapToCanvas(const QPoint& viewportPos) const { return m_vp.toCanvas(QPointF(viewportPos)); }
QPointF CanvasWidget::mapFromCanvas(const QPointF& canvasPos) const { return m_vp.toView(canvasPos); }

double CanvasWidget::zoomMultiplier() const { return m_vp.zoomMultiplier(); }

bool CanvasWidget::isZoomedOut() const { return m_vp.zoom <= kZoomedOutZoom - m_options.zoomBreakpoint; }

void CanvasWidget::setAnimationsEnabled(bool on)
{
    m_animations = on;
    if (!on)
        finishViewport();
}

void CanvasWidget::autoFit()
{
    m_fitQueued = false;
    zoomToFit();
    finishViewport();
    m_autoFit = true; // until the user moves the view (Obsidian fits once the view has its size)
}

void CanvasWidget::setViewport(double x, double y, double zoom)
{
    m_fitQueued = false;
    m_autoFit = false;
    m_zoomCenter.reset();
    m_animTimer.stop();
    m_vp.x = m_target.x = x;
    m_vp.y = m_target.y = y;
    m_vp.zoom = m_target.zoom = clampZoom(zoom);
    updateSceneTransform();
    emit viewportChanged();
}

void CanvasWidget::zoomBy(double delta, std::optional<QPointF> viewAnchor)
{
    m_autoFit = false;
    m_target.zoom = clampZoom(m_target.zoom + delta);
    m_zoomCenter = viewAnchor ? *viewAnchor - QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0) : QPointF(0, 0);
    startAnimation();
}

void CanvasWidget::resetZoom() { zoomBy(-m_target.zoom); }

void CanvasWidget::zoomToRect(const QRectF& rect)
{
    const auto z = canvas::zoomToRect(rect, QSizeF(viewport()->size()));
    if (!z)
        return;
    m_autoFit = false;
    m_target.x = z->center.x();
    m_target.y = z->center.y();
    m_target.zoom = z->zoom;
    m_zoomCenter.reset();
    startAnimation();
}

void CanvasWidget::zoomToFit()
{
    const QRectF box = allNodesBounds(m_doc.data());
    if (box.isNull() && m_doc.data().nodes.isEmpty())
        return;
    zoomToRect(box);
}

void CanvasWidget::zoomToSelection()
{
    if (m_selection.isEmpty())
        return;
    zoomToRect(itemsBounds(m_doc.data(), m_selection));
}

void CanvasWidget::panBy(const QPointF& d)
{
    if (!d.isNull())
        m_autoFit = false;
    m_vp.x += d.x();
    m_vp.y += d.y();
    m_target.x = m_vp.x;
    m_target.y = m_vp.y;
    updateSceneTransform();
    if (!m_animTimer.isActive())
        emit viewportChanged();
}

void CanvasWidget::panIntoView(const QRectF& rect0)
{
    const double g = gridSpacing(m_vp.zoom);
    const QRectF rect = rect0.adjusted(-g, -g, g, g);
    const QRectF vis = m_vp.visibleRect();
    if (rect.width() > vis.width() || rect.height() > vis.height()) {
        zoomToRect(rect);
        return;
    }
    QPointF d;
    if (rect.left() < vis.left())
        d.setX(rect.left() - vis.left());
    if (rect.top() < vis.top())
        d.setY(rect.top() - vis.top());
    if (rect.right() > vis.right())
        d.setX(rect.right() - vis.right());
    if (rect.bottom() > vis.bottom())
        d.setY(rect.bottom() - vis.bottom());
    if (!d.isNull())
        panBy(d);
}

void CanvasWidget::startAnimation()
{
    if (!m_animations) {
        finishViewport();
        return;
    }
    if (!m_animTimer.isActive()) {
        m_animClock.start();
        m_animTimer.start(16, this);
    }
}

void CanvasWidget::finishViewport()
{
    m_animTimer.stop();
    // One step with a = 1 (Obsidian's finishViewportAnimation).
    const double before = m_vp.scale();
    const double z = clampZoom(m_target.zoom);
    if (m_zoomCenter) {
        const double after = std::pow(2.0, z);
        m_vp.x -= m_zoomCenter->x() / after - m_zoomCenter->x() / before;
        m_vp.y -= m_zoomCenter->y() / after - m_zoomCenter->y() / before;
        m_target.x = m_vp.x;
        m_target.y = m_vp.y;
    }
    m_vp.zoom = m_target.zoom = z;
    m_vp.x = m_target.x;
    m_vp.y = m_target.y;
    m_zoomCenter.reset();
    updateSceneTransform();
    emit viewportChanged();
}

void CanvasWidget::stepAnimation()
{
    const double dt = std::max<double>(1, double(m_animClock.restart()));
    double a = 1 - std::pow(canvasui::kZoomAnimation, dt);
    const double d = clampZoom(m_target.zoom);
    m_target.zoom = d;
    const double m = d - m_vp.zoom;
    if (std::abs(m) < 0.01)
        a = 1;
    const double before = m_vp.scale();
    if (m == 0) {
        m_vp.zoom = d;
    } else if (m_zoomCenter) {
        m_vp.zoom += m * a;
        const double after = m_vp.scale();
        m_vp.x -= m_zoomCenter->x() / after - m_zoomCenter->x() / before;
        m_vp.y -= m_zoomCenter->y() / after - m_zoomCenter->y() / before;
        m_target.x = m_vp.x;
        m_target.y = m_vp.y;
    } else {
        m_vp.zoom += m * a;
    }
    const double wx = m_target.x - m_vp.x;
    m_vp.x += wx * a;
    if (wx != 0 && std::abs(wx) < 0.01)
        m_vp.x = m_target.x;
    const double wy = m_target.y - m_vp.y;
    m_vp.y += wy * a;
    if (wy != 0 && std::abs(wy) < 0.01)
        m_vp.y = m_target.y;
    updateSceneTransform();
    if (m_vp.x == m_target.x && m_vp.y == m_target.y && m_vp.zoom == m_target.zoom) {
        m_animTimer.stop();
        m_zoomCenter.reset();
        emit viewportChanged();
    }
}

void CanvasWidget::timerEvent(QTimerEvent* e)
{
    if (e->timerId() == m_animTimer.timerId()) {
        stepAnimation();
        return;
    }
    if (e->timerId() == m_autoPan.timerId()) {
        autoPanTick();
        return;
    }
    QGraphicsView::timerEvent(e);
}

void CanvasWidget::updateSceneTransform()
{
    const QSizeF size(viewport()->size());
    m_vp.size = m_target.size = size;
    const double s = m_vp.scale();
    m_root->setTransform(QTransform(s, 0, 0, s, size.width() / 2 - m_vp.x * s, size.height() / 2 - m_vp.y * s));
    if (m_itemsZoom != m_vp.zoom) {
        m_itemsZoom = m_vp.zoom;
        m_syncedRevision = ~0ull; // labels, handles and zoomed-out placeholders depend on the zoom
    }
    syncItems();
    positionEditor();
    updateOverlays();
    viewport()->update();
}

void CanvasWidget::resizeEvent(QResizeEvent* e)
{
    QGraphicsView::resizeEvent(e);
    scene()->setSceneRect(QRectF(QPointF(0, 0), QSizeF(viewport()->size())));
    updateSceneTransform();
    if ((m_fitQueued || m_autoFit) && viewport()->width() > 0 && viewport()->height() > 0)
        autoFit();
}

void CanvasWidget::showEvent(QShowEvent* e)
{
    QGraphicsView::showEvent(e);
    m_syncedRevision = ~0ull;
    scene()->setSceneRect(QRectF(QPointF(0, 0), QSizeF(viewport()->size())));
    if ((m_fitQueued || m_autoFit) && viewport()->width() > 0 && viewport()->height() > 0)
        autoFit();
    updateSceneTransform();
}

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

QRectF CanvasWidget::nodeLabelRect(const Node& n) const
{
    if (n.type != NodeType::File && n.type != NodeType::Link)
        return {};
    const double zm = zoomMultiplier();
    const Palette& pal = Palette::current();
    const double h = QFontMetricsF(canvasui::uiFont(pal.uiFontPx)).height() * 1.1 * zm;
    return QRectF(n.x, n.y - 4 * zm - h, n.width, h);
}

QRectF CanvasWidget::groupLabelRect(const Node& n) const
{
    if (!n.isGroup() || (n.label.isEmpty() && m_labelTarget != u"node:" + n.id))
        return {};
    const double zm = zoomMultiplier();
    const Palette& pal = Palette::current();
    const QFont f = canvasui::uiFont(pal.uiFontPx * 1.5);
    const QFontMetricsF fm(f);
    const double textW = fm.horizontalAdvance(n.label.isEmpty() ? tr("Untitled group") : n.label);
    const double w = std::min(std::ceil(textW) + 17, n.width / zm); // padding 4px 8px (+1 slack), max-width 100%
    const double h = pal.uiFontPx * 1.5 + 8;          // line-height 1
    return QRectF(n.x, n.y - 4 * zm - h * zm, w * zm, h * zm);
}

QRectF CanvasWidget::edgeLabelRect(const Edge& e) const
{
    const bool editing = m_labelTarget == u"edge:" + e.id;
    if (e.label.isEmpty() && !editing)
        return {};
    const auto g = edgeGeometry(e);
    if (!g)
        return {};
    const double zm = zoomMultiplier();
    const Palette& pal = Palette::current();
    const QFont f = canvasui::uiFont(pal.edgeLabelPx);
    const QFontMetricsF fm(f);
    const double maxW = 17 * pal.edgeLabelPx; // 17em
    const QString text = e.label.isEmpty() ? tr("Add text...") : e.label;
    const QRectF tr = fm.boundingRect(QRectF(0, 0, maxW, 1e6), Qt::TextWordWrap, text);
    const int lines = std::max(1, int(std::lround(tr.height() / fm.lineSpacing())));
    const double w = std::min(maxW, std::ceil(tr.width()) + 2) + 12;
    const double h = lines * pal.edgeLabelPx * 1.3 + 12;
    const QPointF c = g->center();
    return QRectF(c.x() - w * zm / 2, c.y() - h * zm / 2, w * zm, h * zm);
}

std::optional<EdgeGeometry> CanvasWidget::edgeGeometry(const Edge& e) const
{
    const Node* from = m_doc.data().node(e.fromNode);
    const Node* to = m_doc.data().node(e.toNode);
    if (!from || !to || e.opaque)
        return std::nullopt;
    return canvas::edgeGeometry(from->rect(), e.fromSide, to->rect(), e.toSide, e.fromEnd, e.toEnd);
}

void CanvasWidget::syncItems()
{
    if (!m_root)
        return;
    const CanvasData& d = m_doc.data();
    const bool full = m_syncedRevision != m_doc.revision();
    m_syncedRevision = m_doc.revision();
    Q_UNUSED(full);
    // Nodes: groups below everything (larger first), then cards in list order; a single selected card
    // is raised above the others (Obsidian's renderZIndex()).
    const QList<int> order = paintOrder(d);
    QSet<QString> seen;
    const bool singleSelected = m_selection.size() == 1;
    for (int rank = 0; rank < order.size(); ++rank) {
        const Node& n = d.nodes[order[rank]];
        seen.insert(n.id);
        CanvasItem*& item = m_nodeItems[n.id];
        if (!item)
            item = new CanvasItem(this, CanvasItem::Kind::Node, n.id, m_root);
        QRectF bounds = n.rect().adjusted(-8, -8, 8, 24); // selection ring + shadows
        if (const QRectF lr = n.isGroup() ? groupLabelRect(n) : nodeLabelRect(n); !lr.isNull())
            bounds = bounds.united(lr.adjusted(-2, -2, 2, 2));
        item->setBounds(bounds);
        double z = n.isGroup() ? -1.0 - (order.size() - rank) : 1.0 + rank;
        if (!n.isGroup() && singleSelected && m_selection.contains(n.id))
            z = 1e6;
        item->setZValue(z);
        item->update();
    }
    for (auto it = m_nodeItems.begin(); it != m_nodeItems.end();) {
        if (!seen.contains(it.key())) {
            delete it.value();
            m_contents.remove(it.key());
            it = m_nodeItems.erase(it);
        } else {
            ++it;
        }
    }
    // Edges (between groups and cards) and their labels (above the edges).
    QSet<QString> seenEdges;
    for (const Edge& e : d.edges) {
        const auto g = edgeGeometry(e);
        if (!g)
            continue;
        seenEdges.insert(e.id);
        CanvasItem*& item = m_edgeItems[e.id];
        if (!item)
            item = new CanvasItem(this, CanvasItem::Kind::Edge, e.id, m_root);
        const double pad = 14 * zoomMultiplier() + 12;
        item->setBounds(g->bounds().adjusted(-pad, -pad, pad, pad));
        item->setZValue(0);
        item->update();
        const QRectF lr = edgeLabelRect(e);
        CanvasItem*& label = m_edgeLabelItems[e.id];
        if (lr.isNull()) {
            delete label;
            m_edgeLabelItems.remove(e.id);
        } else {
            if (!label)
                label = new CanvasItem(this, CanvasItem::Kind::EdgeLabel, e.id, m_root);
            label->setBounds(lr.adjusted(-6, -6, 6, 6));
            label->setZValue(0.5);
            label->update();
        }
    }
    for (auto it = m_edgeItems.begin(); it != m_edgeItems.end();) {
        if (!seenEdges.contains(it.key())) {
            delete it.value();
            it = m_edgeItems.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_edgeLabelItems.begin(); it != m_edgeLabelItems.end();) {
        if (!seenEdges.contains(it.key())) {
            delete it.value();
            it = m_edgeLabelItems.erase(it);
        } else {
            ++it;
        }
    }
    m_overlay->setBounds(m_vp.visibleRect().adjusted(-200, -200, 200, 200));
    m_overlay->update();
    if (m_controls)
        m_controls->setHistory(canUndo(), canRedo());
    updateMediaWidgets();
}

void CanvasWidget::updateMediaWidgets()
{
    using canvasui::EmbedContent;
    const QRectF visible = m_vp.visibleRect();
    QSet<QString> keep;
    const bool live = !isZoomedOut() && m_error.isEmpty() && isVisible();
    for (const Node& n : m_doc.data().nodes) {
        if (n.type != NodeType::File || !n.isShown())
            continue;
        auto* content = dynamic_cast<EmbedContent*>(contentFor(n.id));
        MediaHost* host = m_media.contains(n.id) ? &m_media[n.id] : nullptr;
        const bool busy = host && host->proxy && host->proxy->widget() && host->proxy->widget()->property("ocEmbedBusy").toBool();
        if (!content || (!busy && (!live || !n.rect().intersects(visible))))
            continue;
        const QString key = content->request().key();
        if (!host || !host->proxy || host->key != key) {
            if (host && host->proxy)
                host->proxy->deleteLater();
            EmbedContext ctx;
            ctx.request = content->request();
            QWidget* w = content->provider()->createWidget(ctx, nullptr);
            if (!w)
                continue;
            auto* proxy = new QGraphicsProxyWidget(m_root);
            proxy->setWidget(w);
            m_media.insert(n.id, MediaHost{proxy, key});
            host = &m_media[n.id];
        }
        keep.insert(n.id);
        const QRectF inner = n.rect().adjusted(2, 2, -2, -2); // inside the card's border and selection ring
        host->proxy->setGeometry(inner);
        if (host->proxy->widget() && host->proxy->widget()->size() != inner.size().toSize())
            host->proxy->widget()->resize(inner.size().toSize());
        const CanvasItem* item = m_nodeItems.value(n.id);
        host->proxy->setZValue((item ? item->zValue() : 1) + 0.5);
        host->proxy->setVisible(n.id != m_editingId);
    }
    for (auto it = m_media.begin(); it != m_media.end();) {
        if (!keep.contains(it.key())) {
            if (it->proxy)
                it->proxy->deleteLater();
            it = m_media.erase(it);
        } else {
            ++it;
        }
    }
}

QWidget* CanvasWidget::mediaWidget(const QString& nodeId) const
{
    const auto it = m_media.constFind(nodeId);
    return it != m_media.cend() && it->proxy ? it->proxy->widget() : nullptr;
}

bool CanvasWidget::overFocusedMedia(const QPointF& scenePos) const
{
    if (m_selection.size() != 1)
        return false;
    const auto it = m_media.constFind(*m_selection.begin());
    return it != m_media.cend() && it->proxy && it->proxy->isVisible() && it->proxy->sceneBoundingRect().contains(scenePos);
}

void CanvasWidget::paintItem(QPainter* p, const CanvasItem& item)
{
    switch (item.kind()) {
    case CanvasItem::Kind::Node:
        if (const Node* n = m_doc.data().node(item.id()))
            paintNode(p, *n);
        break;
    case CanvasItem::Kind::Edge:
        if (const Edge* e = m_doc.data().edge(item.id()))
            paintEdge(p, *e, false);
        break;
    case CanvasItem::Kind::EdgeLabel:
        if (const Edge* e = m_doc.data().edge(item.id()))
            paintEdge(p, *e, true);
        break;
    case CanvasItem::Kind::Overlay:
        paintOverlay(p);
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Card contents
// ---------------------------------------------------------------------------------------------

std::unique_ptr<canvasui::CardContent> CanvasWidget::buildContent(const Node& n)
{
    using namespace canvasui;
    VaultSession* s = m_session;
    Vault* vault = s ? s->vault() : nullptr;
    const ThemePtr theme = currentTheme();
    static StyleSheetPtr styles;
    static quint64 stylesGen = ~0ull;
    if (!styles || stylesGen != theme->generation()) {
        styles = StyleSheet::build(*theme);
        stylesGen = theme->generation();
    }
    switch (n.type) {
    case NodeType::Text:
        return std::make_unique<MarkdownContent>(Text::fromString(n.text), styles, resolver());
    case NodeType::Link:
        return std::make_unique<PlaceholderContent>(u"globe"_s, QUrl(n.url).host().isEmpty() ? n.url : QUrl(n.url).host(), n.url);
    case NodeType::File: {
        TFile* f = vault ? vault->file(n.file) : nullptr;
        if (!f)
            return std::make_unique<PlaceholderContent>(u"triangle-alert"_s, tr("“%1” could not be found.").arg(n.file + n.subpath));
        const QString ext = f->extension().toLower();
        if (ext == u"md") {
            Text text;
            if (Document* d = s->documents() ? s->documents()->find(f->path()) : nullptr) {
                text = d->text();
            } else if (const std::optional<QByteArray> bytes = vault->read(f->path(), nullptr, nullptr)) {
                text = decodeFile(*bytes).text;
            }
            QString title;
            if (!n.subpath.isEmpty()) {
                if (const md::CachedMetadata* meta = s->metadata() ? s->metadata()->fileCache(f->path()) : nullptr) {
                    if (const auto r = resolveSubpath(*meta, n.subpath)) {
                        const int from = std::clamp(r->startLine, 0, text.lineCount() - 1);
                        const int to = r->endLine < 0 ? text.lineCount() - 1 : std::clamp(r->endLine, from, text.lineCount() - 1);
                        text = Text::fromString(text.slice(text.lineStart(from), text.lineEnd(to)));
                    }
                }
            } else if (!s->vaultConfig() || s->vaultConfig()->flag(u"showInlineTitle"_s)) {
                title = f->basename();
            }
            return std::make_unique<MarkdownContent>(text, styles, resolver(), title);
        }
        if (isImagePath(f->path()))
            return std::make_unique<ImageContent>(vault->absolutePath(f->path()));
        if (ext != u"canvas") {
            // PDF, audio, video: the media module's embed providers.
            if (std::shared_ptr<EmbedProvider> provider = EmbedRegistry::instance().provider(ext))
                return std::make_unique<EmbedContent>(provider, EmbedRequest::fromLink(vault->absolutePath(f->path()), f->path(), n.subpath));
        }
        if (ext == u"canvas") {
            if (const std::optional<QByteArray> bytes = vault->read(f->path(), nullptr, nullptr))
                if (std::optional<CanvasData> data = parseCanvas(*bytes))
                    return std::make_unique<MinimapContent>(std::move(*data));
            return std::make_unique<PlaceholderContent>(u"layout-dashboard"_s, f->name());
        }
        const QString icon = ext == u"pdf" ? u"file-text"_s
            : QStringList{u"mp3"_s, u"wav"_s, u"m4a"_s, u"ogg"_s, u"flac"_s, u"webm"_s, u"3gp"_s}.contains(ext) ? u"file-audio"_s
            : QStringList{u"mp4"_s, u"mkv"_s, u"mov"_s, u"ogv"_s}.contains(ext)                              ? u"file-video"_s
                                                                                                              : u"file"_s;
        return std::make_unique<PlaceholderContent>(icon, f->name(), ext.toUpper());
    }
    default:
        return nullptr;
    }
}

canvasui::CardContent* CanvasWidget::contentFor(const QString& nodeId)
{
    const Node* n = m_doc.data().node(nodeId);
    if (!n)
        return nullptr;
    QString key = nodeTypeName(n->type) + u'\x1f';
    switch (n->type) {
    case NodeType::Text:
        key += n->text;
        break;
    case NodeType::Link:
        key += n->url;
        break;
    case NodeType::File: {
        TFile* f = m_session && m_session->vault() ? m_session->vault()->file(n->file) : nullptr;
        key += n->file + n->subpath + u'\x1f' + QString::number(f ? f->mtimeMs() : -1) + u'\x1f' + QString::number(f ? f->size() : -1);
        if (Document* d = f && m_session->documents() ? m_session->documents()->find(f->path()) : nullptr)
            key += u'\x1f' + QString::number(d->version());
        break;
    }
    default:
        break;
    }
    ContentEntry& entry = m_contents[nodeId];
    if (!entry.content || entry.key != key) {
        entry.content = buildContent(*n);
        entry.key = key;
    }
    return entry.content.get();
}

void CanvasWidget::invalidateFile(const QString& path)
{
    for (const Node& n : m_doc.data().nodes)
        if (n.type == NodeType::File && (n.file == path || path.isEmpty()))
            m_contents.remove(n.id);
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

namespace {
void paintCardShadow(QPainter* p, const QRectF& r, double radius, bool dragging)
{
    p->save();
    p->setPen(Qt::NoPen);
    if (dragging) {
        // --shadow-l
        const struct {
            double dy, blur, alpha;
        } layers[] = {{1.8, 7.3, 0.071}, {6.3, 24.7, 0.112}, {15, 30, 0.1}};
        for (const auto& l : layers) {
            for (int i = 4; i >= 1; --i) {
                const double spread = l.blur * i / 8;
                p->setBrush(QColor(0, 0, 0, int(255 * l.alpha / 4)));
                p->drawRoundedRect(r.adjusted(-spread, -spread + l.dy, spread, spread + l.dy), radius + spread, radius + spread);
            }
        }
    } else {
        // --shadow-stationary: 0 0.5px 1px 0.5px rgba(0, 0, 0, 0.1)
        p->setBrush(QColor(0, 0, 0, 26));
        p->drawRoundedRect(r.adjusted(-0.5, 0, 0.5, 1), radius + 0.5, radius + 0.5);
    }
    p->restore();
}

// Text drawn at `px` scaled by `k` (CSS "font-size: px; transform: scale(k)").
void drawScaledText(QPainter* p, const QRectF& rect, double k, const QFont& font, const QColor& color, const QString& text,
                    int flags, bool elide = true)
{
    if (k <= 0 || rect.isEmpty())
        return;
    p->save();
    p->translate(rect.topLeft());
    p->scale(k, k);
    p->setFont(font);
    p->setPen(color);
    const QRectF local(0, 0, rect.width() / k, rect.height() / k);
    const QFontMetricsF fm(font);
    QString shown = text;
    if (elide && fm.horizontalAdvance(text) > local.width() + 0.5)
        shown = fm.elidedText(text, Qt::ElideRight, local.width());
    // Single lines never wrap (measured and drawn widths differ by rounding at small scales).
    if (!(flags & Qt::TextWordWrap) || fm.horizontalAdvance(shown) <= local.width() + 1)
        flags &= ~Qt::TextWordWrap;
    p->drawText(local, flags | Qt::TextDontClip, shown);
    p->restore();
}
} // namespace

void CanvasWidget::paintNode(QPainter* p, const Node& n)
{
    using namespace canvasui;
    const Palette& pal = Palette::current();
    const CanvasColor cc = resolveColor(n.color, pal);
    const bool selected = m_selection.contains(n.id);
    const bool dragging = m_drag && m_drag->started && ((m_drag->kind == Drag::Kind::Move && !m_drag->clone && m_drag->moving.contains(n.id))
                                                        || ((m_drag->kind == Drag::Kind::Resize) && m_drag->nodeId == n.id));
    const bool editing = n.id == m_editingId;
    const bool zoomedOut = isZoomedOut() && !editing;
    const double zm = zoomMultiplier();
    const QRectF r = n.rect();
    const double radius = 8; // --radius-m
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);

    if (!n.isGroup())
        paintCardShadow(p, r, radius, dragging);
    QPainterPath outer;
    outer.addRoundedRect(r, radius, radius);
    const QRectF inner = r.adjusted(1, 1, -1, -1);
    QPainterPath innerPath;
    innerPath.addRoundedRect(inner, radius - 1, radius - 1);
    if (!n.isGroup())
        p->fillPath(outer, pal.card);
    if (n.isGroup() || cc.themed)
        p->fillPath(innerPath, withAlpha(cc.color, 0.07));

    // Contents.
    p->save();
    p->setClipPath(innerPath, Qt::IntersectClip);
    if (n.isGroup()) {
        if (!n.background.isEmpty() && m_session && m_session->vault()) {
            const QString dest = m_session->vault()->file(n.background) ? n.background : resolveLinkPath(n.background);
            if (!dest.isEmpty()) {
                const QString abs = m_session->vault()->absolutePath(dest);
                const QSize nat = ImageCache::instance().naturalSize(abs);
                if (nat.isValid() && !nat.isEmpty()) {
                    const double scale = std::abs(p->combinedTransform().m11());
                    if (n.backgroundStyle == u"repeat") {
                        const int want = std::max(16, nat.width());
                        if (const auto img = ImageCache::instance().image(abs, want, 1.0)) {
                            for (double y = inner.top(); y < inner.bottom(); y += nat.height())
                                for (double x = inner.left(); x < inner.right(); x += nat.width())
                                    p->drawImage(QRectF(x, y, nat.width(), nat.height()), *img);
                        }
                    } else {
                        // cover (and "ratio": the card keeps the image's aspect ratio, so cover fills it too)
                        const QSizeF sz = QSizeF(nat).scaled(inner.size(), Qt::KeepAspectRatioByExpanding);
                        const QRectF target(inner.center().x() - sz.width() / 2, inner.center().y() - sz.height() / 2, sz.width(), sz.height());
                        const int want = std::clamp(int(std::ceil(target.width() * scale / 128.0)) * 128, 32, std::max(32, nat.width()));
                        if (const auto img = ImageCache::instance().image(abs, want, 1.0))
                            p->drawImage(target, *img);
                    }
                }
            }
        }
    } else if (zoomedOut && (n.type == NodeType::Text || (n.type == NodeType::File && !keepsContentZoomedOut(n)))) {
        // .canvas-node-placeholder
        p->fillRect(inner, withAlpha(cc.color, 0.10));
        if (n.type == NodeType::Text) {
            const double s = std::min(inner.width(), inner.height()) * 0.4;
            const double scale = std::abs(p->combinedTransform().m11());
            const QPixmap pm = IconProvider::instance().pixmap(u"align-left"_s, std::max(4, int(std::ceil(s * scale))), withAlpha(cc.color, 0.3), 2,
                                                             1.0);
            p->drawPixmap(QRectF(inner.center().x() - s / 2, inner.center().y() - s / 2, s, s), pm, QRectF(pm.rect()));
        } else {
            TFile* f = m_session && m_session->vault() ? m_session->vault()->file(n.file) : nullptr;
            QString name = f ? fileShortName(f->path()) : n.file;
            if (f && !n.subpath.isEmpty() && f->isMarkdown())
                name += u" › "_s + n.subpath.mid(1);
            QFont font = uiFont(32, 600);
            p->setFont(font);
            p->setPen(pal.textNormal);
            p->drawText(inner.adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWrapAnywhere, name);
        }
    } else if (!editing) {
        if (CardContent* c = contentFor(n.id))
            c->paint(p, inner, m_scroll.value(n.id));
    }
    p->restore();

    // Border (.canvas-node-container) and selection outline.
    p->setBrush(Qt::NoBrush);
    if (!selected) {
        if (cc.themed) {
            p->setPen(QPen(withAlpha(cc.color, 0.7), 2));
            p->drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
        } else {
            p->setPen(QPen(cc.color, 1));
            p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
        }
    } else if (cc.themed) {
        // border + inset 1px + 2px ring, all in the node's color
        p->setPen(QPen(cc.color, 4));
        p->drawRoundedRect(r, radius, radius);
    } else {
        // border accent + 2px accent ring
        p->setPen(QPen(pal.accent, 3));
        p->drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), radius + 0.5, radius + 0.5);
    }

    // Labels.
    if (n.isGroup()) {
        const QRectF lr = groupLabelRect(n);
        if (!lr.isNull() && m_labelTarget != u"node:" + n.id) {
            QColor bg = cc.themed ? cc.color : withAlpha(cc.color, 0.10);
            QColor fg = pal.textMuted;
            if (cc.themed)
                fg = isLightColor(cc.color) ? pal.textOnAccentInverted : pal.textOnAccent;
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(lr, 4 * zm, 4 * zm);
            drawScaledText(p, lr.adjusted(8 * zm, 4 * zm, -8 * zm, -4 * zm), zm, uiFont(pal.uiFontPx * 1.5), fg, n.label,
                           Qt::AlignLeft | Qt::AlignVCenter);
        }
    } else if ((n.type == NodeType::File || n.type == NodeType::Link) && m_vp.zoom > kZoomedOutZoom && m_options.cardLabelVisibility != u"never") {
        const bool hovered = m_hover.node == n.id;
        if (m_options.cardLabelVisibility != u"hover" || hovered || (selected && m_selection.size() == 1)) {
            const QRectF lr = nodeLabelRect(n);
            QString text;
            bool missing = false;
            if (n.type == NodeType::Link) {
                text = n.url;
            } else {
                TFile* f = m_session && m_session->vault() ? m_session->vault()->file(n.file) : nullptr;
                missing = !f;
                text = f ? fileShortName(f->path()) : n.file;
                if (f && f->isMarkdown() && !n.subpath.isEmpty())
                    text += u" › "_s + n.subpath.mid(1);
            }
            const QColor fg = selected || m_hover.label == n.id ? pal.textMuted : pal.labelColor;
            QRectF tr = lr;
            if (missing) {
                const double s = pal.uiFontPx * zm;
                const double scale = std::abs(p->combinedTransform().m11());
                const QPixmap pm = IconProvider::instance().pixmap(u"triangle-alert"_s, std::max(4, int(std::ceil(s * scale))), fg, 2, 1.0);
                p->drawPixmap(QRectF(lr.left(), lr.center().y() - s / 2 + 1 * zm, s, s), pm, QRectF(pm.rect()));
                tr.setLeft(tr.left() + s + 4 * zm);
            }
            drawScaledText(p, tr, zm, uiFont(pal.uiFontPx), fg, text, Qt::AlignLeft | Qt::AlignBottom);
        }
    }
}

void CanvasWidget::paintEdge(QPainter* p, const Edge& e, bool label)
{
    using namespace canvasui;
    const auto g = edgeGeometry(e);
    if (!g)
        return;
    const Palette& pal = Palette::current();
    const CanvasColor cc = resolveColor(e.color, pal);
    const double zm = zoomMultiplier();
    p->setRenderHint(QPainter::Antialiasing);
    if (label) {
        const QRectF lr = edgeLabelRect(e);
        if (lr.isNull() || m_labelTarget == u"edge:" + e.id)
            return;
        p->setPen(Qt::NoPen);
        p->setBrush(pal.card);
        p->drawRoundedRect(lr, 4, 4);
        const QRectF text = lr.adjusted(6 * zm, 6 * zm, -6 * zm, -6 * zm);
        drawScaledText(p, text, zm, uiFont(pal.edgeLabelPx), pal.textNormal, e.label, Qt::AlignCenter | Qt::TextWordWrap, false);
        return;
    }
    const bool focused = m_selection.contains(e.id);
    const bool connecting = m_drag && (m_drag->kind == Drag::Kind::Connect || m_drag->kind == Drag::Kind::EdgeEnd);
    const bool hovered = !connecting && m_hover.edge == e.id;
    if (m_drag && m_drag->kind == Drag::Kind::EdgeEnd && m_drag->edgeId == e.id && m_drag->started)
        return; // drawn by the overlay while it is being reconnected
    if (focused || hovered) {
        QPen hl(withAlpha(cc.color, 0.10), 24 * zm, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(hl);
        p->setBrush(Qt::NoBrush);
        p->drawPath(g->path);
    }
    QPen pen(cc.color, (focused || hovered ? 5.5 : 2) * zm, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(g->path);
    p->setPen(QPen(cc.color, 1 * zm, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(cc.color);
    const Node* from = m_doc.data().node(e.fromNode);
    const Node* to = m_doc.data().node(e.toNode);
    Q_UNUSED(from);
    Q_UNUSED(to);
    if (g->fromArrow)
        p->drawPolygon(arrowHead(g->fromAnchor, e.fromSide, zm));
    if (g->toArrow)
        p->drawPolygon(arrowHead(g->toAnchor, e.toSide, zm));
}

void CanvasWidget::paintOverlay(QPainter* p)
{
    using namespace canvasui;
    const Palette& pal = Palette::current();
    const double zm = zoomMultiplier();
    p->setRenderHint(QPainter::Antialiasing);

    // The multi-selection box (".canvas-selection.mod-group-selection").
    if (m_selection.size() > 1 && !(m_drag && m_drag->started && m_drag->kind == Drag::Kind::Move)) {
        const QRectF box = selectionBounds().adjusted(-10, -10, 10, 10);
        p->setPen(QPen(withAlpha(pal.accent, 0.30), 3));
        p->setBrush(withAlpha(pal.accent, 0.03));
        p->drawRoundedRect(box, 3, 3);
    }

    // Connection point of the hovered side (".canvas-node-connection-point").
    if (!m_drag && !m_readOnly && m_hover.connectSide && !m_hover.node.isEmpty()) {
        if (const Node* n = m_doc.data().node(m_hover.node)) {
            const QRectF c = connectionPoint(n->rect(), *m_hover.connectSide, zm);
            p->setPen(QPen(pal.border, 3));
            p->setBrush(pal.accent);
            p->drawEllipse(c.adjusted(1.5, 1.5, -1.5, -1.5));
        }
    }

    if (!m_drag || !m_drag->started)
        return;
    const Drag& d = *m_drag;
    auto dummy = [&](const QRectF& r) {
        // ".canvas-node.is-dummy": 4px accent border, accent 20% fill
        paintCardShadow(p, r, 8, true);
        p->setPen(QPen(pal.accent, 4));
        p->setBrush(withAlpha(pal.accent, 0.20));
        p->drawRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
    };
    switch (d.kind) {
    case Drag::Kind::BoxSelect: {
        p->setPen(QPen(pal.accent, 2));
        p->setBrush(withAlpha(pal.accent, 0.10));
        p->drawRect(d.box);
        break;
    }
    case Drag::Kind::Move:
        if (d.clone)
            for (auto it = d.startRects.cbegin(); it != d.startRects.cend(); ++it)
                dummy(it.value().translated(d.offset));
        break;
    case Drag::Kind::NewCard:
        if (!d.cardRect.isNull())
            dummy(d.cardRect);
        break;
    case Drag::Kind::Connect:
    case Drag::Kind::EdgeEnd: {
        const Node* fixed = m_doc.data().node(d.fixedNode);
        if (!fixed)
            break;
        QRectF freeRect(d.pointer, QSizeF(0, 0));
        Side freeSide = d.freeSide;
        if (d.target) {
            freeRect = m_doc.data().nodes[d.target->node].rect();
            freeSide = d.target->side;
        }
        EdgeGeometry g = d.movingFrom ? canvas::edgeGeometry(freeRect, freeSide, fixed->rect(), d.fixedSide, d.freeEnd, d.fixedEnd)
                                      : canvas::edgeGeometry(fixed->rect(), d.fixedSide, freeRect, freeSide, d.fixedEnd, d.freeEnd);
        QColor color = pal.defaultColor;
        if (d.kind == Drag::Kind::EdgeEnd)
            if (const Edge* e = m_doc.data().edge(d.edgeId))
                color = resolveColor(e->color, pal).color;
        p->setPen(QPen(color, 2 * zm, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawPath(g.path);
        p->setPen(QPen(color, 1 * zm));
        p->setBrush(color);
        if (g.fromArrow)
            p->drawPolygon(arrowHead(g.fromAnchor, d.movingFrom ? freeSide : d.fixedSide, zm));
        if (g.toArrow)
            p->drawPolygon(arrowHead(g.toAnchor, d.movingFrom ? d.fixedSide : freeSide, zm));
        break;
    }
    default:
        break;
    }

    // Snap guides (".canvas-snaps": circles r=4 and lines, accent at 60%).
    if (!d.guides.isEmpty()) {
        QColor c = withAlpha(pal.accent, 0.6);
        for (const std::optional<AxisSnap>* axis : {&d.guides.x, &d.guides.y}) {
            if (!*axis)
                continue;
            for (const SnapMatch& m : (*axis)->matches) {
                QList<QPointF> pts;
                for (const QPointF& s : m.src)
                    pts << s + d.guides.offset;
                pts << m.dest;
                if (pts.isEmpty())
                    continue;
                double minX = pts[0].x(), minY = pts[0].y(), maxX = minX, maxY = minY;
                p->setPen(Qt::NoPen);
                p->setBrush(c);
                for (const QPointF& pt : pts) {
                    p->drawEllipse(pt, 4, 4);
                    minX = std::min(minX, pt.x());
                    minY = std::min(minY, pt.y());
                    maxX = std::max(maxX, pt.x());
                    maxY = std::max(maxY, pt.y());
                }
                p->setPen(QPen(c, 1));
                p->drawLine(QPointF(minX, minY), QPointF(maxX, maxY));
            }
        }
    }
}

void CanvasWidget::paintGrid(QPainter* p, const QRectF& viewRect)
{
    const Palette& pal = Palette::current();
    p->fillRect(viewRect, pal.background);
    const double s = m_vp.scale();
    const double D = gridSpacing(m_vp.zoom);
    const double step = D * s;
    if (step < 4)
        return;
    const double W = viewport()->width(), H = viewport()->height();
    // The pattern's origin (Obsidian: x = W/2 - (x % D) * scale), a 0.7px dot at (0.7, 0.7) per cell.
    double x0 = W / 2 - std::fmod(m_vp.x, D) * s;
    double y0 = H / 2 - std::fmod(m_vp.y, D) * s;
    x0 -= std::ceil((x0 - viewRect.left()) / step) * step;
    y0 -= std::ceil((y0 - viewRect.top()) / step) * step;
    QList<QPointF> pts;
    for (double y = y0; y < viewRect.bottom() + step; y += step)
        for (double x = x0; x < viewRect.right() + step; x += step)
            pts << QPointF(x + 0.7, y + 0.7);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(pal.dots, 1.4, Qt::SolidLine, Qt::RoundCap));
    p->drawPoints(pts.constData(), int(pts.size()));
    p->restore();
}

void CanvasWidget::drawBackground(QPainter* painter, const QRectF& rect) { paintGrid(painter, rect); }

void CanvasWidget::drawForeground(QPainter* painter, const QRectF& rect)
{
    Q_UNUSED(rect);
    using namespace canvasui;
    const Palette& pal = Palette::current();
    QString message = m_error;
    bool centered = !m_error.isEmpty();
    if (message.isEmpty() && m_doc.data().nodes.isEmpty() && !m_drag)
        message = tr("Drag from below or double click\nSpace + Drag to pan\nCtrl + Scroll to zoom"); // ".canvas-placeholder-message"
    if (message.isEmpty())
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QFont f = uiFont(pal.edgeLabelPx);
    const QFontMetricsF fm(f);
    const QRectF tb = fm.boundingRect(QRectF(0, 0, viewport()->width() * 0.7, 1e5), Qt::AlignCenter | Qt::TextWordWrap, message);
    const double lines = std::max(1.0, std::round(tb.height() / fm.lineSpacing()));
    const QSizeF box(tb.width() + 48, lines * pal.edgeLabelPx * 1.5 + 32);
    // Placed at the canvas origin (it is part of the canvas and scales with it).
    const double k = centered ? 1.0 : m_vp.scale();
    const QPointF c = centered ? QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0) : m_vp.toView(QPointF(0, 0));
    painter->translate(c);
    painter->scale(k, k);
    const QRectF r(-box.width() / 2, -box.height() / 2, box.width(), box.height());
    painter->setPen(Qt::NoPen);
    painter->setBrush(withAlpha(pal.accent, 0.10));
    painter->drawRoundedRect(r, 8, 8);
    painter->setFont(f);
    painter->setPen(pal.accent);
    painter->drawText(r.adjusted(24, 16, -24, -16), Qt::AlignCenter | Qt::TextWordWrap, message);
    painter->restore();
}

// ---------------------------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------------------------

void CanvasWidget::setSelectionInternal(QSet<QString> ids)
{
    if (ids == m_selection)
        return;
    if (!m_editingId.isEmpty() && !ids.contains(m_editingId))
        stopEditing();
    if (!m_labelTarget.isEmpty())
        finishLabelEdit(true);
    if (QGraphicsItem* fi = scene()->focusItem(); fi && fi != m_editorProxy)
        scene()->clearFocus(); // a media card's widget had the keys
    m_selection = std::move(ids);
    m_syncedRevision = ~0ull;
    syncItems();
    updateOverlays();
    viewport()->update();
    emit selectionChanged();
}

void CanvasWidget::setSelection(const QSet<QString>& ids)
{
    QSet<QString> valid;
    for (const QString& id : ids)
        if (m_doc.data().node(id) || m_doc.data().edge(id))
            valid.insert(id);
    setSelectionInternal(valid);
}

void CanvasWidget::selectOnly(const QString& id) { setSelection({id}); }

void CanvasWidget::selectAll()
{
    QSet<QString> all;
    for (const Node& n : m_doc.data().nodes)
        if (n.isShown())
            all.insert(n.id);
    setSelectionInternal(all);
}

void CanvasWidget::deselectAll() { setSelectionInternal({}); }

QRectF CanvasWidget::selectionBounds() const { return itemsBounds(m_doc.data(), m_selection); }

void CanvasWidget::setReadOnly(bool readOnly)
{
    if (m_readOnly == readOnly)
        return;
    stopEditing();
    m_readOnly = readOnly;
    m_controls->setReadOnly(readOnly);
    updateOverlays();
    viewport()->update();
    emit readOnlyChanged(readOnly);
}

// ---------------------------------------------------------------------------------------------
// Creating and editing
// ---------------------------------------------------------------------------------------------

QString CanvasWidget::createTextNode(const QPointF& pos, Anchor anchor, std::optional<QSizeF> size, const QString& text, bool edit)
{
    if (m_readOnly)
        return {};
    Node n;
    n.type = NodeType::Text;
    n.text = text;
    const QSizeF sz = size.value_or(QSizeF(kDefaultTextWidth, kDefaultTextHeight));
    n.setRect(QRectF(placeRect(pos, sz, anchor), sz));
    const QString id = m_doc.addNode(std::move(n));
    commit();
    if (edit) {
        selectOnly(id);
        startEditing(id);
    }
    return id;
}

QString CanvasWidget::createFileNode(const QString& path, const QPointF& pos, Anchor anchor, std::optional<QSizeF> size, const QString& subpath,
                                     bool select)
{
    if (m_readOnly || path.isEmpty())
        return {};
    Node n;
    n.type = NodeType::File;
    n.file = path;
    n.subpath = subpath;
    QSizeF sz = size.value_or(QSizeF(kDefaultFileWidth, kDefaultFileHeight));
    // Media keep their aspect ratio (Obsidian resizes the card once the image has loaded).
    if (isImagePath(path) && m_session && m_session->vault()) {
        const QSize nat = ImageCache::instance().naturalSize(m_session->vault()->absolutePath(path));
        if (nat.isValid() && nat.height() > 0) {
            const double ratio = double(nat.width()) / nat.height();
            sz = QSizeF(std::min(sz.width(), sz.height() * ratio), std::min(sz.height(), sz.width() / ratio));
        }
    } else if (m_session && m_session->vault() && !size) {
        // Audio cards take the player's height, videos their picture's ratio (Obsidian resizes the card
        // once the media has loaded).
        const QString ext = path.section(u'.', -1).toLower();
        static const QStringList audio = {u"mp3"_s, u"wav"_s, u"m4a"_s, u"3gp"_s, u"flac"_s, u"ogg"_s, u"oga"_s, u"opus"_s};
        static const QStringList video = {u"mp4"_s, u"webm"_s, u"ogv"_s, u"mov"_s, u"mkv"_s};
        if (const std::shared_ptr<EmbedProvider> provider = EmbedRegistry::instance().provider(ext);
            provider && (audio.contains(ext) || video.contains(ext))) {
            const EmbedRequest req = EmbedRequest::fromLink(m_session->vault()->absolutePath(path), path, subpath);
            const QSizeF natural = provider->size(req, sz.width());
            if (audio.contains(ext)) {
                sz.setHeight(natural.height());
            } else if (natural.height() > 0) {
                const double ratio = natural.width() / natural.height();
                sz = QSizeF(std::min(sz.width(), sz.height() * ratio), std::min(sz.height(), sz.width() / ratio));
            }
        }
    }
    n.setRect(QRectF(placeRect(pos, sz, anchor), sz));
    const QString id = m_doc.addNode(std::move(n));
    commit();
    if (select)
        selectOnly(id);
    return id;
}

QStringList CanvasWidget::createFileNodes(const QStringList& paths, const QPointF& pos)
{
    QStringList ids;
    if (m_readOnly)
        return ids;
    int i = 0;
    for (const QString& p : paths) {
        const QPointF at(pos.x() + (kDefaultFileWidth + 45) * (i % 10), pos.y() + (kDefaultFileHeight + 45) * (i / 10));
        Node n;
        n.type = NodeType::File;
        n.file = p;
        n.setRect(QRectF(at, QSizeF(kDefaultFileWidth, kDefaultFileHeight)));
        ids << m_doc.addNode(std::move(n));
        ++i;
    }
    commit();
    setSelection(QSet<QString>(ids.begin(), ids.end()));
    return ids;
}

QString CanvasWidget::createLinkNode(const QString& url, const QPointF& pos, Anchor anchor, std::optional<QSizeF> size)
{
    if (m_readOnly || url.isEmpty())
        return {};
    Node n;
    n.type = NodeType::Link;
    n.url = url;
    QSizeF sz = size.value_or(QSizeF(kDefaultFileWidth, kDefaultFileHeight));
    static const QRegularExpression youtube(uR"(^https?://(www\.)?(youtube\.com|youtu\.be)/)"_s);
    if (!size && youtube.match(url).hasMatch())
        sz = QSizeF(640, 360);
    n.setRect(QRectF(placeRect(pos, sz, anchor), sz));
    const QString id = m_doc.addNode(std::move(n));
    commit();
    selectOnly(id);
    return id;
}

QString CanvasWidget::createGroupNode(const QRectF& rect, const QString& label, bool editLabel)
{
    if (m_readOnly)
        return {};
    Node n;
    n.type = NodeType::Group;
    n.label = label.isEmpty() ? tr("Untitled group") : label; // Obsidian names new groups
    n.setRect(rect);
    const QString id = m_doc.addNode(std::move(n));
    commit();
    selectOnly(id);
    if (editLabel)
        editGroupLabel(id);
    return id;
}

void CanvasWidget::deleteSelection()
{
    if (m_readOnly || m_selection.isEmpty())
        return;
    stopEditing();
    m_doc.removeItems(m_selection);
    m_selection.clear();
    commit();
    emit selectionChanged();
}

void CanvasWidget::createGroupFromSelection()
{
    if (m_readOnly || m_selection.isEmpty())
        return;
    const QRectF box = selectionBounds().adjusted(-20, -20, 20, 20);
    createGroupNode(box);
}

void CanvasWidget::duplicateSelection()
{
    if (m_readOnly || m_selection.isEmpty())
        return;
    const Json clip = m_doc.selectionData(m_selection);
    const QStringList added = m_doc.importClone(clip, QPointF(0, 0));
    commit();
    setSelection(QSet<QString>(added.begin(), added.end()));
}

void CanvasWidget::setSelectionColor(const QString& color)
{
    if (m_readOnly || m_selection.isEmpty())
        return;
    m_doc.setColor(m_selection, color);
    commit();
    viewport()->update();
}

void CanvasWidget::setSelectionEdgeEnds(EdgeEnd fromEnd, EdgeEnd toEnd)
{
    if (m_readOnly)
        return;
    m_doc.setEdgeEnds(m_selection, fromEnd, toEnd);
    commit();
}

void CanvasWidget::copySelection()
{
    if (m_selection.isEmpty())
        return;
    auto* md = new QMimeData;
    md->setData(u"obsidian/canvas"_s, stringifyCompact(m_doc.selectionData(m_selection)));
    QGuiApplication::clipboard()->setMimeData(md);
}

void CanvasWidget::cutSelection()
{
    if (m_readOnly)
        return;
    copySelection();
    deleteSelection();
}

void CanvasWidget::paste()
{
    if (m_readOnly)
        return;
    const QMimeData* md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return;
    const QPointF center = m_vp.toCanvas(QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
    if (md->hasFormat(u"obsidian/canvas"_s)) {
        const std::optional<Json> clip = parseJson(md->data(u"obsidian/canvas"_s));
        if (!clip || !clip->value(u"nodes").isArray() || !clip->value(u"edges").isArray())
            return;
        QPointF offset;
        const Json c = clip->value(u"center");
        if (c.isObject())
            offset = center - QPointF(c.value(u"x").toDouble(), c.value(u"y").toDouble());
        const QStringList added = m_doc.importClone(*clip, offset);
        commit();
        setSelection(QSet<QString>(added.begin(), added.end()));
        return;
    }
    // Files copied in the file explorer / file manager inside the vault.
    if (md->hasUrls() && m_session && m_session->vault()) {
        QStringList paths;
        for (const QUrl& u : md->urls())
            if (u.isLocalFile())
                if (const auto vp = m_session->vault()->vaultPath(u.toLocalFile()); vp && m_session->vault()->file(*vp))
                    paths << *vp;
        if (!paths.isEmpty()) {
            createFileNodes(paths, center);
            return;
        }
    }
    const QString text = md->text();
    if (text.isEmpty())
        return;
    static const QRegularExpression url(uR"(^https?://\S+$)"_s);
    if (url.match(text.trimmed()).hasMatch())
        createLinkNode(text.trimmed(), center, Anchor::Center);
    else
        createTextNode(center, Anchor::Center, std::nullopt, text, false);
}

// ---------------------------------------------------------------------------------------------
// In-place editing
// ---------------------------------------------------------------------------------------------

EditorView* CanvasWidget::activeEditor() const { return m_editor; }

void CanvasWidget::startEditing(const QString& id)
{
    if (m_readOnly)
        return;
    const Node* n = m_doc.data().node(id);
    if (!n)
        return;
    if (n->isGroup()) {
        editGroupLabel(id);
        return;
    }
    std::shared_ptr<Document> doc;
    if (n->type == NodeType::Text) {
        auto holder = std::make_shared<MemoryDocument>();
        holder->storage = std::make_unique<MemoryStorage>(n->text.toUtf8());
        holder->doc = std::make_unique<Document>(holder->storage.get(), u"canvas-card:"_s + id + u".md"_s);
        holder->doc->setAutosaveDelayMs(24 * 3600 * 1000);
        if (!holder->doc->load())
            return;
        doc = std::shared_ptr<Document>(holder, holder->doc.get());
    } else if (n->type == NodeType::File && m_session && m_session->vault() && m_session->documents()) {
        TFile* f = m_session->vault()->file(n->file);
        if (!f || !f->isMarkdown())
            return;
        DocumentManager* dm = m_session->documents();
        Document* d = dm->acquire(f->path());
        if (!d)
            return;
        QPointer<DocumentManager> guard = dm;
        doc = std::shared_ptr<Document>(d, [guard](Document* p) {
            if (guard)
                guard->release(p);
        });
    } else {
        return;
    }
    stopEditing();
    if (m_selection != QSet<QString>{id})
        setSelectionInternal({id});
    // Obsidian zooms in when the card would be too small to edit.
    if (m_vp.zoom < kZoomedOutZoom - m_options.zoomBreakpoint + 0.1) {
        zoomToRect(n->rect());
        finishViewport();
    }
    m_editingId = id;
    m_editDoc = doc;
    m_editor = new EditorView;
    m_editor->setObjectName(u"ocCanvasCardEditor"_s);
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->setDocument(doc);
    m_editor->setMode(EditorView::Mode::LivePreview);
    m_editor->setReadableLineLength(false);
    if (n->type == NodeType::File && !n->subpath.isEmpty() && m_session->metadata()) {
        if (const md::CachedMetadata* meta = m_session->metadata()->fileCache(n->file))
            if (const auto r = resolveSubpath(*meta, n->subpath))
                m_editor->setVisibleLineRange(r->startLine, r->endLine);
    }
    if (LivePreviewDecorator* lp = m_editor->livePreview()) {
        lp->isResolved = resolver()->isResolved;
        lp->resolveAttachment = resolver()->resolveAttachment;
    }
    const canvasui::CanvasColor cc = canvasui::resolveColor(n->color, Palette::current());
    QColor bg = Palette::current().card;
    if (cc.themed) {
        const QColor tint = cc.color;
        bg = QColor::fromRgbF(float(bg.redF() * 0.93 + tint.redF() * 0.07), float(bg.greenF() * 0.93 + tint.greenF() * 0.07),
                              float(bg.blueF() * 0.93 + tint.blueF() * 0.07));
    }
    m_editor->setBackgroundColor(bg);
    m_editor->installEventFilter(this);
    m_editor->viewport()->installEventFilter(this);
    connect(m_editor, &EditorView::linkActivated, this, [this](const QString& target, int kind, bool newLeaf) {
        Q_UNUSED(kind);
        openLinkFromCard(target, newLeaf);
    });
    connect(m_editor, &EditorView::linkHovered, this, [this](const QString& target, int, const QRect& rect) {
        if (target.isEmpty() || !m_editorProxy)
            return;
        const QRectF canvasRect = m_editorProxy->mapRectToScene(QRectF(rect));
        const QRect global(viewport()->mapToGlobal(canvasRect.topLeft().toPoint()), canvasRect.size().toSize());
        emit linkHovered(target, global);
    });
    if (n->type == NodeType::Text)
        connect(doc.get(), &Document::changed, this, [this] { onEditorTextChanged(); });
    m_editorProxy = new QGraphicsProxyWidget(m_root);
    m_editorProxy->setWidget(m_editor);
    m_editorProxy->setZValue(3e6);
    positionEditor();
    m_editorProxy->show();
    scene()->setFocusItem(m_editorProxy);
    m_editor->setFocus();
    if (n->type == NodeType::Text)
        m_editor->setCursorPosition(doc->text().length(), false);
    viewport()->update();
    updateOverlays();
}

void CanvasWidget::positionEditor()
{
    if (!m_editorProxy)
        return;
    const Node* n = m_doc.data().node(m_editingId);
    if (!n) {
        QTimer::singleShot(0, this, [this] { stopEditing(); });
        return;
    }
    const QRectF inner = n->rect().adjusted(1, 1, -1, -1);
    double top = 16;
    if (n->type == NodeType::Text) {
        if (canvasui::CardContent* c = contentFor(n->id))
            top = canvasui::MarkdownContent::topPadding(inner.height(), c->contentHeight(inner.width()));
    }
    m_editor->setMargins(16, top, 16);
    m_editorProxy->setGeometry(inner);
    if (m_editor->size() != inner.size().toSize())
        m_editor->resize(inner.size().toSize());
}

void CanvasWidget::onEditorTextChanged()
{
    if (!m_editDoc || m_editingId.isEmpty())
        return;
    if (m_doc.setText(m_editingId, m_editDoc->text().toString())) {
        m_contents.remove(m_editingId);
        m_textCommit.start();
        emit edited(); // saved soon (the history step follows the typing pause)
    }
}

void CanvasWidget::stopEditing()
{
    if (m_editingId.isEmpty() && !m_editorProxy)
        return;
    const QString id = m_editingId;
    if (m_editDoc && m_doc.data().node(id) && m_doc.data().node(id)->type == NodeType::Text)
        m_doc.setText(id, m_editDoc->text().toString());
    m_editingId.clear();
    if (m_editor) {
        m_editor->removeEventFilter(this);
        m_editor->viewport()->removeEventFilter(this);
        disconnect(m_editor, nullptr, this, nullptr);
        m_editor->setDocument(nullptr);
    }
    if (m_editDoc)
        disconnect(m_editDoc.get(), nullptr, this, nullptr);
    m_editDoc.reset();
    if (m_editorProxy) {
        QGraphicsProxyWidget* proxy = m_editorProxy;
        m_editorProxy = nullptr;
        m_editor = nullptr;
        proxy->hide();
        proxy->deleteLater(); // may be called from the editor's own event handler
    }
    m_contents.remove(id);
    commit();
    if (hasFocus() || (window() && window()->isActiveWindow()))
        setFocus();
    viewport()->update();
}

void CanvasWidget::openLinkFromCard(const QString& target, bool newLeaf)
{
    emit linkActivated(target, newLeaf);
}

void CanvasWidget::editGroupLabel(const QString& groupId)
{
    const Node* n = m_doc.data().node(groupId);
    if (!n || !n->isGroup() || m_readOnly)
        return;
    finishLabelEdit(true);
    m_labelTarget = u"node:"_s + groupId;
    if (!m_labelEdit) {
        m_labelEdit = new QLineEdit(viewport());
        m_labelEdit->setObjectName(u"ocCanvasLabelEditor"_s);
        m_labelEdit->setFrame(false);
        m_labelEdit->installEventFilter(this);
        connect(m_labelEdit, &QLineEdit::editingFinished, this, [this] { finishLabelEdit(true); });
    }
    m_labelEdit->setText(n->label);
    m_labelEdit->setPlaceholderText(QString());
    m_labelEdit->selectAll();
    m_syncedRevision = ~0ull;
    syncItems();
    updateOverlays();
    m_labelEdit->show();
    m_labelEdit->setFocus();
}

void CanvasWidget::editEdgeLabel(const QString& edgeId)
{
    const Edge* e = m_doc.data().edge(edgeId);
    if (!e || m_readOnly)
        return;
    finishLabelEdit(true);
    if (m_selection != QSet<QString>{edgeId})
        setSelectionInternal({edgeId});
    m_labelTarget = u"edge:"_s + edgeId;
    if (!m_labelEdit) {
        m_labelEdit = new QLineEdit(viewport());
        m_labelEdit->setObjectName(u"ocCanvasLabelEditor"_s);
        m_labelEdit->setFrame(false);
        m_labelEdit->installEventFilter(this);
        connect(m_labelEdit, &QLineEdit::editingFinished, this, [this] { finishLabelEdit(true); });
    }
    m_labelEdit->setText(e->label);
    m_labelEdit->setPlaceholderText(tr("Add text..."));
    m_labelEdit->selectAll();
    m_syncedRevision = ~0ull;
    syncItems();
    updateOverlays();
    m_labelEdit->show();
    m_labelEdit->setFocus();
}

QLineEdit* CanvasWidget::labelEditor() const { return m_labelEdit && m_labelEdit->isVisible() ? m_labelEdit : nullptr; }

void CanvasWidget::finishLabelEdit(bool commitEdit)
{
    if (m_labelTarget.isEmpty())
        return;
    const QString target = m_labelTarget;
    m_labelTarget.clear();
    const QString text = m_labelEdit ? m_labelEdit->text() : QString();
    if (m_labelEdit) {
        m_labelEdit->hide();
        m_labelEdit->clearFocus();
    }
    if (commitEdit) {
        if (target.startsWith(u"node:"))
            m_doc.setGroupLabel(target.mid(5), text);
        else if (target.startsWith(u"edge:"))
            m_doc.setEdgeLabel(target.mid(5), text.trimmed().isEmpty() ? QString() : text);
        commit();
    }
    m_syncedRevision = ~0ull;
    syncItems();
    updateOverlays();
    setFocus();
}

// ---------------------------------------------------------------------------------------------
// Selection-menu actions
// ---------------------------------------------------------------------------------------------

namespace {
// Obsidian's C(): the cards to align, and the box to align them in — a single selected group's
// contents inside the group (inset 20), else the selected cards and their box.
std::pair<QStringList, QRectF> alignTargets(const CanvasData& d, const QSet<QString>& selection)
{
    if (selection.size() == 1) {
        const Node* g = d.node(*selection.begin());
        if (g && g->isGroup()) {
            QStringList ids;
            for (int i : nodesInside(d, g->rect(), d.nodeIndex(g->id)))
                ids << d.nodes[i].id;
            return {ids, g->rect().adjusted(20, 20, -20, -20)};
        }
    }
    QStringList ids;
    QSet<QString> set;
    for (const Node& n : d.nodes)
        if (n.isShown() && selection.contains(n.id)) {
            ids << n.id;
            set.insert(n.id);
        }
    return {ids, itemsBounds(d, set)};
}
} // namespace

void CanvasWidget::alignSelection(Align how)
{
    if (m_readOnly)
        return;
    const auto [ids, box] = alignTargets(m_doc.data(), m_selection);
    m_doc.align(ids, how, box);
    commit();
}

void CanvasWidget::distributeSelection(Qt::Orientation orientation)
{
    if (m_readOnly)
        return;
    const auto [ids, box] = alignTargets(m_doc.data(), m_selection);
    m_doc.distribute(ids, orientation, box);
    commit();
}

void CanvasWidget::justifySelection(Qt::Orientation orientation)
{
    if (m_readOnly)
        return;
    const auto [ids, box] = alignTargets(m_doc.data(), m_selection);
    m_doc.justify(ids, orientation, box);
    commit();
}

void CanvasWidget::arrangeSelection(Arrange how)
{
    if (m_readOnly)
        return;
    const auto [ids, box] = alignTargets(m_doc.data(), m_selection);
    m_doc.arrange(ids, how, box, gridSpacing(m_vp.zoom));
    commit();
}

// ---------------------------------------------------------------------------------------------
// Floating widgets
// ---------------------------------------------------------------------------------------------

void CanvasWidget::updateOverlays()
{
    if (!m_controls)
        return;
    const int W = viewport()->width(), H = viewport()->height();
    m_controls->move(W - m_controls->width() - 8 + 2, 8);
    m_controls->raise();
    m_cardMenu->setVisible(!m_readOnly);
    m_cardMenu->move((W - m_cardMenu->width()) / 2, H - 16 - m_cardMenu->height() + 3);
    m_cardMenu->raise();
    m_controls->setHistory(canUndo(), canRedo());

    // Label editor over its label.
    if (m_labelEdit && !m_labelTarget.isEmpty()) {
        const Palette& pal = Palette::current();
        const double zm = zoomMultiplier();
        const double s = m_vp.scale();
        QRectF canvasRect;
        double px = pal.uiFontPx;
        if (m_labelTarget.startsWith(u"node:")) {
            if (const Node* n = m_doc.data().node(m_labelTarget.mid(5))) {
                px = pal.uiFontPx * 1.5;
                const double h = (px + 8) * zm;
                canvasRect = QRectF(n->x, n->y - 4 * zm - h, std::max(n->width, 200.0 * zm), h);
            }
        } else if (const Edge* e = m_doc.data().edge(m_labelTarget.mid(5))) {
            px = pal.edgeLabelPx;
            if (const auto g = edgeGeometry(*e)) {
                const double w = 17 * px * zm, h = (px * 1.3 + 12) * zm;
                canvasRect = QRectF(g->center().x() - w / 2, g->center().y() - h / 2, w, h);
            }
        }
        if (!canvasRect.isNull()) {
            QFont f = canvasui::uiFont(std::max(6.0, px * zm * s));
            m_labelEdit->setFont(f);
            m_labelEdit->setAlignment(m_labelTarget.startsWith(u"edge:") ? Qt::AlignCenter : Qt::AlignLeft);
            QPalette qp = m_labelEdit->palette();
            qp.setColor(QPalette::Base, pal.card);
            qp.setColor(QPalette::Text, pal.textNormal);
            m_labelEdit->setPalette(qp);
            m_labelEdit->setGeometry(m_vp.toView(canvasRect).toAlignedRect());
            m_labelEdit->raise();
        }
    }

    // The selection menu (".canvas-menu") above the selection.
    const bool dragging = m_drag && m_drag->started;
    if (m_selection.isEmpty() || dragging) {
        m_selectionMenu->hide();
        return;
    }
    // Rebuild the buttons when the selection changes.
    static const char* kProp = "ocSelectionKey";
    QStringList keyParts(m_selection.begin(), m_selection.end());
    keyParts.sort();
    QString key = keyParts.join(u',') + (m_readOnly ? u"|ro"_s : QString());
    if (m_selection.size() == 1)
        if (const Edge* e = m_doc.data().edge(*m_selection.begin()))
            key += u"|l"_s + QString::number(!e->label.isEmpty()) + QString::number(int(e->fromEnd)) + QString::number(int(e->toEnd));
    if (m_selection.size() == 1)
        if (const Node* n = m_doc.data().node(*m_selection.begin()))
            key += u"|b"_s + n->background;
    if (m_selectionMenu->property(kProp).toString() != key) {
        m_selectionMenu->setProperty(kProp, key);
        QList<canvasui::SelectionMenu::Item> items;
        const CanvasData& d = m_doc.data();
        // Obsidian orders by z-index; the "last" item decides the menu.
        QList<QString> nodesSel, edgesSel;
        for (int i : paintOrder(d))
            if (m_selection.contains(d.nodes[i].id))
                nodesSel << d.nodes[i].id;
        for (const Edge& e : d.edges)
            if (m_selection.contains(e.id))
                edgesSel << e.id;
        const bool single = m_selection.size() == 1;
        const bool lastIsNode = !nodesSel.isEmpty() && edgesSel.isEmpty();
        const Node* lastNode = nodesSel.isEmpty() ? nullptr : d.node(nodesSel.last());
        const Edge* lastEdge = edgesSel.isEmpty() ? nullptr : d.edge(edgesSel.last());
        if (!m_readOnly) {
            items << canvasui::SelectionMenu::Item{u"remove"_s, u"trash-2"_s, tr("Remove"), [this](canvasui::CanvasButton*) { deleteSelection(); }};
            items << canvasui::SelectionMenu::Item{u"color"_s, u"palette"_s, tr("Set color"), [this](canvasui::CanvasButton*) {
                                                       QString current;
                                                       const CanvasData& dd = m_doc.data();
                                                       for (const QString& id : std::as_const(m_selection)) {
                                                           if (const Node* n = dd.node(id))
                                                               current = n->color;
                                                           else if (const Edge* e = dd.edge(id))
                                                               current = e->color;
                                                       }
                                                       m_selectionMenu->toggleColorPicker(current, [this](const QString& c) { setSelectionColor(c); });
                                                       updateOverlays();
                                                   }};
        }
        items << canvasui::SelectionMenu::Item{u"zoom-to-selection"_s, u"scan-search"_s, tr("Zoom to selection\n(Shift+2)"),
                                               [this](canvasui::CanvasButton*) { zoomToSelection(); }};
        if (!m_readOnly) {
            if (!single && lastIsNode)
                items << canvasui::SelectionMenu::Item{u"create-group"_s, u"group"_s, tr("Create group"),
                                                       [this](canvasui::CanvasButton*) { createGroupFromSelection(); }};
            if ((!single && lastIsNode) || (single && lastNode && lastNode->isGroup()))
                items << canvasui::SelectionMenu::Item{u"align"_s, u"align-start-vertical"_s, tr("Align"), [this](canvasui::CanvasButton* b) {
                                                           QMenu* m = newMenu();
                                                           auto add = [&](const QString& t, const char* icon, std::function<void()> f) {
                                                               QList<MenuEntry> e{{QString(), t, QString::fromLatin1(icon), std::move(f)}};
                                                               buildSectionedMenu(m, e);
                                                           };
                                                           add(tr("Align left"), "align-start-vertical", [this] { alignSelection(Align::Left); });
                                                           add(tr("Align center"), "align-center-vertical", [this] { alignSelection(Align::Center); });
                                                           add(tr("Align right"), "align-end-vertical", [this] { alignSelection(Align::Right); });
                                                           m->addSeparator();
                                                           add(tr("Align top"), "align-start-horizontal", [this] { alignSelection(Align::Top); });
                                                           add(tr("Align middle"), "align-center-horizontal", [this] { alignSelection(Align::Middle); });
                                                           add(tr("Align bottom"), "align-end-horizontal", [this] { alignSelection(Align::Bottom); });
                                                           m->addSeparator();
                                                           add(tr("Arrange in a row"), "columns-3", [this] { arrangeSelection(Arrange::Row); });
                                                           add(tr("Arrange in a column"), "rows-3", [this] { arrangeSelection(Arrange::Column); });
                                                           add(tr("Arrange in a grid"), "layout-grid", [this] { arrangeSelection(Arrange::Grid); });
                                                           m->addSeparator();
                                                           add(tr("Distribute horizontal spacing"), "align-horizontal-space-around",
                                                               [this] { distributeSelection(Qt::Horizontal); });
                                                           add(tr("Distribute vertical spacing"), "align-vertical-space-around",
                                                               [this] { distributeSelection(Qt::Vertical); });
                                                           m->addSeparator();
                                                           add(tr("Justify horizontally"), "stretch-horizontal", [this] { justifySelection(Qt::Horizontal); });
                                                           add(tr("Justify vertically"), "stretch-vertical", [this] { justifySelection(Qt::Vertical); });
                                                           popupMenu(m, b->mapToGlobal(b->rect().bottomLeft()));
                                                       }};
            if (lastEdge && edgesSel.size() == int(m_selection.size())) {
                QString icon = u"arrow-right"_s;
                if (lastEdge->fromEnd == EdgeEnd::Arrow)
                    icon = u"move-horizontal"_s;
                else if (lastEdge->toEnd == EdgeEnd::None)
                    icon = u"minus"_s;
                items << canvasui::SelectionMenu::Item{u"line-direction"_s, icon, tr("Line direction"), [this, icon](canvasui::CanvasButton* b) {
                                                           QMenu* m = newMenu();
                                                           auto add = [&](const QString& t, const QString& ic, EdgeEnd from, EdgeEnd to) {
                                                               QAction* a = m->addAction(IconProvider::instance().icon(ic, 16, 2.0), t);
                                                               a->setCheckable(true);
                                                               a->setChecked(ic == icon);
                                                               connect(a, &QAction::triggered, this, [this, from, to] { setSelectionEdgeEnds(from, to); });
                                                           };
                                                           add(tr("Nondirectional"), u"minus"_s, EdgeEnd::None, EdgeEnd::None);
                                                           add(tr("Unidirectional"), u"arrow-right"_s, EdgeEnd::None, EdgeEnd::Arrow);
                                                           add(tr("Bidirectional"), u"move-horizontal"_s, EdgeEnd::Arrow, EdgeEnd::Arrow);
                                                           popupMenu(m, b->mapToGlobal(b->rect().bottomLeft()));
                                                       }};
            }
            if (single && lastEdge && !lastEdge->label.isEmpty()) {
                const QString id = lastEdge->id;
                items << canvasui::SelectionMenu::Item{u"remove-label"_s, u"square-x"_s, tr("Remove label"), [this, id](canvasui::CanvasButton*) {
                                                           m_doc.setEdgeLabel(id, QString());
                                                           commit();
                                                       }};
            }
            if (single && lastEdge) {
                const QString id = lastEdge->id;
                items << canvasui::SelectionMenu::Item{u"edit"_s, u"square-pen"_s, tr("Edit label"),
                                                       [this, id](canvasui::CanvasButton*) { editEdgeLabel(id); }};
            } else if (single && lastNode) {
                const bool editable = lastNode->isGroup() || lastNode->type == NodeType::Text
                    || (lastNode->type == NodeType::File && m_session && m_session->vault() && m_session->vault()->file(lastNode->file)
                        && m_session->vault()->file(lastNode->file)->isMarkdown());
                const QString id = lastNode->id;
                if (editable)
                    items << canvasui::SelectionMenu::Item{u"edit"_s, u"square-pen"_s, lastNode->isGroup() ? tr("Edit label") : tr("Edit"),
                                                           [this, id](canvasui::CanvasButton*) { startEditing(id); }};
                if (lastNode->isGroup())
                    items << canvasui::SelectionMenu::Item{u"background"_s, u"image"_s,
                                                           lastNode->background.isEmpty() ? tr("Set background") : tr("Edit background"),
                                                           [this, id](canvasui::CanvasButton* b) {
                                                               const Node* g = m_doc.data().node(id);
                                                               if (!g)
                                                                   return;
                                                               if (g->background.isEmpty()) {
                                                                   pickGroupBackground(id);
                                                                   return;
                                                               }
                                                               QMenu* m = newMenu();
                                                               auto style = [this, id](const QString& s) {
                                                                   const Node* gg = m_doc.data().node(id);
                                                                   if (gg && m_doc.setGroupBackground(id, gg->background, s))
                                                                       commit();
                                                               };
                                                               m->addAction(IconProvider::instance().icon(u"repeat"_s, 16, 2.0), tr("Replace background"), this,
                                                                            [this, id] { pickGroupBackground(id); });
                                                               m->addAction(IconProvider::instance().icon(u"image-off"_s, 16, 2.0), tr("Remove background"), this,
                                                                            [this, id] {
                                                                                if (m_doc.setGroupBackground(id, QString(), QString()))
                                                                                    commit();
                                                                            });
                                                               m->addSeparator();
                                                               const QString cur = g->backgroundStyle;
                                                               auto opt = [&](const QString& t, const QString& ic, const QString& s, bool checked) {
                                                                   QAction* a = m->addAction(IconProvider::instance().icon(ic, 16, 2.0), t);
                                                                   a->setCheckable(true);
                                                                   a->setChecked(checked);
                                                                   connect(a, &QAction::triggered, this, [style, s] { style(s); });
                                                               };
                                                               opt(tr("Cover"), u"scaling"_s, u"cover"_s, cur.isEmpty() || cur == u"cover");
                                                               opt(tr("Keep aspect ratio"), u"ratio"_s, u"ratio"_s, cur == u"ratio");
                                                               opt(tr("Repeat"), u"layout-grid"_s, u"repeat"_s, cur == u"repeat");
                                                               popupMenu(m, b->mapToGlobal(b->rect().bottomLeft()));
                                                           }};
            }
        }
        m_selectionMenu->setItems(items);
    }
    if (m_selectionMenu->button(u"color"_s) == nullptr && m_selectionMenu->isColorPickerOpen())
        m_selectionMenu->toggleColorPicker({}, {});
    // Position (Obsidian's render()): centered above the selection's box.
    const CanvasData& d = m_doc.data();
    const QRectF box = selectionBounds();
    const double s = m_vp.scale();
    double R = box.top() - 10 / s;
    bool anyGroup = false, lastFile = false;
    QString last;
    for (int i : paintOrder(d))
        if (m_selection.contains(d.nodes[i].id))
            last = d.nodes[i].id;
    for (const QString& id : std::as_const(m_selection))
        if (const Node* n = d.node(id); n && n->isGroup())
            anyGroup = true;
    if (const Node* n = d.node(last); n && n->type == NodeType::File)
        lastFile = true;
    if (anyGroup)
        R = box.top() - 35 / std::sqrt(s);
    else if (lastFile)
        R = box.top() - 30 / std::sqrt(s);
    if (m_selection.size() == 1)
        if (const Edge* e = d.edge(*m_selection.begin()))
            if (const auto g = edgeGeometry(*e)) {
                const QRectF lr = edgeLabelRect(*e);
                const double V = lr.isNull() ? 20 : lr.height() * s;
                R = std::min(R, g->center().y() - 20 / s - V / 2 / s);
            }
    m_selectionMenu->adjustSize();
    const QRect tb = m_selectionMenu->toolbarRect();
    double F = (box.center().x() - m_vp.x) * s - tb.width() / 2.0 + W / 2.0;
    double top = (R - m_vp.y) * s - tb.height() + H / 2.0;
    F = std::clamp(F, 10.0, std::max(10.0, W - tb.width() - 10.0));
    top = std::clamp(top, 10.0, std::max(10.0, H - tb.height() - 10.0));
    m_selectionMenu->move(int(std::round(F)) - tb.x(), int(std::round(top)));
    m_selectionMenu->show();
    m_selectionMenu->raise();
}

// ---------------------------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------------------------

QImage CanvasWidget::renderImage(double zoom)
{
    const QRectF box = allNodesBounds(m_doc.data()).adjusted(-100, -100, 100, 100);
    if (m_doc.data().nodes.isEmpty() || box.isEmpty())
        return {};
    const double scale = std::pow(2.0, std::min(1.0, zoom));
    const QSize size = (box.size() * scale).toSize().boundedTo(QSize(16384, 16384));
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Palette::current().background);
    // Render with the canvas transform set to the image, nothing selected, no hover.
    const QSet<QString> selection = m_selection;
    const Hover hover = m_hover;
    const QString editing = m_editingId;
    m_selection.clear();
    m_hover = Hover{};
    const QTransform saved = m_root->transform();
    const Viewport savedVp = m_vp;
    m_vp.zoom = std::min(1.0, zoom); // labels, edge widths and placeholders follow the image's zoom
    const bool overlayVisible = m_overlay->isVisible();
    m_overlay->setVisible(false);
    m_root->setTransform(QTransform(scale, 0, 0, scale, -box.left() * scale, -box.top() * scale));
    m_syncedRevision = ~0ull;
    syncItems();
    {
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        scene()->render(&p, QRectF(QPointF(0, 0), QSizeF(size)), QRectF(QPointF(0, 0), QSizeF(size)));
    }
    m_vp = savedVp;
    m_root->setTransform(saved);
    m_overlay->setVisible(overlayVisible);
    m_selection = selection;
    m_hover = hover;
    Q_UNUSED(editing);
    m_syncedRevision = ~0ull;
    syncItems();
    return img;
}

} // namespace oc
