#pragma once

#include "ui/workspace/workspace.h"

#include <QAbstractScrollArea>
#include <QCollator>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

class QLineEdit;

namespace oc {

class DragGhost;
class ErrorTooltip;
class TAbstractFile;
class TFile;
class TFolder;
class Vault;

// Obsidian's file explorer sort orders (view state "sortOrder"). Folders always come first, sorted
// by name; the order only applies to files.
enum class ExplorerSortOrder { Alphabetical, AlphabeticalReverse, ByModifiedTime, ByModifiedTimeReverse, ByCreatedTime, ByCreatedTimeReverse };
QString sortOrderId(ExplorerSortOrder order); // "alphabetical", "byModifiedTimeReverse", ...
std::optional<ExplorerSortOrder> sortOrderFromId(const QString& id);

// Obsidian's natural file-name order (Intl.Collator with numeric collation, case-insensitive):
// "Note 2" < "Note 10" < "note 11".
int naturalCompare(const QString& a, const QString& b);

// The file explorer's tree (.nav-files-container): a custom-painted, virtualized list of the vault's
// folders and files in Obsidian's layout — 13 px items with 4/8/4/24 px padding, 17 px per level with
// indentation guides, collapse triangles, the extension tag of non-Markdown files, hover / active /
// selected / focused / drag-over states. It follows the Vault's created/deleted/renamed/modified
// signals and keeps expansion, selection and scroll position across changes.
//
// Input mirrors Obsidian's tree: click opens a file (Ctrl/middle click: new tab, Ctrl+Alt: split) or
// toggles a folder; Alt+click toggles the selection, Shift+click selects a range; ↑/↓ move the focus
// (Shift extends the selection, Ctrl also opens), ←/→ collapse/expand, Enter/Space open, F2
// renames, Delete deletes, Escape clears the selection. Items are dragged onto folders to move them
// (hovered folders expand after 750 ms); drags leaving the tree are offered to the dragOutside* handlers.
// The tree only does view work; what opening/renaming/deleting/moving means is up to the handlers.
class FileTree : public QAbstractScrollArea {
    Q_OBJECT
public:
    struct Handlers {
        // A file was clicked / chosen with the keyboard (keyboardNav: Ctrl+↑/↓ preview, don't focus it).
        std::function<void(TFile* file, Workspace::NewLeaf mode, bool keyboardNav)> open;
        // Right click (item null = the empty area, i.e. the vault root).
        std::function<void(TAbstractFile* item, const QPoint& globalPos)> contextMenu;
        // Rename validation (Obsidian's messages; empty = fine). final: the name is about to be saved.
        std::function<QString(TAbstractFile* item, const QString& name, bool final)> validateName;
        // Performs a rename to `name` (a basename for files); false + *error keeps the editor open.
        std::function<bool(TAbstractFile* item, const QString& name, QString* error)> rename;
        std::function<void(const QList<TAbstractFile*>& items)> remove; // Delete key
        std::function<void(const QList<TAbstractFile*>& items, TFolder* target)> move; // drop on a folder
        // Dragging outside the tree (e.g. onto the workspace): move returns whether something there
        // accepts the items (and shows its own feedback), drop performs it, leave clears the feedback
        // (the pointer came back or the drag ended).
        std::function<bool(const QList<TAbstractFile*>& items, const QPoint& globalPos)> dragOutsideMove;
        std::function<bool(const QList<TAbstractFile*>& items, const QPoint& globalPos)> dragOutsideDrop;
        std::function<void()> dragOutsideLeave;
        // The view wants keyboard focus / its leaf activated (Alt/Shift clicks, keyboard use).
        std::function<void()> activate;
        // Ctrl+C / Ctrl+X (the action items) and Ctrl+V (into the focused folder, or the focused
        // file's folder, else the root) — Obsidian's "obsidian/files" clipboard.
        std::function<void(const QList<TAbstractFile*>& items, bool cut)> copy;
        std::function<void(TFolder* target)> paste;
        // Files dropped from another application (Obsidian's importAttachments).
        std::function<void(const QList<QUrl>& urls, TFolder* target)> importFiles;
        // The pointer entered a file's row (Obsidian's "hover-link" for page previews) / left it.
        std::function<void(TFile* file, const QRect& globalRect)> hover;
        std::function<void()> unhover;
    };

    explicit FileTree(QWidget* parent = nullptr);
    ~FileTree() override;

    Handlers& handlers() { return m_handlers; }

    void setVault(Vault* vault); // rebuilds everything
    Vault* vault() const { return m_vault; }

    void setSortOrder(ExplorerSortOrder order);
    ExplorerSortOrder sortOrder() const { return m_order; }
    void setShowUnsupported(bool show);
    bool showUnsupported() const { return m_showUnsupported; }
    // Which extensions are "supported" (shown unless showUnsupported); default: all.
    void setSupportedPredicate(std::function<bool(const QString& extension)> fn);
    // Offline vault: dimmed, no drag & drop, no renaming.
    void setReadOnly(bool readOnly);
    bool isReadOnly() const { return m_readOnly; }
    // Whether the view's leaf is the active one (focus rings are only shown then).
    void setLeafActive(bool active);

    // ---- expansion -----------------------------------------------------------------------------
    bool isExpanded(const TAbstractFile* folder) const;
    void setExpanded(TAbstractFile* folder, bool expanded);
    QStringList expandedPaths() const;                // sorted
    void setExpandedPaths(const QStringList& paths);  // collapses all others
    bool isAllCollapsed() const { return m_allCollapsed; }
    void setAllCollapsed(bool collapsed);             // collapse / expand every folder
    void toggleAllCollapsed() { setAllCollapsed(!m_allCollapsed); }

    // ---- rows (the visible, flattened tree) ------------------------------------------------------
    int rowCount() const;
    TAbstractFile* itemAt(int row) const;
    int rowOf(const TAbstractFile* item) const;       // -1 if not visible
    QStringList visiblePaths() const;                 // folders with a trailing "/"
    QRect rowRect(int row) const;                     // the item box, viewport coordinates
    int rowAt(const QPoint& viewportPos) const;       // -1 if none
    void scrollToItem(const TAbstractFile* item, int margin = 4);
    void reveal(TAbstractFile* item);                 // expand the ancestors and scroll to it
    void flash(TAbstractFile* item);                  // .is-flashing for 750 ms

    // ---- state -----------------------------------------------------------------------------------
    void setActiveFile(TFile* file);                  // the open file (.is-active)
    TFile* activeFile() const;
    TAbstractFile* focusedItem() const;
    void setFocusedItem(TAbstractFile* item, bool scroll = true);
    QList<TAbstractFile*> selectedItems() const;      // in visible order
    void setSelectedItems(const QList<TAbstractFile*>& items);
    void clearSelection();
    // Obsidian's getSelectedItems(): the selection, else the focused item.
    QList<TAbstractFile*> actionItems() const;
    void setMenuItem(TAbstractFile* item);            // .has-active-menu while its context menu is open
    void setCutItems(const QList<TAbstractFile*>& items); // .is-cut

    // ---- rename ----------------------------------------------------------------------------------
    void startRename(TAbstractFile* item);
    bool commitRename();                              // false (editor stays) if invalid / failed
    void cancelRename();
    TAbstractFile* renamingItem() const;
    QLineEdit* renameEditor() const { return m_editor; }
    ErrorTooltip* errorTooltip() const { return m_error; }

    // ---- drag & drop -----------------------------------------------------------------------------
    bool isDragging() const { return m_drag.active; }
    TFolder* dropTarget() const;                      // the highlighted folder (root included)
    QList<TAbstractFile*> draggedItems() const;
    void cancelDrag();

    // Keys the tree handles itself. While the tree has focus they take precedence over global hotkeys
    // (Obsidian's view scope), e.g. F2 renames the focused item rather than the open note.
    bool handleKey(QKeyEvent* e);
    bool wantsKey(const QKeyEvent* e) const;

signals:
    void allCollapsedChanged(bool allCollapsed);
    void expansionChanged(); // a folder was expanded/collapsed (the view saves the folds)

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
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void changeEvent(QEvent* e) override;

private:
    struct Node {
        TAbstractFile* item = nullptr;
        Node* parent = nullptr;
        std::vector<Node*> children;
        int depth = -1;
        bool folder = false;
        bool expanded = false;
        bool sorted = false;
        std::optional<QCollatorSortKey> key;     // folder name / file basename
        std::optional<QCollatorSortKey> nameKey; // file name (tie-break)
    };
    struct Metrics {
        double lineHeight = 16.9;
        double rowHeight = 24.9;
        double pitch = 26.9;
        double padTop = 4, padBottom = 24, padX = 12;
        double indent = 17;       // children margin 12 + guide 1 + padding 4
        double textLeft = 24;     // --nav-item-padding left
        double textRight = 8;
        double radius = 4;
        QFont font;
        QFont tagFont;
    };

    // model
    void clearNodes();
    void rebuild();
    Node* nodeFor(const TAbstractFile* item) const { return m_nodes.value(item); }
    Node* addNode(TAbstractFile* item);
    void removeNode(Node* node);
    bool isShown(const Node* n) const; // unsupported files hidden
    void ensureKeys(Node* n);
    void sortChildren(Node* folder);
    bool lessThan(Node* a, Node* b);
    void invalidateSort(Node* folder);
    void invalidateAllSorts();
    void ensureRows() const;
    void rowsChanged(); // schedule relayout + repaint
    void onCreated(TAbstractFile* item);
    void onDeleted(TAbstractFile* item);
    void onRenamed(TAbstractFile* item, const QString& oldPath);
    void onModified(TFile* file);
    void setNodeExpanded(Node* n, bool expanded, bool user);
    void updateAllCollapsedFromNodes();

    // geometry / painting
    void updateMetrics();
    void updateScrollRange();
    double rowTop(int row) const; // content coordinates
    void paintRow(QPainter& p, int row, const QRect& r);
    int lastDescendantRow(int row) const;
    QRect titleRect(int row) const;

    // interaction
    void clickRow(int row, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    bool handleSelection(Node* n, Qt::KeyboardModifiers mods, bool mouse, Qt::MouseButton button);
    void openNode(Node* n, Workspace::NewLeaf mode, bool keyboardNav);
    void keyOpen(Node* n, Qt::KeyboardModifiers mods, bool navKey);
    void moveFocus(bool forward, Qt::KeyboardModifiers mods, QKeyEvent* e);
    void setFocusedNode(Node* n, bool scroll);
    QList<Node*> rangeBetween(Node* a, Node* b) const;
    void deleteSelection();
    void updateHover(const QPoint& pos);

    // rename
    void positionEditor();
    void onRenameEdited();
    void showRenameError(const QString& message, bool transient);

    // drag
    void beginDrag(const QPoint& pos);
    void updateDrag(const QPoint& viewportPos, const QPoint& globalPos);
    void finishDrag(const QPoint& viewportPos, const QPoint& globalPos, bool commit);
    Node* dropNodeAt(const QPoint& viewportPos) const;
    bool canDropInto(Node* folder) const;
    Node* highlightedFolder() const { return m_drag.active ? m_drag.target : m_externalTarget; }
    void setExternalTarget(Node* target);
    void autoScrollTick();

    Handlers m_handlers;
    QPointer<Vault> m_vault;
    QList<QMetaObject::Connection> m_connections;
    QHash<const TAbstractFile*, Node*> m_nodes;
    Node* m_root = nullptr;
    QCollator m_collator;
    ExplorerSortOrder m_order = ExplorerSortOrder::Alphabetical;
    bool m_showUnsupported = false;
    std::function<bool(const QString&)> m_supported;
    bool m_readOnly = false;
    bool m_leafActive = false;
    bool m_allCollapsed = true;

    mutable std::vector<Node*> m_rows;
    mutable QHash<const Node*, int> m_rowIndex;
    mutable bool m_rowsDirty = true;
    Metrics m_m;

    Node* m_activeFile = nullptr;   // .is-active
    Node* m_focused = nullptr;      // .has-focus
    Node* m_anchor = nullptr;       // last clicked (shift range base, keyboard start)
    QList<Node*> m_selected;        // .is-selected (insertion order)
    Node* m_menuNode = nullptr;     // .has-active-menu
    QList<Node*> m_cut;             // .is-cut
    Node* m_flash = nullptr;        // .is-flashing
    QTimer m_flashTimer;
    QString m_focusAfterDelete;     // where the focus goes once deleted items are gone
    int m_hoverRow = -1;

    // mouse press bookkeeping
    int m_pressRow = -1;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QPoint m_pressPos;
    bool m_pressOnEmpty = false;

    // rename
    Node* m_renaming = nullptr;
    QLineEdit* m_editor = nullptr;
    QPointer<ErrorTooltip> m_error; // a child of the window
    bool m_committing = false;

    // drag
    struct Drag {
        bool active = false;
        QList<Node*> nodes;
        Node* target = nullptr;    // folder node (m_root = vault root) or null
        bool outside = false;      // pointer outside the tree
        bool outsideAccepts = false;
        Node* expandCandidate = nullptr;
        QPoint lastViewportPos;
        QPoint lastGlobalPos;
    } m_drag;
    Node* m_externalTarget = nullptr; // folder under a drag from another application
    QTimer m_expandTimer;
    QTimer m_autoScroll;
    QPointer<DragGhost> m_ghost;

    QObject* m_keyGuard = nullptr; // app-level filter while focused (tree keys beat global hotkeys)
};

// .tooltip.mod-error: the red validation bubble shown below the item being renamed.
class ErrorTooltip : public QWidget {
    Q_OBJECT
public:
    explicit ErrorTooltip(QWidget* window);
    void showAt(const QString& message, const QRect& anchorGlobal, int timeoutMs = 0);
    void dismiss();
    QString message() const { return m_text; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_text;
    QTimer m_timer;
    int m_arrowX = 0;
};

// .drag-ghost: icon + title (+ the drop action) following the pointer during a drag.
class DragGhost : public QWidget {
    Q_OBJECT
public:
    DragGhost(QWidget* window, const QString& icon, const QString& title);
    void setAction(const QString& action);
    QString action() const { return m_action; }
    void moveToGlobal(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void resizeToContents();
    QString m_icon, m_title, m_action;
};

} // namespace oc
