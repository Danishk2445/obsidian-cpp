#pragma once

// Internal workspace implementation: the model tree (splits / tab groups / leaves, mirroring
// Obsidian's WorkspaceSplit / WorkspaceTabs / WorkspaceLeaf), Workspace::Impl (model operations,
// persistence, drag & drop) and the widgets that render it. Not part of the frozen interface; tests
// include it to exercise the model operations directly.

#include "ui/workspace/workspace.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <functional>

class QMenu;

namespace oc {

class DropOverlay;
class IconButton;
class LeafWidget;
class PopoutWindow;
class SplitWidget;
class TabGroupWidget;
class TabHeaderBar;
class TAbstractFile;
class ViewHeader;
class WorkspaceSplit;
class WorkspaceWidget;

// Drop regions of a tab group (Obsidian's getDropDirection()).
enum class DropEdge { Center, Left, Right, Top, Bottom };

// "filename.ext" / "a/b/filename.ext" -> "filename" (Obsidian's TFile.basename).
QString fileBaseName(const QString& path);

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

class WorkspaceItem {
public:
    enum class Kind { Split, Tabs };
    virtual ~WorkspaceItem() = default;

    Kind kind() const { return m_kind; }
    bool isSplit() const { return m_kind == Kind::Split; }
    bool isTabs() const { return m_kind == Kind::Tabs; }
    WorkspaceSplit* asSplit();
    WorkspaceTabs* asTabs();
    virtual QWidget* widget() const = 0;

    QString id;
    WorkspaceSplit* parent = nullptr;
    double dimension = 0; // Obsidian "dimension": flex weight in percent of the parent; 0 = unset
    QJsonObject extra;    // unknown keys, written back unchanged

protected:
    explicit WorkspaceItem(Kind kind)
        : m_kind(kind)
    {
    }

private:
    Kind m_kind;
};

class WorkspaceTabs final : public WorkspaceItem {
public:
    // Internal access: WorkspaceTabs is a friend of Workspace and WorkspaceLeaf, so it can expose the
    // private implementation to the rest of the workspace code (and to tests).
    using Impl = Workspace::Impl;
    static Impl* impl(const Workspace* ws) { return ws->d; }

    explicit WorkspaceTabs(Workspace::Impl* d);
    ~WorkspaceTabs() override;

    QWidget* widget() const override;
    WorkspaceLeaf* current() const { return leaves.value(currentTab); }

    QList<WorkspaceLeaf*> leaves;
    int currentTab = 0;
    bool stacked = false; // round-tripped; stacked rendering is not implemented
    // The widget is deleted later than the model item (deleteLater: it may be handling the event that
    // removed the item); the destructor detaches it from the model so it never touches a freed item.
    QPointer<TabGroupWidget> w;
};

using WorkspaceImpl = WorkspaceTabs::Impl;

class WorkspaceSplit final : public WorkspaceItem {
public:
    // Window: the root split of a pop-out window (Obsidian's WorkspaceWindow, a "window" child of the
    // floating split).
    enum class Role { Nested, Root, Left, Right, Window };
    // vertical = children side by side (Obsidian's "vertical" direction), else stacked.
    WorkspaceSplit(WorkspaceImpl* d, Role role, bool vertical);
    ~WorkspaceSplit() override;

    QWidget* widget() const override;
    bool isSidedock() const { return role == Role::Left || role == Role::Right; }
    bool isWindow() const { return role == Role::Window; }

    Role role;
    bool vertical;
    QList<WorkspaceItem*> children;
    int width = 300;        // sidedocks only
    bool collapsed = false; // sidedocks only
    QPointer<SplitWidget> w; // see WorkspaceTabs::w
    // Windows only: the pop-out showing this split and the geometry it was opened / restored with
    // (workspace.json "x", "y", "width", "height", "maximize", "zoom").
    QPointer<PopoutWindow> popout;
    QPointer<DropOverlay> dropOverlay;
    QRect geometry;          // invalid size: default; hasPosition: x / y were given
    bool hasPosition = false;
    bool maximize = false;
    double zoom = 0;         // round-tripped (the clone has no per-window zoom)
};

// ---------------------------------------------------------------------------------------------
// Workspace::Impl
// ---------------------------------------------------------------------------------------------

struct Workspace::Impl {
    explicit Impl(Workspace* q);
    ~Impl();

    Workspace* q;
    WorkspaceSplit* root = nullptr;
    WorkspaceSplit* left = nullptr;
    WorkspaceSplit* right = nullptr;
    // Pop-out windows: the children of Obsidian's floating split (its id and unknown keys are kept).
    QList<WorkspaceSplit*> windows;
    QString floatingId;
    QJsonObject floatingExtra;
    bool quitting = false;                   // the main window is closing: pop-outs keep their leaves
    WorkspaceSplit* closingWindow = nullptr; // closeWindow() in progress
    QPointer<WorkspaceWidget> widget;
    QPointer<DropOverlay> overlay;
    QPointer<WorkspaceLeaf> active;
    quint64 activeCounter = 0;

    struct LeafData {
        quint64 activeTime = 0;
        QJsonObject extra; // unknown leaf keys ("group", ...)
        QPointer<LeafWidget> widget;
        bool working = false; // setViewState re-entrancy guard (Obsidian's leaf.working)
    };
    QHash<const WorkspaceLeaf*, LeafData> leafData;

    struct ClosedLeaf {
        QString leafId, parentId, rootId;
        int index = -1;
        QJsonObject viewState, eState;
        QList<WorkspaceLeaf::HistoryEntry> back, forward;
    };
    QList<ClosedLeaf> closed; // most recent first, max 10 (Obsidian)

    QStringList lastOpenFiles;
    QString activeFilePath;
    TFile* openingFile = nullptr; // set while WorkspaceLeaf::openFile() runs (fileOpened() payload)
    QPointer<QObject> connectedVault;
    QObject* vaultBridge = nullptr;
    QObject* activationFilter = nullptr;
    QJsonObject ribbonHidden; // "left-ribbon".hiddenItems
    QJsonObject extraTop;     // unknown top-level keys ("floating", "right-ribbon", ...)
    QTimer saveTimer;
    bool layoutReady = false;
    bool destroying = false;
    int batchDepth = 0;
    bool batchDirty = false;

    // ---- helpers ------------------------------------------------------------------------------
    static QString genId();
    ViewRegistry* registry() const { return q->m_registry; }
    MainWindow* window() const { return q->m_window; }
    bool frameless() const;
    int topRightReserve() const;
    bool windowFocused() const;              // the main window
    bool windowFocused(const QWidget* w) const; // w's window (a pop-out's widgets use their own)
    QList<WorkspaceSplit*> tops() const;      // root, left, right, then the windows (iterateAllLeaves order)
    QList<WorkspaceSplit*> mainAreas() const; // root, then the windows

    // ---- creation / views ---------------------------------------------------------------------
    WorkspaceLeaf* createLeaf(const QString& id = {}); // with an empty view
    View* createView(const QString& type, WorkspaceLeaf* leaf);
    void setLeafView(WorkspaceLeaf* leaf, View* view);
    void disposeView(View* view);
    LeafWidget* leafWidget(const WorkspaceLeaf* leaf) const;
    void setViewState(WorkspaceLeaf* leaf, const QJsonObject& viewState, const QJsonObject& eState, bool recordHistory);

    // ---- structure ----------------------------------------------------------------------------
    void insertLeaf(WorkspaceTabs* tabs, int index, WorkspaceLeaf* leaf);
    void takeLeaf(WorkspaceLeaf* leaf); // removes from its group; empty groups/splits are cleaned up
    void removeItem(WorkspaceItem* item);
    void insertItem(WorkspaceSplit* parent, int index, WorkspaceItem* item);
    void splitItem(WorkspaceItem* target, WorkspaceItem* item, bool vertical, bool before);
    void ensureRootLeaf();
    void closeLeaf(WorkspaceLeaf* leaf, bool recordUndo);
    void deleteTree(WorkspaceItem* item); // immediate teardown (restore / destructor)
    void releaseWidget(QWidget* w);

    // ---- queries ------------------------------------------------------------------------------
    WorkspaceSplit* topSplit(const WorkspaceItem* item) const;
    WorkspaceSplit* topSplit(const WorkspaceLeaf* leaf) const;
    bool isRootLeaf(const WorkspaceLeaf* leaf) const;
    void forEachLeaf(const WorkspaceItem* item, const std::function<void(WorkspaceLeaf*)>& fn) const;
    void forEachTabs(const WorkspaceItem* item, const std::function<void(WorkspaceTabs*)>& fn) const;
    QList<WorkspaceLeaf*> leavesUnder(const WorkspaceItem* item) const;
    WorkspaceLeaf* mostRecentLeaf(const WorkspaceItem* under) const;
    WorkspaceLeaf* mostRecentInGroup(const WorkspaceTabs* tabs) const;
    bool canNavigate(const WorkspaceLeaf* leaf) const;
    WorkspaceLeaf* leafById(const QString& id) const;
    WorkspaceTabs* tabsById(const WorkspaceItem* under, const QString& id) const;
    bool isGroupActive(const WorkspaceTabs* tabs) const;
    WorkspaceSplit* windowOf(const WorkspaceLeaf* leaf) const;    // the leaf's pop-out split, else null
    WorkspaceSplit* windowSplit(const QWidget* topLevel) const;   // the pop-out split shown by a window
    WorkspaceSplit* containerOf(const WorkspaceLeaf* leaf) const; // Obsidian getContainer(): its window, else root
    bool isMainLeaf(const WorkspaceLeaf* leaf) const;             // in the root split or a pop-out
    // Obsidian's getMostRecentLeaf(): over the root split and the pop-outs (but `except`).
    WorkspaceLeaf* mostRecentMainLeaf(const WorkspaceSplit* except = nullptr) const;
    QWidget* mainTop() const; // the main window (the workspace widget's top level)
    QWidget* topOf(const WorkspaceLeaf* leaf) const; // the window showing the leaf

    // ---- pop-out windows ----------------------------------------------------------------------
    // A new, empty window split and its PopoutWindow (shown). `geometry` as in Workspace::openPopoutLeaf.
    WorkspaceSplit* openWindow(const QRect& geometry, bool hasPosition);
    void showWindow(WorkspaceSplit* win); // creates / shows its PopoutWindow with the stored geometry
    void closeWindow(WorkspaceSplit* win);  // Obsidian WorkspaceWindow.close(): its leaves are closed
    void removeWindow(WorkspaceSplit* win); // drops an emptied window and its PopoutWindow
    PopoutWindow* moveToWindow(WorkspaceLeaf* leaf, const QRect& geometry, bool hasPosition);
    void onWindowActivated(QWidget* top); // Obsidian WorkspaceContainer.onFocus()
    void updateWindowTitles();
    void focusWindowOf(const WorkspaceLeaf* leaf); // raise + activate the leaf's window (Obsidian focusLeaf)
    void mainWindowClosing();

    // ---- operations (UI, commands and tests) ----------------------------------------------------
    void selectTab(WorkspaceTabs* tabs, int index);
    void moveLeaf(WorkspaceLeaf* leaf, WorkspaceTabs* target, int index);
    bool splitLeafTo(WorkspaceLeaf* leaf, WorkspaceTabs* target, DropEdge edge);
    void moveLeafToSide(WorkspaceLeaf* leaf, WorkspaceSplit* side);
    WorkspaceLeaf* createLeafInGroup(WorkspaceTabs* tabs); // Obsidian createLeafInTabGroup()
    WorkspaceLeaf* appendNewTab(WorkspaceTabs* tabs);      // "+" button / workspace:new-tab
    WorkspaceLeaf* createLeafBySplit(WorkspaceLeaf* leaf, bool vertical, bool before);
    void onFileViewDeleted(FileView* view);
    void closeOthers(WorkspaceLeaf* leaf);
    void closeAfter(WorkspaceLeaf* leaf);
    void closeAll(WorkspaceTabs* tabs);
    void goHistory(WorkspaceLeaf* leaf, int delta);
    QList<QPair<QString, QString>> historyItems(const WorkspaceLeaf* leaf, bool back) const; // (title, icon), nearest first

    // ---- drag & drop --------------------------------------------------------------------------
    struct DropTarget {
        // NewWindow: released outside every window — Obsidian moves the tab to a new pop-out there.
        enum class Kind { None, Tabs, Side, NewWindow };
        Kind kind = Kind::None;
        WorkspaceTabs* tabs = nullptr;
        WorkspaceSplit* side = nullptr;
        DropEdge edge = DropEdge::Center;
        int index = -1;
        QRect rect;             // highlight, in `host` coordinates
        QWidget* host = nullptr; // the workspace widget (main window) or a pop-out's root split widget
        QPoint globalPos;        // NewWindow: the drop point
        bool isValid() const { return kind != Kind::None; }
    };
    // `in`: only look inside that top-level window (drops delivered to a window; Wayland, where global
    // positions don't relate windows to each other).
    DropTarget dropTargetAt(const WorkspaceLeaf* dragged, const QPoint& globalPos, QWidget* in = nullptr) const;
    DropTarget dropTargetInArea(const WorkspaceLeaf* dragged, WorkspaceSplit* area, QWidget* host, const QPoint& globalPos) const;
    QWidget* windowAt(const QPoint& globalPos) const; // our top-level window under a global point (or null)
    bool applyDrop(WorkspaceLeaf* leaf, const DropTarget& target);
    struct DragState {
        QPointer<WorkspaceLeaf> leaf;
        bool active = false;
        DropTarget target;
        QPointer<QWidget> source;   // the window the drag started in
        QPixmap ghost;
        QPointer<QWidget> ghostTop; // the window showing the ghost
    } drag;
    void beginDrag(WorkspaceLeaf* leaf, const QPoint& globalPos);
    void updateDrag(const QPoint& globalPos);
    void endDrag(const QPoint& globalPos, bool commit);
    DropOverlay* ensureOverlay();                // the main window's
    DropOverlay* ensureOverlay(QWidget* top);    // that window's
    void hideOverlays();
    // Drag feedback in `top` (target highlight; the ghost at `ghostGlobal` unless null); the other
    // windows' overlays are cleared.
    void showDragFeedback(const DropTarget& target, QWidget* top, const QPoint* ghostGlobal);
    // Wayland: windows can't relate global positions, so a tab leaving its window continues as a
    // native drag (QDrag) that our windows accept through an event filter on their QWindow.
    void startNativeDrag();
    void installNativeDrop();
    QWidget* topForWindowHandle(const QObject* window) const;
    bool nativeDragging = false;
    QObject* nativeDrop = nullptr;
    struct NativeDropAt {
        QPointer<QWidget> top; // the window it was dropped in
        QPoint globalPos;
    } nativeDropAt;
    QList<QPointer<PopoutWindow>> pendingDisposal; // pop-outs emptied during a native drag
    void disposePopout(PopoutWindow* p);

    // ---- notifications ------------------------------------------------------------------------
    void beginBatch() { ++batchDepth; }
    void endBatch();
    void structureChanged(); // dimensions + widgets resync + tops + save + layoutChanged()
    // Obsidian's WorkspaceSplit.recomputeChildrenDimensions(), for every split: children share the
    // space equally unless all of them have a dimension, which are then rescaled to sum to 100.
    void normalizeDimensions();
    void syncWidgets();
    void updateTopFlags();
    void refreshChrome(); // repaint headers (active group / focus / titles)
    void leafTitleChanged(WorkspaceLeaf* leaf);
    void updateActiveFile(TFile* hint = nullptr);
    void recordRecent(const QString& path);
    void activeChanged(WorkspaceLeaf* old);

    // ---- persistence --------------------------------------------------------------------------
    QJsonObject serializeItem(const WorkspaceItem* item) const;
    QJsonObject serializeLeaf(const WorkspaceLeaf* leaf) const;
    WorkspaceItem* parseItem(const QJsonObject& o, WorkspaceSplit::Role role, QList<QPair<WorkspaceLeaf*, QJsonObject>>& leaves);
    void clearLayout();
    void buildEmptyLayout();
    void buildDefaultLayout();
    QJsonObject ribbonHiddenItems() const;
    void applyRibbonHidden();
    void saveNow();
    QString layoutPath() const;

    // ---- vault events -------------------------------------------------------------------------
    void connectVault();
    void onVaultRenamed(TAbstractFile* item, const QString& oldPath);
    void onVaultDeleted(TAbstractFile* item);
    void onVaultCreated(TAbstractFile* item);
};

// ---------------------------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------------------------

// 3px resize handle overlaying the edge of a pane; shows a 1px divider line, 3px accent on hover
// (Obsidian's .workspace-leaf-resize-handle).
class ResizeHandle : public QWidget {
    Q_OBJECT
public:
    enum class Line { Right, Left, Bottom };
    ResizeHandle(Line line, QWidget* parent);
    std::function<void(QPoint globalPos)> onDragStart;
    std::function<void(QPoint globalPos)> onDrag;
    std::function<void()> onDragFinished;

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    Line m_line;
    bool m_hover = false;
    bool m_dragging = false;
};

class SplitWidget : public QWidget {
    Q_OBJECT
public:
    SplitWidget(WorkspaceImpl* d, WorkspaceSplit* split);
    void sync();
    void relayout();
    WorkspaceSplit* split() const { return m_split; }
    void detachModel() { m_split = nullptr; } // the model item is being deleted

protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void startHandleDrag(QPoint globalPos);
    void dragHandle(int index, QPoint globalPos);
    WorkspaceImpl* d;
    WorkspaceSplit* m_split;
    QList<ResizeHandle*> m_handles;
    QList<int> m_dragSizes; // pixel sizes at drag start
    int m_dragOrigin = 0;
};

class TabHeaderBar : public QWidget {
    Q_OBJECT
public:
    TabHeaderBar(WorkspaceImpl* d, WorkspaceTabs* tabs, QWidget* parent);
    void detachModel(); // the model item is being deleted

    void setSidedock(bool sidedock);
    void setTop(bool top);
    bool isTop() const { return m_top; }
    void setRightReserve(int px);
    void relayout();

    int tabCount() const { return int(m_tabs.size()); }
    QRect tabRect(int index) const;
    QRect closeRect(int index) const;
    int tabAt(const QPoint& pos) const;
    struct InsertLocation {
        QRect rect;
        int index = 0;
    };
    InsertLocation insertLocation(int x) const;
    IconButton* newTabButton() const { return m_newTab; }
    IconButton* tabListButton() const { return m_tabList; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void changeEvent(QEvent*) override;
    bool event(QEvent*) override;

private:
    struct TabGeom {
        QRectF rect;  // .workspace-tab-header
        QRectF inner; // .workspace-tab-header-inner
        QRectF icon;
        QRectF title;
        QRectF close; // close button or pin status icon
        bool showIcon = false;
    };
    void paintRootTab(QPainter& p, int index, const TabGeom& g);
    void paintSideTab(QPainter& p, int index, const TabGeom& g);
    void updateHover(const QPoint& pos);
    void refreshDragRegion();
    void showTabMenu(int index, const QPoint& globalPos);
    void showTabList();
    WorkspaceLeaf* leafAt(int index) const;
    bool closeVisible(int index) const;

    WorkspaceImpl* d;
    WorkspaceTabs* m_group;
    bool m_sidedock = false;
    bool m_top = false;
    int m_reserve = 0;
    QVector<TabGeom> m_tabs;
    double m_scroll = 0;
    int m_snappedTab = -2;     // current tab last scrolled into view
    double m_clipL = 0, m_clipR = 0; // visible part of the (scrolling) tab strip
    double m_lockedWidth = -1; // Obsidian lockTabWidths(): keep widths while closing with the mouse
    int m_hoverTab = -1;
    bool m_hoverClose = false;
    int m_pressTab = -1;
    bool m_pressClose = false;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QPoint m_pressPos;
    bool m_dragging = false;
    IconButton* m_newTab = nullptr;
    IconButton* m_tabList = nullptr;
};

class TabGroupWidget : public QWidget {
    Q_OBJECT
public:
    TabGroupWidget(WorkspaceImpl* d, WorkspaceTabs* tabs);
    void sync();
    void detachModel(); // the model item is being deleted
    TabHeaderBar* header() const { return m_header; }
    WorkspaceTabs* tabs() const { return m_tabs; }
    void releaseLeaves();

protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void layoutChildren();
    WorkspaceImpl* d;
    WorkspaceTabs* m_tabs;
    TabHeaderBar* m_header;
};

// Title area of the view header: "folder / sub / basename" breadcrumb, centered and elided.
class ViewHeaderTitle : public QWidget {
    Q_OBJECT
public:
    explicit ViewHeaderTitle(QWidget* parent);
    void setParts(const QStringList& folders, const QString& title);
    void setActive(bool active);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QString text() const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QStringList m_folders;
    QString m_title;
    bool m_active = false;
};

class ViewHeader : public QWidget {
    Q_OBJECT
public:
    ViewHeader(WorkspaceImpl* d, WorkspaceLeaf* leaf, QWidget* parent);
    void refresh();         // title, nav buttons, actions
    void refreshActions();  // re-query the view's headerActions()
    ViewHeaderTitle* title() const { return m_title; }
    IconButton* backButton() const { return m_back; }
    IconButton* forwardButton() const { return m_forward; }
    IconButton* moreButton() const { return m_more; }

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void showMoreMenu();
    void showHistoryMenu(bool back);
    WorkspaceImpl* d;
    WorkspaceLeaf* m_leaf;
    IconButton* m_back;
    IconButton* m_forward;
    ViewHeaderTitle* m_title;
    QWidget* m_actions;
    IconButton* m_more;
    QList<QPointer<QWidget>> m_viewActions;
};

class LeafWidget : public QWidget {
    Q_OBJECT
public:
    LeafWidget(WorkspaceImpl* d, WorkspaceLeaf* leaf);
    WorkspaceLeaf* leaf() const { return m_leaf; }
    void setView(View* view);
    void refresh(); // header visibility / contents
    ViewHeader* header() const { return m_header; }

protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void layoutChildren();
    WorkspaceImpl* d;
    WorkspaceLeaf* m_leaf;
    ViewHeader* m_header = nullptr;
    QPointer<View> m_view;
};

// left sidedock | root split | right sidedock (Workspace::widget()).
class WorkspaceWidget : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceWidget(WorkspaceImpl* d);
    void relayout();
    QRect sideRect(Workspace::Side side) const;

protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    WorkspaceImpl* d;
    ResizeHandle* m_leftHandle;
    ResizeHandle* m_rightHandle;
    int m_dragStartWidth = 0;
    int m_dragStartX = 0;
};

// Drop target highlight + drag ghost, drawn over the whole window during a tab drag.
class DropOverlay : public QWidget {
    Q_OBJECT
public:
    explicit DropOverlay(QWidget* parent);
    void setTarget(const QRect& rect); // window coordinates; null = none
    void setGhost(const QPixmap& ghost, const QPoint& pos);
    void moveGhost(const QPoint& pos);
    QRect targetRect() const { return m_target; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QRect m_target;
    QRectF m_shown;
    QRectF m_from;
    QVariantAnimation m_anim;
    QPixmap m_ghost;
    QPoint m_ghostPos;
};

} // namespace oc
