#pragma once

#include "core/util/match_range.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <functional>
#include <memory>
#include <vector>

class QLineEdit;
class QTextLayout;

namespace oc {

class DragGhost;

// Obsidian's generic tree (.tree-item > .tree-item-self, plus the search-result pieces the sidebar
// views share): a custom-painted, virtualized list of nodes in the file explorer's metrics — 13 px
// items with 4/8/4/24 px padding, 17 px per level with indentation guides, the right-triangle collapse
// chevron, an optional 14 px icon in the chevron's place, a faint prefix, the title (wrapping like
// Obsidian's pre-wrap .tree-item-inner, with match highlights), an optional sub-text line and a
// right-aligned flair (count). States: hover, .is-active, .is-selected, .has-focus (focus ring while
// the view's leaf is active), .has-active-menu, being renamed, being dragged / dragged over.
//
// Besides items, a tree holds pane section headers ("Linked mentions"), search-result match snippets
// (.search-result-file-match rows drawn inside their parent item's .search-result-file-matches box,
// with hover buttons for more context, an action button such as "Link" and destination chips) and
// messages (.search-empty-state / .pane-empty).
//
// The tree is only a view: clients build detached nodes and hand them over with setRoots(); expansion,
// focus, selection and scroll position carry over by node id. What clicking, renaming, dropping ...
// means is up to the handlers. Input mirrors Obsidian's tree: click (Ctrl / middle: new tab), the
// chevron toggles, Alt+click / Shift+click select (when enabled), ↑/↓ move the focus (Shift extends
// the selection, Ctrl also opens), ←/→ collapse / expand, Enter / Space open, F2 renames, Delete
// removes, Escape clears the selection; items can be dragged to reorder (when enabled).
class TreeList : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class Kind {
        Item,    // .tree-item-self
        Header,  // pane section header (backlink-pane > .tree-item-self): heading colour, medium weight
        Match,   // .search-result-file-match inside its parent item's matches box
        Message, // .search-empty-state (left) / .pane-empty (centered)
    };
    enum class DropPlacement { Before, After, Inside };

    struct Node {
        Kind kind = Kind::Item;
        QString id;                // stable key: expansion, focus and selection survive rebuilds
        QString icon;              // Item: Lucide icon in the chevron's place (non-collapsible items)
        QString prefix;            // Item: text before the title (a nested tag's "parent/")
        QString title;             // Item/Header: the text; Match: unused (see content); Message: the text
        QList<MatchRange> matches; // Item: highlighted title ranges; Match: absolute content ranges
        bool boldMatches = false;  // Item: highlights are bold (.suggestion-highlight) instead of marked
        QString subtext;           // Item: .tree-item-inner-subtext
        QString flair;             // Item/Header: .tree-item-flair
        QString tooltip;
        bool collapsible = false;
        bool collapsed = false;
        bool active = false;       // .is-active
        bool faint = false;        // dimmed title (unresolved links)
        bool hidden = false;       // filtered out (with its children)
        bool selectable = true;
        bool draggable = true;
        bool centered = false;     // Message: .pane-empty
        int spaceBefore = 0;       // extra vertical space above / below the row (px)
        int spaceAfter = 0;
        // Match rows: content[from, to) with "..." around it, the hover buttons that extend it, an action
        // button ("Link") and destination chips (e.g. file names).
        std::shared_ptr<const QString> content;
        int from = 0;
        int to = 0;
        bool ellipsisBefore = false;
        bool ellipsisAfter = false;
        bool moreBefore = false;
        bool moreAfter = false;
        QString button;
        QStringList chips;
        QStringList chipTooltips;
        QVariant data;             // client payload
        int type = 0;              // client-defined kind

        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;

        Node* add(Kind kind, const QString& id = {});
        Node* add(std::unique_ptr<Node> child);
        bool hasVisibleChildren() const;

    private:
        friend class TreeList;
        struct Layout;
        mutable std::shared_ptr<Layout> layout;
    };
    using NodePtr = std::unique_ptr<Node>;

    struct Handlers {
        // A row was clicked / chosen with the keyboard (Enter, Space, Ctrl+↑/↓: keyboard = true). Not
        // called for clicks the selection logic consumed.
        std::function<void(Node* node, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool keyboard)> activate;
        std::function<void(Node* node, bool collapsed)> collapsedChanged; // by the user (chevron, keys)
        std::function<void(Node* node, const QPoint& globalPos)> contextMenu; // node null: empty area
        std::function<void(Node* node, const QRect& globalRect)> hover;       // for page previews
        std::function<void()> unhover;
        std::function<void(Node* match, bool before)> moreContext;            // match hover buttons
        std::function<void(Node* match)> button;                              // match action button
        std::function<void(Node* match, int chip)> chip;                      // match destination chip
        std::function<void(Node* node, const QString& text)> renamed;         // inline rename accepted
        std::function<void(const QList<Node*>& nodes)> remove;                // Delete key
        std::function<void(Node* node)> rename;                               // F2
        // Drag & drop reordering: may `nodes` go there? (default: yes), and perform the drop.
        std::function<bool(const QList<Node*>& nodes, Node* target, DropPlacement placement)> canDrop;
        std::function<void(const QList<Node*>& nodes, Node* target, DropPlacement placement)> drop;
        std::function<void()> activateView; // the view wants focus / its leaf activated
    };

    explicit TreeList(QWidget* parent = nullptr);
    ~TreeList() override;

    Handlers& handlers() { return m_handlers; }

    // ---- model ------------------------------------------------------------------------------------
    // Replaces the tree. With keepState, nodes whose id existed keep their expansion (unless
    // keepCollapse is false), focus, selection and hover; the scroll position is kept.
    void setRoots(std::vector<NodePtr> roots, bool keepCollapse = true);
    void clear();
    const std::vector<NodePtr>& roots() const { return m_roots; }
    Node* findNode(const QString& id) const;
    QList<Node*> allNodes() const; // preorder
    void nodeChanged(Node* node);  // its text / flags changed: relayout
    void layoutChanged();          // several nodes changed (hidden flags, ...)

    // Content margins of the list (the pane's padding).
    void setContentMargins(int left, int top, int right, int bottom);

    // ---- expansion --------------------------------------------------------------------------------
    void setCollapsed(Node* node, bool collapsed);
    void toggleCollapsed(Node* node) { setCollapsed(node, !node->collapsed); }
    // Collapse / expand every collapsible item (Obsidian's toggleCollapseAll).
    void setAllCollapsed(bool collapsed);
    bool isAllCollapsed() const { return m_allCollapsed; }
    void setAllCollapsedFlag(bool collapsed) { m_allCollapsed = collapsed; }

    // ---- rows -------------------------------------------------------------------------------------
    int rowCount() const;
    Node* nodeAt(int row) const;
    int rowOf(const Node* node) const;  // -1 if not visible
    QRect rowRect(int row) const;       // the row box (viewport coordinates)
    int rowAt(const QPoint& viewportPos) const;
    QStringList visibleTitles() const;  // indented by depth ("  child"), for tests
    void scrollToNode(const Node* node, int margin = 4);
    QRect chevronRect(int row) const;
    QRect buttonRect(int row) const;             // match action button
    QRect moreButtonRect(int row, bool before) const;
    QRect chipRect(int row, int chip) const;

    // ---- state ------------------------------------------------------------------------------------
    void setLeafActive(bool active); // focus rings only in the active leaf
    Node* focusedNode() const { return m_focused; }
    void setFocusedNode(Node* node, bool scroll = true);
    void setSelectionEnabled(bool on) { m_selectionEnabled = on; }
    QList<Node*> selectedNodes() const; // in row order
    void setSelectedNodes(const QList<Node*>& nodes);
    void clearSelection();
    QList<Node*> actionNodes() const;   // the selection, else the focused node
    void setMenuNode(Node* node);       // .has-active-menu while its context menu is open
    Node* hoveredNode() const;

    // ---- rename -----------------------------------------------------------------------------------
    void startRename(Node* node);
    void commitRename();
    void cancelRename();
    Node* renamingNode() const { return m_renaming; }
    QLineEdit* renameEditor() const { return m_editor; }

    // ---- drag & drop ------------------------------------------------------------------------------
    void setDragEnabled(bool on) { m_dragEnabled = on; }
    bool isDragging() const { return m_drag.active; }
    Node* dropTarget() const { return m_drag.target; }
    DropPlacement dropPlacement() const { return m_drag.placement; }
    void cancelDrag();

    // Keys the tree handles itself (they take precedence over global hotkeys while it has focus).
    bool handleKey(QKeyEvent* e);
    bool wantsKey(const QKeyEvent* e) const;

signals:
    void rowsChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void changeEvent(QEvent* e) override;

private:
    struct Metrics;
    enum class Hit { None, Row, Chevron, Button, MoreBefore, MoreAfter, Chip };

    void updateMetrics();
    void ensureRows() const;
    void ensureGeometry() const;
    void invalidate();
    void updateScrollRange();
    int depthOf(const Node* n) const;
    int rowLeft(const Node* n) const;
    int rowHeight(const Node* n, int width) const; // without spaceBefore/After
    void layoutNode(const Node* n, int width) const;
    int textWidth(const Node* n, int rowWidth) const;
    bool isFirstMatch(int row) const;
    bool isLastMatch(int row) const;
    void paintItem(QPainter& p, int row, const QRect& r);
    void paintMatch(QPainter& p, int row, const QRect& r);
    void paintMessage(QPainter& p, int row, const QRect& r);
    void paintGuides(QPainter& p, int first, int last);
    Hit hitTest(const QPoint& pos, int* row, int* chip = nullptr) const;
    void updateHover(const QPoint& pos);
    void clickRow(int row, Hit hit, int chip, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    bool handleSelection(Node* n, Qt::KeyboardModifiers mods, bool mouse, Qt::MouseButton button);
    void moveFocus(bool forward, Qt::KeyboardModifiers mods);
    bool focusable(const Node* n) const;
    QList<Node*> rangeBetween(Node* a, Node* b) const;
    void positionEditor();
    // drag
    void beginDrag();
    void updateDrag(const QPoint& viewportPos, const QPoint& globalPos);
    void finishDrag(bool commit);
    void computeDropTarget(const QPoint& viewportPos);

    Handlers m_handlers;
    std::vector<NodePtr> m_roots;
    std::unique_ptr<Metrics> m_m;
    int m_marginLeft = 12, m_marginTop = 4, m_marginRight = 12, m_marginBottom = 32;

    mutable std::vector<Node*> m_rows;
    mutable std::vector<int> m_tops;    // content y of each row box (after spaceBefore)
    mutable std::vector<int> m_heights; // row box heights
    mutable QHash<const Node*, int> m_rowIndex;
    mutable bool m_rowsDirty = true;
    mutable bool m_geometryDirty = true;
    mutable int m_contentHeight = 0;
    mutable int m_layoutWidth = -1;
    mutable quint64 m_themeGeneration = 0;

    bool m_allCollapsed = false;
    bool m_leafActive = false;
    bool m_selectionEnabled = false;
    bool m_dragEnabled = false;
    Node* m_focused = nullptr;
    Node* m_anchor = nullptr;
    QList<Node*> m_selected;
    Node* m_menuNode = nullptr;
    int m_hoverRow = -1;
    Hit m_hoverHit = Hit::None;
    int m_hoverChip = -1;

    int m_pressRow = -1;
    Hit m_pressHit = Hit::None;
    int m_pressChip = -1;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QPoint m_pressPos;

    Node* m_renaming = nullptr;
    QLineEdit* m_editor = nullptr;

    struct Drag {
        bool active = false;
        QList<Node*> nodes;
        Node* target = nullptr;
        DropPlacement placement = DropPlacement::Before;
        bool valid = false;
        QPoint lastViewportPos;
        QPoint lastGlobalPos;
    } m_drag;
    QTimer m_expandTimer;
    QTimer m_autoScroll;
    QPointer<DragGhost> m_ghost;
    QObject* m_keyGuard = nullptr;
};

} // namespace oc
