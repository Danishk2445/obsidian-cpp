#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class QKeyEvent;

namespace oc {

class Document;
class EditorView;

namespace vim {
class VimEngine;
class VimPanel;
struct Clipboard;
} // namespace vim

// Vim key bindings for one EditorView (Obsidian's "Vim key bindings", app.json "vimMode"): the
// behaviour of the CodeMirror-vim build Obsidian ships — normal, insert, replace, visual,
// visual-line and visual-block modes; motions, operators, text objects, counts, registers ("a-"z,
// "A-"Z, "0-"9, "-, "", "_, and "+ / "* for the system clipboard), marks, "." repeat, macros, search
// and ex commands. The ":" and "/" command line is a panel docked at the bottom of the editor; the
// cursor is a block outside insert mode.
//
// Keys reach it through an EditorHooks key handler, before the editor's own handling; typing in
// insert mode still goes through the editor's input path (auto-pairs, list continuation, IME,
// Greentext Guard, editor suggests). Every vim command and every insert session is one undo step
// of the document's History. Obsidian's hotkeys (the application's HotkeyDispatcher) keep
// precedence over vim, as in Obsidian.
//
// Create one per editor while vim mode is on; deleting it restores the normal editor.
class VimController : public QObject {
    Q_OBJECT
public:
    enum class Mode { Normal, Insert, Replace, Visual, VisualLine, VisualBlock };

    explicit VimController(EditorView* view, QObject* parent = nullptr);
    ~VimController() override;

    static VimController* of(const EditorView* view); // the controller attached to a view, if any

    EditorView* view() const { return m_view; }
    Mode mode() const;
    QString modeName() const;  // "normal", "insert", "replace", "visual", "visual-line", "visual-block"
    QString pendingKeys() const; // a command being typed ("d2", "\"a", "g")
    vim::VimPanel* panel() const;

    // How ":w", ":q", ":wq" and ":x" run Obsidian commands ("editor:save-file", "workspace:close").
    // Without a runner ":w" saves the document itself.
    void setCommandRunner(std::function<bool(const QString& commandId)> runner);

    // The view's document was replaced (MarkdownView::loadFile): vim starts over in normal mode.
    void documentChanged();

    // Feed keys in vim notation as if typed ("d2w", "ci(<Esc>"); for scripts and tests.
    void feedKeys(const QString& keys);
    // Run an ex command line (without the leading ':').
    void executeEx(const QString& commandLine);

    // Registers / search / macros are shared by all editors (like vim.js's global state).
    static void resetGlobalState();              // tests
    static void setClipboard(const vim::Clipboard& clipboard); // tests: a fake "+ register

signals:
    void modeChanged(oc::VimController::Mode mode);

private:
    bool handleKeyEvent(QKeyEvent* event);
    void bindDocument();
    void syncCursorShape();
    void onSelectionChanged();
    bool eventFilter(QObject* obj, QEvent* ev) override;

    QPointer<EditorView> m_view;
    std::unique_ptr<vim::VimEngine> m_engine;
    vim::VimPanel* m_panel = nullptr;
    QPointer<Document> m_doc;
    QMetaObject::Connection m_docConn;
    int m_keyHook = 0;
    int m_layer = -1;
    std::function<bool(const QString&)> m_runner;
    // the editor's main head before the last edit (insert-mode recording for ".")
    qsizetype m_lastHead = 0;
    qsizetype m_headBeforeEdit = 0;
    quint64 m_seenVersion = 0;
    quint64 m_editVersion = 0;
    Mode m_lastMode = Mode::Normal;
};

} // namespace oc
