// Motions, word/paragraph/sentence/bracket scanning and text objects (vim.js "motions" and their
// helpers).
#include "editor/vim/vim_engine.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"

#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBoundaryFinder>

#include <algorithm>

namespace oc::vim {

namespace {
VimGlobal& G() { return VimGlobal::instance(); }

QChar charAt(const QString& s, int i) { return i >= 0 && i < s.size() ? s.at(i) : QChar(); }

bool isSpaceCh(QChar c) { return !c.isNull() && c.isSpace(); }

// CodeMirror.isWordChar in Obsidian: [\w\p{Alphabetic}\p{Number}_] (a lone surrogate is not)
bool isWordChar(QChar c)
{
    if (c.isNull() || c.isSurrogate())
        return false;
    if (c == u'_' || c.isLetterOrNumber())
        return true;
    const QChar::Category cat = c.category();
    return cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_NonSpacing;
}

// vim.js wordCharTest / bigWordCharTest
bool wordTest0(QChar c) { return isWordChar(c); }
bool wordTest1(QChar c) { return !c.isNull() && !isWordChar(c) && !c.isSpace(); }
bool bigWordTest(QChar c) { return !c.isNull() && !c.isSpace(); }

int findFirstNonWhiteSpaceCharacter(const QString& text)
{
    for (int i = 0; i < text.size(); ++i)
        if (!text.at(i).isSpace())
            return i;
    return int(text.size());
}

bool isEndOfSentenceSymbol(QChar c) { return c == u'.' || c == u'?' || c == u'!'; }
// vim.js isWhiteSpaceString(line[i]): out of range is `undefined`, which is not whitespace
bool isWhiteSpaceChar(QChar c) { return !c.isNull() && c.isSpace(); }

// charIdxInLine with JS indexOf/lastIndexOf semantics
int charIdxInLine(int start, const QString& line, const QString& character, bool forward, bool includeChar)
{
    int idx;
    if (forward) {
        idx = int(line.indexOf(character, std::max(0, start + 1)));
        if (idx != -1 && !includeChar)
            idx -= 1;
    } else {
        const int from = start - 1;
        if (from < 0)
            idx = line.startsWith(character) ? 0 : -1;
        else
            idx = int(line.lastIndexOf(character, from));
        if (idx != -1 && !includeChar)
            idx += 1;
    }
    return idx;
}
} // namespace

Pos VimEngine::moveToFirstNonWhiteSpaceCharacter(Pos head) const
{
    return Pos(head.line, findFirstNonWhiteSpaceCharacter(getLine(head.line)));
}

double VimEngine::charCoordsLeft(Pos p) const
{
    return m_view->layout().caretRect(indexFromPos(p)).x();
}

Pos VimEngine::coordsChar(double x, double y) const
{
    DocLayout& l = m_view->layout();
    if (y < 0)
        return Pos(0, 0);
    if (y >= l.totalHeight())
        return posFromIndex(text().length());
    return posFromIndex(l.hitTest(QPointF(x, y)));
}

int VimEngine::getUserVisibleTop() const
{
    DocLayout& l = m_view->layout();
    const double y = std::max(0.0, m_view->scrollTop() + 6);
    return std::min(l.lineAtY(y), lastLine());
}

int VimEngine::getUserVisibleBottom() const
{
    DocLayout& l = m_view->layout();
    const double y = std::max(0.0, m_view->scrollTop() + m_view->viewport()->height() - 10);
    if (y >= l.totalHeight())
        return lastLine();
    return std::min(l.lineAtY(y), lastLine());
}

void VimEngine::recordLastCharacterSearch(int increment, const Args& args)
{
    G().lastCharacterSearch.increment = increment;
    G().lastCharacterSearch.forward = args.forward;
    G().lastCharacterSearch.selectedCharacter = args.selectedCharacter;
}

// ---------------------------------------------------------------------------------------------
// motions
// ---------------------------------------------------------------------------------------------
VimEngine::MotionResult VimEngine::runMotion(Motion m, Pos head, Args& args, const InputState& prevInputState)
{
    switch (m) {
    case Motion::MoveToTopLine: {
        const int line = std::min(getUserVisibleTop() + args.repeat - 1, lastLine());
        return MotionResult::pos(Pos(line, findFirstNonWhiteSpaceCharacter(getLine(line))));
    }
    case Motion::MoveToMiddleLine: {
        const int line = (getUserVisibleTop() + getUserVisibleBottom()) / 2;
        return MotionResult::pos(Pos(line, findFirstNonWhiteSpaceCharacter(getLine(line))));
    }
    case Motion::MoveToBottomLine: {
        const int line = std::max(getUserVisibleBottom() - args.repeat + 1, 0);
        return MotionResult::pos(Pos(line, findFirstNonWhiteSpaceCharacter(getLine(line))));
    }
    case Motion::ExpandToLine:
        // forward to the end of the line, and further lines with a count
        return MotionResult::pos(Pos(head.line + args.repeat - 1, kEol));
    case Motion::FindNext: {
        if (!G().query)
            return MotionResult::none();
        bool prev = !args.forward;
        prev = G().isReversed ? !prev : prev;
        const Query q = *G().query;
        highlightSearchMatches(q);
        if (auto p = findNext(prev, q, args.repeat))
            return MotionResult::pos(*p);
        return MotionResult::none();
    }
    case Motion::FindAndSelectNextInclusive: {
        if (!G().query)
            return MotionResult::none();
        bool prev = !args.forward;
        prev = G().isReversed ? !prev : prev;
        const auto next = findNextFromAndToInclusive(prev, *G().query, args.repeat);
        if (!next)
            return MotionResult::none();
        if (prevInputState.op != Operator::None)
            return MotionResult::range(next->first, next->second);
        const Pos from = next->first;
        const Pos to(next->second.line, next->second.ch - 1);
        if (m_visualMode) {
            if (m_visualLine || m_visualBlock) {
                m_visualLine = false;
                m_visualBlock = false;
                signalModeChange();
            }
            const Pos anchor = m_sel.anchor;
            if (G().isReversed)
                return args.forward ? MotionResult::range(anchor, from) : MotionResult::range(anchor, to);
            return args.forward ? MotionResult::range(anchor, to) : MotionResult::range(anchor, from);
        }
        m_visualMode = true;
        m_visualLine = false;
        m_visualBlock = false;
        signalModeChange();
        return prev ? MotionResult::range(to, from) : MotionResult::range(from, to);
    }
    case Motion::GoToMark: {
        const auto pos = getMarkPos(args.selectedCharacter);
        if (!pos)
            return MotionResult::none();
        return MotionResult::pos(args.linewise ? Pos(pos->line, findFirstNonWhiteSpaceCharacter(getLine(pos->line))) : *pos);
    }
    case Motion::MoveToOtherHighlightedEnd:
        if (m_visualBlock && args.sameLine) {
            return MotionResult::range(clipCursorToContent(Pos(m_sel.anchor.line, m_sel.head.ch)),
                                       clipCursorToContent(Pos(m_sel.head.line, m_sel.anchor.ch)));
        }
        return MotionResult::range(m_sel.head, m_sel.anchor);
    case Motion::JumpToMark: {
        Pos best = head;
        for (int i = 0; i < args.repeat; ++i) {
            const Pos cursor = best;
            for (auto it = m_marks.cbegin(); it != m_marks.cend(); ++it) {
                const QString& key = it.key();
                if (key.size() != 1 || !(key.at(0) >= u'a' && key.at(0) <= u'z'))
                    continue;
                const auto markOpt = findBookmark(it.value());
                if (!markOpt)
                    continue;
                const Pos mark = *markOpt;
                const bool isWrongDirection = args.forward ? cursorIsBefore(mark, cursor) : cursorIsBefore(cursor, mark);
                if (isWrongDirection)
                    continue;
                if (args.linewise && mark.line == cursor.line)
                    continue;
                const bool equal = cursor == best;
                const bool between = args.forward ? cursorIsBetween(cursor, mark, best) : cursorIsBetween(best, mark, cursor);
                if (equal || between)
                    best = mark;
            }
        }
        if (args.linewise)
            best = Pos(best.line, findFirstNonWhiteSpaceCharacter(getLine(best.line)));
        return MotionResult::pos(best);
    }
    case Motion::MoveByCharacters:
        return MotionResult::pos(moveByCharacters(head, args));
    case Motion::MoveByLines:
        return MotionResult::pos(moveByLines(head, args));
    case Motion::MoveByDisplayLines:
        return MotionResult::pos(moveByDisplayLines(head, args));
    case Motion::MoveByPage: {
        // CodeMirror's page motion: the cursor moves a viewport height
        DocLayout& l = m_view->layout();
        const qsizetype pos = indexFromPos(head);
        const QRectF cr = l.caretRect(pos);
        const double h = m_view->viewport()->height();
        const double y = cr.center().y() + (args.forward ? 1 : -1) * h * args.repeat;
        if (y < 0)
            return MotionResult::pos(Pos(0, 0));
        if (y >= l.totalHeight())
            return MotionResult::pos(posFromIndex(text().length()));
        return MotionResult::pos(posFromIndex(l.hitTest(QPointF(cr.x(), y))));
    }
    case Motion::MoveByParagraph:
        return MotionResult::pos(findParagraphMotion(head, args.repeat, args.forward ? 1 : -1));
    case Motion::MoveBySentence:
        return MotionResult::pos(findSentence(head, args.repeat, args.forward ? 1 : -1));
    case Motion::MoveByScroll: {
        DocLayout& l = m_view->layout();
        int repeat = args.repeat;
        if (!repeat) {
            const StyleSheetPtr ss = l.styleSheet();
            const double lineH = ss ? ss->line(StyleId::Normal).lineBox : 24;
            repeat = int(m_view->viewport()->height() / (2 * lineH));
        }
        const double origTop = l.caretRect(indexFromPos(head)).top();
        args.repeat = std::max(1, repeat);
        const Pos curEnd = moveByDisplayLines(head, args);
        const double destTop = l.caretRect(indexFromPos(curEnd)).top();
        m_view->setScrollTop(m_view->scrollTop() + destTop - origTop);
        return MotionResult::pos(curEnd);
    }
    case Motion::MoveByWords:
        return MotionResult::pos(moveToWord(head, args.repeat, args.forward, args.wordEnd, args.bigWord));
    case Motion::MoveTillCharacter: {
        const auto curEnd = moveToCharacter(args.repeat, args.forward, args.selectedCharacter, head);
        const int increment = args.forward ? -1 : 1;
        recordLastCharacterSearch(increment, args);
        if (!curEnd)
            return MotionResult::none();
        return MotionResult::pos(Pos(curEnd->line, curEnd->ch + increment));
    }
    case Motion::MoveToCharacter: {
        recordLastCharacterSearch(0, args);
        return MotionResult::pos(moveToCharacter(args.repeat, args.forward, args.selectedCharacter, head).value_or(head));
    }
    case Motion::MoveToSymbol:
        return MotionResult::pos(findSymbol(args.repeat, args.forward, args.selectedCharacter));
    case Motion::MoveToColumn:
        m_lastHPos = args.repeat - 1;
        m_lastHSPos = charCoordsLeft(head);
        return MotionResult::pos(clipCursorToContent(Pos(getCursor().line, args.repeat - 1)));
    case Motion::MoveToEol:
        return MotionResult::pos(moveToEolMotion(head, args, false));
    case Motion::MoveToFirstNonWhiteSpaceCharacter:
        return MotionResult::pos(moveToFirstNonWhiteSpaceCharacter(head));
    case Motion::MoveToMatchedSymbol: {
        const Pos p = moveToMatchedSymbol(head);
        if (p.line < 0)
            return MotionResult::none();
        return MotionResult::pos(p);
    }
    case Motion::MoveToStartOfLine:
        return MotionResult::pos(Pos(head.line, 0));
    case Motion::MoveToLineOrEdgeOfDocument: {
        int lineNum = args.forward ? lastLine() : firstLine();
        if (args.repeatIsExplicit)
            lineNum = args.repeat - 1;
        return MotionResult::pos(Pos(lineNum, findFirstNonWhiteSpaceCharacter(getLine(lineNum))));
    }
    case Motion::MoveToStartOfDisplayLine: {
        const qsizetype p = m_view->layout().visualLineStart(indexFromPos(head));
        return MotionResult::pos(posFromIndex(p));
    }
    case Motion::MoveToEndOfDisplayLine: {
        const qsizetype start = text().lineStart(head.line);
        qsizetype p = m_view->layout().visualLineEnd(indexFromPos(head));
        if (p > start)
            p -= 1;
        return MotionResult::pos(posFromIndex(p));
    }
    case Motion::TextObjectManipulation:
        return textObjectManipulation(head, args);
    case Motion::RepeatLastCharacterSearch: {
        const auto& last = G().lastCharacterSearch;
        const bool forward = args.forward == last.forward;
        const int increment = (last.increment ? 1 : 0) * (forward ? -1 : 1);
        // vim.js nudges the cursor (cm.moveH) so a repeated "t" doesn't get stuck
        const Pos cur = getCursor();
        const Pos start = Pos(cur.line, std::clamp(cur.ch - increment, 0, lineLength(cur.line)));
        args.inclusive = forward;
        const auto curEnd = moveToCharacter(args.repeat, forward, last.selectedCharacter, start);
        if (!curEnd)
            return MotionResult::pos(head);
        return MotionResult::pos(Pos(curEnd->line, curEnd->ch + increment));
    }
    case Motion::MoveToLastNonBlank: {
        const int line = std::min(head.line + args.repeat - 1, lastLine());
        const QString text = getLine(line);
        int ch = int(text.size()) - 1;
        while (ch > 0 && text.at(ch).isSpace())
            --ch;
        return MotionResult::pos(Pos(line, std::max(0, ch)));
    }
    case Motion::None:
        break;
    }
    return MotionResult::none();
}

Pos VimEngine::moveByCharacters(Pos head, const Args& args) const
{
    // Like CodeMirror-vim (cur.ch +/- repeat, clipped later), but by grapheme clusters. Past the
    // end of the line the column keeps counting so operators ("3x") reach the line end.
    const QString line = getLine(head.line);
    const int len = int(line.size());
    int ch = std::clamp(head.ch, 0, len);
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, line);
    for (int i = 0; i < args.repeat; ++i) {
        if (args.forward) {
            if (ch >= len) {
                ch += args.repeat - i;
                break;
            }
            f.setPosition(ch);
            const qsizetype n = f.toNextBoundary();
            ch = n < 0 ? len : int(n);
        } else {
            if (ch <= 0)
                break;
            f.setPosition(ch);
            const qsizetype n = f.toPreviousBoundary();
            ch = n < 0 ? 0 : int(n);
        }
    }
    return Pos(head.line, ch);
}

Pos VimEngine::moveByLines(Pos head, Args& args)
{
    const Pos cur = head;
    int endCh = cur.ch;
    // keep the horizontal position through vertical motions (and "$")
    switch (m_lastMotion) {
    case Motion::MoveByLines:
    case Motion::MoveByDisplayLines:
    case Motion::MoveByScroll:
    case Motion::MoveToColumn:
    case Motion::MoveToEol:
        endCh = m_lastHPos;
        break;
    default:
        m_lastHPos = endCh;
    }
    const int repeat = args.repeat + args.repeatOffset;
    int line = args.forward ? cur.line + repeat : cur.line - repeat;
    const int first = firstLine();
    const int last = lastLine();
    // Folded lines are skipped (CodeMirror's vertical motion jumps over them).
    {
        const DocLayout& l = m_view->layout();
        int posV = cur.line;
        const int dir = args.forward ? 1 : -1;
        for (int i = 0; i < std::abs(repeat) && posV + dir >= first && posV + dir <= last; ++i) {
            posV += dir;
            while (l.isHidden(posV) && posV + dir >= first && posV + dir <= last)
                posV += dir;
        }
        const bool hasMarkedText = args.forward ? posV > line : posV < line;
        if (hasMarkedText)
            line = posV;
    }
    // vim goes to the start/end of the line when there's no line to move to
    if (line < first && cur.line == first)
        return moveToStartOfLine(head);
    if (line > last && cur.line == last)
        return moveToEolMotion(head, args, true);
    if (args.toFirstChar) {
        endCh = findFirstNonWhiteSpaceCharacter(getLine(line));
        m_lastHPos = endCh;
    }
    m_lastHSPos = charCoordsLeft(clipPos(Pos(line, endCh)));
    return Pos(line, endCh);
}

Pos VimEngine::moveByDisplayLines(Pos head, Args& args)
{
    const Pos cur = head;
    switch (m_lastMotion) {
    case Motion::MoveByDisplayLines:
    case Motion::MoveByScroll:
    case Motion::MoveByLines:
    case Motion::MoveToColumn:
    case Motion::MoveToEol:
        break;
    default:
        m_lastHSPos = charCoordsLeft(cur);
    }
    DocLayout& l = m_view->layout();
    qsizetype pos = indexFromPos(cur);
    for (int i = 0; i < args.repeat; ++i) {
        const qsizetype np = l.verticalMove(pos, m_lastHSPos, args.forward ? 1 : -1);
        if (np == pos)
            break; // hit the side: stay on the first/last display line
        pos = np;
    }
    const Pos res = posFromIndex(pos);
    m_lastHPos = res.ch;
    return res;
}

Pos VimEngine::moveToEolMotion(Pos head, const Args& args, bool keepHPos)
{
    const Pos retval(head.line + args.repeat - 1, kEol);
    Pos end = clipPos(retval);
    end.ch--;
    if (!keepHPos) {
        m_lastHPos = kEol;
        m_lastHSPos = charCoordsLeft(clipPos(end));
    }
    return retval;
}

std::optional<Pos> VimEngine::moveToCharacter(int repeat, bool forward, const QString& character, Pos head) const
{
    if (character.isEmpty())
        return std::nullopt;
    const Pos cur = head;
    int start = cur.ch;
    int idx = -1;
    for (int i = 0; i < repeat; ++i) {
        const QString line = getLine(cur.line);
        idx = charIdxInLine(start, line, character, forward, true);
        if (idx == -1)
            return std::nullopt;
        start = idx;
    }
    return Pos(cur.line, idx);
}

// ---------------------------------------------------------------------------------------------
// words
// ---------------------------------------------------------------------------------------------
std::optional<VimEngine::Word> VimEngine::findWord(Pos cur, bool forward, bool bigWord, bool emptyLineIsWord) const
{
    int lineNum = cur.line;
    int pos = cur.ch;
    QString line = getLine(lineNum);
    const int dir = forward ? 1 : -1;
    using Test = bool (*)(QChar);
    const QList<Test> tests = bigWord ? QList<Test>{bigWordTest} : QList<Test>{wordTest0, wordTest1};
    auto isLine = [&](int l) { return l >= firstLine() && l <= lastLine(); };

    if (emptyLineIsWord && line.isEmpty()) {
        lineNum += dir;
        if (!isLine(lineNum))
            return std::nullopt;
        line = getLine(lineNum);
        pos = forward ? 0 : int(line.size());
    }
    while (true) {
        if (emptyLineIsWord && line.isEmpty())
            return Word{0, 0, lineNum};
        const int stop = dir > 0 ? int(line.size()) : -1;
        int wordStart = stop, wordEnd = stop;
        // find the bounds of the next word
        while (pos != stop) {
            bool foundWord = false;
            for (int i = 0; i < tests.size() && !foundWord; ++i) {
                if (tests[i](charAt(line, pos))) {
                    wordStart = pos;
                    while (pos != stop && tests[i](charAt(line, pos)))
                        pos += dir;
                    wordEnd = pos;
                    foundWord = wordStart != wordEnd;
                    if (wordStart == cur.ch && lineNum == cur.line && wordEnd == wordStart + dir) {
                        continue; // started at the end of a word: find the next one
                    }
                    return Word{std::min(wordStart, wordEnd + 1), std::max(wordStart, wordEnd), lineNum};
                }
            }
            if (!foundWord)
                pos += dir;
        }
        lineNum += dir;
        if (!isLine(lineNum))
            return std::nullopt;
        line = getLine(lineNum);
        pos = dir > 0 ? 0 : int(line.size());
    }
}

Pos VimEngine::moveToWord(Pos cur, int repeat, bool forward, bool wordEnd, bool bigWord) const
{
    const Pos curStart = cur;
    QList<Word> words;
    if ((forward && !wordEnd) || (!forward && wordEnd))
        repeat++;
    const bool emptyLineIsWord = !(forward && wordEnd); // for "e", empty lines are not words
    for (int i = 0; i < repeat; ++i) {
        const auto word = findWord(cur, forward, bigWord, emptyLineIsWord);
        if (!word) {
            const int eodCh = lineLength(lastLine());
            words.push_back(forward ? Word{eodCh, eodCh, lastLine()} : Word{0, 0, 0});
            break;
        }
        words.push_back(*word);
        cur = Pos(word->line, forward ? word->to - 1 : word->from);
    }
    const bool shortCircuit = words.size() != repeat;
    const Word firstWord = words.first();
    Word lastWord = words.takeLast();
    if (forward && !wordEnd) {
        // w: not started in the middle of a word -> discard the extra word at the end
        if (!shortCircuit && (firstWord.from != curStart.ch || firstWord.line != curStart.line))
            lastWord = words.takeLast();
        return Pos(lastWord.line, lastWord.from);
    }
    if (forward && wordEnd)
        return Pos(lastWord.line, lastWord.to - 1);
    if (!forward && wordEnd) {
        // ge
        if (!shortCircuit && (firstWord.to != curStart.ch || firstWord.line != curStart.line))
            lastWord = words.takeLast();
        return Pos(lastWord.line, lastWord.to);
    }
    return Pos(lastWord.line, lastWord.from); // b
}

std::optional<VimEngine::Span> VimEngine::expandWordUnderCursor(bool inclusive, bool innerWord, bool bigWord, bool noSymbol, bool multiline,
                                                                std::optional<Pos> cursor) const
{
    Pos cur;
    if (cursor) {
        cur = *cursor;
    } else {
        // getHead: with one character selected the "real" head is the left end
        cur = getCursor("head");
        if (getSelection().size() == 1)
            cur = cursorMin(cur, getCursor("anchor"));
    }
    const QString line = getLine(cur.line);
    QString endLine = line;
    const int startLineNumber = cur.line;
    int endLineNumber = startLineNumber;
    int idx = cur.ch;
    std::optional<Word> wordOnNextLine;
    using Test = bool (*)(QChar);
    Test test = noSymbol ? wordTest0 : bigWordTest;
    if (innerWord && isSpaceCh(charAt(line, idx))) {
        test = [](QChar c) { return isSpaceCh(c); };
    } else {
        while (!test(charAt(line, idx))) {
            idx++;
            if (idx >= line.size()) {
                if (!multiline)
                    return std::nullopt;
                idx--;
                wordOnNextLine = findWord(cur, true, bigWord, true);
                break;
            }
        }
        if (bigWord) {
            test = bigWordTest;
        } else {
            test = wordTest0;
            if (!test(charAt(line, idx)))
                test = wordTest1;
        }
    }
    int end = idx, start = idx;
    while (test(charAt(line, end)) && end < line.size())
        end++;
    while (start >= 0 && test(charAt(line, start)))
        start--;
    start++;
    if (wordOnNextLine) {
        end = wordOnNextLine->to;
        endLineNumber = wordOnNextLine->line;
        endLine = getLine(endLineNumber);
        if (endLine.isEmpty() && end == 0)
            end++;
    } else {
        while (test(charAt(line, end)) && end < line.size())
            end++;
    }
    if (inclusive) {
        // trailing whitespace if there is some, else leading whitespace (not indentation)
        const int wordEnd = end;
        const bool startsWithSpace = cur.ch <= start && isSpaceCh(charAt(line, cur.ch));
        if (!startsWithSpace)
            while (isSpaceCh(charAt(endLine, end)) && end < endLine.size())
                end++;
        if (wordEnd == end || startsWithSpace) {
            const int wordStart = start;
            while (start > 0 && isSpaceCh(charAt(line, start - 1)))
                start--;
            if (!start && !startsWithSpace)
                start = wordStart;
        }
    }
    return Span{Pos(startLineNumber, start), Pos(endLineNumber, end)};
}

// ---------------------------------------------------------------------------------------------
// paragraphs and sentences
// ---------------------------------------------------------------------------------------------
Pos VimEngine::findParagraphMotion(Pos head, int repeat, int dir) const { return findParagraph(head, repeat, dir, false).start; }

VimEngine::Span VimEngine::findParagraph(Pos head, int repeat, int dir, bool inclusive) const
{
    int line = head.line;
    const int min = firstLine();
    const int max = lastLine();
    auto isEmpty = [&](int i) { return getLine(i).isEmpty(); };
    auto isBoundary = [&](int i, int d, bool any) {
        if (any)
            return isEmpty(i) != isEmpty(i + d);
        return !isEmpty(i) && isEmpty(i + d);
    };
    int i = line;
    if (dir) {
        while (min <= i && i <= max && repeat > 0) {
            if (isBoundary(i, dir, false))
                repeat--;
            i += dir;
        }
        return Span{Pos(i, 0), Pos(i, 0)};
    }
    if (m_visualLine && isBoundary(line, 1, true)) {
        const Pos anchor = m_sel.anchor;
        if (isBoundary(anchor.line, -1, true)) {
            if (!inclusive || anchor.line != line)
                line += 1;
        }
    }
    bool startState = isEmpty(line);
    for (i = line; i <= max && repeat; i++) {
        if (isBoundary(i, 1, true)) {
            if (!inclusive || isEmpty(i) != startState)
                repeat--;
        }
    }
    const Pos end(i, 0);
    // select the boundary before the paragraph for the last one
    if (i > max && !startState)
        startState = true;
    else
        inclusive = false;
    for (i = line; i > min; i--) {
        if (!inclusive || isEmpty(i) == startState || i == line) {
            if (isBoundary(i, -1, true))
                break;
        }
    }
    return Span{Pos(i, 0), end};
}

Pos VimEngine::getSentence(Pos cur, int repeat, int dir, bool inclusive) const
{
    struct Idx {
        QString line;
        bool lineNull = false;
        int ln = 0, pos = 0, dir = 1;
    };
    auto nextChar = [](Idx& c) {
        if (c.pos + c.dir < 0 || c.pos + c.dir >= c.line.size())
            c.lineNull = true;
        else
            c.pos += c.dir;
    };
    auto ch = [](const Idx& c, int p) { return charAt(c.line, p); };
    auto forward = [&](int ln, int pos, int d) -> QPair<int, int> {
        Idx c{getLine(ln), false, ln, pos, d};
        if (c.line.isEmpty())
            return {c.ln, c.pos};
        int lastSentencePos = c.pos;
        nextChar(c); // skip the character we start on
        while (!c.lineNull) {
            lastSentencePos = c.pos;
            if (isEndOfSentenceSymbol(ch(c, c.pos))) {
                if (!inclusive)
                    return {c.ln, c.pos + 1};
                nextChar(c);
                while (!c.lineNull) {
                    if (isWhiteSpaceChar(ch(c, c.pos))) {
                        lastSentencePos = c.pos;
                        nextChar(c);
                    } else {
                        break;
                    }
                }
                return {c.ln, lastSentencePos + 1};
            }
            nextChar(c);
        }
        return {c.ln, lastSentencePos + 1};
    };
    auto reverse = [&](int ln, int pos, int d) -> QPair<int, int> {
        const QString line = getLine(ln);
        Idx c{line, false, ln, pos, d};
        if (c.line.isEmpty())
            return {c.ln, c.pos};
        int lastSentencePos = c.pos;
        nextChar(c);
        while (!c.lineNull) {
            const QChar x = ch(c, c.pos);
            if (!isWhiteSpaceChar(x) && !isEndOfSentenceSymbol(x)) {
                lastSentencePos = c.pos;
            } else if (isEndOfSentenceSymbol(x)) {
                if (!inclusive)
                    return {c.ln, lastSentencePos};
                if (isWhiteSpaceChar(ch(c, c.pos + 1)) && c.pos + 1 < c.line.size())
                    return {c.ln, c.pos + 1};
                return {c.ln, lastSentencePos};
            }
            nextChar(c);
        }
        c.line = line;
        if (inclusive && isWhiteSpaceChar(ch(c, c.pos)) && c.pos < c.line.size())
            return {c.ln, c.pos};
        return {c.ln, lastSentencePos};
    };
    QPair<int, int> idx{cur.line, cur.ch};
    while (repeat > 0) {
        idx = dir < 0 ? reverse(idx.first, idx.second, dir) : forward(idx.first, idx.second, dir);
        repeat--;
    }
    return Pos(idx.first, idx.second);
}

Pos VimEngine::findSentence(Pos cur, int repeat, int dir) const
{
    struct Idx {
        QString line;
        bool null = false;
        int ln = 0, pos = 0, dir = 1;
    };
    auto isLine = [&](int l) { return l >= firstLine() && l <= lastLine(); };
    auto nextChar = [&](Idx& idx) {
        if (idx.pos + idx.dir < 0 || idx.pos + idx.dir >= idx.line.size()) {
            idx.ln += idx.dir;
            if (!isLine(idx.ln)) {
                idx.null = true;
                return;
            }
            idx.line = getLine(idx.ln);
            idx.pos = idx.dir > 0 ? 0 : int(idx.line.size()) - 1;
        } else {
            idx.pos += idx.dir;
        }
    };
    auto forward = [&](int ln, int pos, int d) -> QPair<int, int> {
        const QString line = getLine(ln);
        bool stop = line.isEmpty();
        Idx curr{line, false, ln, pos, d};
        QPair<int, int> lastValid{curr.ln, curr.pos};
        const bool skipEmptyLines = curr.line.isEmpty();
        nextChar(curr);
        while (!curr.null) {
            lastValid = {curr.ln, curr.pos};
            const QChar c = charAt(curr.line, curr.pos);
            if (curr.line.isEmpty() && !skipEmptyLines)
                return {curr.ln, curr.pos};
            if (stop && !curr.line.isEmpty() && !isWhiteSpaceChar(c))
                return {curr.ln, curr.pos};
            if (isEndOfSentenceSymbol(c) && !stop
                && (curr.pos == curr.line.size() - 1 || isWhiteSpaceChar(charAt(curr.line, curr.pos + 1))))
                stop = true;
            nextChar(curr);
        }
        // end of document: the last non-whitespace character of the last valid line
        const QString l = getLine(lastValid.first);
        lastValid.second = 0;
        for (int i = int(l.size()) - 1; i >= 0; --i) {
            if (!isWhiteSpaceChar(l.at(i))) {
                lastValid.second = i;
                break;
            }
        }
        return lastValid;
    };
    auto reverse = [&](int ln, int pos, int d) -> QPair<int, int> {
        const QString line = getLine(ln);
        Idx curr{line, false, ln, pos, d};
        int lvLn = curr.ln;
        std::optional<int> lvPos;
        bool skipEmptyLines = curr.line.isEmpty();
        nextChar(curr);
        while (!curr.null) {
            const QChar c = charAt(curr.line, curr.pos);
            if (curr.line.isEmpty() && !skipEmptyLines) {
                if (lvPos)
                    return {lvLn, *lvPos};
                return {curr.ln, curr.pos};
            }
            if (isEndOfSentenceSymbol(c) && lvPos && !(curr.ln == lvLn && curr.pos + 1 == *lvPos))
                return {lvLn, *lvPos};
            if (!curr.line.isEmpty() && !isWhiteSpaceChar(c)) {
                skipEmptyLines = false;
                lvLn = curr.ln;
                lvPos = curr.pos;
            }
            nextChar(curr);
        }
        // start of document: the first non-whitespace character of the last valid line
        const QString l = getLine(lvLn);
        int p = 0;
        for (int i = 0; i < l.size(); ++i) {
            if (!isWhiteSpaceChar(l.at(i))) {
                p = i;
                break;
            }
        }
        return {lvLn, p};
    };
    QPair<int, int> idx{cur.line, cur.ch};
    while (repeat > 0) {
        idx = dir < 0 ? reverse(idx.first, idx.second, dir) : forward(idx.first, idx.second, dir);
        repeat--;
    }
    return Pos(idx.first, idx.second);
}

// ---------------------------------------------------------------------------------------------
// brackets, quotes, tags, symbols
// ---------------------------------------------------------------------------------------------
std::optional<Pos> VimEngine::scanForBracket(Pos where, int dir, const QString& brackets) const
{
    // CodeMirror 5 scanForBracket (as Obsidian's adapter implements it): the first unmatched
    // bracket of `brackets` in direction dir.
    static const QHash<QChar, QString> matching = {{u'(', QStringLiteral(")>")}, {u')', QStringLiteral("(<")}, {u'[', QStringLiteral("]>")},
                                                   {u']', QStringLiteral("[<")}, {u'{', QStringLiteral("}>")}, {u'}', QStringLiteral("{<")},
                                                   {u'<', QStringLiteral(">>")}, {u'>', QStringLiteral("<<")}};
    constexpr int maxScanLineLength = 10000;
    constexpr int maxScanLines = 1000;
    QList<QChar> stack;
    const int lineEnd = dir > 0 ? std::min(where.line + maxScanLines, lastLine() + 1) : std::max(firstLine() - 1, where.line - maxScanLines);
    for (int ln = where.line; ln != lineEnd; ln += dir) {
        const QString line = getLine(ln);
        if (line.isEmpty())
            continue;
        int pos = dir > 0 ? 0 : int(line.size()) - 1;
        const int end = dir > 0 ? int(line.size()) : -1;
        if (line.size() > maxScanLineLength)
            continue;
        if (ln == where.line)
            pos = where.ch - (dir < 0 ? 1 : 0);
        for (; pos != end; pos += dir) {
            if (pos < 0 || pos >= line.size())
                break;
            const QChar c = line.at(pos);
            if (!brackets.contains(c))
                continue;
            const QString m = matching.value(c);
            if (m.isEmpty())
                continue;
            if ((m.at(1) == u'>') == (dir > 0)) {
                stack.push_back(c);
            } else if (stack.isEmpty()) {
                return Pos(ln, pos);
            } else {
                stack.removeLast();
            }
        }
    }
    return std::nullopt;
}

std::optional<Pos> VimEngine::findMatchingBracket(Pos where) const
{
    // the bracket at `where` and its partner (plain text scan, like CodeMirror's matchBrackets)
    const QString line = getLine(where.line);
    const QChar c = charAt(line, where.ch);
    static const QString open = QStringLiteral("([{"), close = QStringLiteral(")]}");
    const int oi = int(open.indexOf(c)), ci = int(close.indexOf(c));
    if (oi < 0 && ci < 0)
        return std::nullopt;
    const bool forward = oi >= 0;
    const QChar mine = c;
    const QChar partner = forward ? close.at(oi) : open.at(ci);
    const qsizetype start = indexFromPos(where);
    const Text& t = text();
    int depth = 0;
    constexpr qsizetype maxScan = 10000;
    if (forward) {
        for (qsizetype p = start + 1; p < t.length() && p - start <= maxScan; ++p) {
            const QChar x = t.charAt(p);
            if (x == mine)
                depth++;
            else if (x == partner && depth-- == 0)
                return posFromIndex(p);
        }
    } else {
        for (qsizetype p = start - 1; p >= 0 && start - p <= maxScan; --p) {
            const QChar x = t.charAt(p);
            if (x == mine)
                depth++;
            else if (x == partner && depth-- == 0)
                return posFromIndex(p);
        }
    }
    return std::nullopt;
}

Pos VimEngine::moveToMatchedSymbol(Pos head) const
{
    const int line = head.line;
    int ch = head.ch;
    const QString lineText = getLine(line);
    for (; ch < lineText.size(); ch++) {
        const QChar symbol = lineText.at(ch);
        if (QStringLiteral("()[]{}").contains(symbol))
            break; // (no string/comment tokens to skip in Markdown)
    }
    if (ch < lineText.size()) {
        if (auto m = findMatchingBracket(Pos(line, ch)))
            return *m;
        return Pos(-1, -1);
    }
    return head;
}

Pos VimEngine::findSymbol(int repeat, bool forward, const QString& symb) const
{
    Pos cur = getCursor();
    const int increment = forward ? 1 : -1;
    const int endLine = forward ? lineCount() : -1;
    int line = cur.line;
    struct State {
        QString lineText;
        QChar nextCh;
        QChar lastCh;
        int index = 0;
        QChar symb;
        QChar reverseSymb;
        bool forward = true;
        int depth = 0;
        bool curMoveThrough = false;
    } st;
    st.lineText = getLine(line);
    st.nextCh = charAt(st.lineText, cur.ch);
    st.index = cur.ch;
    st.symb = symb.isEmpty() ? QChar() : symb.at(0);
    st.forward = forward;
    if (forward) {
        if (st.symb == u')')
            st.reverseSymb = u'(';
        else if (st.symb == u'}')
            st.reverseSymb = u'{';
    } else {
        if (st.symb == u'(')
            st.reverseSymb = u')';
        else if (st.symb == u'{')
            st.reverseSymb = u'}';
    }
    enum class Mode { None, Bracket, Section, Comment, Method, Preprocess } mode = Mode::None;
    switch (st.symb.unicode()) {
    case '(':
    case ')':
    case '{':
    case '}':
        mode = Mode::Bracket;
        break;
    case '[':
    case ']':
        mode = Mode::Section;
        break;
    case '*':
    case '/':
        mode = Mode::Comment;
        break;
    case 'm':
    case 'M':
        mode = Mode::Method;
        break;
    case '#':
        mode = Mode::Preprocess;
        break;
    default:
        return cur;
    }
    // init
    if (mode == Mode::Section) {
        st.curMoveThrough = true;
        st.symb = (forward ? u']' : u'[') == st.symb ? u'{' : u'}';
    } else if (mode == Mode::Method) {
        st.symb = st.symb == u'm' ? u'{' : u'}';
        st.reverseSymb = st.symb == u'{' ? u'}' : u'{';
    } else if (mode == Mode::Preprocess) {
        st.index = 0;
    }
    auto isComplete = [&]() -> bool {
        switch (mode) {
        case Mode::Bracket:
            if (st.nextCh == st.symb) {
                st.depth++;
                if (st.depth >= 1)
                    return true;
            } else if (st.nextCh == st.reverseSymb) {
                st.depth--;
            }
            return false;
        case Mode::Section:
            return st.index == 0 && st.nextCh == st.symb;
        case Mode::Comment: {
            const bool found = st.lastCh == u'*' && st.nextCh == u'/';
            st.lastCh = st.nextCh;
            return found;
        }
        case Mode::Method:
            return st.nextCh == st.symb;
        case Mode::Preprocess:
            if (st.nextCh == u'#') {
                static const QRegularExpression tok(QStringLiteral("^#(\\w+)"));
                const QString token = tok.match(st.lineText).captured(1);
                if (token == u"endif") {
                    if (st.forward && st.depth == 0)
                        return true;
                    st.depth++;
                } else if (token == u"if") {
                    if (!st.forward && st.depth == 0)
                        return true;
                    st.depth--;
                }
                if (token == u"else" && st.depth == 0)
                    return true;
            }
            return false;
        default:
            return false;
        }
    };
    while (line != endLine && repeat) {
        st.index += increment;
        st.nextCh = charAt(st.lineText, st.index);
        if (st.nextCh.isNull()) {
            line += increment;
            st.lineText = getLine(line);
            if (increment > 0) {
                st.index = 0;
            } else {
                const int lineLen = int(st.lineText.size());
                st.index = lineLen > 0 ? lineLen - 1 : 0;
            }
            st.nextCh = charAt(st.lineText, st.index);
        }
        if (isComplete()) {
            cur = Pos(line, st.index);
            repeat--;
        }
    }
    if (!st.nextCh.isNull() || st.curMoveThrough)
        return Pos(line, st.index);
    return cur;
}

std::optional<VimEngine::Span> VimEngine::selectCompanionObject(Pos head, QChar symb, bool inclusive) const
{
    QString brackets;
    QChar openSym;
    switch (symb.unicode()) {
    case '(':
    case ')':
        brackets = QStringLiteral("()");
        openSym = u'(';
        break;
    case '[':
    case ']':
        brackets = QStringLiteral("[]");
        openSym = u'[';
        break;
    case '{':
    case '}':
        brackets = QStringLiteral("{}");
        openSym = u'{';
        break;
    case '<':
    case '>':
        brackets = QStringLiteral("<>");
        openSym = u'<';
        break;
    default:
        return std::nullopt;
    }
    const QChar curChar = charAt(getLine(head.line), head.ch);
    // scanForBracket needs an offset when the cursor is on the opening bracket
    const int offset = curChar == openSym ? 1 : 0;
    auto start = scanForBracket(Pos(head.line, head.ch + offset), -1, brackets);
    auto end = scanForBracket(Pos(head.line, head.ch + offset), 1, brackets);
    if (!start || !end)
        return std::nullopt;
    Pos s = *start, e = *end;
    if ((s.line == e.line && s.ch > e.ch) || s.line > e.line)
        std::swap(s, e);
    if (inclusive)
        e.ch += 1;
    else
        s.ch += 1;
    return Span{s, e};
}

VimEngine::Span VimEngine::findBeginningAndEnd(Pos head, QChar symb, bool inclusive) const
{
    Pos cur = head;
    const QString line = getLine(cur.line);
    std::optional<int> start, end;
    const int firstIndex = int(line.indexOf(symb));
    // look backwards for the beginning first, but if the cursor is before the first symbol, move to it
    if (cur.ch < firstIndex) {
        cur.ch = firstIndex;
    } else if (firstIndex < cur.ch && charAt(line, cur.ch) == symb) {
        // on the closing symbol (Markdown has no string tokens to tell an opening one apart)
        end = cur.ch;
        --cur.ch;
    }
    if (charAt(line, cur.ch) == symb && !end) {
        start = cur.ch + 1;
    } else {
        for (int i = cur.ch; i > -1 && !start; i--)
            if (charAt(line, i) == symb)
                start = i + 1;
    }
    if (start && !end) {
        for (int i = *start; i < line.size() && !end; i++)
            if (line.at(i) == symb)
                end = i;
    }
    if (!start || !end)
        return Span{cur, cur};
    int s = *start, e = *end;
    if (inclusive) {
        --s;
        ++e;
    }
    return Span{Pos(cur.line, s), Pos(cur.line, e)};
}

VimEngine::Span VimEngine::expandTagUnderCursor(Pos head, bool inclusive) const
{
    // The innermost <tag ...>...</tag> pair around the cursor (text based; Markdown has no HTML
    // syntax nodes). A cursor inside a tag counts as inside its element.
    const Text& t = text();
    const qsizetype pos = indexFromPos(head);
    const qsizetype windowFrom = std::max<qsizetype>(0, pos - 20000);
    const qsizetype windowTo = std::min(t.length(), pos + 20000);
    const QString s = t.slice(windowFrom, windowTo);
    const qsizetype p = pos - windowFrom;
    static const QRegularExpression tagRe(QStringLiteral("<(/?)([A-Za-z][A-Za-z0-9:_-]*)(?:\\s[^<>]*)?(/?)>"));
    struct Tag {
        qsizetype from, to;
        QString name;
        bool close;
    };
    QList<Tag> tags;
    for (auto it = tagRe.globalMatch(s); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (!m.captured(3).isEmpty())
            continue; // self-closing
        tags.push_back(Tag{m.capturedStart(), m.capturedEnd(), m.captured(2).toLower(), !m.captured(1).isEmpty()});
    }
    // pair them with a stack
    struct Pair {
        Tag open, close;
    };
    QList<Pair> pairs;
    QList<Tag> stack;
    for (const Tag& tag : std::as_const(tags)) {
        if (!tag.close) {
            stack.push_back(tag);
            continue;
        }
        for (qsizetype i = stack.size() - 1; i >= 0; --i) {
            if (stack[i].name == tag.name) {
                pairs.push_back(Pair{stack[i], tag});
                stack.resize(i);
                break;
            }
        }
    }
    std::optional<Pair> best;
    for (const Pair& pr : std::as_const(pairs)) {
        if (pr.open.from <= p && p < pr.close.to) {
            if (!best || pr.open.from >= best->open.from)
                best = pr;
        }
    }
    if (!best)
        return Span{head, head};
    if (inclusive)
        return Span{posFromIndex(windowFrom + best->open.from), posFromIndex(windowFrom + best->close.to)};
    return Span{posFromIndex(windowFrom + best->open.to), posFromIndex(windowFrom + best->close.from)};
}

QPair<Pos, Pos> VimEngine::expandSelection(Pos start, Pos end, bool move) const
{
    Pos head = move ? start : m_sel.head;
    Pos anchor = move ? start : m_sel.anchor;
    if (cursorIsBefore(end, start))
        std::swap(start, end);
    if (cursorIsBefore(head, anchor)) {
        head = cursorMin(start, head);
        anchor = cursorMax(anchor, end);
    } else {
        anchor = cursorMin(start, anchor);
        head = cursorMax(head, end);
        head = offsetCursor(head, 0, -1);
        if (head.ch == -1 && head.line != firstLine())
            head = Pos(head.line - 1, lineLength(head.line - 1));
    }
    return {anchor, head};
}

VimEngine::MotionResult VimEngine::textObjectManipulation(Pos head, Args& args)
{
    QString character = args.selectedCharacter;
    if (character == u"b")
        character = QStringLiteral("(");
    else if (character == u"B")
        character = QStringLiteral("{");
    const bool inclusive = !args.textObjectInner;
    std::optional<Span> tmp;
    bool move = false;
    const QChar c = character.size() == 1 ? character.at(0) : QChar();
    if (QStringLiteral("(){}[]<>").contains(c) && !c.isNull()) {
        move = true;
        tmp = selectCompanionObject(head, c, inclusive);
        if (!tmp) {
            // not inside a pair: the next opening/closing symbol on or after the cursor
            const qsizetype from = indexFromPos(head);
            const QString rest = text().slice(from, std::min(text().length(), from + 200000));
            const qsizetype i = rest.indexOf(c);
            if (i >= 0)
                tmp = selectCompanionObject(posFromIndex(from + i), c, inclusive);
        }
    } else if (c == u'\'' || c == u'"' || c == u'`') {
        move = true;
        tmp = findBeginningAndEnd(head, c, inclusive);
    } else if (c == u'W' || c == u'w') {
        int repeat = args.repeat ? args.repeat : 1;
        while (repeat-- > 0) {
            auto repeated = expandWordUnderCursor(inclusive, !inclusive, c == u'W', c == u'W', true,
                                                  tmp ? std::optional<Pos>(tmp->end) : std::nullopt);
            if (repeated) {
                if (!tmp)
                    tmp = repeated;
                tmp->end = repeated->end;
            }
        }
    } else if (c == u'p') {
        tmp = findParagraph(head, args.repeat, 0, inclusive);
        args.linewise = true;
        if (m_visualMode) {
            if (!m_visualLine)
                m_visualLine = true;
        } else {
            // (vim.js also flags the pending operator linewise, but its input state is already cleared
            // here; motionArgs.linewise is what makes the operator linewise)
            tmp->end.line--;
        }
    } else if (c == u't') {
        tmp = expandTagUnderCursor(head, inclusive);
    } else if (c == u's') {
        // account for the cursor on an end-of-sentence symbol
        Pos h = head;
        const QString content = getLine(h.line);
        if (h.ch > 0 && isEndOfSentenceSymbol(charAt(content, h.ch)))
            h.ch -= 1;
        const Pos end = getSentence(h, args.repeat, 1, inclusive);
        Pos start = getSentence(h, args.repeat, -1, inclusive);
        // "a" only takes the space after the sentence if there is one before and after
        if (isWhiteSpaceChar(charAt(getLine(start.line), start.ch)) && isWhiteSpaceChar(charAt(getLine(end.line), end.ch - 1)))
            start = Pos(start.line, start.ch + 1);
        tmp = Span{start, end};
    }
    if (!tmp)
        return MotionResult::none(); // no valid text object: don't move
    if (!m_visualMode)
        return MotionResult::range(tmp->start, tmp->end);
    const auto sel = expandSelection(tmp->start, tmp->end, move);
    return MotionResult::range(sel.first, sel.second);
}

} // namespace oc::vim
