#pragma once

// FROZEN INTERFACE (M1) — views hosted by workspace leaves (Obsidian's ItemView / FileView).

#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class QMenu;

namespace oc {

class TFile;
class VaultSession;
class Workspace;
class WorkspaceLeaf;

class View : public QWidget {
    Q_OBJECT
public:
    explicit View(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~View() override;

    WorkspaceLeaf* leaf() const { return m_leaf; }
    Workspace* workspace() const;
    VaultSession* session() const;

    // Obsidian view type id: "markdown", "file-explorer", "search", "graph", "empty", ...
    virtual QString viewType() const = 0;
    virtual QString displayText() const = 0;  // tab title
    virtual QString icon() const = 0;         // Lucide/Obsidian icon name

    // Persistent state (workspace.json: leaf.state.state). setState may be called again on a live
    // view to navigate (e.g. open another file in the same markdown view).
    virtual QJsonObject state() const { return {}; }
    virtual void setState(const QJsonObject& state) { Q_UNUSED(state); }
    // Ephemeral state (scroll position, cursor) — stored in navigation history entries.
    virtual QJsonObject ephemeralState() const { return {}; }
    virtual void setEphemeralState(const QJsonObject& eState) { Q_UNUSED(eState); }

    // Whether state changes of this view create back/forward history entries.
    virtual bool navigation() const { return true; }
    // Whether the leaf shows the view header (back/forward, title, actions). Sidebar views: false.
    virtual bool showsViewHeader() const { return true; }
    // Items for the header's "More options" menu.
    virtual void populatePaneMenu(QMenu* menu) { Q_UNUSED(menu); }
    // Extra action buttons shown in the view header (right side), e.g. reading/editing toggle.
    virtual QList<QWidget*> headerActions() { return {}; }

    virtual void onOpen() {}   // after the view is attached to its leaf
    virtual void onClose() {}  // before the view is destroyed
    virtual void focusContent() { setFocus(); }

signals:
    void titleChanged();  // displayText()/icon() changed
    void stateChanged();  // persistent state changed (workspace saves, debounced)

private:
    WorkspaceLeaf* m_leaf;
};

// A view showing one file of the vault.
class FileView : public View {
    Q_OBJECT
public:
    using View::View;

    TFile* file() const { return m_file; }
    virtual bool canAcceptExtension(const QString& extension) const = 0;
    // Load `file` into this view (replacing the current one). Called by setState({"file": path}).
    virtual void loadFile(TFile* file) = 0;
    // The file was renamed/moved (path already updated on the TFile).
    virtual void onRename(TFile* file, const QString& oldPath) { Q_UNUSED(file); Q_UNUSED(oldPath); emit titleChanged(); }
    // The file was deleted: default closes the view's leaf.
    virtual void onDelete(TFile* file);

    QString displayText() const override;
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override; // {"file": "<vault path>"}

protected:
    TFile* m_file = nullptr;
};

} // namespace oc
