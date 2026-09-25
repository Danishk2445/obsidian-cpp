// The vim command dispatcher: key handling, the input state machine, evalInput (motions +
// operators), modes, selections, marks, dot repeat and macros. Behaviour follows the vim.js
// Obsidian 1.13 ships; comments name the vim.js functions ported.
#include "editor/vim/vim_engine.h"

#include "editor/vim/vim_keys.h"
#include "editor/vim/vim_panel.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc::vim {

const QList<KeyCommand>& defaultKeymap(); // vim_keymap.cpp

// ---------------------------------------------------------------------------------------------
// small state types
// ---------------------------------------------------------------------------------------------
void InputState::pushRepeatDigit(const QString& n)
{
    if (op == Operator::None)
        prefixRepeat += n;
    else
        motionRepeat += n;
}

int InputState::getRepeat() const
{
    constexpr qint64 kMax = 100000; // vim.js has no limit; keep loops finite
    qint64 repeat = 0;
    if (!prefixRepeat.isEmpty() || !motionRepeat.isEmpty()) {
        repeat = 1;
        if (!prefixRepeat.isEmpty())
            repeat *= std::min(prefixRepeat.left(9).toLongLong(), kMax);
        if (!motionRepeat.isEmpty())
            repeat *= std::min(motionRepeat.left(9).toLongLong(), kMax);
    }
    return int(std::min(repeat, kMax));
}

QString HistoryController::nextMatch(const QString& input, bool up)
{
    const int dir = up ? -1 : 1;
    if (!initialPrefix)
        initialPrefix = input;
    int i = iterator + dir;
    for (; up ? i >= 0 : i < historyBuffer.size(); i += dir) {
        const QString& element = historyBuffer.at(i);
        for (int j = 0; j <= element.size(); ++j) {
            if (*initialPrefix == element.left(j)) {
                iterator = i;
                return element;
            }
        }
    }
    if (i >= historyBuffer.size()) {
        iterator = int(historyBuffer.size());
        return *initialPrefix;
    }
    return input;
}

void HistoryController::pushInput(const QString& input)
{
    historyBuffer.removeAll(input);
    if (!input.isEmpty())
        historyBuffer.push_back(input);
}

void HistoryController::reset()
{
    initialPrefix.reset();
    iterator = int(historyBuffer.size());
}

VimGlobal& VimGlobal::instance()
{
    static VimGlobal g;
    return g;
}

void VimGlobal::reset()
{
    Clipboard cb = instance().registerController.clipboard();
    instance() = VimGlobal();
    instance().registerController.setClipboard(std::move(cb));
}

namespace {
VimGlobal& G() { return VimGlobal::instance(); }

bool isWhiteSpaceString(const QString& s)
{
    for (QChar c : s)
        if (!c.isSpace())
            return false;
    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------------------------
VimEngine::VimEngine(EditorView* view)
    : m_view(view)
    , m_inputState(std::make_shared<InputState>())
{
}

VimEngine::~VimEngine() = default;

void VimEngine::enterVimMode()
{
    // vim.js enterVimMode: normal mode (the cursor stays where it is); a selection is visual mode
    m_busy++;
    if (somethingSelected())
        handleExternalSelection();
    m_busy--;
    signalModeChange();
}

void VimEngine::leaveVimMode()
{
    if (m_insertEditGroup && doc())
        doc()->history().endGroup();
    m_insertEditGroup = false;
    m_insertMode = false;
    m_visualMode = m_visualLine = m_visualBlock = false;
    m_overwrite = false;
    m_highlight.reset();
}

// ---------------------------------------------------------------------------------------------
// document access
// ---------------------------------------------------------------------------------------------
Document* VimEngine::doc() const { return m_view ? m_view->document() : nullptr; }
const Text& VimEngine::text() const { return doc()->text(); }
int VimEngine::lineCount() const { return doc() ? text().lineCount() : 1; }

QString VimEngine::getLine(int line) const
{
    if (!doc() || line < 0 || line >= lineCount())
        return {};
    return text().line(line);
}

int VimEngine::lineLength(int line) const
{
    if (!doc() || line < 0 || line >= lineCount())
        return 0;
    return text().lineLength(line);
}

qsizetype VimEngine::indexFromPos(Pos p) const
{
    const Text& t = text();
    if (p.line < 0)
        return 0;
    if (p.line >= t.lineCount())
        return t.length();
    const qsizetype ls = t.lineStart(p.line);
    return std::min<qsizetype>(ls + std::max(0, p.ch), t.lineEnd(p.line));
}

Pos VimEngine::posFromIndex(qsizetype i) const
{
    const Text& t = text();
    i = std::clamp<qsizetype>(i, 0, t.length());
    const int line = t.lineAt(i);
    return Pos(line, int(i - t.lineStart(line)));
}

Pos VimEngine::clipPos(Pos p) const
{
    if (p.line < 0)
        return Pos(0, 0);
    if (p.line > lastLine())
        return Pos(lastLine(), lineLength(lastLine()));
    return Pos(p.line, std::clamp(p.ch, 0, lineLength(p.line)));
}

QString VimEngine::getRange(Pos a, Pos b) const
{
    qsizetype x = indexFromPos(a), y = indexFromPos(b);
    if (x > y)
        std::swap(x, y);
    return text().slice(x, y);
}

Pos VimEngine::getCursor(const char* which) const
{
    const SelRange& r = m_view->selection().main();
    qsizetype o = r.head;
    if (qstrcmp(which, "anchor") == 0)
        o = r.anchor;
    else if (qstrcmp(which, "start") == 0)
        o = r.from();
    else if (qstrcmp(which, "end") == 0)
        o = r.to();
    return posFromIndex(o);
}

void VimEngine::setCursor(Pos p)
{
    m_busy++;
    m_view->setSelection(Selection::cursor(indexFromPos(p)), false, false);
    m_busy--;
}

void VimEngine::setSelection(Pos anchor, Pos head)
{
    m_busy++;
    m_view->setSelection(Selection::single(indexFromPos(anchor), indexFromPos(head)), false, false);
    m_busy--;
}

void VimEngine::setSelections(const QList<Range>& ranges, int primary)
{
    if (ranges.isEmpty())
        return;
    QList<SelRange> out;
    for (const Range& r : ranges)
        out.push_back(SelRange(indexFromPos(r.anchor), indexFromPos(r.head)));
    m_busy++;
    m_view->setSelection(Selection::create(out, std::clamp(primary, 0, int(out.size()) - 1)), false, false);
    m_busy--;
}

QList<Range> VimEngine::listSelections() const
{
    QList<Range> out;
    for (const SelRange& r : m_view->selection().ranges())
        out.push_back(Range{posFromIndex(r.anchor), posFromIndex(r.head)});
    return out;
}

QStringList VimEngine::getSelections() const
{
    QStringList out;
    for (const SelRange& r : m_view->selection().ranges())
        out << text().slice(r.from(), r.to());
    return out;
}

QString VimEngine::getSelection() const { return getSelections().join(QLatin1Char('\n')); }

bool VimEngine::somethingSelected() const { return !m_view->selection().allEmpty(); }

bool VimEngine::readOnly() const { return !doc() || m_view->isReadOnly(); }

void VimEngine::applyChanges(const QList<ChangeSpec>& changes, std::optional<Selection> selection, const QString& userEvent)
{
    if (readOnly() || changes.isEmpty())
        return;
    Document* d = doc();
    // drop no-op replacements (never rewrite unchanged text)
    QList<ChangeSpec> specs;
    for (const ChangeSpec& c : changes)
        if (c.from != c.to || !c.insert.isEmpty())
            if (c.to - c.from != c.insert.size() || d->text().slice(c.from, c.to) != c.insert)
                specs.push_back(c);
    if (specs.isEmpty()) {
        if (selection) {
            m_busy++;
            m_view->setSelection(*selection, false, false);
            m_busy--;
        }
        return;
    }
    Transaction tr;
    tr.changes = ChangeSet::of(specs, d->text().length());
    tr.selection = selection;
    tr.userEvent = userEvent;
    tr.scrollIntoView = false;
    m_busy++;
    d->dispatch(tr, m_view, m_view->selection());
    m_busy--;
}

void VimEngine::replaceRange(const QString& text, Pos from, std::optional<Pos> to)
{
    qsizetype a = indexFromPos(from), b = to ? indexFromPos(*to) : a;
    if (a > b)
        std::swap(a, b);
    applyChanges({ChangeSpec{a, b, text}});
}

void VimEngine::replaceSelection(const QString& t)
{
    // CodeMirror's state.replaceSelection: every range replaced, cursors after the insertions
    const Selection& sel = m_view->selection();
    QList<ChangeSpec> specs;
    for (const SelRange& r : sel.ranges())
        specs.push_back({r.from(), r.to(), t});
    const ChangeSet cs = ChangeSet::of(specs, text().length());
    QList<SelRange> after;
    for (const SelRange& r : sel.ranges())
        after.push_back(SelRange::cursor(cs.mapPos(r.to(), 1)));
    applyChanges(specs, Selection::create(after, sel.mainIndex()));
}

void VimEngine::replaceSelections(const QStringList& texts)
{
    const Selection& sel = m_view->selection();
    QList<ChangeSpec> specs;
    for (int i = 0; i < sel.size(); ++i)
        specs.push_back({sel.ranges()[i].from(), sel.ranges()[i].to(), texts.value(i)});
    applyChanges(specs);
}

// ---------------------------------------------------------------------------------------------
// bookmarks (CodeMirror setBookmark) and marks
// ---------------------------------------------------------------------------------------------
int VimEngine::setBookmark(Pos p, bool insertLeft)
{
    const int id = m_nextBookmark++;
    m_bookmarks.insert(id, Bookmark{indexFromPos(p), insertLeft ? 1 : -1});
    return id;
}

std::optional<Pos> VimEngine::findBookmark(int id) const
{
    auto it = m_bookmarks.constFind(id);
    if (it == m_bookmarks.cend() || it->offset < 0 || !doc() || it->offset > text().length())
        return std::nullopt;
    return posFromIndex(it->offset);
}

void VimEngine::clearBookmark(int id) { m_bookmarks.remove(id); }

void VimEngine::updateMark(const QString& name, Pos pos)
{
    static const QRegularExpression latin(QStringLiteral("^\\w$"));
    if (name != u"<" && name != u">" && !latin.match(name).hasMatch())
        return;
    if (auto it = m_marks.find(name); it != m_marks.end())
        clearBookmark(*it);
    m_marks.insert(name, setBookmark(pos));
}

std::optional<Pos> VimEngine::markPos(const QString& name) const
{
    auto it = m_marks.constFind(name);
    return it == m_marks.cend() ? std::nullopt : findBookmark(*it);
}

std::optional<Pos> VimEngine::getMarkPos(const QString& name) const
{
    if (name == u"'" || name == u"`") {
        auto self = const_cast<VimEngine*>(this);
        if (auto p = self->jumpListFind(-1))
            return p;
        return Pos(0, 0);
    }
    if (name == u".")
        return getLastEditPos();
    return markPos(name);
}

std::optional<Pos> VimEngine::getLastEditPos() const
{
    if (!m_hasLastChangeEnd)
        return std::nullopt;
    return posFromIndex(m_lastChangeEnd);
}

// jump list (vim.js createCircularJumpList; per editor here)
void VimEngine::jumpListAdd(Pos oldCur, Pos newCur)
{
    auto& jl = m_jumpList;
    auto useNextSlot = [&](Pos cursor) {
        const int next = ++jl.pointer % JumpList::size;
        if (jl.buffer[next] >= 0)
            clearBookmark(jl.buffer[next]);
        jl.buffer[next] = setBookmark(cursor);
    };
    const int current = ((jl.pointer % JumpList::size) + JumpList::size) % JumpList::size;
    const int curMark = jl.pointer >= 0 ? jl.buffer[current] : -1;
    if (curMark >= 0) {
        const auto markPos = findBookmark(curMark);
        if (markPos && !(*markPos == oldCur))
            useNextSlot(oldCur);
    } else {
        useNextSlot(oldCur);
    }
    useNextSlot(newCur);
    jl.head = jl.pointer;
    jl.tail = std::max(0, jl.pointer - JumpList::size + 1);
}

int VimEngine::jumpListMove(int offset)
{
    auto& jl = m_jumpList;
    jl.pointer += offset;
    if (jl.pointer > jl.head)
        jl.pointer = jl.head;
    else if (jl.pointer < jl.tail)
        jl.pointer = jl.tail;
    auto at = [&](int p) { return jl.buffer[((p % JumpList::size) + JumpList::size) % JumpList::size]; };
    int mark = at(jl.pointer);
    if (mark >= 0 && !findBookmark(mark)) {
        const int inc = offset > 0 ? 1 : -1;
        const Pos oldCur = getCursor();
        do {
            jl.pointer += inc;
            mark = at(jl.pointer);
            if (mark >= 0) {
                const auto newCur = findBookmark(mark);
                if (newCur && !(*newCur == oldCur))
                    break;
            }
        } while (jl.pointer < jl.head && jl.pointer > jl.tail);
    }
    return mark;
}

std::optional<Pos> VimEngine::jumpListFind(int offset)
{
    const int old = m_jumpList.pointer;
    const int mark = jumpListMove(offset);
    m_jumpList.pointer = old;
    return mark >= 0 ? findBookmark(mark) : std::nullopt;
}

void VimEngine::recordJumpPosition(Pos oldCur, Pos newCur)
{
    if (!(oldCur == newCur))
        jumpListAdd(oldCur, newCur);
}

// ---------------------------------------------------------------------------------------------
// editor events
// ---------------------------------------------------------------------------------------------
void VimEngine::onDocumentChanged(const ChangeDesc& changes, const ChangeSet& set, qsizetype headBefore, bool fromThisView, bool userInput)
{
    for (auto it = m_bookmarks.begin(); it != m_bookmarks.end(); ++it) {
        if (it->offset < 0)
            continue;
        const auto mapped = changes.mapPosOpt(std::min(it->offset, changes.length()), it->assoc, MapMode::TrackDel);
        it->offset = mapped ? *mapped : -1;
    }
    // getLastEditEnd: the end of the last changed range; undo/redo: where the change starts
    changes.iterChangedRanges([&](qsizetype, qsizetype, qsizetype fromB, qsizetype toB) {
        m_lastChangeEnd = toB;
        m_hasLastChangeEnd = true;
        if (m_trackChangeStart && (!m_changeStart || fromB < *m_changeStart))
            m_changeStart = fromB;
    });
    // insert mode: record what was typed (vim.js onChange), relative to the cursor
    Q_UNUSED(userInput);
    if (!m_insertMode || !m_recordingInsert || !fromThisView || G().macroModeState.isPlaying)
        return;
    InsertChanges& last = G().macroModeState.lastInsertModeChanges;
    set.iterChanges([&](qsizetype fromA, qsizetype toA, qsizetype fromB, qsizetype, const QString& inserted) {
        if (headBefore < fromA || headBefore > toA)
            return; // not at this cursor (another cursor of a multi-cursor insert, or elsewhere)
        if (last.maybeReset) {
            last.changes.clear();
            last.maybeReset = false;
        }
        InsertOp op;
        op.delBefore = int(headBefore - fromA);
        op.delAfter = int(toA - headBefore);
        op.text = inserted;
        const qsizetype headAfter = m_view->selection().main().head;
        op.cursorBack = int(std::clamp<qsizetype>(fromB + inserted.size() - headAfter, 0, inserted.size()));
        op.overwrite = m_overwrite;
        last.changes.push_back(op);
    }, true);
}

void VimEngine::onSelectionChanged(bool causedByEdit)
{
    if (m_busy > 0)
        return;
    if (m_insertMode) {
        // vim.js onCursorActivity: a cursor move outside an edit starts a new insert change
        InsertChanges& last = G().macroModeState.lastInsertModeChanges;
        if (G().macroModeState.isPlaying || causedByEdit)
            return;
        last.maybeReset = true;
        if (m_insertEnd >= 0)
            clearBookmark(m_insertEnd);
        m_insertEnd = setBookmark(getCursor(), true);
        // vim: moving the cursor in insert mode starts a new undo step
        if (m_insertEditGroup && doc()) {
            doc()->history().endGroup();
            doc()->history().beginGroup();
        }
        return;
    }
    handleExternalSelection();
}

void VimEngine::onFocusChanged(bool focused)
{
    Q_UNUSED(focused);
    // another view may edit the document meanwhile: don't let its edits join the insert session
    if (m_insertEditGroup && doc() && m_busy == 0) {
        doc()->history().endGroup();
        doc()->history().beginGroup();
    }
}

void VimEngine::handleExternalSelection()
{
    // vim.js handleExternalSelection: mouse selections are visual characterwise
    const Pos anchor = getCursor("anchor");
    const Pos head = getCursor("head");
    if (m_visualMode && !somethingSelected()) {
        exitVisualMode(false);
    } else if (!m_visualMode && !m_insertMode && somethingSelected()) {
        m_visualMode = true;
        m_visualLine = false;
        m_visualBlock = false;
        signalModeChange();
    }
    if (m_visualMode) {
        const bool fwd = !cursorIsBefore(head, anchor);
        m_sel.head = offsetCursor(head, 0, fwd ? -1 : 0);
        m_sel.anchor = offsetCursor(anchor, 0, fwd ? 0 : -1);
        updateMark(QStringLiteral("<"), cursorMin(head, anchor));
        updateMark(QStringLiteral(">"), cursorMax(head, anchor));
    } else if (!m_insertMode) {
        m_lastHPos = getCursor().ch;
    }
}

void VimEngine::signalModeChange()
{
    if (modeChanged)
        modeChanged();
}

QString VimEngine::modeName() const
{
    if (m_insertMode)
        return m_overwrite ? QStringLiteral("replace") : QStringLiteral("insert");
    if (m_visualMode)
        return m_visualLine ? QStringLiteral("visual-line") : m_visualBlock ? QStringLiteral("visual-block") : QStringLiteral("visual");
    return QStringLiteral("normal");
}

bool VimEngine::hasPendingInput() const
{
    const InputState& s = *m_inputState;
    return s.op != Operator::None || !s.prefixRepeat.isEmpty() || !s.motionRepeat.isEmpty() || !s.keyBuffer.isEmpty()
           || !s.registerName.isEmpty();
}

QString VimEngine::pendingKeys() const { return m_inputState->keyBuffer.join(QString()); }

// ---------------------------------------------------------------------------------------------
// undo grouping: one undo step per vim command (and per insert session)
// ---------------------------------------------------------------------------------------------
void VimEngine::beginEdit()
{
    if (m_editDepth++ == 0 && doc())
        doc()->history().beginGroup();
}

void VimEngine::endEdit()
{
    if (--m_editDepth == 0 && doc())
        doc()->history().endGroup();
}

// ---------------------------------------------------------------------------------------------
// key handling (vim.js findKey)
// ---------------------------------------------------------------------------------------------
const QList<KeyCommand>& VimEngine::keymap() const { return defaultKeymap(); }

namespace {
enum class CmdMatch { No, Partial, Full };
CmdMatch commandMatch(const QString& pressed, const QString& mapped)
{
    const bool isLastCharacter = mapped.endsWith(QLatin1String("<character>"));
    const bool isLastRegister = mapped.endsWith(QLatin1String("<register>"));
    if (isLastCharacter || isLastRegister) {
        const qsizetype prefixLen = mapped.size() - (isLastCharacter ? 11 : 10);
        const QString pressedPrefix = pressed.left(prefixLen);
        const QString mappedPrefix = mapped.left(prefixLen);
        if (pressedPrefix == mappedPrefix && pressed.size() > prefixLen)
            return CmdMatch::Full;
        return mappedPrefix.startsWith(pressedPrefix) ? CmdMatch::Partial : CmdMatch::No;
    }
    if (pressed == mapped)
        return CmdMatch::Full;
    return mapped.startsWith(pressed) ? CmdMatch::Partial : CmdMatch::No;
}

QString lastChar(const QString& keys)
{
    static const QRegularExpression special(QStringLiteral("^.*(<[^>]+>)$"));
    const QRegularExpressionMatch m = special.match(keys);
    QString c = m.hasMatch() ? m.captured(1) : keys.right(1);
    if (!m.hasMatch() && keys.size() >= 2 && keys.back().isLowSurrogate() && keys.at(keys.size() - 2).isHighSurrogate())
        c = keys.right(2);
    if (c.size() > 1 && !(c.size() == 2 && c.at(0).isHighSurrogate())) {
        if (c == u"<CR>" || c == u"<S-CR>")
            c = QStringLiteral("\n");
        else if (c == u"<Space>" || c == u"<S-Space>")
            c = QStringLiteral(" ");
        else
            c.clear();
    }
    return c;
}
} // namespace

bool VimEngine::commandMatches(const QString& keys, const QString& contextIn, const InputState& st, QList<const KeyCommand*>* partial,
                               QList<const KeyCommand*>* full) const
{
    const QString context = st.op != Operator::None ? QStringLiteral("operatorPending") : contextIn;
    auto scan = [&](const QList<KeyCommand>& map) {
        for (const KeyCommand& command : map) {
            if (context == u"insert" && command.context != u"insert")
                continue;
            if (!command.context.isEmpty() && command.context != context)
                continue;
            if (st.op != Operator::None && command.type == CmdType::Action)
                continue;
            const CmdMatch m = commandMatch(keys, command.keys);
            if (m == CmdMatch::Partial)
                partial->push_back(&command);
            else if (m == CmdMatch::Full)
                full->push_back(&command);
        }
    };
    if (!m_noremap)
        scan(G().userKeymap);
    scan(keymap());
    return !partial->isEmpty() || !full->isEmpty();
}

VimEngine::Match VimEngine::matchCommand(const QString& keys, const QString& context, InputState& inputState) const
{
    QList<const KeyCommand*> partial, full;
    commandMatches(keys, context, inputState, &partial, &full);
    Match r;
    if (full.isEmpty() && partial.isEmpty())
        return r;
    if (full.isEmpty()) {
        r.type = MatchType::Partial;
        r.expectLiteralNext = partial.size() == 1 && partial.first()->keys.endsWith(QLatin1String("<character>"));
        return r;
    }
    const KeyCommand* best = full.first();
    if (best->keys.endsWith(QLatin1String("<character>")) || best->keys.endsWith(QLatin1String("<register>"))) {
        const QString character = lastChar(keys);
        if (character.isEmpty()) {
            r.type = MatchType::Clear;
            return r;
        }
        inputState.selectedCharacter = character;
    }
    r.type = MatchType::Full;
    r.command = best;
    return r;
}

void VimEngine::clearInputState()
{
    m_inputState = std::make_shared<InputState>();
    // "vim-command-done": after <C-o> in insert mode, the next completed (or cancelled) command
    // returns to insert mode — when its input state clears, before it runs, like vim.js
    if (m_insertModeReturnArmed && m_insertModeReturn && !m_visualMode) {
        m_insertModeReturnArmed = false;
        m_insertModeReturn = false;
        if (!m_insertMode) {
            Args a;
            actEnterInsertMode(a);
        }
    }
}

void VimEngine::logKey(const QString& key)
{
    MacroModeState& mm = G().macroModeState;
    if (mm.isPlaying)
        return;
    Register& reg = G().registerController.get(mm.latestRegister);
    reg.pushText(key);
}

bool VimEngine::handleKeyInsertMode(const QString& key, const KeyCommand** command, bool* consumed)
{
    *command = nullptr;
    *consumed = false;
    if (key == u"<Esc>") {
        if (m_visualMode)
            exitVisualMode();
        else
            exitInsertMode();
        clearInputState();
        *consumed = true;
        return true;
    }
    InputState& st = *m_inputState;
    // an insert-mode mapping prefix typed too slowly is plain text (insertModeEscKeysTimeout)
    if (!st.keyBuffer.isEmpty() && m_insertKeyTimer.isValid() && m_insertKeyTimer.elapsed() > G().insertModeEscKeysTimeout)
        clearInputState();
    InputState& s = *m_inputState;
    s.keyBuffer.push_back(key);
    const QString keys = s.keyBuffer.join(QString());
    const bool keysAreChars = key.size() == 1;
    const Match match = matchCommand(keys, QStringLiteral("insert"), s);
    if (match.type == MatchType::None) {
        clearInputState();
        return false;
    }
    if (match.type == MatchType::Partial) {
        m_expectLiteralNext = match.expectLiteralNext;
        m_insertKeyTimer.start();
        if (keysAreChars) {
            const QList<Range> sels = listSelections();
            if (!s.hasChangeQueue || s.changeQueueRemoved.size() != sels.size()) {
                s.hasChangeQueue = true;
                s.changeQueueRemoved = QStringList();
                s.changeQueueInserted.clear();
            }
            s.changeQueueInserted += key;
            for (int i = 0; i < sels.size(); ++i) {
                const Pos from = cursorMin(sels[i].anchor, sels[i].head);
                const Pos to = cursorMax(sels[i].anchor, sels[i].head);
                const QString t = getRange(from, m_overwrite ? offsetCursor(to, 0, 1) : to);
                while (s.changeQueueRemoved.size() <= i)
                    s.changeQueueRemoved.push_back(QString());
                s.changeQueueRemoved[i] += t;
            }
        }
        *consumed = !keysAreChars;
        return *consumed; // characters are typed normally until the mapping completes
    }
    m_expectLiteralNext = false;
    m_insertKeyTimer.invalidate();
    if (match.type == MatchType::Full && s.hasChangeQueue) {
        // take back the characters typed while the mapping was pending
        const QList<Range> sels = listSelections();
        m_busy++;
        for (int i = 0; i < sels.size(); ++i) {
            const Pos here = sels[i].head;
            replaceRange(s.changeQueueRemoved.value(i), offsetCursor(here, 0, -int(s.changeQueueInserted.size())), here);
        }
        m_busy--;
        auto& changes = G().macroModeState.lastInsertModeChanges.changes;
        for (int i = 0; i < int(s.changeQueueInserted.size()) && !changes.isEmpty(); ++i)
            changes.removeLast();
    }
    if (match.type != MatchType::Full) {
        clearInputState();
        return false;
    }
    *command = match.command;
    return true;
}

bool VimEngine::handleKeyNonInsertMode(const QString& key, const QString& origin, const KeyCommand** command, bool* consumed)
{
    *command = nullptr;
    *consumed = false;
    MacroModeState& mm = G().macroModeState;
    if (mm.isRecording) {
        if (key == u"q") {
            mm.isRecording = false;
            if (m_panel)
                m_panel->closeRecording();
            clearInputState();
            *consumed = true;
            return true;
        }
        if (origin != u"mapping")
            logKey(key);
    }
    if (key == u"<Esc>" && (m_visualMode || m_insertMode)) {
        if (m_visualMode)
            exitVisualMode();
        else
            exitInsertMode();
        clearInputState();
        *consumed = true;
        return true;
    }
    InputState& s = *m_inputState;
    s.keyBuffer.push_back(key);
    const QString keys = s.keyBuffer.join(QString());
    static const QRegularExpression countOnly(QStringLiteral("^[1-9]\\d*$"));
    if (countOnly.match(keys).hasMatch()) {
        *consumed = true;
        return true;
    }
    static const QRegularExpression keysMatcher(QStringLiteral("^(\\d*)(.*)$"), QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch km = keysMatcher.match(keys);
    const QString context = m_visualMode ? QStringLiteral("visual") : QStringLiteral("normal");
    QString mainKey = km.captured(2).isEmpty() ? km.captured(1) : km.captured(2);
    if (!s.operatorShortcut.isEmpty() && s.operatorShortcut.right(1) == mainKey)
        mainKey = s.operatorShortcut; // "gUU": repeat the last key of a multi-key operator
    const Match match = matchCommand(mainKey, context, s);
    if (match.type == MatchType::None) {
        clearInputState();
        return false;
    }
    if (match.type == MatchType::Partial) {
        m_expectLiteralNext = match.expectLiteralNext;
        *consumed = true;
        return true;
    }
    if (match.type == MatchType::Clear) {
        clearInputState();
        *consumed = true;
        return true;
    }
    m_expectLiteralNext = false;
    InputState& s2 = *m_inputState;
    s2.keyBuffer.clear();
    if (!km.captured(1).isEmpty() && km.captured(1) != u"0")
        s2.pushRepeatDigit(km.captured(1));
    *command = match.command;
    return true;
}

bool VimEngine::handleKey(const QString& key, const QString& origin)
{
    if (!doc() || key.isEmpty())
        return false;
    // a "long" notification (":registers") closes on the next key; <CR> only closes it
    if (m_closeNotificationOnKey) {
        m_closeNotificationOnKey = false;
        if (m_panel)
            m_panel->closeNotification();
        if (key == u"<CR>") {
            clearInputState();
            return true;
        }
    }
    // Multiple cursors outside visual-block mode: vim.js runs the key for every selection; here
    // it applies to the main one (Esc leaves multi-cursor mode).
    if (m_view->selection().size() > 1 && !m_visualBlock && !m_wasInVisualBlock && !m_insertMode && !m_visualMode && key == u"<Esc>") {
        m_busy++;
        setCursor(clipCursorToContent(getCursor()));
        m_busy--;
        clearInputState();
        return true;
    }
    // Obsidian: Esc in normal mode also removes the search highlight
    if (key == u"<Esc>" && !m_insertMode && !m_visualMode)
        clearSearchHighlight();
    const KeyCommand* command = nullptr;
    bool consumed = false;
    const bool wasInsert = m_insertMode;
    bool result = false;
    if (m_insertMode)
        handleKeyInsertMode(key, &command, &consumed);
    else
        handleKeyNonInsertMode(key, origin, &command, &consumed);
    if (!command) {
        // unmatched: in normal/visual mode a character is swallowed, other keys go to the editor
        result = consumed || (!wasInsert && !m_insertMode && (key.size() == 1 || (key.size() == 2 && key.at(0).isHighSurrogate())));
    } else {
        const KeyCommand cmd = *command; // the keymap may change while it runs (:map)
        runCommand_(cmd);
        result = true;
    }
    return result;
}

void VimEngine::handleEx(const QString& input)
{
    if (!doc())
        return;
    m_busy++;
    beginEdit();
    processExCommand(input);
    clearInputState();
    endEdit();
    m_busy--;
    ensureCursorVisible();
}

void VimEngine::overwriteType(const QString& t)
{
    // Obsidian's overWriteSelection: the character after each cursor is replaced (not a line break)
    if (readOnly())
        return;
    const Selection& sel = m_view->selection();
    const Text& tx = text();
    QList<ChangeSpec> specs;
    QList<SelRange> after;
    for (const SelRange& r : sel.ranges()) {
        qsizetype to = r.to();
        if (r.empty() && to < tx.length() && tx.charAt(to) != u'\n')
            to = to + 1;
        specs.push_back({r.from(), to, t});
    }
    const ChangeSet cs = ChangeSet::of(specs, tx.length());
    for (const SelRange& r : sel.ranges())
        after.push_back(SelRange::cursor(cs.mapPos(r.to() + (r.empty() && r.to() < tx.length() && tx.charAt(r.to()) != u'\n' ? 1 : 0), 1)));
    applyChanges(specs, Selection::create(after, sel.mainIndex()), QStringLiteral("input.type"));
    ensureCursorVisible();
}

void VimEngine::afterCopy()
{
    if (m_insertMode) {
        setCursor(getCursor());
        return;
    }
    handleKey(QStringLiteral("<Esc>"));
}

void VimEngine::ensureCursorVisible()
{
    // CodeMirror scrolls the cursor into view after a vim operation (no extra margin)
    if (!doc())
        return;
    const QRectF r = m_view->caretRectAt(m_view->selection().main().head);
    const double vh = m_view->viewport()->height();
    if (r.top() < 0)
        m_view->setScrollTop(m_view->scrollTop() + r.top());
    else if (r.bottom() > vh)
        m_view->setScrollTop(m_view->scrollTop() + r.bottom() - vh);
}

void VimEngine::runCommand_(const KeyCommand& command)
{
    m_busy++;
    beginEdit();
    if (command.type == CmdType::KeyToKey)
        doKeyToKey(command.toKeys, &command);
    else
        processCommand(command);
    endEdit();
    m_busy--;
    ensureCursorVisible();
}

bool VimEngine::wouldHandle(const QString& key) const
{
    if (!doc() || key.isEmpty())
        return false;
    if (m_closeNotificationOnKey && key == u"<CR>")
        return true;
    if (m_insertMode) {
        if (key == u"<Esc>")
            return true;
        InputState st = *m_inputState;
        st.keyBuffer.push_back(key);
        const Match m = matchCommand(st.keyBuffer.join(QString()), QStringLiteral("insert"), st);
        if (m.type == MatchType::Full)
            return true;
        return m.type == MatchType::Partial && key.size() > 1;
    }
    if (key.size() == 1 || key == u"<Esc>")
        return true;
    InputState st = *m_inputState;
    st.keyBuffer.push_back(key);
    static const QRegularExpression keysMatcher(QStringLiteral("^(\\d*)(.*)$"), QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch km = keysMatcher.match(st.keyBuffer.join(QString()));
    const QString mainKey = km.captured(2).isEmpty() ? km.captured(1) : km.captured(2);
    const Match m = matchCommand(mainKey, m_visualMode ? QStringLiteral("visual") : QStringLiteral("normal"), st);
    return m.type != MatchType::None;
}

void VimEngine::doKeyToKey(const QString& keys, const KeyCommand* fromKey)
{
    // prevent infinite recursion through mappings
    if (fromKey) {
        for (const KeyCommand& k : std::as_const(m_keyToKeyStack))
            if (k.keys == fromKey->keys && k.context == fromKey->context && k.toKeys == fromKey->toKeys)
                return;
    }
    const bool noremapBefore = m_noremap;
    if (fromKey) {
        m_keyToKeyStack.push_back(*fromKey);
        m_noremap = fromKey->user ? fromKey->noremap : true;
    }
    m_keyToKeyDepth++;
    for (const QString& k : splitKeys(keys)) {
        if (m_virtualPrompt) {
            sendKeyToPrompt(k);
            continue;
        }
        const bool wasInsert = m_insertMode;
        const bool result = handleKey(k, QStringLiteral("mapping"));
        if (!result && wasInsert && m_insertMode) {
            // plain typing inside a mapping / :normal
            QString t = k;
            if (k.startsWith(u'<') && k.size() > 1) {
                if (k == u"<Space>")
                    t = QStringLiteral(" ");
                else if (k == u"<CR>")
                    t = QStringLiteral("\n");
                else if (k == u"<Tab>")
                    t = QStringLiteral("\t");
                else if (k == u"<BS>") {
                    const qsizetype c = m_view->selection().main().head;
                    if (c > 0)
                        applyChanges({ChangeSpec{c - 1, c, QString()}}, Selection::cursor(c - 1), QStringLiteral("delete.backward"));
                    continue;
                } else {
                    continue;
                }
            }
            replaceSelection(t);
        }
    }
    m_keyToKeyDepth--;
    if (fromKey)
        m_keyToKeyStack.removeLast();
    m_noremap = m_keyToKeyStack.isEmpty() ? false : noremapBefore;
    if (m_keyToKeyDepth == 0 && m_virtualPrompt) {
        // the keys ended inside a prompt: show it for real
        VirtualPrompt vp = *std::exchange(m_virtualPrompt, std::nullopt);
        showPrompt(vp.prefix, vp.desc, vp.value, false, vp.onClose, vp.onKeyDown, vp.onKeyUp);
    }
}

void VimEngine::sendKeyToPrompt(const QString& key)
{
    VirtualPrompt& vp = *m_virtualPrompt;
    PromptClose close = [this](const std::optional<QString>& v) {
        if (!m_virtualPrompt)
            return;
        if (v)
            m_virtualPrompt->value = *v;
        else
            m_virtualPrompt.reset();
    };
    QString k = key;
    if (key.startsWith(u'<') && key.size() > 1) {
        const QString lower = key.mid(1, key.size() - 2).toLower();
        const QString last = lower.section(u'-', -1);
        if (last == u"lt") {
            k = QStringLiteral("<");
        } else if (last == u"space") {
            k = QStringLiteral(" ");
        } else if (last == u"cr") {
            k = QStringLiteral("\n");
        } else {
            if (vp.onKeyDown && vp.onKeyDown(key, vp.value, close))
                return;
            if (!m_virtualPrompt)
                return;
            if (key == u"<Esc>") {
                m_virtualPrompt.reset();
                return;
            }
            if (key == u"<BS>")
                m_virtualPrompt->value.chop(1);
            if (m_virtualPrompt && m_virtualPrompt->onKeyUp)
                m_virtualPrompt->onKeyUp(key, m_virtualPrompt->value, close);
            return;
        }
    }
    if (k == u"\n") {
        VirtualPrompt p = *std::exchange(m_virtualPrompt, std::nullopt);
        if (p.onClose)
            p.onClose(p.value);
        return;
    }
    vp.value += k;
}

// ---------------------------------------------------------------------------------------------
// command dispatcher (vim.js commandDispatcher)
// ---------------------------------------------------------------------------------------------
void VimEngine::processCommand(const KeyCommand& command)
{
    m_inputState->repeatOverride = command.repeatOverride;
    switch (command.type) {
    case CmdType::Motion:
        processMotion(command);
        break;
    case CmdType::Operator:
        processOperator(command);
        break;
    case CmdType::OperatorMotion:
        processOperatorMotion(command);
        break;
    case CmdType::Action:
        processAction(command);
        break;
    case CmdType::Search:
        processSearch(command);
        break;
    case CmdType::Ex:
    case CmdType::KeyToEx:
        processEx(command);
        break;
    default:
        break;
    }
}

void VimEngine::processMotion(const KeyCommand& command)
{
    m_inputState->motion = command.motion;
    m_inputState->motionArgs = command.motionArgs;
    evalInput();
}

void VimEngine::processOperator(const KeyCommand& command)
{
    InputState& st = *m_inputState;
    if (st.op != Operator::None) {
        if (st.op == command.op) {
            // "dd": an operator typed twice acts linewise
            st.motion = Motion::ExpandToLine;
            st.motionArgs = Args();
            st.motionArgs.linewise = true;
            evalInput();
            return;
        }
        // two different operators in a row: both are dropped (vim.js sets the new operator on the
        // discarded input state)
        clearInputState();
        return;
    }
    InputState& s = *m_inputState;
    s.op = command.op;
    s.operatorArgs = command.operatorArgs;
    if (command.keys.size() > 1)
        s.operatorShortcut = command.keys;
    if (command.exitVisualBlock) {
        m_visualBlock = false;
        updateCmSelection();
    }
    if (m_visualMode)
        evalInput(); // operating on the selection: no motion needed
}

void VimEngine::processOperatorMotion(const KeyCommand& command)
{
    const bool visualMode = m_visualMode;
    if (visualMode && command.operatorMotionArgs.visualLine)
        m_visualLine = true;
    processOperator(command);
    if (!visualMode)
        processMotion(command);
}

void VimEngine::processAction(const KeyCommand& command)
{
    std::shared_ptr<InputState> st = m_inputState;
    const int repeat = st->getRepeat();
    const bool repeatIsExplicit = repeat != 0;
    Args actionArgs = command.actionArgs;
    if (!st->selectedCharacter.isEmpty())
        actionArgs.selectedCharacter = st->selectedCharacter;
    if (command.op != Operator::None)
        processOperator(command);
    if (command.motion != Motion::None)
        processMotion(command);
    if (command.motion != Motion::None || command.op != Operator::None)
        evalInput();
    actionArgs.repeat = repeat ? repeat : 1;
    actionArgs.repeatIsExplicit = repeatIsExplicit;
    actionArgs.registerName = st->registerName;
    clearInputState();
    m_lastMotion = Motion::None;
    if (command.isEdit)
        recordLastEdit(st, &command);
    runAction(command.action, actionArgs);
}

void VimEngine::recordLastEdit(const std::shared_ptr<InputState>& inputState, const KeyCommand* actionCommand)
{
    MacroModeState& mm = G().macroModeState;
    if (mm.isPlaying)
        return;
    m_lastEditInputState = inputState;
    if (actionCommand)
        m_lastEditActionCommand = *actionCommand;
    else
        m_lastEditActionCommand.reset();
    mm.lastInsertModeChanges.changes.clear();
    mm.lastInsertModeChanges.expectCursorActivityForChange = false;
    mm.lastInsertModeChanges.visualBlock = m_visualBlock ? m_sel.head.line - m_sel.anchor.line : 0;
}

void VimEngine::evalInput()
{
    // If a motion is set, run it (and the operator on its range); otherwise nothing yet.
    std::shared_ptr<InputState> stp = m_inputState;
    InputState& inputState = *stp;
    const Motion motion = inputState.motion;
    Args& motionArgs = inputState.motionArgs;
    const Operator op = inputState.op;
    Args& operatorArgs = inputState.operatorArgs;
    const QString registerName = inputState.registerName;
    Range& sel = m_sel;
    const Pos origHead = m_visualMode ? clipCursorToContent(sel.head) : getCursor("head");
    const Pos origAnchor = m_visualMode ? clipCursorToContent(sel.anchor) : getCursor("anchor");
    const Pos oldHead = origHead;
    const Pos oldAnchor = origAnchor;
    std::optional<Pos> newHead, newAnchor;
    int repeat;
    if (op != Operator::None)
        recordLastEdit(stp);
    if (inputState.repeatOverride)
        repeat = *inputState.repeatOverride;
    else
        repeat = inputState.getRepeat();
    if (repeat > 0 && motionArgs.explicitRepeat) {
        motionArgs.repeatIsExplicit = true;
    } else if (motionArgs.noRepeat || (!motionArgs.explicitRepeat && repeat == 0)) {
        repeat = 1;
        motionArgs.repeatIsExplicit = false;
    }
    if (!inputState.selectedCharacter.isEmpty())
        motionArgs.selectedCharacter = operatorArgs.selectedCharacter = inputState.selectedCharacter;
    motionArgs.repeat = repeat;
    clearInputState();
    if (motion != Motion::None) {
        const MotionResult motionResult = runMotion(motion, origHead, motionArgs, inputState);
        m_lastMotion = motion;
        if (!motionResult.valid)
            return;
        if (motionArgs.toJumplist) {
            if (m_jumpList.cachedCursor) {
                recordJumpPosition(*m_jumpList.cachedCursor, motionResult.head);
                m_jumpList.cachedCursor.reset();
            } else {
                recordJumpPosition(origHead, motionResult.head);
            }
        }
        if (motionResult.isRange) {
            newAnchor = motionResult.anchor;
            newHead = motionResult.head;
        } else {
            newHead = motionResult.head;
        }
        if (m_visualMode) {
            if (!(m_visualBlock && newHead->ch >= kEol))
                newHead = clipCursorToContent(*newHead, oldHead);
            if (newAnchor)
                newAnchor = clipCursorToContent(*newAnchor);
            const Pos a = newAnchor ? *newAnchor : oldAnchor;
            newAnchor = a;
            sel.anchor = a;
            sel.head = *newHead;
            updateCmSelection();
            updateMark(QStringLiteral("<"), cursorIsBefore(a, *newHead) ? a : *newHead);
            updateMark(QStringLiteral(">"), cursorIsBefore(a, *newHead) ? *newHead : a);
        } else if (op == Operator::None) {
            newHead = clipCursorToContent(*newHead, oldHead);
            setCursor(*newHead);
        }
    }
    if (op == Operator::None)
        return;
    if (operatorArgs.lastSel) {
        // replaying a visual mode operation (".")
        newAnchor = oldAnchor;
        const LastSel lastSel = *operatorArgs.lastSel;
        const int lineOffset = std::abs(lastSel.head.line - lastSel.anchor.line);
        const int chOffset = std::abs(lastSel.head.ch - lastSel.anchor.ch);
        if (lastSel.visualLine)
            newHead = Pos(oldAnchor.line + lineOffset, oldAnchor.ch);
        else if (lastSel.visualBlock)
            newHead = Pos(oldAnchor.line + lineOffset, oldAnchor.ch + chOffset);
        else if (lastSel.head.line == lastSel.anchor.line)
            newHead = Pos(oldAnchor.line, oldAnchor.ch + chOffset);
        else
            newHead = Pos(oldAnchor.line + lineOffset, oldAnchor.ch);
        m_visualMode = true;
        m_visualLine = lastSel.visualLine;
        m_visualBlock = lastSel.visualBlock;
        m_sel = Range{*newAnchor, *newHead};
        updateCmSelection();
    } else if (m_visualMode) {
        operatorArgs.lastSel = LastSel{sel.anchor, sel.head, m_visualBlock, m_visualLine};
    }
    Pos curStart, curEnd;
    bool linewise;
    CmSelection cmSel;
    if (m_visualMode) {
        curStart = cursorMin(m_sel.head, m_sel.anchor);
        curEnd = cursorMax(m_sel.head, m_sel.anchor);
        linewise = m_visualLine || operatorArgs.linewise;
        const QString mode = m_visualBlock ? QStringLiteral("block") : linewise ? QStringLiteral("line") : QStringLiteral("char");
        const auto np = updateSelectionForSurrogateCharacters(curStart, curEnd);
        cmSel = makeCmSelection(Range{np.first, np.second}, mode);
        if (linewise) {
            if (mode == u"block") {
                for (Range& r : cmSel.ranges)
                    r.head.ch = lineLength(r.head.line); // linewise operators in visual block extend to eol
            } else if (mode == u"line") {
                cmSel.ranges[0].head = Pos(cmSel.ranges[0].head.line + 1, 0);
            }
        }
    } else {
        curStart = newAnchor ? *newAnchor : oldAnchor;
        curEnd = newHead ? *newHead : oldHead;
        if (cursorIsBefore(curEnd, curStart))
            std::swap(curStart, curEnd);
        linewise = motionArgs.linewise || operatorArgs.linewise;
        if (linewise) {
            // expandSelectionToLine
            curStart.ch = 0;
            curEnd.ch = 0;
            curEnd.line++;
        } else if (motionArgs.forward) {
            // clipToLine: trailing newlines (+ whitespace) are not part of a forward motion's range
            const QString selection = getRange(curStart, curEnd);
            static const QRegularExpression trailing(QStringLiteral("\\n\\s*$"));
            if (trailing.match(selection).hasMatch()) {
                QStringList lines = selection.split(QLatin1Char('\n'));
                lines.removeLast();
                QString line;
                for (line = lines.isEmpty() ? QString() : lines.takeLast(); !lines.isEmpty() && !line.isEmpty() && isWhiteSpaceString(line);
                     line = lines.takeLast()) {
                    curEnd.line--;
                    curEnd.ch = 0;
                }
                if (!line.isEmpty()) {
                    curEnd.line--;
                    curEnd.ch = lineLength(curEnd.line);
                } else {
                    curEnd.ch = 0;
                }
            }
        }
        const bool exclusive = !motionArgs.inclusive || linewise;
        const auto np = updateSelectionForSurrogateCharacters(curStart, curEnd);
        cmSel = makeCmSelection(Range{np.first, np.second}, QStringLiteral("char"), exclusive);
    }
    setSelections(cmSel.ranges, cmSel.primary);
    m_lastMotion = Motion::None;
    operatorArgs.repeat = repeat; // indent in visual mode
    operatorArgs.registerName = registerName;
    operatorArgs.linewise = linewise;
    const std::optional<Pos> operatorMoveTo = runOperator(op, operatorArgs, cmSel.ranges, oldAnchor, newHead.value_or(oldHead));
    if (m_visualMode)
        exitVisualMode(!operatorMoveTo.has_value());
    if (operatorMoveTo)
        setCursor(*operatorMoveTo);
}

// ---------------------------------------------------------------------------------------------
// selections and modes
// ---------------------------------------------------------------------------------------------
Pos VimEngine::clipCursorToContent(Pos cur, std::optional<Pos> oldCur) const
{
    const bool includeLineBreak = m_insertMode || m_visualMode;
    const int line = std::clamp(cur.line, firstLine(), lastLine());
    const QString text = getLine(line);
    const int maxCh = int(text.size()) - 1 + (includeLineBreak ? 1 : 0);
    int ch = std::min(std::max(0, cur.ch), maxCh);
    ch = std::max(ch, 0);
    // never inside a surrogate pair
    if (ch < text.size() && text.at(ch).isLowSurrogate()) {
        int direction = 1;
        if (oldCur && oldCur->line == line && oldCur->ch > ch)
            direction = -1;
        ch += direction;
        if (ch > maxCh)
            ch -= 2;
    }
    return Pos(line, std::max(0, ch));
}

QPair<Pos, Pos> VimEngine::updateSelectionForSurrogateCharacters(Pos start, Pos end) const
{
    if (start.line == end.line && start.ch >= end.ch - 1) {
        const QString text = getLine(start.line);
        if (start.ch >= 0 && start.ch < text.size() && text.at(start.ch).isHighSurrogate())
            end.ch += 1;
    }
    return {start, end};
}

VimEngine::CmSelection VimEngine::makeCmSelection(Range sel, const QString& mode, bool exclusive) const
{
    Pos head = sel.head, anchor = sel.anchor;
    if (mode == u"char") {
        const int headOffset = !exclusive && !cursorIsBefore(sel.head, sel.anchor) ? 1 : 0;
        const int anchorOffset = cursorIsBefore(sel.head, sel.anchor) ? 1 : 0;
        return CmSelection{{Range{offsetCursor(sel.anchor, 0, anchorOffset), offsetCursor(sel.head, 0, headOffset)}}, 0};
    }
    if (mode == u"line") {
        if (!cursorIsBefore(sel.head, sel.anchor)) {
            anchor.ch = 0;
            if (head.line > lastLine())
                head.line = lastLine();
            head.ch = lineLength(head.line);
        } else {
            head.ch = 0;
            anchor.ch = lineLength(anchor.line);
        }
        return CmSelection{{Range{anchor, head}}, 0};
    }
    // block
    const int top = std::min(anchor.line, head.line);
    int fromCh = anchor.ch;
    const int bottom = std::max(anchor.line, head.line);
    int toCh = head.ch;
    if (fromCh < toCh)
        toCh += 1;
    else
        fromCh += 1;
    const int height = bottom - top + 1;
    const int primary = head.line == top ? 0 : height - 1;
    CmSelection out;
    for (int i = 0; i < height; ++i)
        out.ranges.push_back(Range{Pos(top + i, fromCh), Pos(top + i, toCh)});
    out.primary = primary;
    return out;
}

void VimEngine::updateCmSelection()
{
    const QString mode = m_visualLine ? QStringLiteral("line") : m_visualBlock ? QStringLiteral("block") : QStringLiteral("char");
    const CmSelection cmSel = makeCmSelection(m_sel, mode);
    setSelections(cmSel.ranges, cmSel.primary);
    if (!m_visualBlock) {
        // resync with the editor's selection (it may have been clamped)
        const Pos anchor = getCursor("anchor");
        const Pos head = getCursor("head");
        const bool fwd = !cursorIsBefore(head, anchor);
        m_sel.head = offsetCursor(head, 0, fwd ? -1 : 0);
        m_sel.anchor = offsetCursor(anchor, 0, fwd ? 0 : -1);
    }
}

void VimEngine::updateLastSelection()
{
    Pos anchor = m_sel.anchor;
    Pos head = m_sel.head;
    if (m_lastPastedText) {
        head = posFromIndex(indexFromPos(anchor) + m_lastPastedText->size());
        m_lastPastedText.reset();
    }
    if (m_lastSelection) {
        clearBookmark(m_lastSelection->anchorMark);
        clearBookmark(m_lastSelection->headMark);
    }
    LastSelection ls;
    ls.anchorMark = setBookmark(anchor);
    ls.headMark = setBookmark(head);
    ls.anchor = anchor;
    ls.head = head;
    ls.visualMode = m_visualMode;
    ls.visualLine = m_visualLine;
    ls.visualBlock = m_visualBlock;
    m_lastSelection = ls;
}

void VimEngine::exitVisualMode(bool moveHead)
{
    if (moveHead)
        setCursor(clipCursorToContent(m_sel.head));
    updateLastSelection();
    m_visualMode = false;
    m_visualLine = false;
    m_visualBlock = false;
    if (!m_insertMode)
        signalModeChange();
}

void VimEngine::selectForInsert(Pos head, int height)
{
    QList<Range> sel;
    for (int i = 0; i < height; ++i) {
        const Pos lineHead = offsetCursor(head, i, 0);
        sel.push_back(Range{lineHead, lineHead});
    }
    setSelections(sel, 0);
}

Pos VimEngine::selectBlock(Pos selectionEnd)
{
    // vim.js selectBlock: a rectangular block with selectionEnd as one corner
    QList<Range> selections;
    const QList<Range> ranges = listSelections();
    Pos head = clipPos(selectionEnd);
    const bool isClipped = !(selectionEnd == head);
    const Pos curHead = getCursor("head");
    int primIndex = 0;
    for (int i = 0; i < ranges.size(); ++i)
        if (ranges[i].anchor == curHead || ranges[i].head == curHead) {
            primIndex = i;
            break;
        }
    const bool wasClipped = ranges[primIndex].head == ranges[primIndex].anchor;
    const int max = int(ranges.size()) - 1;
    const int index = max - primIndex > primIndex ? max : 0;
    Pos base = ranges[index].anchor;
    const int firstLine = std::min(base.line, head.line);
    const int lastL = std::max(base.line, head.line);
    int baseCh = base.ch, headCh = head.ch;
    const int dir = ranges[index].head.ch - baseCh;
    const int newDir = headCh - baseCh;
    if (dir > 0 && newDir <= 0) {
        baseCh++;
        if (!isClipped)
            headCh--;
    } else if (dir < 0 && newDir >= 0) {
        baseCh--;
        if (!wasClipped)
            headCh++;
    } else if (dir < 0 && newDir == -1) {
        baseCh--;
        headCh++;
    }
    for (int line = firstLine; line <= lastL; ++line)
        selections.push_back(Range{Pos(line, baseCh), Pos(line, headCh)});
    setSelections(selections);
    base.ch = baseCh;
    return base;
}

QPair<Pos, Pos> VimEngine::getSelectedAreaRange()
{
    if (m_visualMode) {
        const QList<Range> selections = listSelections();
        const Range start = selections.first();
        const Range end = selections.last();
        const Pos selectionStart = cursorIsBefore(start.anchor, start.head) ? start.anchor : start.head;
        const Pos selectionEnd = cursorIsBefore(end.anchor, end.head) ? end.head : end.anchor;
        return {selectionStart, selectionEnd};
    }
    // replaying the action: the last selection's shape at the cursor
    Pos selectionStart = getCursor();
    Pos selectionEnd = getCursor();
    const LastSelection& ls = *m_lastSelection;
    if (ls.visualBlock) {
        const int width = std::abs(ls.head.ch - ls.anchor.ch);
        const int height = std::abs(ls.head.line - ls.anchor.line);
        selectionEnd = Pos(selectionStart.line + height, selectionStart.ch + width);
        QList<Range> selections;
        for (int i = selectionStart.line; i < selectionEnd.line; ++i)
            selections.push_back(Range{Pos(i, selectionStart.ch), Pos(i, selectionEnd.ch)});
        setSelections(selections);
    } else {
        const Pos start = findBookmark(ls.anchorMark).value_or(ls.anchor);
        const Pos end = findBookmark(ls.headMark).value_or(ls.head);
        const int line = end.line - start.line;
        const int ch = end.ch - start.ch;
        selectionEnd = Pos(selectionEnd.line + line, line ? selectionEnd.ch : ch + selectionEnd.ch);
        if (ls.visualLine) {
            selectionStart = Pos(selectionStart.line, 0);
            selectionEnd = Pos(selectionEnd.line, lineLength(selectionEnd.line));
        }
        setSelection(selectionStart, selectionEnd);
    }
    return {selectionStart, selectionEnd};
}

void VimEngine::enterInsertMode()
{
    Args a;
    actEnterInsertMode(a);
}

void VimEngine::exitInsertMode(bool keepCursor)
{
    MacroModeState& mm = G().macroModeState;
    const bool isPlaying = mm.isPlaying;
    InsertChanges& lastChange = mm.lastInsertModeChanges;
    if (!isPlaying) {
        if (m_insertEnd >= 0)
            clearBookmark(m_insertEnd);
        m_insertEnd = -1;
    }
    if (!isPlaying && m_insertModeRepeat > 1) {
        // "3ifoo<Esc>": insert the text twice more
        repeatLastEdit(m_insertModeRepeat - 1, true);
        if (m_lastEditInputState)
            m_lastEditInputState->repeatOverride = m_insertModeRepeat;
    }
    m_insertModeRepeat = 0;
    m_insertMode = false;
    m_recordingInsert = false;
    if (!keepCursor) {
        const Pos c = getCursor();
        setCursor(Pos(c.line, std::max(0, c.ch - 1)));
    }
    m_overwrite = false;
    G().registerController.get(QStringLiteral(".")).setText(lastChange.text());
    if (m_insertEditGroup) {
        m_insertEditGroup = false;
        if (doc())
            doc()->history().endGroup();
    }
    signalModeChange();
    if (mm.isRecording)
        logInsertModeChange();
}

// ---------------------------------------------------------------------------------------------
// dot repeat and macros
// ---------------------------------------------------------------------------------------------
void VimEngine::repeatLastEdit(int repeat, bool repeatForInsert)
{
    MacroModeState& mm = G().macroModeState;
    const bool wasPlaying = mm.isPlaying;
    mm.isPlaying = true;
    const bool isAction = m_lastEditActionCommand.has_value();
    const std::shared_ptr<InputState> cachedInputState = m_inputState;
    auto repeatCommand = [&] {
        if (isAction) {
            const KeyCommand cmd = *m_lastEditActionCommand;
            processAction(cmd);
        } else {
            evalInput();
        }
    };
    auto repeatInsert = [&](int n) {
        if (!mm.lastInsertModeChanges.changes.isEmpty()) {
            // repeating "cw" does not repeat the insertion (like desktop vim)
            n = !m_lastEditActionCommand ? 1 : n;
            const QList<InsertOp> changes = mm.lastInsertModeChanges.changes;
            repeatInsertModeChanges(changes, n);
        }
    };
    m_inputState = m_lastEditInputState;
    if (isAction && m_lastEditActionCommand->interlaceInsertRepeat) {
        // "o"/"O" repeats interlace the insertions so they land on separate lines
        for (int i = 0; i < repeat; ++i) {
            m_inputState = m_lastEditInputState;
            repeatCommand();
            repeatInsert(1);
        }
    } else {
        if (!repeatForInsert)
            repeatCommand();
        repeatInsert(repeat);
    }
    m_inputState = cachedInputState;
    if (m_insertMode && !repeatForInsert)
        exitInsertMode();
    mm.isPlaying = wasPlaying;
}

void VimEngine::repeatInsertModeChanges(const QList<InsertOp>& changes, int repeat)
{
    const Pos head = getCursor("head");
    const int visualBlock = G().macroModeState.lastInsertModeChanges.visualBlock;
    if (visualBlock) {
        selectForInsert(head, visualBlock + 1);
        repeat = int(listSelections().size());
        setCursor(head);
    }
    for (int i = 0; i < repeat; ++i) {
        if (visualBlock)
            setCursor(offsetCursor(head, i, 0));
        for (const InsertOp& op : changes) {
            const qsizetype c = indexFromPos(getCursor());
            const qsizetype len = text().length();
            const qsizetype from = std::max<qsizetype>(0, c - op.delBefore);
            qsizetype to = std::min(len, c + op.delAfter);
            if (op.overwrite) // replace mode overwrites as much as it types, within the line
                to = std::min(c + op.text.size(), text().lineEnd(text().lineAt(c)));
            const qsizetype cursor = from + op.text.size() - op.cursorBack;
            applyChanges({ChangeSpec{from, to, op.text}}, Selection::cursor(cursor), QStringLiteral("input.type"));
        }
    }
    if (visualBlock)
        setCursor(offsetCursor(head, 0, 1));
}

void VimEngine::logInsertModeChange()
{
    MacroModeState& mm = G().macroModeState;
    if (mm.isPlaying)
        return;
    G().registerController.get(mm.latestRegister).pushInsertModeChanges(mm.lastInsertModeChanges);
}

void VimEngine::executeMacroRegister(const QString& registerName)
{
    MacroModeState& mm = G().macroModeState;
    Register& reg = G().registerController.get(registerName);
    if (registerName == u":") {
        // the last ex command
        const QString cmd = reg.keyBuffer.value(0);
        if (!cmd.isEmpty())
            processExCommand(cmd);
        mm.isPlaying = false;
        return;
    }
    const QStringList keyBuffer = reg.keyBuffer;
    const QList<InsertChanges> insertChanges = reg.insertModeChanges;
    int imc = 0;
    mm.isPlaying = true;
    mm.replaySearchQueries = reg.searchQueries;
    for (const QString& chunk : keyBuffer) {
        for (const QString& key : splitKeys(chunk)) {
            handleKey(key, QStringLiteral("macro"));
            if (m_insertMode) {
                const QList<InsertOp> changes = imc < insertChanges.size() ? insertChanges[imc++].changes : QList<InsertOp>();
                mm.lastInsertModeChanges.changes = changes;
                repeatInsertModeChanges(changes, 1);
                exitInsertMode();
            }
        }
    }
    mm.isPlaying = false;
}

} // namespace oc::vim
