#include "editor/vim/vim_controller.h"

#include "editor/vim/vim_engine.h"
#include "editor/vim/vim_keys.h"
#include "editor/vim/vim_panel.h"

#include "core/documents/document.h"
#include "editor/editor_hooks.h"
#include "editor/editor_view.h"

#include <QHash>
#include <QInputMethodEvent>
#include <QKeyEvent>

namespace oc {

using namespace vim;

namespace {
QHash<const EditorView*, VimController*>& registry()
{
    static QHash<const EditorView*, VimController*> r;
    return r;
}
} // namespace

VimController::VimController(EditorView* view, QObject* parent)
    : QObject(parent)
    , m_view(view)
{
    registry().insert(view, this);
    m_panel = new VimPanel(view);
    m_keyHook = EditorHooks::global().addKeyHandler([this](EditorView& v, QKeyEvent* e) {
        if (&v != m_view.data())
            return false;
        return handleKeyEvent(e);
    });
    // search matches (".obsidian-search-match-highlight")
    m_layer = view->addDecorationLayer([this](int line, LineDecoration& d) {
        if (!m_engine || !m_view || !m_view->document())
            return;
        const auto& q = m_engine->highlightQuery();
        if (!q || !q->re.isValid())
            return;
        const Text& t = m_view->document()->text();
        if (line >= t.lineCount())
            return;
        const QString s = t.line(line);
        qsizetype pos = 0;
        while (pos <= s.size()) {
            const QRegularExpressionMatch m = q->re.match(s, pos);
            if (!m.hasMatch())
                break;
            if (m.capturedEnd() > m.capturedStart())
                d.marks.append(Mark{int(m.capturedStart()), int(m.capturedEnd()), StyleId::SearchMatch});
            pos = std::max(m.capturedEnd(), m.capturedStart() + 1);
        }
    });
    connect(view, &EditorView::selectionChanged, this, &VimController::onSelectionChanged);
    connect(view, &EditorView::focusChangedIn, this, [this](bool in) {
        if (m_engine)
            m_engine->onFocusChanged(in);
        syncCursorShape();
    });
    connect(view, &EditorView::modeChanged, this, [this] { syncCursorShape(); });
    view->installEventFilter(this);
    bindDocument();
}

VimController::~VimController()
{
    registry().remove(m_view.data());
    EditorHooks::global().remove(m_keyHook);
    disconnect(m_docConn);
    if (m_engine)
        m_engine->leaveVimMode();
    m_engine.reset();
    if (m_view) {
        m_view->removeEventFilter(this);
        m_view->removeDecorationLayer(m_layer);
        m_view->setCursorShape(EditorView::CursorShape::Bar);
    }
    delete m_panel;
}

VimController* VimController::of(const EditorView* view) { return registry().value(view); }

VimPanel* VimController::panel() const { return m_panel; }

void VimController::setCommandRunner(std::function<bool(const QString&)> runner)
{
    m_runner = std::move(runner);
    if (m_engine)
        m_engine->runCommand = m_runner;
}

void VimController::documentChanged() { bindDocument(); }

void VimController::bindDocument()
{
    if (!m_view)
        return;
    Document* d = m_view->document();
    if (d == m_doc && m_engine)
        return;
    disconnect(m_docConn);
    if (m_engine)
        m_engine->leaveVimMode();
    m_doc = d;
    m_engine = std::make_unique<VimEngine>(m_view);
    m_engine->setPanel(m_panel);
    m_engine->runCommand = m_runner;
    m_engine->modeChanged = [this] { syncCursorShape(); };
    m_engine->highlightChanged = [this] {
        if (m_view)
            m_view->refreshDecorations();
    };
    if (m_panel) {
        m_panel->closePrompt();
        m_panel->closeNotification();
    }
    if (!d) {
        syncCursorShape();
        return;
    }
    m_lastHead = m_headBeforeEdit = m_view->selection().main().head;
    m_seenVersion = m_editVersion = d->version();
    m_docConn = connect(d, &Document::changed, this, [this](const DocUpdate& u) {
        if (!m_engine || !m_view)
            return;
        // the head before this edit: the selection handler may already have seen the new one
        const qsizetype headBefore = m_editVersion == m_doc->version() ? m_headBeforeEdit : m_lastHead;
        m_engine->onDocumentChanged(u.tr.changes.desc(), u.tr.changes, headBefore, u.origin == m_view.data(), u.tr.isUserEvent(u"input"));
        m_lastHead = m_view->selection().main().head;
        m_seenVersion = m_doc->version();
    });
    m_engine->enterVimMode();
    syncCursorShape();
}

void VimController::onSelectionChanged()
{
    if (!m_view)
        return;
    bindDocument();
    Document* d = m_view->document();
    const qsizetype head = m_view->selection().main().head;
    bool causedByEdit = false;
    if (d && d->version() != m_seenVersion) {
        causedByEdit = true;
        m_headBeforeEdit = m_lastHead;
        m_editVersion = d->version();
        m_seenVersion = d->version();
    }
    m_lastHead = head;
    if (m_engine && !m_engine->executing())
        m_engine->onSelectionChanged(causedByEdit);
    syncCursorShape();
}

VimController::Mode VimController::mode() const
{
    if (!m_engine)
        return Mode::Normal;
    if (m_engine->insertMode())
        return m_engine->overwrite() ? Mode::Replace : Mode::Insert;
    if (m_engine->visualMode())
        return m_engine->visualLine() ? Mode::VisualLine : m_engine->visualBlock() ? Mode::VisualBlock : Mode::Visual;
    return Mode::Normal;
}

QString VimController::modeName() const { return m_engine ? m_engine->modeName() : QStringLiteral("normal"); }

QString VimController::pendingKeys() const { return m_engine ? m_engine->pendingKeys() : QString(); }

void VimController::syncCursorShape()
{
    if (!m_view)
        return;
    const Mode m = mode();
    const bool block = m != Mode::Insert && m_view->mode() != EditorView::Mode::Reading && m_engine;
    m_view->setCursorShape(block ? EditorView::CursorShape::Block : EditorView::CursorShape::Bar, m == Mode::VisualBlock);
    if (m != m_lastMode) {
        m_lastMode = m;
        emit modeChanged(m);
    }
}

bool VimController::handleKeyEvent(QKeyEvent* e)
{
    if (!m_view || !m_view->document() || m_view->mode() == EditorView::Mode::Reading)
        return false;
    bindDocument();
    if (!m_engine)
        return false;
    const QString key = keyFromEvent(e);
    if (key.isEmpty())
        return false;
    VimEngine& eng = *m_engine;
    const bool overwriteTyping = eng.insertMode() && eng.overwrite() && key.size() == 1;
    if (e->type() == QEvent::ShortcutOverride)
        return eng.wouldHandle(key) || overwriteTyping || (key == u"<C-c>" && !m_view->selection().allEmpty());
    // Ctrl+C with a selection copies (the editor's copy), then leaves visual mode
    if (key == u"<C-c>" && !m_view->selection().allEmpty()) {
        m_view->copy();
        eng.afterCopy();
        syncCursorShape();
        return true;
    }
    bool handled = eng.handleKey(key);
    if (!handled && eng.insertMode() && eng.overwrite()) {
        // replace mode: typed characters overwrite, Backspace only moves left (Obsidian's plugin)
        if (key.size() == 1 || (key.size() == 2 && key.at(0).isHighSurrogate())) {
            eng.overwriteType(e->text().isEmpty() ? key : e->text());
            handled = true;
        } else if (key == u"<BS>") {
            const qsizetype h = m_view->selection().main().head;
            if (h > 0)
                m_view->setSelection(Selection::cursor(h - 1), true, true);
            handled = true;
        }
    }
    syncCursorShape();
    return handled;
}

bool VimController::eventFilter(QObject* obj, QEvent* ev)
{
    // IME commits outside insert mode are keys (like Obsidian's compositionend handling)
    if (obj == m_view && ev->type() == QEvent::InputMethod && m_engine && m_view && m_view->mode() != EditorView::Mode::Reading
        && !m_engine->insertMode()) {
        auto* ime = static_cast<QInputMethodEvent*>(ev);
        const QString commit = ime->commitString();
        for (const QString& k : splitKeys(commit))
            m_engine->handleKey(k);
        syncCursorShape();
        ev->accept();
        return true;
    }
    return QObject::eventFilter(obj, ev);
}

void VimController::feedKeys(const QString& keys)
{
    bindDocument();
    if (!m_engine)
        return;
    for (const QString& k : splitKeys(keys)) {
        if (!m_engine->handleKey(k) && m_engine->insertMode() && m_view) {
            // typing (without the editor's input handlers)
            if (m_engine->overwrite() && k.size() == 1) {
                m_engine->overwriteType(k);
            } else if (k == u"<CR>") {
                m_view->insertNewline();
            } else if (k == u"<BS>") {
                m_view->deleteBackward();
            } else if (!k.startsWith(u'<') || k.size() == 1) {
                m_view->insertText(k);
            }
        }
    }
    syncCursorShape();
}

void VimController::executeEx(const QString& commandLine)
{
    bindDocument();
    if (m_engine)
        m_engine->handleEx(commandLine);
    syncCursorShape();
}

void VimController::resetGlobalState() { VimGlobal::reset(); }

void VimController::setClipboard(const Clipboard& clipboard) { VimGlobal::instance().registerController.setClipboard(clipboard); }

} // namespace oc
