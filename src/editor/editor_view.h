#pragma once

#include "core/documents/document.h"
#include "render/layout/doc_layout.h"
#include "text/selection.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QPointer>

#include <memory>

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
    qsizetype positionAt(const QPoint& viewportPos);

    DocLayout& layout() { return m_layout; }
    double contentLeft() const { return m_contentLeft; }
    double contentWidth() const { return m_layout.contentWidth(); }
    double topPadding() const { return m_marginTop; }

    // Extra decoration layers (search highlights, spellcheck, ...), applied in order to every laid-out
    // line after the mode's own decorations. Call refreshDecorations() when a layer's output changes.
    using DecorationLayer = std::function<void(int line, LineDecoration&)>;
    int addDecorationLayer(DecorationLayer layer); // returns an id for removeDecorationLayer()
    void removeDecorationLayer(int id);
    void refreshDecorations(int fromLine = 0, int toLine = -1);

signals:
    void selectionChanged();
    void cursorMoved(int line, int column);
    void modeChanged(Mode mode);
    void scrolled(double top);
    void focusChangedIn(bool focused);
    void contextMenuRequestedAt(const QPoint& globalPos, qsizetype docPos);
    // A rendered link was clicked (live preview / reading). kind: 0 wikilink, 1 embed, 2 markdown link,
    // 3 url, 4 tag. newLeaf: Ctrl held.
    void linkActivated(const QString& target, int kind, bool newLeaf);

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
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void timerEvent(QTimerEvent* e) override;
    bool event(QEvent* e) override;
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
    QList<std::pair<int, DecorationLayer>> m_layers;
    int m_nextLayerId = 1;
};

} // namespace oc
