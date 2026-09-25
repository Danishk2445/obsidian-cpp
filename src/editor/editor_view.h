#pragma once

#include "core/documents/document.h"
#include "editor/fold/fold_service.h"
#include "render/layout/doc_layout.h"
#include "text/selection.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QPointer>

#include <memory>

class QMenu;

namespace oc {

class SourceDecorator;
class LivePreviewDecorator;

// The Markdown editor widget: a virtualized, custom-painted text view over a shared Document.
// M1: source mode (block styling). Live preview decorations plug in through LineDecorator (M2).
class EditorView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class Mode { Source, LivePreview, Reading };
    LivePreviewDecorator* livePreview() const { return m_lp.get(); }

    explicit EditorView(QWidget* parent = nullptr);
    ~EditorView() override;

    void setDocument(std::shared_ptr<Document> doc);
    Document* document() const { return m_doc.get(); }
    std::shared_ptr<Document> documentPtr() const { return m_doc; }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool isReadOnly() const;
    void setReadOnly(bool ro);

    // Layout options
    void setReadableLineLength(bool on);
    bool readableLineLength() const { return m_readable; }
    void setMargins(qreal horizontal, qreal top, qreal bottom);
    void setLineWrapping(bool on);
    // Obsidian's "Show line number": a gutter of line numbers left of the text in the editing modes, inside
    // the text column (like .cm-gutters in .cm-contentContainer); the inline title keeps the full column.
    void setShowLineNumbers(bool on);
    bool showLineNumbers() const { return m_lineNumbers; }

    // Selection
    const Selection& selection() const { return m_sel; }
    void setSelection(const Selection& sel, bool scroll = true, bool userEvent = true);
    qsizetype cursorPosition() const { return m_sel.main().head; }
    void setCursorPosition(qsizetype pos, bool scroll = true) { setSelection(Selection::cursor(pos), scroll); }
    QString selectedText() const;

    // Scrolling
    double scrollTop() const;         // doc y at the top of the viewport
    void setScrollTop(double y);
    void scrollToPos(qsizetype pos, bool center = false);
    // Ephemeral state for navigation history: {"cursor": {from, to}, "scroll": topLine}
    QJsonObject ephemeralState() const;
    void setEphemeralState(const QJsonObject& state);

    // Editing API (used by commands, hooks and tests)
    void replaceSelections(const QString& text, const QString& userEvent = QStringLiteral("input.type"));
    void dispatch(const Transaction& tr);
    void insertText(const QString& text) { replaceSelections(text); }
    bool undo();
    bool redo();
    void selectAll();
    void cut();
    void copy();
    void paste();
    void pasteText(const QString& text, const QString& userEvent = QStringLiteral("input.paste"));
    void deleteBackward(bool byWord = false);
    void deleteForward(bool byWord = false);
    void insertNewline();
    void indentSelection(bool more);
    void moveLines(int direction);   // Alt+Up/Down
    void copyLines(int direction);   // Shift+Alt+Up/Down
    void deleteLines();              // Mod+Shift+K / Mod+D
    void addCursorVertical(int direction);
    void collapseToMainCursor();

    // Movement (extend = shift)
    void moveHorizontal(int dir, bool byWord, bool extend);
    void moveVertical(int dir, bool extend, int pages = 0);
    void moveLineBoundary(bool forward, bool extend);
    void moveDocBoundary(bool forward, bool extend);

    // Geometry helpers (widget coordinates)
    QPointF docToViewport(const QPointF& docPt) const;
    QPointF viewportToDoc(const QPointF& vp) const;
    QRectF caretRectInViewport() const;
    QRectF caretRectAt(qsizetype pos) const; // viewport coordinates of a caret at `pos`
    qsizetype positionAt(const QPoint& viewportPos);

    // Caret shape. Block: a filled box over the character at the cursor, drawn in
    // --interactive-accent with the character in --text-on-accent (vim's "fat cursor" in normal,
    // visual and replace modes); for a forward selection the box covers the character before the
    // head (the last selected one) unless the head is at a line break. mainOnly: draw it for the
    // main range only (vim visual-block mode).
    enum class CursorShape { Bar, Block };
    void setCursorShape(CursorShape shape, bool mainOnly = false);
    CursorShape cursorShape() const { return m_cursorShape; }
    // Viewport rectangle of the block cursor for `range` (empty when the shape is Bar or the
    // position isn't laid out as text).
    QRectF blockCursorRect(const SelRange& range) const;

    // A panel docked at the bottom of the editor, below the scrolling text and spanning its full
    // width (CodeMirror's bottom panels; vim's ":" / "/" prompt). The view reserves its
    // sizeHint() height; call updateBottomPanel() after its content or visibility changed.
    void setBottomPanel(QWidget* panel);
    QWidget* bottomPanel() const { return m_bottomPanel; }
    void updateBottomPanel();

    // A widget shown above the first line that scrolls with the text (Obsidian's inline title and
    // properties). It is reparented to the viewport; its heightForWidth() (or sizeHint height) plus
    // `bottomMargin` is reserved above the text, at the text column's width.
    void setTopWidget(QWidget* widget, int bottomMargin = 0);
    QWidget* topWidget() const { return m_topWidget; }
    void updateTopWidget(); // re-measure after its content changed
    // Let the top widget extend `px` to the left of the text column (into the fold gutter, e.g. for
    // the properties' collapse indicator); it is then measured and laid out at that larger width.
    void setTopWidgetOverhang(int px);

    DocLayout& layout() { return m_layout; }
    double contentLeft() const { return m_contentLeft; }
    double contentWidth() const { return m_layout.contentWidth(); }
    double topPadding() const { return m_marginTop; }
    double documentHeight() const; // margins + all (visible) lines, as currently measured/estimated

    // Show only lines [from, to] (to = -1: to the end), like an embed of a section. Folds still apply.
    void setVisibleLineRange(int from, int to);
    QPair<int, int> visibleLineRange() const { return {m_clipFrom, m_clipTo}; }

    // Extra decoration layers (search highlights, spellcheck, ...), applied in order to every laid-out
    // line after the mode's own decorations. Call refreshDecorations() when a layer's output changes.
    using DecorationLayer = std::function<void(int line, LineDecoration&)>;
    int addDecorationLayer(DecorationLayer layer); // returns an id for removeDecorationLayer()
    void removeDecorationLayer(int id);
    void refreshDecorations(int fromLine = 0, int toLine = -1);

    // Folding (Obsidian: "Fold heading", "Fold indent" and the frontmatter). Folds belong to this
    // view; the owner persists them (folds()/setFolds()).
    void setFoldOptions(const FoldOptions& options);
    const FoldOptions& foldOptions() const { return m_foldOptions; }
    std::optional<FoldRange> foldableAt(int line) const;
    bool isLineFoldable(int line) const; // cheap (for fold indicators)
    const QList<FoldRange>& folds() const { return m_folds; }
    void setFolds(const QList<FoldRange>& folds); // restore; ranges not matching the text are dropped
    bool isFoldedAt(int line) const;              // a fold starts at this line
    bool foldLine(int line);
    bool unfoldLine(int line);
    bool toggleFoldLine(int line);
    bool foldAtSelection();   // fold the region starting at a cursor's line (Ctrl+Shift+[)
    bool unfoldAtSelection(); // unfold folds starting at the cursors' lines (Ctrl+Shift+])
    void foldAll();           // every foldable region, nested ones included (Ctrl+Alt+[)
    void unfoldAll();         // (Ctrl+Alt+])
    bool foldMore();          // fold the innermost unfolded region around the selection
    bool foldLess();          // unfold the outermost folded region around the selection
    QRectF foldIndicatorRect(int line); // viewport coordinates (empty if the line isn't laid out)

signals:
    void foldsChanged();
    void selectionChanged();
    void cursorMoved(int line, int column);
    void modeChanged(Mode mode);
    void scrolled(double top);
    void focusChangedIn(bool focused);
    void contextMenuRequestedAt(const QPoint& globalPos, qsizetype docPos);
    void contextMenuAboutToShow(QMenu* menu, qsizetype docPos); // the menu can still be changed
    // A rendered link was clicked (live preview / reading). kind: 0 wikilink, 1 embed, 2 markdown link,
    // 3 url, 4 tag. newLeaf: Ctrl held.
    void linkActivated(const QString& target, int kind, bool newLeaf);
    // The mouse moved onto a link (kind as above; rect: its box in viewport coordinates), or off it
    // (empty target). Drives the page preview popover.
    void linkHovered(const QString& target, int kind, const QRect& rect);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void keyPressEvent(QKeyEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void timerEvent(QTimerEvent* e) override;
    bool event(QEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    bool focusNextPrevChild(bool next) override { Q_UNUSED(next); return false; } // Tab is for indentation

private:
    void onDocumentChanged(const DocUpdate& u);
    void onSyntaxProgress(const md::TreeUpdate& u);
    void updateGeometryAndLayout();
    void updateScrollbar();
    void ensureVisibleLayout();
    void restartBlink();
    void invalidateSelectionLines(const Selection& a, const Selection& b);
    void emitCursor();
    void updatePrimarySelection();
    qsizetype clusterBoundary(qsizetype pos, int dir) const;
    qsizetype wordBoundary(qsizetype pos, int dir) const;
    void selectWordAt(qsizetype pos, bool extend);
    void selectLineAt(qsizetype pos, bool extend);
    void autoScrollTick();
    void applyTheme();

    std::shared_ptr<Document> m_doc;
    QMetaObject::Connection m_docConn, m_syntaxConn, m_roConn;
    std::unique_ptr<SourceDecorator> m_decorator;
    std::unique_ptr<LivePreviewDecorator> m_lp;
    LineDecorator* activeDecorator() const;
    void syncRevealSelection(const Selection& old);
    bool handleWidgetClick(const QPointF& docPt);
    bool handleLinkClick(qsizetype pos, Qt::KeyboardModifiers mods);
    StyleSheetPtr m_styles;
    DocLayout m_layout;
    Selection m_sel;
    Selection m_revealed; // selection the live preview reveal state currently reflects
    Mode m_mode = Mode::Source;
    bool m_readOnly = false;
    bool m_readable = true;
    qreal m_marginX = 32, m_marginTop = 32, m_marginBottom = 32;
    qreal m_topBase = 32;       // top margin without the top widget
    QPointer<QWidget> m_topWidget;
    int m_topWidgetMargin = 0;
    qreal m_topWidgetH = 0;     // reserved height (widget + margin)
    int m_topOverhang = 0;      // setTopWidgetOverhang()
    void positionTopWidget();
    qreal measureTopWidget() const; // height + margin at the current width (0 when hidden)
    QPointer<QWidget> m_bottomPanel;
    CursorShape m_cursorShape = CursorShape::Bar;
    bool m_cursorMainOnly = false;
    qsizetype blockCursorPos(const SelRange& r) const;
    void paintBlockCursor(QPainter& p, const SelRange& r);
    double m_contentLeft = 0;
    double m_scrollTop = 0; // doc y at the viewport top (can be negative: top margin)
    bool m_settingScroll = false;

    // caret
    QBasicTimer m_blink;
    bool m_caretOn = true;

    // IME
    QString m_preedit;
    int m_preeditCursor = 0;

    // mouse
    enum class DragUnit { None, Char, Word, Line };
    DragUnit m_drag = DragUnit::None;
    SelRange m_dragOrigin;
    QPoint m_lastMouse;
    QBasicTimer m_autoScroll;
    QElapsedTimer m_lastClickTime;
    int m_clickCount = 0;
    QPoint m_lastClickPos;

    void applyExtraDecorations(int line, LineDecoration& d) const;
    // folding
    void applyFolds(bool emitChange = true);            // folds -> hidden lines in the layout
    void mapFolds(const DocUpdate& u);                  // follow an edit (edits inside a fold unfold it)
    Selection skipFolds(const Selection& sel, const Selection& old) const;
    bool addFold(const FoldRange& r);
    void paintFoldIndicators(QPainter& p);
    void paintLineNumbers(QPainter& p);
    double gutterWidth() const;
    bool m_lineNumbers = false;
    double m_columnLeft = 0, m_columnWidth = 0; // the text column including the gutter
    int foldLineAtIndicator(const QPoint& vp);          // line whose indicator is under vp, or -1
    void setHoverLine(int line);
    void applyFoldPlaceholder(int line, LineDecoration& d) const;
    FoldOptions m_foldOptions;
    QList<FoldRange> m_folds; // sorted by line; may nest
    int m_hoverLine = -1;
    qsizetype m_dropPos = -1; // drop caret while dragging over the text
    int m_clipFrom = 0;  // setVisibleLineRange()
    int m_clipTo = -1;
    QString m_hoverLink; // linkHovered(): "kind:from:target" of the link under the mouse
    void updateHoveredLink(const QPoint& viewportPos);
    QList<std::pair<int, DecorationLayer>> m_layers;
    int m_nextLayerId = 1;
};

} // namespace oc
