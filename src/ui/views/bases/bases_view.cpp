#include "ui/views/bases/bases_view.h"

#include "core/files/file_manager.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "text/codec.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_layout.h"
#include "ui/workspace/workspace.h"

#include <QVBoxLayout>

namespace oc::basesui {

using namespace oc::bases;

BasesFileView::BasesFileView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_controller(new BasesController(session(), BasesController::Mode::File, this))
{
    setObjectName(QStringLiteral("ocBasesView"));
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_controller);
    setFocusProxy(m_controller);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { save(); });
    connect(m_controller, &BasesController::baseEdited, this, &BasesFileView::onEdited);
    connect(m_controller, &BasesController::viewChanged, this, [this] {
        emit stateChanged();
        emit titleChanged();
    });
    if (VaultSession* s = session(); s && s->workspace()) {
        // Obsidian: "this" follows the file opened in the workspace (file-open), unless pinned
        m_fileOpenedConn = connect(s->workspace(), &Workspace::fileOpened, this, [this](TFile* f) {
            if (!f || !isVisible())
                return;
            if (WorkspaceLeaf* l = this->leaf(); l && l->isPinned())
                return;
            m_controller->setThisFile(f->path());
        });
    }
}

BasesFileView::~BasesFileView()
{
    if (m_saveTimer.isActive())
        save();
}

QString BasesFileView::icon() const
{
    const BaseView* v = m_controller->currentView();
    if (const ViewTypeInfo* info = basesui::viewType(v ? v->type : QString()))
        return info->icon;
    return QStringLiteral("layout-list");
}

bool BasesFileView::canAcceptExtension(const QString& extension) const { return extension.compare(QLatin1String("base"), Qt::CaseInsensitive) == 0; }

void BasesFileView::loadFile(TFile* file)
{
    if (m_saveTimer.isActive())
        save();
    m_file = file;
    VaultSession* s = session();
    if (s && s->vault() && !m_modifiedConn) {
        m_modifiedConn = connect(s->vault(), &Vault::modified, this, [this](TFile* f, bool) {
            if (f && f == m_file)
                reload();
        });
    }
    if (file)
        m_controller->setThisFile(file->path());
    reload();
    if (!m_pendingView.isEmpty()) {
        m_controller->selectView(m_pendingView);
        m_pendingView.clear();
    }
    emit titleChanged();
}

void BasesFileView::reload()
{
    VaultSession* s = session();
    if (!m_file || !s || !s->vault())
        return;
    QString err;
    const std::optional<QByteArray> bytes = s->vault()->read(m_file->path(), nullptr, &err);
    if (!bytes) {
        m_controller->setParseError(err);
        return;
    }
    const QString text = decodeFile(*bytes).text.toString();
    if (text == m_lastText && m_controller->base().views.size() > 0 && !m_controller->hasError())
        return; // our own write
    m_lastText = text;
    QString error;
    const std::optional<BaseFile> base = BaseFile::fromString(text, &error);
    if (!base) {
        m_controller->setParseError(error);
        return;
    }
    m_controller->setBase(*base);
}

void BasesFileView::onEdited()
{
    m_pending = m_controller->base().toString();
    if (m_pending == m_lastText) {
        m_saveTimer.stop();
        return;
    }
    m_saveTimer.start();
    emit titleChanged();
}

bool BasesFileView::save()
{
    m_saveTimer.stop();
    VaultSession* s = session();
    if (!m_file || !s || !s->fileManager() || m_pending.isEmpty() || m_pending == m_lastText)
        return true;
    const QString next = m_pending;
    QString error;
    const bool ok = s->fileManager()->editFile(m_file->path(), [&](const Text& text) -> QList<ChangeSpec> {
        if (text.toString() == next)
            return {};
        return {ChangeSpec{0, text.length(), next}};
    }, &error);
    if (ok)
        m_lastText = next;
    else
        m_controller->notice(error);
    return ok;
}

void BasesFileView::onRename(TFile* file, const QString& oldPath)
{
    if (m_controller->thisFile() == oldPath)
        m_controller->setThisFile(file->path());
    FileView::onRename(file, oldPath);
}

void BasesFileView::onClose()
{
    if (m_saveTimer.isActive())
        save();
}

QJsonObject BasesFileView::state() const
{
    QJsonObject s = FileView::state();
    if (!m_controller->viewName().isEmpty())
        s.insert(QStringLiteral("viewName"), m_controller->viewName());
    return s;
}

void BasesFileView::setState(const QJsonObject& state)
{
    m_pendingView = state.value(QStringLiteral("viewName")).toString();
    FileView::setState(state);
    if (!m_pendingView.isEmpty() && m_file) {
        m_controller->selectView(m_pendingView);
        m_pendingView.clear();
    }
}

QJsonObject BasesFileView::ephemeralState() const { return m_controller->ephemeralState(); }

void BasesFileView::setEphemeralState(const QJsonObject& eState)
{
    const QString subpath = eState.value(QStringLiteral("subpath")).toString();
    if (subpath.startsWith(u'#'))
        m_controller->selectView(subpath.mid(1));
    m_controller->setEphemeralState(eState);
    if (eState.value(QStringLiteral("focus")).toBool())
        focusContent();
}

void BasesFileView::focusContent()
{
    if (BaseLayout* l = m_controller->layoutWidget())
        l->setFocus();
    else
        m_controller->setFocus();
}

} // namespace oc::basesui
