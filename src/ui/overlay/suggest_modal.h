#pragma once

#include <QAbstractScrollArea>
#include <QList>
#include <QPointer>
#include <QVariant>
#include <QWidget>

#include <climits>
#include <functional>

class QLineEdit;

namespace oc {

class OverlayLayer;
class SuggestionList;

// One row of a suggestion list (Obsidian's .suggestion-item, "mod-complex" when it has more than a title).
struct Suggestion {
    QString title;
    QList<QPair<int, int>> titleMatches; // [start, end) ranges shown bold (fuzzy match highlights)
    QString prefix;                      // shown before the title as "prefix: " (command palette)
    QString note;                        // second line, small and muted (e.g. the folder of a file)
    QList<QPair<int, int>> noteMatches;
    QStringList hotkeys;                 // right-aligned key chips
    QString icon;                        // left flair icon (Lucide name)
    QString auxIcon;                     // right flair icon
    QString auxText;                     // right-aligned muted text
    bool downranked = false;             // muted (e.g. unresolved / less relevant)
    QVariant data;
};

// Obsidian's SuggestModal ("prompt"): an input over a list of suggestions, 80 px below the top of
// the window, over a dimmed background. The provider is asked for suggestions on every input
// change; ↑/↓, PageUp/PageDown, Home/End, Ctrl+P/N move the selection (wrapping), Enter chooses
// (modifiers are passed on: Mod+Enter usually means "new tab"), Escape or a click outside closes.
class SuggestModal : public QWidget {
    Q_OBJECT
public:
    using Provider = std::function<QList<Suggestion>(const QString& query)>;
    // Extra keys (return true if handled), e.g. Tab in the quick switcher.
    using KeyHandler = std::function<bool(QKeyEvent* e)>;
    struct Instruction {
        QString command; // "↑↓", "↵", "ctrl ↵", "esc"
        QString purpose; // "to navigate", ...
    };

    explicit SuggestModal(OverlayLayer* layer);
    ~SuggestModal() override;

    void setPlaceholder(const QString& text);
    void setInstructions(const QList<Instruction>& instructions);
    void setProvider(Provider provider) { m_provider = std::move(provider); }
    void setKeyHandler(KeyHandler handler) { m_keyHandler = std::move(handler); }
    void setEmptyText(const QString& text) { m_emptyText = text; }
    void setLimit(int limit) { m_limit = limit; }

    void open();                        // shows, focuses the input, fills the list
    void close();                       // emits closed(); the modal deletes itself later
    bool isOpen() const { return m_open; }

    QString query() const;
    void setQuery(const QString& query); // updates suggestions
    void refresh();                      // re-query with the current input
    QLineEdit* input() const { return m_input; }

    const QList<Suggestion>& suggestions() const;
    int selectedIndex() const;
    void setSelectedIndex(int index);
    void chooseSelected(Qt::KeyboardModifiers mods = Qt::NoModifier);

    QRect promptGeometry() const;

signals:
    void chosen(const oc::Suggestion& suggestion, Qt::KeyboardModifiers modifiers);
    void selectionChanged(int index);
    void closed();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void relayout();
    void updateSuggestions();

    QPointer<OverlayLayer> m_layer;
    QWidget* m_prompt = nullptr;
    QLineEdit* m_input = nullptr;
    QWidget* m_clear = nullptr;
    SuggestionList* m_list = nullptr;
    QWidget* m_instructions = nullptr;
    QList<Instruction> m_instructionList;
    Provider m_provider;
    KeyHandler m_keyHandler;
    QString m_emptyText;
    int m_limit = 100;
    bool m_open = false;
};

// Obsidian's .prompt-instructions row ("↑↓ to navigate  ↵ to open ..."), wrapping to the width.
class PromptInstructions : public QWidget {
public:
    explicit PromptInstructions(QWidget* parent = nullptr);
    void setItems(const QList<SuggestModal::Instruction>& items);
    const QList<SuggestModal::Instruction>& items() const { return m_items; }
    void setCentered(bool centered) { m_centered = centered; update(); }
    int naturalWidth() const; // everything on one row, incl. padding
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QList<QList<int>> layoutRows(int width) const;
    QList<SuggestModal::Instruction> m_items;
    bool m_centered = true;
};

// The painted, virtualized list inside a SuggestModal (also used by editor suggest popups).
class SuggestionList : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit SuggestionList(QWidget* parent = nullptr);

    void setSuggestions(QList<Suggestion> items, const QString& emptyText = {});
    const QList<Suggestion>& suggestions() const { return m_items; }
    int selectedIndex() const { return m_selected; }
    void setSelectedIndex(int index, bool scroll = true); // wraps around
    void clearSelection();                                // no row selected (selectedIndex() == -1)
    int preferredWidth() const;                           // widest row's natural width (incl. padding)
    void pageUp();
    void pageDown();
    int contentHeight() const; // total height of all rows incl. padding
    int contentHeightForWidth(int width, int cap = INT_MAX) const; // the same, laid out at `width`
    void setPadding(int px) { m_pad = px; relayoutRows(); }
    int indexAt(const QPoint& viewportPos) const;
    QRect rowRect(int index) const; // viewport coordinates

signals:
    void selectedChanged(int index);
    void activated(int index, Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    struct Row {
        int y = 0;
        int height = 0;
    };
    void relayoutRows();
    int rowHeight(const Suggestion& s, int width) const;
    void ensureVisible(int index);

    QList<Suggestion> m_items;
    QList<Row> m_rows;
    QString m_empty;
    int m_selected = 0;
    int m_pad = 12;
    QPoint m_lastMouse{-1, -1};
};

} // namespace oc

Q_DECLARE_METATYPE(oc::Suggestion)
