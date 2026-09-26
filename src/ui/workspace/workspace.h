#pragma once

// FROZEN INTERFACE (M1) — Obsidian-style workspace: a root split of tab groups plus left/right
// sidedocks (each a split of tab groups), made of leaves hosting views. Serialized in the same shape as
// Obsidian's workspace.json (stored in the clone's per-vault config dir, never in the vault):
//   {"main": {...}, "left": {...}, "right": {...}, "left-ribbon": {"hiddenItems": {...}},
//    "floating"?: {"id", "type":"floating", "children":[{"type":"window", ...}]},  (pop-outs, see below)
//    "active": "<leaf id>", "lastOpenFiles": [...]}
// Nodes: {"id","type":"split","direction":"vertical"|"horizontal","width"?,"collapsed"?,"children":[...]},
//        {"id","type":"tabs","currentTab"?,"children":[...]}, {"id","type":"leaf","state":{"type","state","icon","title"}}
// ("vertical" = children side by side, as in Obsidian.)

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QStringList>

#include <functional>

class QWidget;

namespace oc {

class MainWindow;
class PopoutWindow;
class TFile;
class VaultSession;
class View;
class ViewRegistry;
class Workspace;
class WorkspaceTabs;

class WorkspaceLeaf : public QObject {
    Q_OBJECT
public:
    const QString& id() const { return m_id; }
    Workspace* workspace() const { return m_workspace; }
    WorkspaceTabs* tabs() const { return m_tabs; }
    View* view() const { return m_view; }

    // {"type": "...", "state": {...}, "icon": "...", "title": "..."}
    QJsonObject viewState() const;
    // Switch to the given view (reusing the current view when the type matches) and apply state.
    // eState is applied after the state (scroll/cursor). Records navigation history if requested.
    void setViewState(const QJsonObject& viewState, const QJsonObject& eState = {}, bool recordHistory = true);
    void openFile(TFile* file, const QJsonObject& state = {}, const QJsonObject& eState = {});

    QString displayText() const;
    QString icon() const;

    bool isPinned() const { return m_pinned; }
    void setPinned(bool pinned);

    bool canGoBack() const;
    bool canGoForward() const;
    void goBack();
    void goForward();

    bool isInSidedock() const;
    void detach(); // close: removes the leaf (and destroys its view)

signals:
    void viewChanged();
    void titleChanged();
    void pinnedChanged();
    void historyChanged();

private:
    friend class Workspace;
    friend class WorkspaceTabs;
    WorkspaceLeaf(Workspace* ws, QString id);
    ~WorkspaceLeaf() override;

    struct HistoryEntry {
        QJsonObject viewState;
        QJsonObject eState;
    };

    QString m_id;
    Workspace* m_workspace;
    WorkspaceTabs* m_tabs = nullptr;
    View* m_view = nullptr;
    bool m_pinned = false;
    QList<HistoryEntry> m_back;
    QList<HistoryEntry> m_forward;
};

class Workspace : public QObject {
    Q_OBJECT
public:
    enum class NewLeaf { No, Tab, SplitRight, SplitDown, Window };
    enum class Side { Left, Right };

    Workspace(VaultSession* session, ViewRegistry* registry, MainWindow* window);
    ~Workspace() override;

    VaultSession* session() const { return m_session; }
    ViewRegistry* viewRegistry() const { return m_registry; }
    MainWindow* window() const { return m_window; }

    // The widget tree (left sidedock | root split | right sidedock) placed in the main window.
    QWidget* widget() const;

    // Active leaf: the focused leaf anywhere; activeRootLeaf(): most recent leaf in the root split (or in
    // a pop-out window: Obsidian's getMostRecentLeaf()).
    WorkspaceLeaf* activeLeaf() const;
    WorkspaceLeaf* activeRootLeaf() const;
    void setActiveLeaf(WorkspaceLeaf* leaf, bool focus = true);
    template <typename T> T* activeViewOfType() const;

    // Obsidian getLeaf(): No = reuse the active root leaf unless pinned (then a new tab).
    WorkspaceLeaf* getLeaf(NewLeaf mode = NewLeaf::No);
    WorkspaceLeaf* splitLeaf(WorkspaceLeaf* leaf, Qt::Orientation orientation, bool before = false);
    // A leaf of `type` in a sidedock (created in its own tab when missing).
    WorkspaceLeaf* ensureSideLeaf(const QString& type, Side side, bool reveal = false);
    void revealLeaf(WorkspaceLeaf* leaf); // un-collapse its sidedock and select its tab

    void openFile(TFile* file, NewLeaf mode = NewLeaf::No, const QJsonObject& eState = {});
    QList<WorkspaceLeaf*> leavesOfType(const QString& type) const;
    void iterateAllLeaves(const std::function<void(WorkspaceLeaf*)>& fn) const;
    void iterateRootLeaves(const std::function<void(WorkspaceLeaf*)>& fn) const;

    bool isSideCollapsed(Side side) const;
    void setSideCollapsed(Side side, bool collapsed);
    void toggleSide(Side side);

    // Most-recent-first list of opened files (vault paths), capped at 50.
    QStringList lastOpenFiles() const;

    void undoCloseLeaf();

    QJsonObject serialize() const;
    bool restore(const QJsonObject& json); // false -> caller builds the default layout
    void loadLayout();                     // restore from config (or default layout)
    void requestSaveLayout();              // debounced write of workspace.json to the clone config

    // ---- M1 additions (workspace:* commands) — additive, not part of the original freeze ----------
    // Obsidian duplicateLeaf(): a new leaf (per mode) showing the same view state, eState and history.
    WorkspaceLeaf* duplicateLeaf(WorkspaceLeaf* leaf, NewLeaf mode);
    WorkspaceLeaf* newTab();     // workspace:new-tab — appends an empty tab to the most recent root group
    void nextTab();              // workspace:next-tab — wraps within the active leaf's group
    void previousTab();          // workspace:previous-tab
    void goToTab(int index);     // workspace:goto-tab-N (0-based; ignored when out of range)
    void goToLastTab();          // workspace:goto-last-tab
    void closeActiveLeaf();      // workspace:close — like Obsidian, a pinned leaf is unpinned instead
    void togglePinActiveLeaf();  // workspace:toggle-pin
    bool canUndoCloseLeaf() const;
    // The leaves of `leaf`'s tab group, in tab order; closeLeaves() detaches several in one layout pass.
    QList<WorkspaceLeaf*> groupLeaves(WorkspaceLeaf* leaf) const;
    void closeLeaves(const QList<WorkspaceLeaf*>& leaves);
    int sideWidth(Side side) const;
    void setSideWidth(Side side, int px);

    // ---- Pop-out windows — additive ---------------------------------------------------------------
    // Obsidian's floating split: separate windows (workspace.json "floating": {"type": "floating",
    // "children": [{"type": "window", "direction", "children", "x", "y", "width", "height", "maximize",
    // "zoom"}]}) that each hold a root split of tab groups. Their leaves take part in iterateAllLeaves(),
    // active-leaf tracking, activeRootLeaf() (Obsidian's getMostRecentLeaf(): the root split and the
    // pop-outs), getLeaf() (Obsidian's getUnpinnedLeaf() stays in the active leaf's pop-out), the recent
    // files and navigation history. getLeaf(NewLeaf::Window) = openPopoutLeaf(). A pop-out closes when its
    // last leaf goes; closing it closes its leaves (pinned ones too, each recorded for undo); closing the
    // main window keeps them in workspace.json for the next start.
    // `geometry`: the frame position (the window system may ignore it: Wayland) and size; an invalid
    // rect lets the window system place the window.
    WorkspaceLeaf* openPopoutLeaf(const QRect& geometry = {}); // an empty leaf in a new pop-out
    // Obsidian moveLeafToPopout(): the leaf (view, state and history kept) into a new pop-out, by default
    // as large as the leaf (at least 600x600).
    PopoutWindow* moveLeafToPopout(WorkspaceLeaf* leaf, const QRect& geometry = {});
    QList<PopoutWindow*> popoutWindows() const;              // workspace.json order
    PopoutWindow* popoutOf(const WorkspaceLeaf* leaf) const; // null: the main window (or not attached)
    // Obsidian's leaf.getContainer(): the leaves of `leaf`'s pop-out, else of the root split.
    QList<WorkspaceLeaf*> containerLeaves(const WorkspaceLeaf* leaf) const;
    // The most recently active visible leaf of a pop-out, or of the root split for null.
    WorkspaceLeaf* mostRecentLeafIn(const PopoutWindow* window) const;
    void closePopout(PopoutWindow* window); // like its close button

signals:
    void activeLeafChanged(oc::WorkspaceLeaf* leaf);
    void fileOpened(oc::TFile* file);
    void layoutChanged();

private:
    struct Impl;
    Impl* d;
    VaultSession* m_session;
    ViewRegistry* m_registry;
    MainWindow* m_window;
    friend class WorkspaceLeaf;
    friend class WorkspaceTabs;
};

template <typename T> T* Workspace::activeViewOfType() const
{
    if (WorkspaceLeaf* l = activeLeaf())
        return qobject_cast<T*>(l->view());
    return nullptr;
}

} // namespace oc
