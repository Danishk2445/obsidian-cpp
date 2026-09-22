#include "ui/workspace/view.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/workspace/vault_access.h"
#include "ui/workspace/workspace.h"
#include "ui/workspace/workspace_p.h"

namespace oc {

namespace {
// FileView keeps the requested path even when it can't be resolved to a TFile (no vault yet, or the
// vault is still loading), so the tab title and the saved layout stay meaningful.
constexpr const char* kPathProperty = "ocFilePath";
} // namespace

View::View(WorkspaceLeaf* leaf, QWidget* parent)
    : QWidget(parent)
    , m_leaf(leaf)
{
}

View::~View() = default;

Workspace* View::workspace() const { return m_leaf ? m_leaf->workspace() : nullptr; }

VaultSession* View::session() const
{
    Workspace* ws = workspace();
    return ws ? ws->session() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// FileView
// ---------------------------------------------------------------------------------------------

QString FileView::displayText() const
{
    if (m_file)
        return m_file->basename();
    const QString path = property(kPathProperty).toString();
    if (!path.isEmpty())
        return fileBaseName(path);
    return tr("No file");
}

QJsonObject FileView::state() const
{
    QJsonObject o;
    const QString path = m_file ? m_file->path() : property(kPathProperty).toString();
    if (!path.isEmpty())
        o.insert(QStringLiteral("file"), path);
    return o;
}

void FileView::setState(const QJsonObject& state)
{
    if (!state.contains(QStringLiteral("file")))
        return;
    const QString path = state.value(QStringLiteral("file")).toString();
    setProperty(kPathProperty, path);
    TFile* file = vaultFile(session(), path);
    if (file != m_file)
        loadFile(file); // subclasses set m_file
    emit titleChanged();
}

void FileView::onDelete(TFile* file)
{
    if (!file || file != m_file)
        return;
    if (Workspace* ws = workspace())
        WorkspaceTabs::impl(ws)->onFileViewDeleted(this);
}

} // namespace oc
