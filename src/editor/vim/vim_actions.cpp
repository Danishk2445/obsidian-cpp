// Operators and actions (vim.js "operators" and "actions").
#include "editor/vim/vim_engine.h"

#include "editor/vim/vim_panel.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"
#include "editor/markdown_commands.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc::vim {

namespace {
VimGlobal& G() { return VimGlobal::instance(); }

int firstNonWS(const QString& text)
{
    for (int i = 0; i < text.size(); ++i)
        if (!text.at(i).isSpace())
            return i;
    return int(text.size());
}

bool isWhiteSpaceString(const QString& s)
{
    for (QChar c : s)
        if (!c.isSpace())
            return false;
    return true;
}

int tabSize() { return std::clamp(mdcmd::settings().tabSize, 1, 16); }

// CodeMirror countColumn / indentString (indentUnit "\t" when "useTab", else tabSize spaces)
int countColumn(const QString& s, int ts)
{
    int n = 0;
    for (QChar c : s)
        n = c == u'\t' ? n + ts - (n % ts) : n + 1;
    return n;
}

QString indentString(int cols)
{
    const int ts = tabSize();
    QString out;
    if (mdcmd::settings().useTab)
        for (; cols >= ts; cols -= ts)
            out += u'\t';
    for (; cols > 0; --cols)
        out += u' ';
    return out;
}

int indentUnitColumns() { return tabSize(); }
QString indentUnit() { return mdcmd::settings().useTab ? QStringLiteral("\t") : QString(tabSize(), u' '); }

QString leadingSpace(const QString& s)
{
    int i = 0;
    while (i < s.size() && s.at(i).isSpace())
        ++i;
    return s.left(i);
}

QString swapCase(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (QChar c : s)
        out += c.isUpper() ? QString(c).toLower() : QString(c).toUpper();
    return out;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// operators
// ---------------------------------------------------------------------------------------------
std::optional<Pos> VimEngine::runOperator(Operator op, Args& args, const QList<Range>& ranges, Pos oldAnchor, Pos newHead)
{
    switch (op) {
    case Operator::Change:
        return opChange(args, ranges);
    case Operator::Delete:
        return opDelete(args, ranges);
    case Operator::Indent:
        return opIndent(args, ranges);
    case Operator::IndentAuto:
        // Markdown has no indentation rules: CodeMirror's indentSelection changes nothing
        return moveToFirstNonWhiteSpaceCharacter(ranges[0].anchor);
    case Operator::HardWrap:
        return opHardWrap(args, ranges, oldAnchor);
    case Operator::ChangeCase:
        return opChangeCase(args, ranges, oldAnchor, newHead);
    case Operator::Yank:
        return opYank(args, ranges, oldAnchor);
    case Operator::None:
        break;
    }
    return std::nullopt;
}

std::optional<Pos> VimEngine::opChange(Args& args, const QList<Range>& ranges)
{
    Pos anchor = ranges[0].anchor, head = ranges[0].head;
    QString text;
    Pos finalHead;
    if (!m_visualMode) {
        text = getRange(anchor, head);
        const InputState* lastState = m_lastEditInputState.get();
        if (lastState && lastState->motion == Motion::MoveByWords && !isWhiteSpaceString(text)) {
            // "cw" leaves the whitespace after the word
            static const QRegularExpression trailing(QStringLiteral("\\s+$"));
            const QRegularExpressionMatch m = trailing.match(text);
            if (m.hasMatch() && lastState->motionArgs.forward) {
                head = offsetCursor(head, 0, -int(m.capturedLength()));
                text.chop(m.capturedLength());
            }
        }
        if (args.linewise) {
            anchor = Pos(anchor.line, firstNonWS(getLine(anchor.line)));
            if (head.line > anchor.line)
                head = Pos(head.line - 1, kEol);
        }
        const qsizetype a = std::min(indexFromPos(anchor), indexFromPos(head));
        replaceRange(QString(), anchor, head);
        finalHead = posFromIndex(a);
    } else if (args.fullLine) {
        head.ch = kEol;
        head.line--;
        setSelection(anchor, head);
        text = getSelection();
        const qsizetype a = std::min(indexFromPos(anchor), indexFromPos(head));
        replaceSelection(QString());
        finalHead = posFromIndex(a);
    } else {
        text = getSelection();
        const qsizetype a = indexFromPos(cursorMin(ranges[0].head, ranges[0].anchor));
        replaceSelections(QStringList(QList<QString>(ranges.size(), QString())));
        finalHead = posFromIndex(a);
    }
    G().registerController.pushText(args.registerName, QStringLiteral("change"), text, args.linewise, ranges.size() > 1);
    Args a;
    a.head = finalHead;
    actEnterInsertMode(a);
    return std::nullopt;
}

std::optional<Pos> VimEngine::opDelete(Args& args, const QList<Range>& ranges)
{
    QString text;
    Pos finalHead;
    if (!m_visualBlock) {
        Pos anchor = ranges[0].anchor, head = ranges[0].head;
        if (args.linewise && head.line != firstLine() && anchor.line == lastLine() && anchor.line == head.line - 1) {
            // "dd" on the last line: take the line break before it
            if (anchor.line == firstLine())
                anchor.ch = 0;
            else
                anchor = Pos(anchor.line - 1, lineLength(anchor.line - 1));
        }
        text = getRange(anchor, head);
        const qsizetype a = std::min(indexFromPos(anchor), indexFromPos(head));
        replaceRange(QString(), anchor, head);
        finalHead = posFromIndex(a);
        if (args.linewise)
            finalHead = moveToFirstNonWhiteSpaceCharacter(finalHead);
    } else {
        text = getSelection();
        const qsizetype a = indexFromPos(cursorMin(ranges[0].head, ranges[0].anchor));
        replaceSelections(QStringList(QList<QString>(ranges.size(), QString())));
        finalHead = posFromIndex(a);
    }
    G().registerController.pushText(args.registerName, QStringLiteral("delete"), text, args.linewise, m_visualBlock);
    return clipCursorToContent(finalHead);
}

std::optional<Pos> VimEngine::opIndent(Args& args, const QList<Range>& ranges)
{
    // CodeMirror's indentMore / indentLess over the selected lines (a range ending at a line start
    // doesn't include that line); a count repeats it in visual mode only.
    const int repeat = m_visualMode ? std::max(1, args.repeat) : 1;
    for (int j = 0; j < repeat; ++j) {
        int atLine = -1;
        QList<int> lines;
        for (const Range& rg : ranges) {
            const Pos from = cursorMin(rg.anchor, rg.head), to = cursorMax(rg.anchor, rg.head);
            const qsizetype fromI = indexFromPos(from), toI = indexFromPos(to);
            const bool empty = fromI == toI;
            for (int l = text().lineAt(fromI); l <= text().lineAt(toI); ++l) {
                if (l > atLine && (empty || toI > text().lineStart(l))) {
                    lines.push_back(l);
                    atLine = l;
                }
            }
        }
        QList<ChangeSpec> specs;
        const Text& t = text();
        for (int l : std::as_const(lines)) {
            const qsizetype ls = t.lineStart(l);
            if (args.indentRight) {
                specs.push_back({ls, ls, indentUnit()});
                continue;
            }
            const QString space = leadingSpace(t.line(l));
            if (space.isEmpty())
                continue;
            const QString insert = indentString(std::max(0, countColumn(space, tabSize()) - indentUnitColumns()));
            int keep = 0;
            while (keep < space.size() && keep < insert.size() && space.at(keep) == insert.at(keep))
                ++keep;
            specs.push_back({ls + keep, ls + space.size(), insert.mid(keep)});
        }
        applyChanges(specs);
    }
    return moveToFirstNonWhiteSpaceCharacter(ranges[0].anchor);
}

std::optional<Pos> VimEngine::opChangeCase(Args& args, const QList<Range>& ranges, Pos oldAnchor, Pos newHead)
{
    QStringList swapped;
    for (const QString& s : getSelections()) {
        if (args.toLower == true)
            swapped << s.toLower();
        else if (args.toLower == false)
            swapped << s.toUpper();
        else
            swapped << swapCase(s);
    }
    replaceSelections(swapped);
    if (args.shouldMoveCursor)
        return newHead;
    if (!m_visualMode && args.linewise && ranges[0].anchor.line + 1 == ranges[0].head.line)
        return moveToFirstNonWhiteSpaceCharacter(oldAnchor);
    if (args.linewise)
        return oldAnchor;
    return cursorMin(ranges[0].anchor, ranges[0].head);
}

std::optional<Pos> VimEngine::opYank(Args& args, const QList<Range>& ranges, Pos oldAnchor)
{
    const QString text = getSelection();
    const Pos endPos = m_visualMode ? cursorMin(cursorMin(m_sel.anchor, m_sel.head), cursorMin(ranges[0].head, ranges[0].anchor)) : oldAnchor;
    G().registerController.pushText(args.registerName, QStringLiteral("yank"), text, args.linewise, m_visualBlock);
    return endPos;
}

std::optional<Pos> VimEngine::opHardWrap(Args& args, const QList<Range>& ranges, Pos oldAnchor)
{
    const int from = ranges[0].anchor.line;
    int to = ranges[0].head.line;
    if (args.linewise)
        to--;
    int endRow = hardWrap(from, to, G().textwidth);
    if (endRow > from && args.linewise)
        endRow--;
    return args.keepCursor ? oldAnchor : Pos(endRow, 0);
}

int VimEngine::hardWrap(int from, int to, int column)
{
    // Obsidian's adapter hardWrap (gq / gw)
    struct Space {
        int start, end;
    };
    auto findSpace = [](const QString& line, int max, int min) -> std::optional<Space> {
        if (line.size() < max)
            return std::nullopt;
        const QString before = line.left(max), after = line.mid(max);
        static const QRegularExpression spaceAfterRe(QStringLiteral("^(?:(\\s+)|(\\S+)(\\s+))"));
        static const QRegularExpression spaceBeforeRe(QStringLiteral("(?:(\\s+)|(\\s+)(\\S+))$"));
        const QRegularExpressionMatch sa = spaceAfterRe.match(after);
        const QRegularExpressionMatch sb = spaceBeforeRe.match(before);
        int start = 0, end = 0;
        if (sb.hasMatch() && !sb.hasCaptured(2)) {
            start = max - int(sb.capturedLength(1));
            end = max;
        }
        if (sa.hasMatch() && !sa.hasCaptured(2)) {
            if (!start)
                start = max;
            end = max + int(sa.capturedLength(1));
        }
        if (start)
            return Space{start, end};
        if (sb.hasMatch() && sb.hasCaptured(2) && sb.capturedStart() > min)
            return Space{int(sb.capturedStart()), int(sb.capturedStart() + sb.capturedLength(2))};
        if (sa.hasMatch() && sa.hasCaptured(2)) {
            start = max + int(sa.capturedLength(2));
            return Space{start, start + int(sa.capturedLength(3))};
        }
        return std::nullopt;
    };
    const bool allowMerge = true;
    int row = std::min(from, to);
    int endRow = std::max(from, to);
    static const QRegularExpression nonSpace(QStringLiteral("\\S"));
    int guard = 0;
    while (row <= endRow && row < lineCount() && guard++ < 100000) {
        const QString line = getLine(row);
        if (line.size() > column) {
            if (const auto space = findSpace(line, column, 5)) {
                const QString indent = leadingSpace(line);
                replaceRange(u'\n' + indent, Pos(row, space->start), Pos(row, space->end));
            }
            endRow++;
        } else if (allowMerge && nonSpace.match(line).hasMatch() && row != endRow) {
            const QString nextLine = getLine(row + 1);
            if (!nextLine.isEmpty() && nonSpace.match(nextLine).hasMatch()) {
                QString trimmedLine = line;
                while (!trimmedLine.isEmpty() && trimmedLine.back().isSpace())
                    trimmedLine.chop(1);
                const QString trimmedNextLine = nextLine.mid(leadingSpace(nextLine).size());
                const QString mergedLine = trimmedLine + u' ' + trimmedNextLine;
                const auto space = findSpace(mergedLine, column, 5);
                if ((space && space->start > trimmedLine.size()) || mergedLine.size() < column) {
                    replaceRange(QStringLiteral(" "), Pos(row, int(trimmedLine.size())), Pos(row + 1, int(nextLine.size() - trimmedNextLine.size())));
                    row--;
                    endRow--;
                } else if (trimmedLine.size() < line.size()) {
                    replaceRange(QString(), Pos(row, int(trimmedLine.size())), Pos(row, int(line.size())));
                }
            }
        }
        row++;
    }
    return row;
}

// ---------------------------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------------------------
void VimEngine::runAction(Action a, Args& args)
{
    switch (a) {
    case Action::JumpListWalk:
        actJumpListWalk(args);
        break;
    case Action::Scroll:
        actScroll(args);
        break;
    case Action::ScrollToCursor:
        actScrollToCursor(args);
        break;
    case Action::ReplayMacro:
        actReplayMacro(args);
        break;
    case Action::EnterMacroRecordMode:
        actEnterMacroRecordMode(args);
        break;
    case Action::ToggleOverwrite:
        actToggleOverwrite();
        break;
    case Action::EnterInsertMode:
        actEnterInsertMode(args);
        break;
    case Action::ToggleVisualMode:
        actToggleVisualMode(args);
        break;
    case Action::ReselectLastSelection:
        actReselectLastSelection();
        break;
    case Action::JoinLines:
        actJoinLines(args);
        break;
    case Action::NewLineAndEnterInsertMode:
        actNewLineAndEnterInsertMode(args);
        break;
    case Action::Paste:
        actPaste(args);
        break;
    case Action::Undo:
        actUndo(args);
        break;
    case Action::Redo:
        actRedo(args);
        break;
    case Action::SetRegister:
        m_inputState->registerName = args.selectedCharacter;
        break;
    case Action::InsertRegister:
        actInsertRegister(args);
        break;
    case Action::OneNormalCommand:
        actOneNormalCommand();
        break;
    case Action::SetMark:
        updateMark(args.selectedCharacter, getCursor());
        break;
    case Action::Replace:
        actReplace(args);
        break;
    case Action::IncrementNumberToken:
        actIncrementNumberToken(args);
        break;
    case Action::RepeatLastEdit:
        actRepeatLastEdit(args);
        break;
    case Action::Indent:
        actIndentLine(args);
        break;
    case Action::Fold:
        actFold(args);
        break;
    case Action::None:
        break;
    }
}

void VimEngine::actJumpListWalk(Args& args)
{
    if (m_visualMode)
        return;
    const int mark = jumpListMove(args.forward ? args.repeat : -args.repeat);
    std::optional<Pos> markPos = mark >= 0 ? findBookmark(mark) : std::nullopt;
    setCursor(markPos.value_or(getCursor()));
}

void VimEngine::actScroll(Args& args)
{
    if (m_visualMode)
        return;
    DocLayout& l = m_view->layout();
    const StyleSheetPtr ss = l.styleSheet();
    const double lineHeight = ss ? ss->line(StyleId::Normal).lineBox : 24;
    const int repeat = std::max(1, args.repeat);
    const double top = m_view->scrollTop();
    const double clientHeight = m_view->viewport()->height();
    const double delta = lineHeight * repeat;
    const double newPos = args.forward ? top + delta : top - delta;
    Pos cursor = getCursor();
    QRectF cc = l.caretRect(indexFromPos(cursor));
    if (args.forward) {
        if (newPos > cc.top()) {
            cursor.line = std::min(lastLine(), int(std::ceil(cursor.line + (newPos - cc.top()) / lineHeight)));
            setCursor(clipCursorToContent(cursor));
            cc = l.caretRect(indexFromPos(getCursor()));
            m_view->setScrollTop(cc.top());
        } else {
            m_view->setScrollTop(newPos);
        }
    } else {
        const double newBottom = newPos + clientHeight;
        if (newBottom < cc.bottom()) {
            cursor.line = std::max(0, int(std::floor(cursor.line - (cc.bottom() - newBottom) / lineHeight)));
            setCursor(clipCursorToContent(cursor));
            cc = l.caretRect(indexFromPos(getCursor()));
            m_view->setScrollTop(cc.bottom() - clientHeight);
        } else {
            m_view->setScrollTop(newPos);
        }
    }
}

void VimEngine::actScrollToCursor(Args& args)
{
    DocLayout& l = m_view->layout();
    const int lineNum = getCursor().line;
    const QRectF cc = l.caretRect(indexFromPos(Pos(lineNum, 0)));
    const double height = m_view->viewport()->height();
    double y = cc.top();
    if (args.position == u"center") {
        y = cc.bottom() - height / 2;
    } else if (args.position == u"bottom") {
        const QRectF last = l.caretRect(indexFromPos(Pos(lineNum, std::max(0, lineLength(lineNum) - 1))));
        const double lineHeight = last.bottom() - y;
        y = y - height + lineHeight;
    }
    m_view->setScrollTop(y);
}

void VimEngine::actReplayMacro(Args& args)
{
    QString registerName = args.selectedCharacter;
    MacroModeState& mm = G().macroModeState;
    if (registerName == u"@")
        registerName = mm.latestRegister;
    else
        mm.latestRegister = registerName;
    if (registerName.isEmpty())
        return;
    int repeat = args.repeat;
    while (repeat--)
        executeMacroRegister(registerName);
}

void VimEngine::actEnterMacroRecordMode(Args& args)
{
    MacroModeState& mm = G().macroModeState;
    const QString name = args.selectedCharacter;
    if (!G().registerController.isValid(name))
        return;
    G().registerController.get(name).clear();
    mm.latestRegister = name;
    if (m_panel)
        m_panel->showRecording(QStringLiteral("(recording)[") + name + u']');
    mm.isRecording = true;
}

void VimEngine::actToggleOverwrite()
{
    m_overwrite = !m_overwrite;
    signalModeChange();
}

void VimEngine::actEnterInsertMode(Args& args)
{
    if (readOnly())
        return;
    m_insertMode = true;
    m_insertModeRepeat = args.repeat ? args.repeat : 1;
    const QString insertAt = args.insertAt;
    Pos head = args.head ? *args.head : getCursor("head");
    int height = int(listSelections().size());
    if (insertAt == u"eol") {
        head = Pos(head.line, lineLength(head.line));
    } else if (insertAt == u"bol") {
        head = Pos(head.line, 0);
    } else if (insertAt == u"charAfter") {
        head = updateSelectionForSurrogateCharacters(head, offsetCursor(head, 0, 1)).second;
    } else if (insertAt == u"firstNonBlank") {
        head = updateSelectionForSurrogateCharacters(head, moveToFirstNonWhiteSpaceCharacter(head)).second;
    } else if (insertAt == u"startOfSelectedArea") {
        if (!m_visualMode) {
            m_insertMode = false;
            return;
        }
        if (!m_visualBlock) {
            head = m_sel.head.line < m_sel.anchor.line ? m_sel.head : Pos(m_sel.anchor.line, 0);
        } else {
            head = Pos(std::min(m_sel.head.line, m_sel.anchor.line), std::min(m_sel.head.ch, m_sel.anchor.ch));
            height = std::abs(m_sel.head.line - m_sel.anchor.line) + 1;
        }
    } else if (insertAt == u"endOfSelectedArea") {
        if (!m_visualMode) {
            m_insertMode = false;
            return;
        }
        if (!m_visualBlock) {
            head = m_sel.head.line >= m_sel.anchor.line ? offsetCursor(m_sel.head, 0, 1) : Pos(m_sel.anchor.line, 0);
        } else {
            head = Pos(std::min(m_sel.head.line, m_sel.anchor.line), std::max(m_sel.head.ch, m_sel.anchor.ch) + 1);
            height = std::abs(m_sel.head.line - m_sel.anchor.line) + 1;
        }
    } else if (insertAt == u"inplace") {
        if (m_visualMode) {
            m_insertMode = false;
            return;
        }
    } else if (insertAt == u"lastEdit") {
        if (const auto p = getLastEditPos())
            head = *p;
    }
    m_overwrite = args.replace;
    if (!G().macroModeState.isPlaying) {
        m_recordingInsert = true;
        if (m_insertEnd >= 0)
            clearBookmark(m_insertEnd);
        m_insertEnd = setBookmark(head, true);
    }
    // one undo step for the whole insert session (together with the command that started it)
    if (!m_insertEditGroup && doc()) {
        doc()->history().beginGroup();
        m_insertEditGroup = true;
    }
    if (m_visualMode)
        exitVisualMode();
    selectForInsert(head, height);
    signalModeChange();
}

void VimEngine::actToggleVisualMode(Args& args)
{
    const int repeat = args.repeat;
    const Pos anchor = getCursor();
    if (!m_visualMode) {
        m_visualMode = true;
        m_visualLine = args.linewise;
        m_visualBlock = args.blockwise;
        const Pos head = clipCursorToContent(Pos(anchor.line, anchor.ch + repeat - 1));
        const auto np = updateSelectionForSurrogateCharacters(anchor, head);
        m_sel = Range{np.first, np.second};
        signalModeChange();
        updateCmSelection();
        updateMark(QStringLiteral("<"), cursorMin(anchor, head));
        updateMark(QStringLiteral(">"), cursorMax(anchor, head));
    } else if (m_visualLine != args.linewise || m_visualBlock != args.blockwise) {
        m_visualLine = args.linewise;
        m_visualBlock = args.blockwise;
        signalModeChange();
        updateCmSelection();
    } else {
        exitVisualMode();
    }
}

void VimEngine::actReselectLastSelection()
{
    const std::optional<LastSelection> lastSelection = m_lastSelection;
    if (m_visualMode)
        updateLastSelection();
    if (!lastSelection)
        return;
    const auto anchor = findBookmark(lastSelection->anchorMark);
    const auto head = findBookmark(lastSelection->headMark);
    if (!anchor || !head)
        return; // the marks were destroyed by edits
    m_sel = Range{*anchor, *head};
    m_visualMode = true;
    m_visualLine = lastSelection->visualLine;
    m_visualBlock = lastSelection->visualBlock;
    updateCmSelection();
    updateMark(QStringLiteral("<"), cursorMin(*anchor, *head));
    updateMark(QStringLiteral(">"), cursorMax(*anchor, *head));
    signalModeChange();
}

void VimEngine::actJoinLines(Args& args)
{
    Pos curStart, curEnd;
    if (m_visualMode) {
        curStart = getCursor("anchor");
        curEnd = getCursor("head");
        if (cursorIsBefore(curEnd, curStart))
            std::swap(curStart, curEnd);
        curEnd.ch = lineLength(curEnd.line) - 1;
    } else {
        const int repeat = std::max(args.repeat, 2); // lines to join
        curStart = getCursor();
        curEnd = clipCursorToContent(Pos(curStart.line + repeat - 1, kEol));
    }
    int finalCh = 0;
    static const QRegularExpression nonSpace(QStringLiteral("\\S"));
    for (int i = curStart.line; i < curEnd.line; ++i) {
        finalCh = lineLength(curStart.line);
        QString t;
        int nextStartCh = 0;
        if (!args.keepSpaces) {
            const QString nextLine = getLine(curStart.line + 1);
            nextStartCh = int(nextLine.indexOf(nonSpace));
            if (nextStartCh == -1)
                nextStartCh = int(nextLine.size());
            else
                t = QStringLiteral(" ");
        }
        replaceRange(t, Pos(curStart.line, finalCh), Pos(curStart.line + 1, nextStartCh));
    }
    const Pos curFinalPos = clipCursorToContent(Pos(curStart.line, finalCh));
    if (m_visualMode)
        exitVisualMode(false);
    setCursor(curFinalPos);
}

void VimEngine::newlineAndIndent()
{
    // CodeMirror's insertNewlineAndIndent (Markdown has no indentation rules: keep the line's)
    const qsizetype pos = m_view->selection().main().head;
    const Text& t = text();
    const int line = t.lineAt(pos);
    const qsizetype ls = t.lineStart(line), le = t.lineEnd(line);
    const QString lineText = t.line(line);
    const int indent = countColumn(leadingSpace(lineText), tabSize());
    qsizetype from = pos, to = pos;
    while (to < le && lineText.at(to - ls).isSpace())
        ++to;
    if (from > ls && from < ls + 100 && isWhiteSpaceString(lineText.left(from - ls)))
        from = ls;
    const QString insert = u'\n' + indentString(indent);
    applyChanges({ChangeSpec{from, to, insert}}, Selection::cursor(from + insert.size()), QStringLiteral("input"));
}

void VimEngine::newlineAndIndentBefore()
{
    // Obsidian's newlineAndIndentBefore: a new line above with the current line's indentation
    const qsizetype pos = m_view->selection().main().head;
    const Text& t = text();
    const int line = t.lineAt(pos);
    const qsizetype ls = t.lineStart(line);
    const QString ind = indentString(countColumn(leadingSpace(t.line(line)), tabSize()));
    applyChanges({ChangeSpec{ls, ls, ind + u'\n'}}, Selection::cursor(ls + ind.size()), QStringLiteral("input"));
}

void VimEngine::actNewLineAndEnterInsertMode(Args& args)
{
    if (readOnly())
        return;
    m_insertMode = true;
    if (args.after) {
        Pos insertAt = getCursor();
        insertAt.ch = lineLength(insertAt.line);
        setCursor(insertAt);
        newlineAndIndent();
    } else {
        newlineAndIndentBefore();
    }
    Args a;
    a.repeat = args.repeat;
    actEnterInsertMode(a);
}

void VimEngine::actPaste(Args& args)
{
    const RegisterController::Contents reg = G().registerController.contents(args.registerName);
    QString text = reg.text;
    Pos cur = getCursor();
    if (text.isEmpty())
        return;
    if (args.matchIndent) {
        const int ts = tabSize();
        auto whitespaceLength = [&](const QString& str) {
            int tabs = 0, spaces = 0;
            for (QChar c : str) {
                if (c == u'\t')
                    tabs++;
                else if (c == u' ')
                    spaces++;
            }
            return tabs * ts + spaces;
        };
        const QString currentLine = getLine(getCursor().line);
        const int indent = whitespaceLength(leadingSpace(currentLine));
        QString chomped = text;
        const bool wasChomped = chomped.endsWith(u'\n');
        if (wasChomped)
            chomped.chop(1);
        const int firstIndent = whitespaceLength(leadingSpace(text));
        QStringList lines = chomped.split(u'\n');
        for (QString& l : lines) {
            const QString ws = leadingSpace(l);
            const int newIndent = indent + (whitespaceLength(ws) - firstIndent);
            QString rep;
            if (newIndent > 0)
                rep = mdcmd::settings().useTab ? QString(newIndent / ts, u'\t') : QString(newIndent, u' ');
            l = rep + l.mid(ws.size());
        }
        text = lines.join(u'\n') + (wasChomped ? QStringLiteral("\n") : QString());
    }
    if (args.repeat > 1)
        text = text.repeated(args.repeat);
    const bool linewise = reg.linewise;
    const bool blockwise = reg.blockwise;
    QStringList blockLines;
    if (blockwise) {
        blockLines = text.split(u'\n');
        if (linewise)
            blockLines.removeLast();
        for (QString& l : blockLines)
            if (l.isEmpty())
                l = QStringLiteral(" ");
        cur.ch += args.after ? 1 : 0;
        cur.ch = std::min(lineLength(cur.line), cur.ch);
    } else if (linewise) {
        if (m_visualMode) {
            text = m_visualLine ? text.left(text.size() - 1) : u'\n' + text.left(text.size() - 1) + u'\n';
        } else if (args.after) {
            // paste after the current line: move the trailing newline to the front
            text = u'\n' + text.left(text.size() - 1);
            cur.ch = lineLength(cur.line);
        } else {
            cur.ch = 0;
        }
    } else {
        cur.ch += args.after ? 1 : 0;
    }
    Pos curPosFinal;
    if (m_visualMode) {
        m_lastPastedText = text;
        const auto area = getSelectedAreaRange();
        const Pos selectionStart = area.first;
        const Pos selectionEnd = area.second;
        const QString selectedText = getSelection();
        const int nSel = int(listSelections().size());
        std::optional<Pos> lastSelectionCurEnd;
        if (m_lastSelection)
            lastSelectionCurEnd = findBookmark(m_lastSelection->headMark);
        // the replaced text goes to the unnamed register
        G().registerController.unnamed().setText(selectedText);
        if (blockwise) {
            replaceSelections(QStringList(QList<QString>(nSel, QString())));
            setCursor(selectionStart);
            selectBlock(Pos(selectionStart.line + int(blockLines.size()) - 1, selectionStart.ch));
            replaceSelections(blockLines);
            curPosFinal = selectionStart;
        } else if (m_visualBlock) {
            replaceSelections(QStringList(QList<QString>(nSel, QString())));
            setCursor(selectionStart);
            replaceRange(text, selectionStart, selectionStart);
            curPosFinal = selectionStart;
        } else {
            replaceRange(text, selectionStart, selectionEnd);
            curPosFinal = posFromIndex(indexFromPos(selectionStart) + text.size() - 1);
        }
        if (lastSelectionCurEnd && m_lastSelection) {
            clearBookmark(m_lastSelection->headMark);
            m_lastSelection->headMark = setBookmark(*lastSelectionCurEnd);
        }
        if (linewise)
            curPosFinal.ch = 0;
    } else {
        if (blockwise) {
            setCursor(cur);
            for (int i = 0; i < blockLines.size(); ++i) {
                const int line = cur.line + i;
                if (line > lastLine())
                    replaceRange(QStringLiteral("\n"), Pos(line, 0)); // at the end of the document
                const int lastCh = lineLength(line);
                if (lastCh < cur.ch)
                    replaceRange(QString(cur.ch - lastCh, u' '), Pos(line, lastCh)); // extendLineToColumn
            }
            setCursor(cur);
            selectBlock(Pos(cur.line + int(blockLines.size()) - 1, cur.ch));
            replaceSelections(blockLines);
            curPosFinal = cur;
        } else {
            replaceRange(text, cur);
            if (linewise) {
                const int line = args.after ? cur.line + 1 : cur.line;
                curPosFinal = Pos(line, firstNonWS(getLine(line)));
            } else {
                curPosFinal = cur;
                if (!text.contains(u'\n'))
                    curPosFinal.ch += int(text.size()) - (args.after ? 1 : 0);
            }
        }
    }
    if (m_visualMode)
        exitVisualMode(false);
    setCursor(curPosFinal);
}

void VimEngine::actUndo(Args& args)
{
    // Obsidian: after an undo the cursor goes to the start of the changed text, then vim clips it
    for (int i = 0; i < std::max(1, args.repeat); ++i) {
        m_changeStart.reset();
        m_trackChangeStart = true;
        m_busy++;
        const bool ok = m_view->undo();
        m_busy--;
        m_trackChangeStart = false;
        if (!ok)
            break;
        if (m_changeStart)
            setCursor(posFromIndex(*m_changeStart));
    }
    setCursor(clipCursorToContent(getCursor("start")));
}

void VimEngine::actRedo(Args& args)
{
    for (int i = 0; i < std::max(1, args.repeat); ++i) {
        m_changeStart.reset();
        m_trackChangeStart = true;
        m_busy++;
        const bool ok = m_view->redo();
        m_busy--;
        m_trackChangeStart = false;
        if (!ok)
            break;
        if (m_changeStart)
            setCursor(posFromIndex(*m_changeStart));
    }
    if (somethingSelected() && !m_visualMode)
        setCursor(getCursor("start"));
}

void VimEngine::actInsertRegister(Args& args)
{
    const QString text = G().registerController.contents(args.selectedCharacter).text;
    if (!text.isEmpty())
        replaceSelection(text);
}

void VimEngine::actOneNormalCommand()
{
    exitInsertMode(true);
    m_insertModeReturn = true;
}

void VimEngine::actReplace(Args& args)
{
    const QString replaceWith = args.selectedCharacter;
    Pos curStart = getCursor();
    Pos curEnd;
    const QList<Range> selections = listSelections();
    if (m_visualMode) {
        curStart = getCursor("start");
        curEnd = getCursor("end");
    } else {
        const QString line = getLine(curStart.line);
        int replaceTo = curStart.ch + args.repeat;
        if (replaceTo > line.size())
            replaceTo = int(line.size());
        curEnd = Pos(curStart.line, replaceTo);
    }
    const auto np = updateSelectionForSurrogateCharacters(curStart, curEnd);
    curStart = np.first;
    curEnd = np.second;
    if (replaceWith == u"\n") {
        if (!m_visualMode)
            replaceRange(QString(), curStart, curEnd);
        newlineAndIndent();
        return;
    }
    static const QRegularExpression surrogatePair(QStringLiteral("[\\x{D800}-\\x{DBFF}][\\x{DC00}-\\x{DFFF}]"));
    auto fill = [&](QString s) {
        // every character becomes the new one, line breaks stay
        QString out;
        for (qsizetype i = 0; i < s.size(); ++i) {
            if (s.at(i) == u'\n') {
                out += u'\n';
            } else {
                if (s.at(i).isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate())
                    ++i;
                out += replaceWith;
            }
        }
        return out;
    };
    if (m_visualBlock) {
        // tabs are split in visual block before replacing
        QString sel = getSelection();
        sel.replace(QStringLiteral("\t"), QString(tabSize(), u' '));
        replaceSelections(fill(sel).split(u'\n'));
    } else {
        replaceRange(fill(getRange(curStart, curEnd)), curStart, curEnd);
    }
    if (m_visualMode) {
        curStart = cursorIsBefore(selections[0].anchor, selections[0].head) ? selections[0].anchor : selections[0].head;
        setCursor(curStart);
        exitVisualMode(false);
    } else {
        setCursor(offsetCursor(curEnd, 0, -1));
    }
}

void VimEngine::actIncrementNumberToken(Args& args)
{
    const Pos cur = getCursor();
    const QString lineStr = getLine(cur.line);
    static const QRegularExpression re(QStringLiteral("(-?)(?:(0x)([\\da-f]+)|(0b|0|)(\\d+))"), QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match;
    int start = 0, end = 0;
    bool found = false;
    for (auto it = re.globalMatch(lineStr); it.hasNext();) {
        match = it.next();
        start = int(match.capturedStart());
        end = int(match.capturedEnd());
        if (cur.ch < end) {
            found = true;
            break;
        }
    }
    if (!args.backtrack && end <= cur.ch)
        return;
    if (!found)
        return;
    const QString baseStr = match.hasCaptured(2) ? match.captured(2) : match.captured(4);
    const QString digits = match.hasCaptured(3) ? match.captured(3) : match.captured(5);
    const int increment = args.increase ? 1 : -1;
    const QString lb = baseStr.toLower();
    const int base = lb == u"0b" ? 2 : lb == u"0" ? 8 : lb == u"0x" ? 16 : 10;
    bool ok = false;
    qlonglong number = QString(match.captured(1) + digits).toLongLong(&ok, base);
    if (!ok) {
        // JS parseInt stops at the first invalid digit ("09" in octal is 0)
        QString valid;
        for (QChar c : digits) {
            bool dok = false;
            QString(c).toInt(&dok, base);
            if (!dok)
                break;
            valid += c;
        }
        number = valid.isEmpty() ? 0 : QString(match.captured(1) + valid).toLongLong(nullptr, base);
    }
    number += qlonglong(increment) * args.repeat;
    QString numberStr = QString::number(number, base);
    QString zeroPadding;
    if (!baseStr.isEmpty())
        zeroPadding = QString(std::max<qsizetype>(0, digits.size() - numberStr.size() + match.captured(1).size()), u'0');
    if (numberStr.startsWith(u'-'))
        numberStr = u'-' + baseStr + zeroPadding + numberStr.mid(1);
    else
        numberStr = baseStr + zeroPadding + numberStr;
    replaceRange(numberStr, Pos(cur.line, start), Pos(cur.line, end));
    setCursor(Pos(cur.line, start + int(numberStr.size()) - 1));
}

void VimEngine::actRepeatLastEdit(Args& args)
{
    if (!m_lastEditInputState)
        return;
    int repeat = args.repeat;
    if (repeat && args.repeatIsExplicit)
        m_lastEditInputState->repeatOverride = repeat;
    else
        repeat = m_lastEditInputState->repeatOverride.value_or(repeat);
    repeatLastEdit(repeat, false);
}

void VimEngine::actIndentLine(Args& args)
{
    // insert mode <C-t> / <C-d>: indentMore / indentLess on the selected lines
    const QList<Range> sels = listSelections();
    Args a;
    a.indentRight = args.indentRight;
    const bool wasVisual = m_visualMode;
    m_visualMode = false;
    QList<Range> ranges;
    for (const Range& r : sels)
        ranges.push_back(r);
    const Pos cur = getCursor();
    const int before = lineLength(cur.line);
    opIndent(a, ranges);
    m_visualMode = wasVisual;
    // keep the cursor on the same text
    setCursor(Pos(cur.line, std::max(0, cur.ch + lineLength(cur.line) - before)));
}

void VimEngine::actFold(Args& args)
{
    const int line = getCursor().line;
    if (args.fold == u"za")
        m_view->toggleFoldLine(line);
    else if (args.fold == u"zo")
        m_view->unfoldAtSelection();
    else if (args.fold == u"zc")
        m_view->foldAtSelection();
    else if (args.fold == u"zR")
        m_view->unfoldAll();
    else if (args.fold == u"zM")
        m_view->foldAll();
    setCursor(clipCursorToContent(getCursor()));
}

} // namespace oc::vim
