#pragma once

#include <QList>
#include <QString>

#include <functional>

class QMimeData;

namespace oc {

class EditorView;

// Extension points for editor behaviour (built-in plugins such as Greentext Guard, auto-pairing).
// Handlers run in registration order; returning true consumes the event.
class EditorHooks {
public:
    // Typed text about to replace [from, to) of the main selection (like CodeMirror's inputHandler).
    using InputHandler = std::function<bool(EditorView& view, qsizetype from, qsizetype to, const QString& text)>;
    // Clipboard/drop data about to be pasted (like Obsidian's "editor-paste" event with preventDefault).
    using PasteHandler = std::function<bool(EditorView& view, const QMimeData& data)>;

    static EditorHooks& global();

    int addInputHandler(InputHandler h);
    int addPasteHandler(PasteHandler h);
    void remove(int id);

    bool runInput(EditorView& view, qsizetype from, qsizetype to, const QString& text) const;
    bool runPaste(EditorView& view, const QMimeData& data) const;

private:
    struct Entry {
        int id;
        InputHandler input;
        PasteHandler paste;
    };
    QList<Entry> m_entries;
    int m_next = 1;
};

} // namespace oc
