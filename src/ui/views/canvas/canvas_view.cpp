#include "ui/views/canvas/canvas_view.h"

#include "core/canvas/canvas_links.h"
#include "core/config/clone_paths.h"
#include "core/config/config_store.h"
#include "core/config/json_file.h"
#include "core/vault_session.h"
#include "ui/shell/main_window.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/views/canvas/canvas_widget.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QMenu>
#include <QVBoxLayout>

namespace oc {

using namespace Qt::StringLiterals;
using namespace oc::canvas;

namespace {
QString localDataPath(VaultSession* s)
{
    return ClonePaths::vaultDataDir(s->id()) + u"/canvas-local.json"_s;
}
} // namespace

CanvasView::CanvasView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_canvas(new CanvasWidget(this))
{
    setObjectName(u"ocCanvasView"_s);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_canvas);
    setFocusProxy(m_canvas);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(2000); // Obsidian's TextFileView requestSave debounce
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { save(); });
    connect(m_canvas, &CanvasWidget::edited, this, [this] {
        m_dirty = true;
        requestSave();
    });
    connect(m_canvas, &CanvasWidget::viewportChanged, this, &View::stateChanged);
    connect(m_canvas, &CanvasWidget::readOnlyChanged, this, [this] { saveLocalData(); });
    connect(m_canvas, &CanvasWidget::optionsEdited, this, [this] {
        VaultSession* s = session();
        if (!s || !s->config())
            return;
        QJsonObject o = s->config()->object(u"canvas"_s);
        m_canvas->options().writeTo(o);
        s->config()->setObject(u"canvas"_s, o);
        // Other canvases pick the options up too.
        if (Workspace* ws = workspace())
            for (WorkspaceLeaf* l : ws->leavesOfType(u"canvas"_s))
                if (auto* cv = qobject_cast<CanvasView*>(l->view()); cv && cv != this)
                    cv->canvas()->setOptions(CanvasWidget::Options::fromJson(o));
    });
    connect(m_canvas, &CanvasWidget::linkActivated, this, [this](const QString& linktext, bool newLeaf) {
        static const QRegularExpression scheme(uR"(^[a-zA-Z][a-zA-Z0-9+.-]*:)"_s);
        const int kind = scheme.match(linktext).hasMatch() ? 3 : 0;
        MarkdownView::openLinkText(workspace(), linktext, kind, m_file ? m_file->path() : QString(), newLeaf);
    });
    connect(m_canvas, &CanvasWidget::linkHovered, this, [this](const QString& linktext, const QRect& globalRect) {
        Workspace* ws = workspace();
        if (MainWindow* w = ws ? ws->window() : nullptr)
            PagePreview::of(w)->onLinkHover(u"canvas"_s, linktext, m_file ? m_file->path() : QString(), globalRect, m_canvas->viewport());
    });
}

CanvasView::~CanvasView()
{
    save();
    disconnect(m_vaultConn);
}

void CanvasView::onClose()
{
    save();
    m_canvas->stopEditing();
    save();
}

bool CanvasView::canAcceptExtension(const QString& extension) const { return extension.compare(u"canvas", Qt::CaseInsensitive) == 0; }

void CanvasView::focusContent() { m_canvas->setFocus(); }

void CanvasView::loadFile(TFile* file)
{
    if (m_file && file != m_file) {
        m_canvas->stopEditing();
        save();
    }
    m_file = file;
    m_dirty = false;
    m_invalid = false;
    m_saveTimer.stop();
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    if (vault && !m_vaultConn) {
        m_vaultConn = connect(vault, &Vault::modified, this, [this](TFile* f, bool) { onVaultModified(f); });
        // Cards showing notes and images follow their files.
        connect(vault, &Vault::modified, m_canvas, [this](TFile* f, bool) {
            if (f && f != m_file)
                m_canvas->invalidateFile(f->path());
        });
        connect(vault, &Vault::created, m_canvas, [this](TAbstractFile*, bool) { m_canvas->invalidateFile(QString()); });
        connect(vault, &Vault::deleted, m_canvas, [this](TAbstractFile*, bool) { m_canvas->invalidateFile(QString()); });
        connect(vault, &Vault::renamed, m_canvas, [this](TAbstractFile*, const QString&, bool) { m_canvas->invalidateFile(QString()); });
    }
    m_canvas->setSession(s);
    if (s && s->config()) {
        m_canvas->setOptions(CanvasWidget::Options::fromJson(s->config()->object(u"canvas"_s)));
        if (!m_configConn) // the canvas settings tab, another canvas' snap toggles
            m_configConn = connect(s->config(), &ConfigStore::changed, this, [this](const QString& file, const QString&) {
                VaultSession* ss = session();
                if (file == u"canvas" && ss && ss->config())
                    m_canvas->setOptions(CanvasWidget::Options::fromJson(ss->config()->object(u"canvas"_s)));
            });
    }
    m_canvas->setFilePath(file ? file->path() : QString());
    if (!file || !vault) {
        m_canvas->setData({}, false);
        emit titleChanged();
        return;
    }
    QString err;
    DiskState state;
    const std::optional<QByteArray> bytes = vault->read(file->path(), &state, &err);
    m_bytes = bytes.value_or(QByteArray());
    m_disk = state;
    m_format = FileFormat::detect(m_bytes);
    QString parseError;
    std::optional<CanvasData> data = bytes ? parseCanvas(m_bytes, &parseError) : std::nullopt;
    if (!data) {
        // Never overwrite a file we could not read (Obsidian would fail to open it too).
        m_invalid = true;
        m_canvas->setData({}, false);
        m_canvas->setReadOnly(true);
        m_canvas->setErrorMessage(tr("Failed to open “%1”:\n%2").arg(file->name(), bytes ? parseError : err));
        emit titleChanged();
        return;
    }
    m_saved = *data;
    m_canvas->setErrorMessage(QString());
    m_canvas->setReadOnly(false);
    const bool fit = !m_pendingViewState;
    m_canvas->setData(std::move(*data), fit);
    if (m_pendingViewState) {
        const QJsonObject vs = *m_pendingViewState;
        m_pendingViewState.reset();
        m_canvas->setViewport(vs.value(u"x").toDouble(), vs.value(u"y").toDouble(), vs.value(u"zoom").toDouble());
    }
    applyLocalData();
    emit titleChanged();
}

void CanvasView::onRename(TFile* file, const QString& oldPath)
{
    VaultSession* s = session();
    if (s) {
        // The read-only flag follows the file (Obsidian's localDataManager.handleRename).
        QJsonObject local = JsonFile::read(localDataPath(s));
        if (local.contains(oldPath)) {
            local.insert(file->path(), local.take(oldPath));
            JsonFile::write(localDataPath(s), local);
        }
    }
    m_canvas->setFilePath(file->path());
    FileView::onRename(file, oldPath);
}

void CanvasView::onVaultModified(TFile* file)
{
    if (!file || file != m_file)
        return;
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!vault)
        return;
    DiskState state;
    const std::optional<QByteArray> bytes = vault->read(file->path(), &state, nullptr);
    if (!bytes)
        return;
    if (*bytes == m_bytes) {
        m_disk = state; // our own write, or no real change
        return;
    }
    if (m_dirty) {
        // Our unsaved edits win (Obsidian overwrites on its next save); saving compares against this.
        m_disk = state;
        return;
    }
    std::optional<CanvasData> data = parseCanvas(*bytes);
    m_bytes = *bytes;
    m_disk = state;
    m_format = FileFormat::detect(m_bytes);
    if (!data) {
        m_invalid = true;
        m_canvas->setReadOnly(true);
        return;
    }
    m_invalid = false;
    m_saved = *data;
    m_canvas->reloadData(std::move(*data));
}

void CanvasView::requestSave()
{
    if (!m_invalid)
        m_saveTimer.start();
}

bool CanvasView::save()
{
    m_saveTimer.stop();
    if (!m_dirty || !m_file || m_invalid)
        return true;
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!vault)
        return false;
    const CanvasData& data = m_canvas->data();
    const QByteArray bytes = data.sameAs(m_saved) ? m_bytes : serializeCanvas(data, m_format);
    if (bytes == m_bytes) {
        m_dirty = false;
        return true;
    }
    QString err;
    DiskState newState;
    DocumentStorage::WriteResult r = vault->write(m_file->path(), bytes, m_disk, &newState, &err);
    if (r == DocumentStorage::WriteResult::Conflict) {
        // Changed on disk since we read it: like Obsidian, the canvas being edited wins.
        m_disk = vault->stat(m_file->path());
        r = vault->write(m_file->path(), bytes, m_disk, &newState, &err);
    }
    if (r != DocumentStorage::WriteResult::Ok) {
        if (Workspace* ws = workspace(); ws && ws->window())
            ws->window()->notices()->show(tr("Failed to save “%1”: %2").arg(m_file->name(), err));
        return false;
    }
    m_bytes = bytes;
    m_disk = newState;
    m_saved = data;
    m_dirty = false;
    emit saved();
    return true;
}

QJsonObject CanvasView::state() const
{
    QJsonObject o = FileView::state();
    const Viewport v = m_canvas->targetViewport();
    o.insert(u"viewState"_s, QJsonObject{{u"x"_s, v.x}, {u"y"_s, v.y}, {u"zoom"_s, v.zoom}});
    return o;
}

void CanvasView::setState(const QJsonObject& state)
{
    const QJsonObject vs = state.value(u"viewState").toObject();
    const bool hasViewState = vs.value(u"x").isDouble() && vs.value(u"y").isDouble() && vs.value(u"zoom").isDouble();
    const QString path = state.value(u"file").toString();
    const bool sameFile = m_file && m_file->path() == path;
    if (hasViewState && !sameFile)
        m_pendingViewState = vs;
    FileView::setState(state);
    if (hasViewState && sameFile)
        m_canvas->setViewport(vs.value(u"x").toDouble(), vs.value(u"y").toDouble(), vs.value(u"zoom").toDouble());
}

QJsonObject CanvasView::ephemeralState() const { return {}; }

void CanvasView::setEphemeralState(const QJsonObject& eState)
{
    if (eState.value(u"focus").toBool())
        m_canvas->setFocus();
    const QJsonObject match = eState.value(u"match").toObject();
    QString nodeId = match.value(u"nodeId").toString();
    if (nodeId.isEmpty() && match.contains(u"content")) {
        // A match in the text of the cards (canvasText(): the backlink pane's snippets).
        const CanvasText text = canvasText(m_canvas->data());
        const QJsonArray ranges = match.value(u"matches").toArray();
        if (!ranges.isEmpty() && match.value(u"content").toString() == text.text)
            nodeId = text.nodeAt(ranges.first().toArray().first().toInteger());
    }
    if (const Node* n = m_canvas->data().node(nodeId)) {
        m_canvas->selectOnly(nodeId);
        m_canvas->panIntoView(n->rect());
    }
}

void CanvasView::populatePaneMenu(QMenu* menu)
{
    QAction* a = menu->addAction(tr("Export as image"));
    connect(a, &QAction::triggered, this, [this] {
        auto* dlg = new QFileDialog(this, tr("Export as image"), (m_file ? m_file->basename() : u"canvas"_s) + u".png"_s, tr("PNG Files (*.png)"));
        dlg->setAcceptMode(QFileDialog::AcceptSave);
        dlg->setOption(QFileDialog::DontUseNativeDialog);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QFileDialog::fileSelected, this, [this](const QString& path) {
            if (!renderImage(0).save(path, "PNG"))
                if (Workspace* ws = workspace(); ws && ws->window())
                    ws->window()->notices()->show(tr("Could not write %1").arg(path));
        });
        dlg->open();
    });
}

QImage CanvasView::renderImage(double zoom) const { return m_canvas->renderImage(zoom); }

void CanvasView::applyLocalData()
{
    VaultSession* s = session();
    if (!s || !m_file || m_invalid)
        return;
    const QJsonObject local = JsonFile::read(localDataPath(s)).value(m_file->path()).toObject();
    m_canvas->setReadOnly(local.value(u"readonly").toBool(false));
}

void CanvasView::saveLocalData()
{
    VaultSession* s = session();
    if (!s || !m_file || m_invalid)
        return;
    QJsonObject local = JsonFile::read(localDataPath(s));
    const bool ro = m_canvas->isReadOnly();
    if (ro)
        local.insert(m_file->path(), QJsonObject{{u"readonly"_s, true}});
    else
        local.remove(m_file->path());
    if (local.isEmpty() && !QFileInfo::exists(localDataPath(s)))
        return;
    JsonFile::write(localDataPath(s), local);
}

} // namespace oc
