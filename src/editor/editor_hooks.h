#pragma once

#include <QList>
#include <QString>

#include <functional>

class QKeyEvent;
class QMimeData;

namespace oc {

class EditorView;

// Extension points for editor behaviour (built-in plugins such as Greentext Guard, auto-pairing,
// vim mode). Handlers run in registration order; returning true consumes the event.
class EditorHooks {
public:
    // Typed text about to replace [from, to) of the main selection (like CodeMirror's inputHandler).
    using InputHandler = std::function<bool(EditorView& view, qsizetype from, qsizetype to, const QString& text)>;
    // Clipboard/drop data about to be pasted (like Obsidian's "editor-paste" event with preventDefault).
    using PasteHandler = std::function<bool(EditorView& view, const QMimeData& data)>;
    // Data dropped at document position `pos` (Obsidian's "editor-drop"); the view has moved the cursor there.
    using DropHandler = std::function<bool(EditorView& view, const QMimeData& data, qsizetype pos)>;
    // A key event for the view, before any of its own key handling (like a highest-precedence
    // CodeMirror keydown handler; vim mode). Called with QEvent::ShortcutOverride, which is only a
    // question — return true (without side effects) if the matching KeyPress will be consumed, and
    // the view then accepts the override so no application shortcut takes the key — and with
    // QEvent::KeyPress: handle it and return true to consume it. Unconsumed keys continue to the
    // view's own handling (typing still goes through the input handlers above). The application's
    // HotkeyDispatcher (an application event filter) still sees keys first, like Obsidian's
    // capture-phase keymap.
    using KeyHandler = std::function<bool(EditorView& view, QKeyEvent* event)>;

    static EditorHooks& global();

    int addInputHandler(InputHandler h);
    int addPasteHandler(PasteHandler h);
    int addDropHandler(DropHandler h);
    int addKeyHandler(KeyHandler h);
    void remove(int id);

    bool runInput(EditorView& view, qsizetype from, qsizetype to, const QString& text) const;
    bool runPaste(EditorView& view, const QMimeData& data) const;
    bool runDrop(EditorView& view, const QMimeData& data, qsizetype pos) const;
    bool runKey(EditorView& view, QKeyEvent* event) const;

private:
    struct Entry {
        int id;
        InputHandler input;
        PasteHandler paste;
        DropHandler drop;
        KeyHandler key;
    };
    QList<Entry> m_entries;
    int m_next = 1;
};

} // namespace oc
