#include "ui/workspace/view.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/vault_access.h"
#include "ui/workspace/workspace.h"
#include "ui/workspace/workspace_p.h"

#include <QPointer>

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

void View::paneMenuEntries(QList<MenuEntry>& items, const QString& source)
{
    Q_UNUSED(source);
    // ItemView.onPaneMenu: split the leaf (not from a sidebar: "Split right" is disabled there).
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    Workspace* ws = workspace();
    if (!leaf || !ws)
        return;
    MenuEntry right{QStringLiteral("open"), tr("Split right"), QStringLiteral("separator-vertical"), [ws = QPointer<Workspace>(ws), leaf] {
                        if (ws && leaf)
                            ws->duplicateLeaf(leaf, Workspace::NewLeaf::SplitRight);
                    }};
    right.enabled = !leaf->isInSidedock();
    items << right;
    items << MenuEntry{QStringLiteral("open"), tr("Split down"), QStringLiteral("separator-horizontal"), [ws = QPointer<Workspace>(ws), leaf] {
                           if (ws && leaf)
                               ws->duplicateLeaf(leaf, Workspace::NewLeaf::SplitDown);
                       }};
}

// ---------------------------------------------------------------------------------------------
// FileView
// ---------------------------------------------------------------------------------------------

FileView::FileView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
{
    // Runs after the workspace's own deleted() handler (connected first), which moves leaves off the file.
    if (Vault* v = session() ? session()->vault() : nullptr)
        connect(v, &Vault::deleted, this, [this](TAbstractFile* item) {
            if (m_file && item == m_file) {
                setProperty(kPathProperty, m_file->path());
                m_file = nullptr;
            }
        });
}

void FileView::paneMenuEntries(QList<MenuEntry>& items, const QString& source)
{
    View::paneMenuEntries(items, source);
    if (m_file)
        FileMenuHooks::collect(items, m_file, source, leaf(), this); // "Rename...", "Delete file", ... come from the hooks
}

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
