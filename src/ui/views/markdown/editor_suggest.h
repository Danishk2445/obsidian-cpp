#pragma once

#include "ui/overlay/suggest_modal.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace oc {

class EditorView;
class LinkSuggestProvider;
class MarkdownView;
class OverlayLayer;
struct DocUpdate;
struct LinkSuggestion;
struct TagSuggestion;

// Obsidian's .suggestion-container: a bordered, rounded box in the window's overlay layer holding a
// suggestion list and an optional instructions row, placed under (or, without room, above) a text range.
class SuggestPopup : public QWidget {
    Q_OBJECT
public:
    explicit SuggestPopup(OverlayLayer* layer);
    SuggestionList* list() const { return m_list; }
    void setInstructions(const QList<SuggestModal::Instruction>& items);
    // anchor: the text range's rectangle in the layer's (= window's) coordinates
    void showAt(const QRect& anchor);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPointer<OverlayLayer> m_layer;
    SuggestionList* m_list;
    PromptInstructions* m_instructions;
};

// The editor suggests of one markdown view: "[[" links (files, aliases, headings, blocks, display
// text), "#" tags and "[^" footnotes. Typing (or deleting) re-evaluates the triggers after 50 ms and
// opens the popup; cursor moves only update an open popup. While it is open, ↑/↓, Enter, Tab, Esc (and
// "#"/"^" in the link suggest) go to it.
class EditorSuggest : public QObject {
    Q_OBJECT
public:
    explicit EditorSuggest(MarkdownView* view, EditorView* editor);
    ~EditorSuggest() override;

    void documentChanged(); // the view loaded another document
    void trigger(bool force);
    void close();
    bool isOpen() const;
    SuggestPopup* popup() const { return m_popup; }
    // choose a row: key = "" (click), "Enter", "Tab", "#", "^" (link suggest), like Obsidian
    bool choose(int index, const QString& key);
    void setRequireFocus(bool on) { m_requireFocus = on; } // tests (offscreen focus is unreliable)

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class Kind { None, Link, Tag, Footnote };
    struct Context {
        Kind kind = Kind::None;
        int line = -1;
        int start = 0; // columns on `line`
        int end = 0;
        QString query;
    };
    struct FootnoteItem {
        QString id;
        QString text;
        bool create = false;
    };
    void onDocChanged(const DocUpdate& u);
    void show(const Context& ctx);
    void reposition();
    void setOpen(bool open);
    QString sourcePath() const;
    Suggestion renderLink(const LinkSuggestion& s) const;
    void chooseLink(const LinkSuggestion& s, const QString& key);
    void chooseTag(const TagSuggestion& s, bool tab);
    void chooseFootnote(const FootnoteItem& f);
    bool insertBlockIdElsewhere(const LinkSuggestion& s, const QString& id);

    QPointer<MarkdownView> m_view;
    QPointer<EditorView> m_editor;
    QObject* m_editorObject; // identity for eventFilter()
    QPointer<SuggestPopup> m_popup;
    std::unique_ptr<LinkSuggestProvider> m_links;
    QMetaObject::Connection m_docConn;
    QTimer m_debounce;
    bool m_force = false;
    bool m_open = false;
    bool m_requireFocus = true;
    Context m_ctx;
    QList<LinkSuggestion> m_linkItems;
    QList<TagSuggestion> m_tagItems;
    QList<FootnoteItem> m_footnoteItems;
};

} // namespace oc
