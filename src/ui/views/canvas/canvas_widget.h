#pragma once

// The interactive canvas surface (Obsidian's Canvas class): a QGraphicsView whose scene holds one
// item per card and edge under a root item that carries the canvas transform (Obsidian's
// "translate(W/2, H/2) scale(s) translate(-x, -y)"). Everything is painted from the CanvasDocument;
// interactions (select, box-select, move with snapping, clone with Ctrl-drag, resize, connect,
// reconnect, pan, zoom) are handled here in canvas coordinates.
//
// Why QGraphicsView: the text card editor is a real EditorView embedded with a QGraphicsProxyWidget
// under the root item, so it is scaled with the canvas like Obsidian's in-place editor, and card
// rendering gets per-item caching; the rest is custom painting from the model.

#include "core/canvas/canvas_document.h"
#include "core/canvas/canvas_geometry.h"

#include <QBasicTimer>
#include <QElapsedTimer>
#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class QGraphicsProxyWidget;
class QLineEdit;

namespace oc {

class Document;
class EditorView;
class LivePreviewDecorator;
class MainWindow;
class Modal;
class VaultSession;

namespace canvasui {
class CanvasControls;
class CanvasItem;
class CardContent;
class CardMenu;
class SelectionMenu;
} // namespace canvasui

class CanvasWidget : public QGraphicsView {
    Q_OBJECT
public:
    // The canvas plugin's options (Obsidian's canvas.json plugin data).
    struct Options {
        bool snapToGrid = true;
        bool snapToObjects = true;
        QString defaultWheelBehavior = QStringLiteral("pan"); // "pan" | "zoom"
        QString defaultModDragBehavior = QStringLiteral("menu"); // "menu" | "card" | "note" | "media" | "webpage" | "group"
        QString cardLabelVisibility = QStringLiteral("always"); // "always" | "hover" | "never"
        double zoomBreakpoint = 0;
        QString newFileLocation = QStringLiteral("root");   // "root" | "current" | "folder"
        QString newFileFolderPath;

        static Options fromJson(const QJsonObject& json);
        // Writes the options into `json` (keeping other keys; defaults of Obsidian's are written too).
        void writeTo(QJsonObject& json) const;
    };

    explicit CanvasWidget(QWidget* parent = nullptr);
    ~CanvasWidget() override;

    // ---- context ----
    void setSession(VaultSession* session);
    VaultSession* session() const;
    void setFilePath(const QString& path); // the canvas file (link resolution)
    QString filePath() const { return m_filePath; }
    void setOptions(const Options& options);
    const Options& options() const { return m_options; }

    // ---- content ----
    // New content: history restarts, selection cleared. `fit`: zoom to fit once the view has a size.
    void setData(canvas::CanvasData data, bool fit);
    // The file changed on disk: new content, keeping the viewport and the selection (by id).
    void reloadData(canvas::CanvasData data);
    const canvas::CanvasData& data() const { return m_doc.data(); }
    canvas::CanvasDocument& document() { return m_doc; }
    // A message instead of the canvas (unreadable file); empty = none.
    void setErrorMessage(const QString& message);

    // ---- viewport ----
    canvas::Viewport canvasViewport() const; // current (animated) viewport
    canvas::Viewport targetViewport() const; // where an animation is heading
    void setViewport(double x, double y, double zoom); // immediately, cancels a queued zoom-to-fit
    void zoomBy(double delta, std::optional<QPointF> viewAnchor = std::nullopt);
    void resetZoom();
    void zoomToFit();
    void zoomToSelection();
    void zoomToRect(const QRectF& canvasRect);
    void panBy(const QPointF& canvasDelta);
    void panIntoView(const QRectF& canvasRect);
    bool isZoomToFitQueued() const { return m_fitQueued; }
    // Animations (Obsidian eases viewport changes). Off: changes apply immediately (tests, screenshots).
    void setAnimationsEnabled(bool on);
    static void setAnimationsEnabledByDefault(bool on);
    QPointF mapToCanvas(const QPoint& viewportPos) const;
    QPointF mapFromCanvas(const QPointF& canvasPos) const;

    // ---- selection ----
    const QSet<QString>& selection() const { return m_selection; }
    void setSelection(const QSet<QString>& ids);
    void selectOnly(const QString& id);
    void selectAll();
    void deselectAll();
    QRectF selectionBounds() const;

    // ---- editing ----
    bool isReadOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly);
    QString createTextNode(const QPointF& pos, canvas::Anchor anchor, std::optional<QSizeF> size = std::nullopt,
                           const QString& text = {}, bool edit = true);
    QString createFileNode(const QString& path, const QPointF& pos, canvas::Anchor anchor, std::optional<QSizeF> size = std::nullopt,
                           const QString& subpath = {}, bool select = true);
    QStringList createFileNodes(const QStringList& paths, const QPointF& pos); // a grid of 10 per row
    QString createLinkNode(const QString& url, const QPointF& pos, canvas::Anchor anchor, std::optional<QSizeF> size = std::nullopt);
    QString createGroupNode(const QRectF& rect, const QString& label = {}, bool editLabel = true);
    void deleteSelection();
    void createGroupFromSelection();
    void duplicateSelection();
    void setSelectionColor(const QString& color);
    void setSelectionEdgeEnds(canvas::EdgeEnd fromEnd, canvas::EdgeEnd toEnd);
    void copySelection();
    void cutSelection();
    void paste();
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    // Records the current data as an undo step and announces the edit (saving).
    void commit();

    void startEditing(const QString& id); // text card / note card: in-place editor; group: label
    void stopEditing();
    QString editingId() const { return m_editingId; }
    EditorView* activeEditor() const;
    void editEdgeLabel(const QString& edgeId);
    void editGroupLabel(const QString& groupId);
    QLineEdit* labelEditor() const;

    // Selection-menu actions on the current selection.
    void alignSelection(canvas::Align how);
    void distributeSelection(Qt::Orientation orientation);
    void justifySelection(Qt::Orientation orientation);
    void arrangeSelection(canvas::Arrange how);

    // Widgets (tests).
    canvasui::CanvasControls* controls() const { return m_controls; }
    canvasui::CardMenu* cardMenu() const { return m_cardMenu; }
    canvasui::SelectionMenu* selectionMenu() const { return m_selectionMenu; }
    // Opens the context menu for what is at `viewportPos` (tests use it headless: no exec()).
    QMenu* showContextMenu(const QPoint& viewportPos);
    QMenu* lastMenu() const { return m_lastMenu; }
    // Obsidian's "Canvas help" modal (the controls' help button).
    Modal* showHelp();

    // Painting (called by the items).
    void paintItem(QPainter* p, const canvasui::CanvasItem& item);
    QRectF nodeLabelRect(const canvas::Node& node) const;  // file/link card name above the card
    QRectF groupLabelRect(const canvas::Node& node) const;
    QRectF edgeLabelRect(const canvas::Edge& edge) const;

    // Rendered card contents (tests).
    canvasui::CardContent* contentFor(const QString& nodeId);
    // The live widget of a media card (PDF, audio, video) while it is on screen, else nullptr (tests).
    QWidget* mediaWidget(const QString& nodeId) const;
    // The whole canvas as an image (Obsidian's "Export as image"): every card with a 100px margin at
    // `zoom` (log2, at most 1), without selection or controls.
    QImage renderImage(double zoom = 0);
    // "Convert to file...": a text card becomes a new note (prompt for its name).
    void convertToFile(const QString& nodeId);
    // Re-render cards showing `path` (a note or image changed).
    void invalidateFile(const QString& path);

signals:
    void edited();          // a change was recorded: save soon
    void viewportChanged(); // settled viewport changed (workspace state)
    void selectionChanged();
    void readOnlyChanged(bool readOnly);
    void optionsEdited();   // snap toggles in the settings menu
    // A link in a card (or a file card's label) was activated: open it like Obsidian's openLinkText.
    void linkActivated(const QString& linktext, bool newLeaf);
    void linkHovered(const QString& linktext, const QRect& globalRect);

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void timerEvent(QTimerEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    bool focusNextPrevChild(bool next) override;

public:
    // Called by FileExplorer-style internal drags (no QDrag): files dragged over / dropped on the
    // canvas at a global position. Returns whether the canvas accepts them.
    Q_INVOKABLE bool ocFileDrag(const QStringList& paths, const QPoint& globalPos, bool drop);

private:
    friend class canvasui::CanvasItem;
    struct Drag;
    struct Hover {
        QString node;              // interaction layer target
        canvas::Handle handle = canvas::Handle::None;
        std::optional<canvas::Side> connectSide;
        QString edge;              // edge under the pointer
        bool selectionBox = false; // over the multi-selection box
        QString label;             // group label / file label under the pointer
    };

    // model -> items
    void syncItems();
    void updateSceneTransform();
    void updateOverlays(); // controls, menus, selection menu position
    void markChanged();    // data changed (not recorded)
    void setSelectionInternal(QSet<QString> ids);

    // viewport animation
    void startAnimation();
    void stepAnimation();
    void finishViewport();

    // hit testing (canvas coordinates)
    Hover hitTest(const QPointF& canvasPos) const;
    QString edgeAt(const QPointF& canvasPos) const;
    QString groupLabelAt(const QPointF& canvasPos) const;
    QString fileLabelAt(const QPointF& canvasPos) const;
    bool overSelectionBox(const QPointF& canvasPos) const;
    void updateHover(const QPointF& canvasPos);
    void updateCursor();

    // drags
    void beginMoveSelection(const QPointF& canvasPos, const QString& pressedNode);
    void beginBoxSelect(const QPointF& canvasPos, bool additive, bool mod);
    void beginResize(const QString& nodeId, canvas::Handle handle, const QPointF& canvasPos);
    void beginResizeSelection(canvas::Handle handle, const QPointF& canvasPos);
    void beginConnect(const QString& nodeId, canvas::Side side, const QPointF& canvasPos);
    void beginEdgeDrag(const QString& edgeId, const QPointF& canvasPos);
    void beginPan(const QPoint& viewPos);
    void dragMove(const QPoint& viewPos, Qt::KeyboardModifiers mods);
    void dragEnd(const QPoint& viewPos, Qt::KeyboardModifiers mods, bool cancel);
    void autoPanTick();

    // menus
    void showCreationMenu(QMenu* menu, const QPointF& pos, std::optional<QSizeF> size);
    void fillNodeMenu(QMenu* menu, const QString& nodeId);
    void fillEdgeMenu(QMenu* menu, const QString& edgeId, const QPointF& canvasPos);
    void fillSelectionMenu(QMenu* menu);
    void fillSettingsMenu(QMenu* menu);
    QMenu* newMenu();
    void popupMenu(QMenu* menu, const QPoint& globalPos);
    void showConnectionDropMenu(const QString& edgeId, const QPointF& canvasPos, canvas::Side newNodeSide);
    void pickFileFor(bool media, const std::function<void(const QString&)>& done);
    void promptUrl(const QString& initial, const std::function<void(const QString&)>& done);
    void createFromBox(const QRectF& box, const QPoint& globalPos);
    void swapFile(const QString& nodeId);
    void narrowToHeading(const QString& nodeId);
    void changeUrl(const QString& nodeId);
    void pickGroupBackground(const QString& nodeId);

    // editing
    void finishLabelEdit(bool commitEdit);
    void positionEditor();
    void onEditorTextChanged();
    void openLinkFromCard(const QString& target, bool newLeaf);

    // content
    std::unique_ptr<canvasui::CardContent> buildContent(const canvas::Node& node);
    void paintNode(QPainter* p, const canvas::Node& node);
    void paintEdge(QPainter* p, const canvas::Edge& edge, bool label);
    void paintOverlay(QPainter* p);
    void paintGrid(QPainter* p, const QRectF& viewRect);
    bool isZoomedOut() const;
    double zoomMultiplier() const;
    std::optional<canvas::EdgeGeometry> edgeGeometry(const canvas::Edge& edge) const;
    QList<QRectF> snapTargets(const QSet<QString>& exclude, bool groups) const;
    canvas::SnapOptions snapOptions() const;
    bool canSnap(Qt::KeyboardModifiers mods) const;
    LivePreviewDecorator* resolver();
    MainWindow* mainWindow() const;
    QString resolveLinkPath(const QString& linkpath) const; // vault path or ""

    // session
    QPointer<VaultSession> m_session;
    QString m_filePath;
    Options m_options;

    // model
    canvas::CanvasDocument m_doc;
    quint64 m_syncedRevision = ~0ull;
    QSet<QString> m_selection;
    bool m_readOnly = false;
    QString m_error;

    // viewport (current and target, Obsidian's x/y/zoom and tx/ty/tZoom)
    canvas::Viewport m_vp;
    canvas::Viewport m_target;
    double m_itemsZoom = -100; // zoom the items were last synced at
    std::optional<QPointF> m_zoomCenter; // view offset from the center the zoom animation keeps fixed
    bool m_fitQueued = false;
    bool m_autoFit = false; // the viewport is the automatic initial fit: refit when the view resizes
    void autoFit();
    bool m_animations = true;
    QBasicTimer m_animTimer;
    QElapsedTimer m_animClock;

    // scene
    QGraphicsItem* m_root = nullptr;
    QHash<QString, canvasui::CanvasItem*> m_nodeItems;
    QHash<QString, canvasui::CanvasItem*> m_edgeItems;
    QHash<QString, canvasui::CanvasItem*> m_edgeLabelItems;
    canvasui::CanvasItem* m_overlay = nullptr;
    struct ContentEntry {
        std::shared_ptr<canvasui::CardContent> content;
        QString key; // what the content was built from
    };
    QHash<QString, ContentEntry> m_contents;
    QHash<QString, double> m_scroll; // content scroll of cards
    // Live widgets of media cards (PDF viewer, players) while on screen; input reaches them only when the
    // card is focused (Obsidian's .canvas-node-content-blocker).
    struct MediaHost {
        QPointer<QGraphicsProxyWidget> proxy;
        QString key;
    };
    QHash<QString, MediaHost> m_media;
    void updateMediaWidgets();
    bool overFocusedMedia(const QPointF& scenePos) const;
    std::unique_ptr<LivePreviewDecorator> m_resolver;

    // interaction
    std::unique_ptr<Drag> m_drag;
    Hover m_hover;
    bool m_spaceHeld = false;
    QPoint m_lastMouse;
    QBasicTimer m_autoPan;
    QPointF m_autoPanDelta;
    bool m_suppressContextMenu = false;
    QPointer<QMenu> m_lastMenu;

    // in-place editing
    QString m_editingId;
    std::shared_ptr<Document> m_editDoc;
    EditorView* m_editor = nullptr;
    QGraphicsProxyWidget* m_editorProxy = nullptr;
    QTimer m_textCommit;
    QLineEdit* m_labelEdit = nullptr;
    QString m_labelTarget; // "node:<id>" or "edge:<id>"

    // widgets
    canvasui::CanvasControls* m_controls = nullptr;
    canvasui::CardMenu* m_cardMenu = nullptr;
    canvasui::SelectionMenu* m_selectionMenu = nullptr;
};

} // namespace oc
