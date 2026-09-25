// Search ("/", "?", "*", "#", n, N) and ex commands (":") — vim.js processSearch / processEx,
// the search cursor of Obsidian's CodeMirror adapter, and exCommandDispatcher.
#include "editor/vim/vim_engine.h"

#include "editor/vim/vim_keys.h"
#include "editor/vim/vim_panel.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc::vim {

namespace {
VimGlobal& G() { return VimGlobal::instance(); }

struct ExError {
    QString message;
};

// positions of unescaped separators (vim.js findUnescapedSeparators)
QList<int> findUnescapedSeparators(const QString& str, QChar separator = u'/')
{
    bool escapeNextChar = false;
    QList<int> out;
    for (int i = 0; i < str.size(); ++i) {
        const QChar c = str.at(i);
        if (!escapeNextChar && c == separator)
            out.push_back(i);
        escapeNextChar = !escapeNextChar && c == u'\\';
    }
    return out;
}

// vim.js splitBySeparator: tokens between separators; nullopt for "foo/bar" (no leading separator)
std::optional<QStringList> splitBySeparator(const QString& argString, QChar separator)
{
    const QList<int> slashes = findUnescapedSeparators(argString, separator);
    if (slashes.isEmpty())
        return QStringList();
    if (slashes.first() != 0)
        return std::nullopt;
    QStringList tokens;
    for (int i = 0; i < slashes.size(); ++i) {
        const int from = slashes[i] + 1;
        const int to = i + 1 < slashes.size() ? slashes[i + 1] : int(argString.size());
        tokens << argString.mid(from, to - from);
    }
    return tokens;
}

// vim.js translateRegex (":set nopcre"): vim's magic escapes to JavaScript
QString translateRegex(const QString& str)
{
    static const QString specials = QStringLiteral("|(){");
    static const QString unescape = QStringLiteral("}");
    bool escapeNextChar = false;
    QString out;
    for (int i = -1; i < str.size(); ++i) {
        const QChar c = i >= 0 ? str.at(i) : QChar();
        const QChar n = i + 1 < str.size() ? str.at(i + 1) : QChar();
        bool specialComesNext = !n.isNull() && specials.contains(n);
        if (escapeNextChar) {
            if (c != u'\\' || !specialComesNext)
                out += c;
            escapeNextChar = false;
        } else if (c == u'\\') {
            escapeNextChar = true;
            if (!n.isNull() && unescape.contains(n))
                specialComesNext = true;
            if (!specialComesNext || n == u'\\')
                out += c;
        } else {
            if (!c.isNull())
                out += c;
            if (specialComesNext && n != u'\\')
                out += u'\\';
        }
    }
    return out;
}

// vim.js translateRegexReplace (":set nopcre"): \1 -> $1, \n -> newline, ...
QString translateRegexReplace(const QString& str)
{
    bool escapeNextChar = false;
    QString out;
    for (int i = -1; i < str.size(); ++i) {
        const QChar c = i >= 0 ? str.at(i) : QChar();
        const QChar n = i + 1 < str.size() ? str.at(i + 1) : QChar();
        if (c == u'\\' && (n == u'n' || n == u'r' || n == u't')) {
            out += n == u'n' ? u'\n' : n == u'r' ? u'\r' : u'\t';
            i++;
        } else if (escapeNextChar) {
            if (!c.isNull())
                out += c;
            escapeNextChar = false;
        } else if (c == u'\\') {
            escapeNextChar = true;
            if (n.isDigit() || n == u'$')
                out += u'$';
            else if (n != u'/' && n != u'\\')
                out += u'\\';
        } else {
            if (c == u'$')
                out += u'$';
            if (!c.isNull())
                out += c;
            if (n == u'/')
                out += u'\\';
        }
    }
    return out;
}

// PCRE mode (the default): an unescaped "&" is the whole match; \/ \\ \n \r \t \& unescape
QString unescapeRegexReplace(QString str)
{
    static const QRegularExpression amp(QStringLiteral("([^\\\\])&"));
    str.replace(amp, QStringLiteral("\\1$&"));
    QString out;
    for (qsizetype i = 0; i < str.size(); ++i) {
        if (str.at(i) == u'\\' && i + 1 < str.size()) {
            const QChar n = str.at(i + 1);
            if (n == u'/' || n == u'\\' || n == u'&') {
                out += n;
                ++i;
                continue;
            }
            if (n == u'n' || n == u'r' || n == u't') {
                out += n == u'n' ? u'\n' : n == u'r' ? u'\r' : u'\t';
                ++i;
                continue;
            }
        }
        out += str.at(i);
    }
    return out;
}

// JavaScript String.prototype.replace replacement patterns ($$, $&, $`, $', $n, $nn, $<name>)
QString expandReplacement(const QString& rep, const QRegularExpressionMatch& m, const QString& subject)
{
    QString out;
    for (qsizetype i = 0; i < rep.size(); ++i) {
        const QChar c = rep.at(i);
        if (c != u'$' || i + 1 >= rep.size()) {
            out += c;
            continue;
        }
        const QChar n = rep.at(i + 1);
        if (n == u'$') {
            out += u'$';
            ++i;
        } else if (n == u'&') {
            out += m.captured(0);
            ++i;
        } else if (n == u'`') {
            out += subject.left(m.capturedStart(0));
            ++i;
        } else if (n == u'\'') {
            out += subject.mid(m.capturedEnd(0));
            ++i;
        } else if (n.isDigit()) {
            int group = n.digitValue();
            qsizetype used = 1;
            if (i + 2 < rep.size() && rep.at(i + 2).isDigit()) {
                const int two = group * 10 + rep.at(i + 2).digitValue();
                if (two >= 1 && two <= m.lastCapturedIndex()) {
                    group = two;
                    used = 2;
                }
            }
            if (group >= 1 && group <= m.regularExpression().captureCount()) {
                out += m.captured(group);
                i += used;
            } else {
                out += c;
            }
        } else if (n == u'<') {
            const qsizetype close = rep.indexOf(u'>', i + 2);
            if (close > 0 && m.regularExpression().namedCaptureGroups().size() > 1) {
                out += m.captured(rep.mid(i + 2, close - i - 2));
                i = close;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

QString escapeRegex(const QString& s)
{
    static const QString specials = QStringLiteral(".?*+$[]/\\(){}|-");
    QString out;
    for (QChar c : s) {
        if (specials.contains(c))
            out += u'\\';
        out += c;
    }
    return out;
}

// a tiny CodeMirror.StringStream
struct Stream {
    QString s;
    int pos = 0;
    bool eol() const { return pos >= s.size(); }
    QChar peek() const { return eol() ? QChar() : s.at(pos); }
    QChar next() { return eol() ? QChar() : s.at(pos++); }
    bool eat(QChar c)
    {
        if (peek() == c) {
            pos++;
            return true;
        }
        return false;
    }
    void eatWhile(QChar c)
    {
        while (peek() == c)
            pos++;
    }
    void backUp(int n) { pos = std::max(0, pos - n); }
    // match a regex anchored at pos
    QRegularExpressionMatch match(const QRegularExpression& re, bool consume = true)
    {
        QRegularExpressionMatch m = re.match(s, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (m.hasMatch() && consume)
            pos = int(m.capturedEnd());
        return m;
    }
    QString rest() const { return s.mid(pos); }
};

struct ExCommandDef {
    QString name;
    QString shortName;
    bool excludeFromCommandHistory = false;
    bool possiblyAsync = false;
};

// vim.js defaultExCommandMap + Obsidian's ":image" + ":quit", ":wq", ":xit" (Obsidian commands)
const QList<ExCommandDef>& exCommandMap()
{
    static const QList<ExCommandDef> map = {
        {"colorscheme", "colo"}, {"map", ""},        {"imap", "im"},       {"nmap", "nm"},          {"vmap", "vm"},
        {"omap", "om"},          {"noremap", "no"},  {"nnoremap", "nn"},   {"vnoremap", "vn"},      {"inoremap", "ino"},
        {"onoremap", "ono"},     {"unmap", ""},      {"mapclear", "mapc"}, {"nmapclear", "nmapc"},  {"vmapclear", "vmapc"},
        {"imapclear", "imapc"},  {"omapclear", "omapc"}, {"write", "w"},   {"undo", "u"},           {"redo", "red"},
        {"set", "se"},           {"setlocal", "setl"}, {"setglobal", "setg"}, {"sort", "sor"},       {"substitute", "s", false, true},
        {"startinsert", "start"}, {"nohlsearch", "noh"}, {"yank", "y"},    {"delmarks", "delm"},    {"marks", "", true},
        {"registers", "reg", true}, {"vglobal", "v"}, {"delete", "d"},     {"join", "j"},           {"normal", "norm"},
        {"global", "g"},         {"image", "image"}, {"quit", "q"},        {"wq", "wq"},            {"xit", "x"},
    };
    return map;
}

const ExCommandDef* matchExCommand(const QString& commandName)
{
    // the command whose shortName is the longest prefix of the name and whose name starts with it
    for (qsizetype i = commandName.size(); i > 0; --i) {
        const QString prefix = commandName.left(i);
        for (const ExCommandDef& c : exCommandMap()) {
            const QString key = c.shortName.isEmpty() ? c.name : c.shortName;
            if (key == prefix)
                return c.name.startsWith(commandName) ? &c : nullptr;
        }
    }
    return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// prompts and notifications
// ---------------------------------------------------------------------------------------------
void VimEngine::showConfirm(const QString& text, bool isLong)
{
    if (!m_panel)
        return;
    m_panel->showNotification(text, isLong);
    m_closeNotificationOnKey = isLong;
}

void VimEngine::showPrompt(const QString& prefix, const QString& desc, const QString& value, bool selectValue,
                           std::function<void(const QString&)> onClose, PromptKeyFn onKeyDown, PromptKeyFn onKeyUp, bool richPrefix)
{
    if (m_keyToKeyDepth > 0) {
        m_virtualPrompt = VirtualPrompt{prefix, desc, value, std::move(onClose), std::move(onKeyDown), std::move(onKeyUp)};
        return;
    }
    if (!m_panel)
        return;
    m_panel->openPrompt(prefix, desc, value, selectValue, std::move(onClose), std::move(onKeyDown), std::move(onKeyUp), richPrefix);
}

// ---------------------------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------------------------
std::optional<Query> VimEngine::parseQuery(const QString& query, bool ignoreCase, bool smartCase)
{
    G().registerController.get(QStringLiteral("/")).setText(query);
    // "regexp/flags": only "i" is honoured
    const QList<int> slashes = findUnescapedSeparators(query);
    QString regexPart;
    bool forceIgnoreCase = false;
    if (slashes.isEmpty()) {
        regexPart = query;
    } else {
        regexPart = query.left(slashes.first());
        forceIgnoreCase = query.mid(slashes.first()).contains(u'i');
    }
    if (regexPart.isEmpty())
        return std::nullopt;
    if (!G().pcre)
        regexPart = translateRegex(regexPart);
    if (smartCase) {
        static const QRegularExpression noUpper(QStringLiteral("^[^A-Z]*$"));
        ignoreCase = noUpper.match(regexPart).hasMatch();
    }
    Query q;
    q.source = regexPart;
    q.ignoreCase = ignoreCase || forceIgnoreCase;
    // like the adapter's search cursor: braces that aren't a quantifier are literal
    static const QRegularExpression braces(QStringLiteral("(\\\\.|\\{(?:\\d+(?:,\\d*)?|,\\d+)\\})|[{}]"));
    QString pattern;
    qsizetype last = 0;
    for (auto it = braces.globalMatch(regexPart); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        pattern += regexPart.mid(last, m.capturedStart() - last);
        pattern += m.hasCaptured(1) ? m.captured(1) : u'\\' + m.captured(0);
        last = m.capturedEnd();
    }
    pattern += regexPart.mid(last);
    QRegularExpression::PatternOptions opts = QRegularExpression::MultilineOption;
    if (q.ignoreCase)
        opts |= QRegularExpression::CaseInsensitiveOption;
    q.re = QRegularExpression(pattern, opts);
    if (!q.re.isValid())
        throw ExError{QStringLiteral("Invalid regex: ") + query};
    return q;
}

std::optional<Query> VimEngine::updateSearchQuery(const QString& rawQuery, bool ignoreCase, bool smartCase, bool* invalid)
{
    if (invalid)
        *invalid = false;
    if (rawQuery.isEmpty())
        return std::nullopt;
    std::optional<Query> query;
    try {
        query = parseQuery(rawQuery, ignoreCase, smartCase);
    } catch (const ExError&) {
        if (invalid)
            *invalid = true;
        return std::nullopt;
    }
    if (!query)
        return std::nullopt;
    highlightSearchMatches(*query);
    G().query = query;
    return query;
}

void VimEngine::highlightSearchMatches(const Query& query)
{
    if (m_highlight && *m_highlight == query)
        return;
    m_highlight = query;
    if (highlightChanged)
        highlightChanged();
}

void VimEngine::clearSearchHighlight()
{
    if (!m_highlight)
        return;
    m_highlight.reset();
    if (highlightChanged)
        highlightChanged();
}

// ---------------------------------------------------------------------------------------------
// search cursor (the adapter's getSearchCursor over CodeMirror's RegExpCursor: per line)
// ---------------------------------------------------------------------------------------------
std::optional<VimEngine::SearchHit> VimEngine::searchForward(const Query& q, qsizetype from) const
{
    const Text& t = text();
    if (from > t.length() || !q.re.isValid())
        return std::nullopt;
    for (int line = t.lineAt(from); line < t.lineCount(); ++line) {
        const QString s = t.line(line);
        const qsizetype ls = t.lineStart(line);
        const qsizetype off = std::max<qsizetype>(0, from - ls);
        if (off > s.size())
            continue;
        const QRegularExpressionMatch m = q.re.match(s, off);
        if (m.hasMatch())
            return SearchHit{ls + m.capturedStart(), ls + m.capturedEnd(), m};
    }
    return std::nullopt;
}

std::optional<VimEngine::SearchHit> VimEngine::searchBackward(const Query& q, qsizetype endAt) const
{
    // the last match that ends at or before endAt
    const Text& t = text();
    if (!q.re.isValid())
        return std::nullopt;
    endAt = std::clamp<qsizetype>(endAt, 0, t.length());
    for (int line = t.lineAt(endAt); line >= 0; --line) {
        const qsizetype ls = t.lineStart(line);
        const QString full = t.line(line);
        const QString s = full.left(std::min<qsizetype>(full.size(), endAt - ls));
        std::optional<SearchHit> best;
        qsizetype pos = 0;
        std::optional<qsizetype> prevEnd;
        while (pos <= s.size()) {
            const QRegularExpressionMatch m = q.re.match(s, pos);
            if (!m.hasMatch())
                break;
            const qsizetype a = m.capturedStart(), b = m.capturedEnd();
            if (!(a == b && prevEnd && a == *prevEnd))
                best = SearchHit{ls + a, ls + b, m};
            prevEnd = b;
            pos = b > a ? b : b + 1;
        }
        if (best)
            return best;
    }
    return std::nullopt;
}

namespace {
// The adapter's cursor state: find(back) continues from the previous match.
struct SearchCursor {
    qsizetype firstOffset = 0;
    std::optional<qsizetype> lastFrom, lastTo;
    bool afterEmptyMatch = false;
    std::optional<QRegularExpressionMatch> match;
};
} // namespace

std::optional<Pos> VimEngine::findNext(bool prev, const Query& query, int repeat)
{
    const Pos pos = getCursor();
    SearchCursor cur;
    cur.firstOffset = indexFromPos(pos);
    auto find = [&](bool back) -> bool {
        std::optional<SearchHit> hit;
        if (back) {
            const qsizetype endAt = cur.lastFrom ? (cur.afterEmptyMatch ? *cur.lastTo - 1 : *cur.lastFrom) : cur.firstOffset;
            hit = searchBackward(query, endAt);
            if (hit && cur.lastFrom && hit->to > endAt)
                hit.reset();
        } else {
            const qsizetype startFrom = cur.lastFrom ? (cur.afterEmptyMatch ? *cur.lastTo + 1 : *cur.lastTo) : cur.firstOffset;
            hit = searchForward(query, startFrom);
        }
        if (!hit) {
            cur.lastFrom.reset();
            cur.lastTo.reset();
            cur.afterEmptyMatch = false;
            return false;
        }
        cur.lastFrom = hit->from;
        cur.lastTo = hit->to;
        cur.match = hit->match;
        cur.afterEmptyMatch = hit->from == hit->to;
        return true;
    };
    for (int i = 0; i < repeat; ++i) {
        bool found = find(prev);
        if (i == 0 && found && posFromIndex(*cur.lastFrom) == pos) {
            // a match at the cursor: take the next one
            const qsizetype lastEndPos = prev ? *cur.lastFrom : *cur.lastTo;
            found = find(prev);
            if (found && cur.match->capturedLength(0) == 0 && *cur.lastFrom == lastEndPos) {
                const Pos p = posFromIndex(lastEndPos);
                if (lineLength(p.line) == p.ch)
                    found = find(prev);
            }
        }
        if (!found) {
            // wrap around
            cur = SearchCursor();
            cur.firstOffset = prev ? text().length() : 0;
            if (!find(prev))
                return std::nullopt;
        }
    }
    return posFromIndex(*cur.lastFrom);
}

std::optional<QPair<Pos, Pos>> VimEngine::findNextFromAndToInclusive(bool prev, const Query& query, int repeat)
{
    const Pos pos = getCursor();
    SearchCursor cur;
    cur.firstOffset = indexFromPos(pos);
    auto find = [&](bool back) -> bool {
        std::optional<SearchHit> hit;
        if (back) {
            const qsizetype endAt = cur.lastFrom ? (cur.afterEmptyMatch ? *cur.lastTo - 1 : *cur.lastFrom) : cur.firstOffset;
            hit = searchBackward(query, endAt);
        } else {
            const qsizetype startFrom = cur.lastFrom ? (cur.afterEmptyMatch ? *cur.lastTo + 1 : *cur.lastTo) : cur.firstOffset;
            hit = searchForward(query, startFrom);
        }
        if (!hit) {
            cur.lastFrom.reset();
            cur.lastTo.reset();
            return false;
        }
        cur.lastFrom = hit->from;
        cur.lastTo = hit->to;
        cur.afterEmptyMatch = hit->from == hit->to;
        return true;
    };
    // go back one result so a match under the cursor is kept
    bool found = find(!prev);
    if (!m_visualMode && found && posFromIndex(*cur.lastFrom) == pos)
        find(!prev);
    for (int i = 0; i < repeat; ++i) {
        found = find(prev);
        if (!found) {
            cur = SearchCursor();
            cur.firstOffset = prev ? text().length() : 0;
            if (!find(prev))
                return std::nullopt;
        }
    }
    return QPair<Pos, Pos>{posFromIndex(*cur.lastFrom), posFromIndex(*cur.lastTo)};
}

// ---------------------------------------------------------------------------------------------
// "/" "?" "*" "#"
// ---------------------------------------------------------------------------------------------
void VimEngine::processSearch(const KeyCommand& command)
{
    const bool forward = command.searchArgs.forward;
    const bool wholeWordOnly = command.searchArgs.wholeWordOnly;
    const bool toJumplist = command.searchArgs.toJumplist;
    G().isReversed = !forward;
    const QString promptPrefix = forward ? QStringLiteral("/") : QStringLiteral("?");
    const std::optional<Query> originalQuery = G().query;
    const double originalScroll = m_view->scrollTop();
    auto handleQuery = [this, toJumplist](const QString& query, bool ignoreCase, bool smartCase) {
        G().searchHistoryController.pushInput(query);
        G().searchHistoryController.reset();
        bool invalid = false;
        updateSearchQuery(query, ignoreCase, smartCase, &invalid);
        if (invalid) {
            showConfirm(QStringLiteral("Invalid regex: ") + query);
            clearInputState();
            return;
        }
        KeyCommand motion;
        motion.type = CmdType::Motion;
        motion.motion = Motion::FindNext;
        motion.motionArgs.forward = true;
        motion.motionArgs.toJumplist = toJumplist;
        processMotion(motion);
    };
    if (command.searchArgs.querySrc == u"prompt") {
        MacroModeState& mm = G().macroModeState;
        if (mm.isPlaying) {
            const QString query = mm.replaySearchQueries.isEmpty() ? QString() : mm.replaySearchQueries.takeFirst();
            handleQuery(query, true, false);
            return;
        }
        // the prompt closes asynchronously (after this key): keep the input state it needs
        const std::shared_ptr<InputState> pendingState = m_inputState;
        auto onClose = [this, handleQuery, originalScroll, pendingState](const QString& query) {
            m_busy++;
            beginEdit();
            m_view->setScrollTop(originalScroll);
            m_inputState = pendingState; // a pending operator ("d/foo") applies to the search motion
            handleQuery(query, true, true);
            if (G().macroModeState.isRecording && !G().macroModeState.isPlaying)
                G().registerController.get(G().macroModeState.latestRegister).pushSearchQuery(query);
            endEdit();
            m_busy--;
            ensureCursorVisible();
        };
        auto onKeyUp = [this, forward, originalScroll](const QString& key, const QString& value, const PromptClose& close) -> bool {
            QString query = value;
            if (key == u"<Up>" || key == u"<Down>") {
                query = G().searchHistoryController.nextMatch(query, key == u"<Up>");
                close(query);
            } else if (!key.isEmpty() && key != u"<Left>" && key != u"<Right>") {
                G().searchHistoryController.reset();
            }
            std::optional<Query> parsed;
            try {
                parsed = parseQuery(query, true, true);
            } catch (const ExError&) {
            }
            if (parsed) {
                // incremental search: show the next match without moving the cursor
                highlightSearchMatches(*parsed);
                if (auto p = findNext(!forward, *parsed))
                    m_view->scrollToPos(indexFromPos(*p), false);
            } else {
                clearSearchHighlight();
                m_view->setScrollTop(originalScroll);
            }
            return false;
        };
        auto onKeyDown = [this, originalQuery, originalScroll](const QString& key, const QString& value, const PromptClose& close) -> bool {
            if (key == u"<Esc>" || key == u"<C-c>" || key == u"<C-[>" || (key == u"<BS>" && value.isEmpty())) {
                G().searchHistoryController.pushInput(value);
                G().searchHistoryController.reset();
                G().query = originalQuery;
                clearSearchHighlight();
                m_view->setScrollTop(originalScroll);
                clearInputState();
                close(std::nullopt);
                m_view->setFocus();
                return true;
            }
            if (key == u"<C-u>") {
                close(QString());
                return true;
            }
            return false; // <Up> / <Down> walk the history on key up
        };
        showPrompt(promptPrefix, QStringLiteral("(JavaScript regexp)"), QString(), true, onClose, onKeyDown, onKeyUp);
        return;
    }
    // wordUnderCursor
    std::optional<Span> word = expandWordUnderCursor(false, false, false, true, false);
    bool isKeyword = true;
    if (!word) {
        word = expandWordUnderCursor(false, false, false, false, false);
        isKeyword = false;
    }
    if (!word) {
        showConfirm(QStringLiteral("No word under cursor"));
        clearInputState();
        return;
    }
    QString query = getLine(word->start.line).mid(word->start.ch, word->end.ch - word->start.ch);
    if (isKeyword && wholeWordOnly)
        query = QStringLiteral("\\b") + query + QStringLiteral("\\b");
    else
        query = escapeRegex(query);
    m_jumpList.cachedCursor = getCursor();
    setCursor(word->start);
    handleQuery(query, true, false);
}

// ---------------------------------------------------------------------------------------------
// ":"
// ---------------------------------------------------------------------------------------------
void VimEngine::processEx(const KeyCommand& command)
{
    if (command.type == CmdType::KeyToEx) {
        processExCommand(command.exInput);
        return;
    }
    auto onClose = [this](const QString& input) {
        G().exCommandHistoryController.pushInput(input);
        G().exCommandHistoryController.reset();
        m_busy++;
        beginEdit();
        processExCommand(input);
        clearInputState();
        clearSearchHighlight();
        endEdit();
        m_busy--;
        ensureCursorVisible();
    };
    auto onKeyDown = [this](const QString& key, const QString& value, const PromptClose& close) -> bool {
        if (key == u"<Esc>" || key == u"<C-c>" || key == u"<C-[>" || (key == u"<BS>" && value.isEmpty())) {
            G().exCommandHistoryController.pushInput(value);
            G().exCommandHistoryController.reset();
            clearInputState();
            clearSearchHighlight();
            close(std::nullopt);
            m_view->setFocus();
            return true;
        }
        if (key == u"<Up>" || key == u"<Down>") {
            close(G().exCommandHistoryController.nextMatch(value, key == u"<Up>"));
            return true;
        }
        if (key == u"<C-u>") {
            close(QString());
            return true;
        }
        if (!key.isEmpty() && key != u"<Left>" && key != u"<Right>")
            G().exCommandHistoryController.reset();
        return false;
    };
    auto onKeyUp = [this](const QString&, const QString& value, const PromptClose&) -> bool {
        // live highlight of a ":s/pattern" being typed
        static const QRegularExpression sub(QStringLiteral("^[^a-zA-Z]*s(?:u(?:b(?:s(?:t(?:i(?:t(?:u(?:t(?:e)?)?)?)?)?)?)?)?)?(\\W.*)$"));
        const QRegularExpressionMatch m = sub.match(value);
        if (!m.hasMatch()) {
            clearSearchHighlight();
            return false;
        }
        const QString arg = m.captured(1);
        const auto tokens = splitBySeparator(arg, arg.at(0));
        if (!tokens || tokens->isEmpty() || tokens->first().isEmpty())
            return false;
        try {
            if (auto q = parseQuery(tokens->first(), true, true))
                highlightSearchMatches(*q);
        } catch (const ExError&) {
        }
        return false;
    };
    showPrompt(QStringLiteral(":"), QString(), m_visualMode ? QStringLiteral("'<,'>") : QString(), false, onClose, onKeyDown, onKeyUp);
}

void VimEngine::processExCommand(const QString& input, std::function<void()> callback)
{
    Register& history = G().registerController.get(QStringLiteral(":"));
    const QString previousCommand = history.toString();
    history.setText(input);
    ExParams params;
    params.input = input;
    params.callback = std::move(callback);
    Stream in{input};
    try {
        // parseInput_: range, then command name
        in.eatWhile(u':');
        auto parseLineSpecOffset = [&](int line) {
            static const QRegularExpression off(QStringLiteral("([+-])?(\\d+)"));
            const QRegularExpressionMatch m = in.match(off);
            if (m.hasMatch()) {
                const int n = m.captured(2).toInt();
                line = m.captured(1) == u"-" ? line - n : line + n;
            }
            return line;
        };
        auto parseLineSpec = [&]() -> std::optional<int> {
            static const QRegularExpression num(QStringLiteral("\\d+"));
            const QRegularExpressionMatch m = in.match(num);
            if (m.hasMatch())
                return m.captured(0).toInt() - 1;
            const QChar c = in.next();
            switch (c.unicode()) {
            case '.':
                return parseLineSpecOffset(getCursor().line);
            case '$':
                return parseLineSpecOffset(lastLine());
            case '\'': {
                const QString markName = in.next();
                const auto markPos = getMarkPos(markName);
                if (!markPos)
                    throw ExError{QStringLiteral("Error: Mark not set")};
                return parseLineSpecOffset(markPos->line);
            }
            case '-':
            case '+':
                in.backUp(1);
                return parseLineSpecOffset(getCursor().line);
            default:
                if (!c.isNull())
                    in.backUp(1);
                return std::nullopt;
            }
        };
        if (in.eat(u'%')) {
            params.line = firstLine();
            params.lineEnd = lastLine();
        } else {
            params.line = parseLineSpec();
            if (params.line && in.eat(u','))
                params.lineEnd = parseLineSpec();
        }
        if (!params.line) {
            if (m_visualMode) {
                if (auto p = getMarkPos(QStringLiteral("<")))
                    params.selectionLine = p->line;
                if (auto p = getMarkPos(QStringLiteral(">")))
                    params.selectionLineEnd = p->line;
            } else {
                params.selectionLine = getCursor().line;
            }
        } else {
            params.selectionLine = params.line;
            params.selectionLineEnd = params.lineEnd;
        }
        static const QRegularExpression name(QStringLiteral("(\\w+|!!|@@|[!#&*<=>@~])"));
        const QRegularExpressionMatch nm = in.match(name);
        params.commandName = nm.hasMatch() ? nm.captured(1) : in.rest();
        if (!nm.hasMatch())
            in.pos = int(in.s.size());
    } catch (const ExError& e) {
        showConfirm(e.message);
        return;
    }
    if (m_visualMode)
        exitVisualMode();
    QString commandName;
    const ExCommandDef* def = nullptr;
    if (params.commandName.isEmpty()) {
        if (params.line)
            commandName = QStringLiteral("move");
    } else if (auto it = G().userExCommands.constFind(params.commandName); it != G().userExCommands.cend()) {
        // ":map :foo :bar" / ":map :foo keys"
        const KeyCommand user = *it;
        if (!user.exInput.isEmpty())
            processExCommand(user.exInput);
        else
            doKeyToKey(user.toKeys, &user);
        return;
    } else {
        def = matchExCommand(params.commandName);
        if (def) {
            commandName = def->name;
            if (def->excludeFromCommandHistory)
                history.setText(previousCommand);
            if (!in.eol()) {
                params.argString = in.rest();
                params.args = params.argString.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            }
        }
    }
    if (commandName.isEmpty()) {
        showConfirm(QStringLiteral("Not an editor command \":") + input + QStringLiteral("\""));
        return;
    }
    try {
        const bool async = exCommand(commandName, params);
        if (!async && params.callback)
            params.callback();
    } catch (const ExError& e) {
        showConfirm(e.message);
    }
}

bool VimEngine::exCommand(const QString& name, ExParams& params)
{
    auto selLine = [&] { return params.selectionLine.value_or(getCursor().line); };
    auto selLineEnd = [&] { return params.selectionLineEnd.value_or(selLine()); };
    if (name == u"move") {
        KeyCommand c;
        c.type = CmdType::Motion;
        c.motion = Motion::MoveToLineOrEdgeOfDocument;
        c.motionArgs.explicitRepeat = true;
        c.motionArgs.linewise = true;
        c.repeatOverride = *params.line + 1;
        processCommand(c);
    } else if (name == u"colorscheme") {
        if (params.args.isEmpty())
            showConfirm(QStringLiteral("obsidian"));
    } else if (name == u"map" || name == u"imap" || name == u"nmap" || name == u"vmap" || name == u"omap" || name == u"noremap"
               || name == u"inoremap" || name == u"nnoremap" || name == u"vnoremap" || name == u"onoremap") {
        QString ctx;
        if (name.startsWith(u'i'))
            ctx = QStringLiteral("insert");
        else if (name.startsWith(u'n') && name != u"noremap")
            ctx = QStringLiteral("normal");
        else if (name.startsWith(u'v'))
            ctx = QStringLiteral("visual");
        else if (name.startsWith(u'o'))
            ctx = QStringLiteral("operatorPending");
        exMap(params, ctx, name.contains(QLatin1String("noremap")));
    } else if (name == u"unmap") {
        if (params.args.isEmpty() || !exUnmap(params.args.first(), QString()))
            showConfirm(QStringLiteral("No such mapping: ") + params.input);
    } else if (name.endsWith(QLatin1String("mapclear"))) {
        mapClear(name == u"mapclear" ? QString()
                 : name.startsWith(u'n')  ? QStringLiteral("normal")
                 : name.startsWith(u'v')  ? QStringLiteral("visual")
                 : name.startsWith(u'i')  ? QStringLiteral("insert")
                                          : QStringLiteral("operatorPending"));
    } else if (name == u"set" || name == u"setlocal" || name == u"setglobal") {
        if (params.args.isEmpty()) {
            showConfirm(QStringLiteral("Invalid mapping: ") + params.input);
            return false;
        }
        QString expr = params.args.first();
        QString optionName = expr.section(u'=', 0, 0);
        std::optional<QString> value;
        if (expr.contains(u'='))
            value = expr.section(u'=', 1);
        bool forceGet = false, forceToggle = false;
        if (optionName.endsWith(u'?')) {
            if (value)
                throw ExError{QStringLiteral("Error: Trailing characters: ") + params.argString};
            optionName.chop(1);
            forceGet = true;
        } else if (optionName.endsWith(u'!')) {
            optionName.chop(1);
            forceToggle = true;
        }
        std::optional<bool> boolValue;
        if (!value && optionName.startsWith(QLatin1String("no"))) {
            optionName = optionName.mid(2);
            boolValue = false;
        }
        // the options this build has (vim.js: pcre, textwidth/tw, insertModeEscKeysTimeout, filetype/ft)
        enum class Opt { Unknown, Pcre, Textwidth, EscTimeout, Filetype } opt = Opt::Unknown;
        if (optionName == u"pcre")
            opt = Opt::Pcre;
        else if (optionName == u"textwidth" || optionName == u"tw")
            opt = Opt::Textwidth;
        else if (optionName == u"insertModeEscKeysTimeout")
            opt = Opt::EscTimeout;
        else if (optionName == u"filetype" || optionName == u"ft")
            opt = Opt::Filetype;
        if (opt == Opt::Unknown) {
            showConfirm(QStringLiteral("Unknown option: ") + optionName);
            return false;
        }
        const bool isBoolean = opt == Opt::Pcre;
        if (isBoolean) {
            if (forceToggle)
                boolValue = !G().pcre;
            else if (!boolValue && !value)
                boolValue = true;
            else if (value && !boolValue) {
                showConfirm(QStringLiteral("Invalid argument: ") + optionName + u'=' + *value);
                return false;
            }
        }
        if ((!isBoolean && !value) || forceGet) {
            switch (opt) {
            case Opt::Pcre:
                showConfirm(QStringLiteral(" ") + (G().pcre ? QString() : QStringLiteral("no")) + optionName);
                break;
            case Opt::Textwidth:
                showConfirm(QStringLiteral("  ") + optionName + u'=' + QString::number(G().textwidth));
                break;
            case Opt::EscTimeout:
                showConfirm(QStringLiteral("  ") + optionName + u'=' + QString::number(G().insertModeEscKeysTimeout));
                break;
            case Opt::Filetype:
                showConfirm(QStringLiteral("  ") + optionName + QStringLiteral("=markdown"));
                break;
            default:
                break;
            }
        } else {
            switch (opt) {
            case Opt::Pcre:
                G().pcre = *boolValue;
                break;
            case Opt::Textwidth:
                if (value && value->toInt() > 1)
                    G().textwidth = value->toInt();
                break;
            case Opt::EscTimeout:
                if (value)
                    G().insertModeEscKeysTimeout = value->toInt();
                break;
            default:
                break;
            }
        }
    } else if (name == u"registers") {
        QString regInfo = QStringLiteral("----------Registers----------\n\n");
        const auto& regs = G().registerController.all();
        QStringList names;
        if (params.args.isEmpty()) {
            names = regs.keys();
            std::sort(names.begin(), names.end());
        } else {
            const QString joined = params.args.join(QString());
            for (QChar c : joined)
                if (G().registerController.isValid(QString(c)))
                    names << QString(c);
        }
        for (const QString& n : std::as_const(names)) {
            const QString t = regs.value(n.toLower()).toString();
            if (!params.args.isEmpty() || !t.isEmpty())
                regInfo += u'"' + n + QStringLiteral("    ") + t + u'\n';
        }
        showConfirm(regInfo, true);
    } else if (name == u"marks") {
        QString info = QStringLiteral("-----------Marks-----------\nmark\tline\tcol\n\n");
        QStringList names = params.args.isEmpty() ? m_marks.keys() : params.args.join(QString()).split(QString(), Qt::SkipEmptyParts);
        std::sort(names.begin(), names.end());
        for (const QString& n : std::as_const(names))
            if (auto p = markPos(n))
                info += n + u'\t' + QString::number(p->line) + u'\t' + QString::number(p->ch) + u'\n';
        showConfirm(info, true);
    } else if (name == u"sort") {
        bool reverse = false, ignoreCase = false, unique = false;
        QString number;
        std::optional<QRegularExpression> pattern;
        if (!params.argString.isEmpty()) {
            Stream args{params.argString};
            if (args.eat(u'!'))
                reverse = true;
            if (!args.eol()) {
                if (!args.peek().isSpace()) {
                    showConfirm(QStringLiteral("Invalid arguments: ") + params.argString);
                    return false;
                }
                while (!args.eol() && args.peek().isSpace())
                    args.next();
                static const QRegularExpression optsRe(QStringLiteral("([dinuox]+)?\\s*(\\/.+\\/)?\\s*"));
                const QRegularExpressionMatch opts = args.match(optsRe);
                if (!opts.captured(1).isEmpty()) {
                    const QString o = opts.captured(1);
                    ignoreCase = o.contains(u'i');
                    unique = o.contains(u'u');
                    const int decimal = (o.contains(u'd') || o.contains(u'n')) ? 1 : 0;
                    const int hex = o.contains(u'x') ? 1 : 0;
                    const int octal = o.contains(u'o') ? 1 : 0;
                    if (decimal + hex + octal > 1) {
                        showConfirm(QStringLiteral("Invalid arguments: ") + params.argString);
                        return false;
                    }
                    number = decimal ? QStringLiteral("decimal") : hex ? QStringLiteral("hex") : octal ? QStringLiteral("octal") : QString();
                }
                if (!opts.captured(2).isEmpty()) {
                    const QString p = opts.captured(2);
                    pattern = QRegularExpression(p.mid(1, p.size() - 2), ignoreCase ? QRegularExpression::CaseInsensitiveOption
                                                                                     : QRegularExpression::NoPatternOption);
                }
            }
        }
        const int lineStart = params.line ? *params.line : firstLine();
        const int lineEnd = params.lineEnd && *params.lineEnd ? *params.lineEnd : (params.line && *params.line ? *params.line : lastLine());
        if (lineStart == lineEnd)
            return false;
        const Pos curStart(lineStart, 0), curEnd(lineEnd, lineLength(lineEnd));
        QStringList textLines = getRange(curStart, curEnd).split(u'\n');
        const QRegularExpression numberRegex = pattern ? *pattern
                                               : number == u"decimal" ? QRegularExpression(QStringLiteral("(-?)([\\d]+)"))
                                               : number == u"hex"     ? QRegularExpression(QStringLiteral("(-?)(?:0x)?([0-9a-f]+)"), QRegularExpression::CaseInsensitiveOption)
                                               : number == u"octal"   ? QRegularExpression(QStringLiteral("([0-7]+)"))
                                                                      : QRegularExpression();
        const int radix = number == u"decimal" ? 10 : number == u"hex" ? 16 : number == u"octal" ? 8 : 0;
        QStringList numPart, textPart;
        if (!number.isEmpty() || pattern) {
            for (const QString& l : std::as_const(textLines)) {
                const QRegularExpressionMatch mp = pattern ? pattern->match(l) : QRegularExpressionMatch();
                if (pattern && mp.hasMatch() && !mp.captured(0).isEmpty())
                    numPart << l;
                else if (!pattern && numberRegex.match(l).hasMatch())
                    numPart << l;
                else
                    textPart << l;
            }
        } else {
            textPart = textLines;
        }
        auto key = [&](const QString& s) { return ignoreCase ? s.toLower() : s; };
        auto compareText = [&](const QString& a0, const QString& b0) {
            const QString a = key(reverse ? b0 : a0), b = key(reverse ? a0 : b0);
            if (number.isEmpty())
                return a < b;
            const QRegularExpressionMatch am = numberRegex.match(a), bm = numberRegex.match(b);
            if (!am.hasMatch())
                return a < b;
            auto val = [&](const QRegularExpressionMatch& m) {
                const QString digits = number == u"octal" ? m.captured(1) : m.captured(1) + m.captured(2);
                return digits.toLower().toLongLong(nullptr, radix);
            };
            return val(am) < val(bm);
        };
        auto comparePattern = [&](const QString& a0, const QString& b0) {
            const QString a = key(pattern->match(reverse ? b0 : a0).captured(0));
            const QString b = key(pattern->match(reverse ? a0 : b0).captured(0));
            return a < b;
        };
        if (pattern)
            std::stable_sort(numPart.begin(), numPart.end(), comparePattern);
        else
            std::stable_sort(numPart.begin(), numPart.end(), compareText);
        if (!pattern && number.isEmpty())
            std::stable_sort(textPart.begin(), textPart.end(), compareText);
        textLines = !reverse ? textPart + numPart : numPart + textPart;
        if (unique) {
            QStringList out;
            for (const QString& l : std::as_const(textLines))
                if (out.isEmpty() || out.last() != l)
                    out << l;
            textLines = out;
        }
        replaceRange(textLines.join(u'\n'), curStart, curEnd);
    } else if (name == u"global" || name == u"vglobal") {
        QString argString = params.argString;
        if (argString.isEmpty()) {
            showConfirm(QStringLiteral("Regular Expression missing from global"));
            return false;
        }
        bool inverted = params.commandName.startsWith(u'v');
        if (argString.startsWith(u'!') && params.commandName.startsWith(u'g')) {
            inverted = true;
            argString = argString.mid(1);
        }
        const int lineStart = params.line ? *params.line : firstLine();
        const int lineEnd = params.lineEnd && *params.lineEnd ? *params.lineEnd : (params.line && *params.line ? *params.line : lastLine());
        const auto tokens = splitBySeparator(argString, u'/');
        QString regexPart = argString, cmd;
        if (tokens && !tokens->isEmpty()) {
            regexPart = tokens->first();
            cmd = tokens->mid(1).join(u'/');
        }
        if (!regexPart.isEmpty()) {
            bool invalid = false;
            updateSearchQuery(regexPart, true, true, &invalid);
            if (invalid) {
                showConfirm(QStringLiteral("Invalid regex: ") + regexPart);
                return false;
            }
        }
        if (!G().query)
            return false;
        const QRegularExpression re = G().query->re;
        QList<int> marks;
        QStringList matchedLines;
        for (int i = lineStart; i <= lineEnd && i <= lastLine(); ++i) {
            const QString line = getLine(i);
            if (re.match(line).hasMatch() != inverted) {
                if (cmd.isEmpty())
                    matchedLines << line;
                else
                    marks << setBookmark(Pos(i, 0), true);
            }
        }
        if (cmd.isEmpty()) {
            showConfirm(matchedLines.join(u'\n'));
            return false;
        }
        for (int id : std::as_const(marks)) {
            const auto p = findBookmark(id);
            clearBookmark(id);
            if (!p || p->ch != 0)
                continue;
            processExCommand(QString::number(p->line + 1) + cmd);
        }
    } else if (name == u"substitute") {
        const QString argString = params.argString;
        std::optional<QStringList> tokens;
        if (!argString.isEmpty())
            tokens = splitBySeparator(argString, argString.at(0));
        else
            tokens = QStringList();
        QString regexPart, trailing;
        std::optional<QString> replacePart;
        QString flagsPart;
        int count = 0;
        bool confirm = false, global = false;
        if (tokens && !tokens->isEmpty()) {
            regexPart = tokens->value(0);
            if (tokens->size() > 1) {
                QString rp = tokens->value(1);
                rp = G().pcre ? unescapeRegexReplace(rp) : translateRegexReplace(rp);
                replacePart = rp;
                G().lastSubstituteReplacePart = rp;
            }
            const QStringList trail = tokens->size() > 2 ? tokens->value(2).split(u' ') : QStringList();
            flagsPart = trail.value(0);
            count = trail.value(1).toInt();
            if (!flagsPart.isEmpty()) {
                confirm = flagsPart.contains(u'c');
                global = flagsPart.contains(u'g');
                if (G().pcre)
                    regexPart = regexPart + u'/' + flagsPart;
                else
                    regexPart = QString(regexPart).replace(u'/', QStringLiteral("\\/")) + u'/' + flagsPart;
            }
        } else if (!argString.isEmpty()) {
            showConfirm(QStringLiteral("Substitutions should be of the form :s/pattern/replace/"));
            return false;
        }
        if (!regexPart.isEmpty()) {
            bool invalid = false;
            updateSearchQuery(regexPart, true, true, &invalid);
            if (invalid) {
                showConfirm(QStringLiteral("Invalid regex: ") + regexPart);
                return false;
            }
        }
        if (!replacePart || replacePart->isEmpty())
            replacePart = G().lastSubstituteReplacePart;
        if (!replacePart) {
            showConfirm(QStringLiteral("No previous substitute regular expression"));
            return false;
        }
        if (!G().query) {
            showConfirm(QStringLiteral("No previous substitute regular expression"));
            return false;
        }
        int lineStart = params.line ? *params.line : getCursor().line;
        int lineEnd = params.lineEnd && *params.lineEnd ? *params.lineEnd : lineStart;
        if (lineStart == firstLine() && lineEnd == lastLine())
            lineEnd = INT_MAX / 2;
        if (count) {
            lineStart = lineEnd;
            lineEnd = lineStart + count - 1;
        }
        const Pos startPos = clipCursorToContent(Pos(lineStart, 0));
        doReplace(confirm, global, lineStart, lineEnd, startPos, *G().query, *replacePart, params.callback);
        return true; // calls the callback itself
    } else if (name == u"startinsert") {
        doKeyToKey(params.argString == u"!" ? QStringLiteral("A") : QStringLiteral("i"), nullptr);
    } else if (name == u"redo") {
        Args a;
        a.repeat = 1;
        actRedo(a);
    } else if (name == u"undo") {
        Args a;
        a.repeat = 1;
        actUndo(a);
    } else if (name == u"write") {
        if (!runCommand || !runCommand(QStringLiteral("editor:save-file")))
            if (doc())
                doc()->save();
    } else if (name == u"quit") {
        if (runCommand)
            runCommand(QStringLiteral("workspace:close"));
    } else if (name == u"wq" || name == u"xit") {
        if (!runCommand || !runCommand(QStringLiteral("editor:save-file")))
            if (doc())
                doc()->save();
        if (runCommand)
            runCommand(QStringLiteral("workspace:close"));
    } else if (name == u"nohlsearch") {
        clearSearchHighlight();
    } else if (name == u"yank") {
        // the lines of the range (Obsidian: the cursor line only)
        const int from = selLine(), to = std::max(selLine(), selLineEnd());
        QStringList lines;
        for (int l = from; l <= to && l <= lastLine(); ++l)
            lines << getLine(l);
        G().registerController.pushText(QStringLiteral("0"), QStringLiteral("yank"), lines.join(u'\n'), true, false);
    } else if (name == u"delete") {
        const int line = selLine();
        const int lineEnd = selLineEnd();
        Args a;
        a.linewise = true;
        opDelete(a, {Range{Pos(line, 0), Pos(lineEnd + 1, 0)}});
        setCursor(clipCursorToContent(getCursor()));
    } else if (name == u"join") {
        const int line = selLine();
        const int lineEnd = selLineEnd();
        setCursor(Pos(line, 0));
        Args a;
        a.repeat = lineEnd - line + 1; // (vim.js passes lineEnd - line, joining one line too few)
        actJoinLines(a);
    } else if (name == u"normal") {
        QString argString = params.argString;
        bool noremap = false;
        if (argString.startsWith(u'!')) {
            argString = argString.mid(1);
            noremap = true;
        }
        while (!argString.isEmpty() && argString.front().isSpace())
            argString.remove(0, 1);
        if (argString.isEmpty()) {
            showConfirm(QStringLiteral("Argument is required."));
            return false;
        }
        const bool noremapBefore = m_noremap;
        auto run = [&] {
            m_noremap = noremap;
            doKeyToKey(argString, nullptr);
            m_noremap = noremapBefore;
            if (m_insertMode)
                exitInsertMode(true);
        };
        if (params.line) {
            const int lineEnd = params.lineEnd ? *params.lineEnd : *params.line;
            QList<int> marks;
            for (int i = *params.line; i <= lineEnd && i <= lastLine(); ++i)
                marks << setBookmark(Pos(i, 0), true);
            for (int id : std::as_const(marks)) {
                const auto p = findBookmark(id);
                clearBookmark(id);
                if (!p)
                    continue;
                setCursor(Pos(p->line, 0));
                run();
            }
        } else {
            run();
        }
    } else if (name == u"delmarks") {
        const QString arg = params.argString.trimmed();
        if (arg.isEmpty()) {
            showConfirm(QStringLiteral("Argument required"));
            return false;
        }
        Stream s{arg};
        while (!s.eol()) {
            while (!s.eol() && s.peek().isSpace())
                s.next();
            if (s.eol())
                break;
            const int count = s.pos;
            if (!s.peek().isLetter() || s.peek().unicode() > 0x7f) {
                showConfirm(QStringLiteral("Invalid argument: ") + arg.mid(count));
                return false;
            }
            const QChar sym = s.next();
            if (s.eat(u'-')) {
                if (!s.peek().isLetter() || s.peek().unicode() > 0x7f) {
                    showConfirm(QStringLiteral("Invalid argument: ") + arg.mid(count));
                    return false;
                }
                const QChar finish = s.next();
                if ((sym.isLower() && finish.isLower()) || (sym.isUpper() && finish.isUpper())) {
                    if (sym.unicode() >= finish.unicode()) {
                        showConfirm(QStringLiteral("Invalid argument: ") + arg.mid(count));
                        return false;
                    }
                    for (char16_t c = sym.unicode(); c <= finish.unicode(); ++c)
                        if (auto it = m_marks.find(QString(QChar(c))); it != m_marks.end()) {
                            clearBookmark(*it);
                            m_marks.erase(it);
                        }
                } else {
                    showConfirm(QStringLiteral("Invalid argument: ") + QString(sym) + u'-');
                    return false;
                }
            } else if (auto it = m_marks.find(QString(sym)); it != m_marks.end()) {
                clearBookmark(*it);
                m_marks.erase(it);
            }
        }
    } else if (name == u"image") {
        // Obsidian's image resizing in Live Preview (not available here)
        const QString a = params.args.value(0);
        if (a != u"grow" && a != u"shrink" && a != u"reset" && a != u"edit")
            throw ExError{QStringLiteral("Error: Usage: :image grow|shrink|reset|edit")};
    }
    return false;
}

void VimEngine::doReplace(bool confirm, bool global, int lineStart, int lineEnd, Pos startPos, const Query& query, const QString& replaceWith,
                          std::function<void()> callback)
{
    const Text& t = text();
    if (!confirm) {
        // all at once, as one transaction (the per-match loop of vim.js, batched)
        QList<ChangeSpec> specs;
        std::optional<qsizetype> lastFrom;
        const int endLine = std::min(lineEnd, t.lineCount() - 1);
        for (int line = std::max(0, startPos.line); line <= endLine; ++line) {
            const QString s = t.line(line);
            const qsizetype ls = t.lineStart(line);
            qsizetype pos = 0;
            std::optional<qsizetype> prevEnd;
            while (pos <= s.size()) {
                const QRegularExpressionMatch m = query.re.match(s, pos);
                if (!m.hasMatch())
                    break;
                const qsizetype a = m.capturedStart(), b = m.capturedEnd();
                if (a == b && prevEnd && a == *prevEnd) {
                    pos = b + 1;
                    continue;
                }
                specs.push_back({ls + a, ls + b, expandReplacement(replaceWith, m, s)});
                lastFrom = ls + a;
                prevEnd = b;
                if (!global)
                    break;
                pos = b > a ? b : b + 1;
            }
        }
        if (specs.isEmpty()) {
            showConfirm(QStringLiteral("No matches for ") + query.source);
            return;
        }
        const ChangeSet cs = ChangeSet::of(specs, t.length());
        const qsizetype cursor = cs.mapPos(*lastFrom, -1);
        applyChanges(specs, Selection::cursor(cursor));
        const Pos lastPos = getCursor();
        setCursor(lastPos);
        m_exMode = false;
        m_lastHPos = lastPos.ch;
        m_lastHSPos = lastPos.ch;
        if (callback)
            callback();
        return;
    }
    // confirm ("c"): step through the matches with y/n/a/q/l
    struct State {
        bool done = false;
        std::optional<qsizetype> from, to;
        std::optional<QRegularExpressionMatch> match;
        QString subject;
        std::optional<Pos> lastPos;
        int modifiedLineNumber = -1;
        bool joined = false;
        int lineEnd = 0;
        qsizetype searchFrom = 0;
    };
    auto st = std::make_shared<State>();
    st->lineEnd = lineEnd;
    st->searchFrom = indexFromPos(startPos);
    m_exMode = true;
    auto replace = [this, st, replaceWith]() {
        if (!st->from)
            return;
        const QString newText = expandReplacement(replaceWith, *st->match, st->subject);
        const int unmodifiedLineNumber = posFromIndex(*st->to).line;
        m_busy++;
        beginEdit();
        replaceRange(newText, posFromIndex(*st->from), posFromIndex(*st->to));
        endEdit();
        m_busy--;
        st->to = *st->from + newText.size();
        st->modifiedLineNumber = posFromIndex(*st->to).line;
        st->lineEnd += st->modifiedLineNumber - unmodifiedLineNumber;
        st->joined = st->modifiedLineNumber < unmodifiedLineNumber;
        st->searchFrom = *st->to;
    };
    auto next = [this, st, query, global, lineStart]() {
        while (true) {
            const auto hit = searchForward(query, st->searchFrom);
            if (!hit)
                break;
            const Pos from = posFromIndex(hit->from);
            if (!(from.line >= lineStart && from.line <= st->lineEnd))
                break;
            st->searchFrom = hit->to > hit->from ? hit->to : hit->to + 1;
            if (!global && from.line == st->modifiedLineNumber && !st->joined)
                continue;
            m_view->scrollToPos(hit->from);
            setSelection(from, posFromIndex(hit->to));
            st->from = hit->from;
            st->to = hit->to;
            st->match = hit->match;
            st->subject = text().line(from.line);
            st->lastPos = from;
            st->done = false;
            return;
        }
        st->done = true;
    };
    auto stop = [this, st, callback](const PromptClose* close) {
        if (close)
            (*close)(std::nullopt);
        m_view->setFocus();
        if (st->lastPos) {
            setCursor(clipCursorToContent(*st->lastPos));
            m_exMode = false;
            m_lastHPos = st->lastPos->ch;
            m_lastHSPos = st->lastPos->ch;
        }
        if (callback)
            callback();
    };
    next();
    if (st->done) {
        showConfirm(QStringLiteral("No matches for ") + query.source);
        return;
    }
    auto onKeyDown = [st, replace, next, stop](const QString& key, const QString&, const PromptClose& close) -> bool {
        if (key == u"y") {
            replace();
            next();
        } else if (key == u"n") {
            next();
        } else if (key == u"a") {
            while (!st->done) {
                replace();
                next();
            }
        } else if (key == u"l") {
            replace();
            stop(&close);
            return true;
        } else if (key == u"q" || key == u"<Esc>" || key == u"<C-c>" || key == u"<C-[>") {
            stop(&close);
            return true;
        }
        if (st->done)
            stop(&close);
        return true; // swallow every key
    };
    showPrompt(QStringLiteral("replace with <strong>") + replaceWith.toHtmlEscaped() + QStringLiteral("</strong> (y/n/a/q/l)"), QString(), QString(),
               false, {}, onKeyDown, {}, true);
}

// ---------------------------------------------------------------------------------------------
// :map
// ---------------------------------------------------------------------------------------------
void VimEngine::exMap(ExParams& params, const QString& ctx, bool noremap)
{
    if (params.args.size() < 2) {
        showConfirm(QStringLiteral("Invalid mapping: ") + params.input);
        return;
    }
    const QString lhs = params.args.at(0), rhs = params.args.at(1);
    if (lhs != u":" && lhs.startsWith(u':')) {
        if (!ctx.isEmpty()) {
            showConfirm(QStringLiteral("Error: Mode not supported for ex mappings"));
            return;
        }
        KeyCommand c;
        c.user = true;
        c.keys = lhs.mid(1);
        if (rhs != u":" && rhs.startsWith(u':'))
            c.exInput = rhs.mid(1);
        else
            c.toKeys = rhs;
        G().userExCommands.insert(lhs.mid(1), c);
        return;
    }
    KeyCommand m;
    m.keys = lhs;
    m.type = CmdType::KeyToKey;
    m.toKeys = rhs;
    m.noremap = noremap;
    m.user = true;
    m.context = ctx;
    G().userKeymap.prepend(m);
}

bool VimEngine::exUnmap(const QString& lhs, const QString& ctx)
{
    if (lhs != u":" && lhs.startsWith(u':'))
        return G().userExCommands.remove(lhs.mid(1)) > 0;
    for (qsizetype i = 0; i < G().userKeymap.size(); ++i) {
        if (G().userKeymap[i].keys == lhs && G().userKeymap[i].context == ctx) {
            G().userKeymap.removeAt(i);
            return true;
        }
    }
    return false;
}

void VimEngine::mapClear(const QString& ctx)
{
    if (ctx.isEmpty()) {
        G().userKeymap.clear();
        return;
    }
    QList<KeyCommand> kept;
    for (const KeyCommand& m : std::as_const(G().userKeymap)) {
        if (m.context == ctx)
            continue;
        if (m.context.isEmpty()) {
            // a mapping for every mode: keep it for the other modes
            for (const char* c : {"normal", "insert", "visual"}) {
                if (ctx == QLatin1String(c))
                    continue;
                KeyCommand copy = m;
                copy.context = QString::fromLatin1(c);
                kept << copy;
            }
            continue;
        }
        kept << m;
    }
    G().userKeymap = kept;
}

} // namespace oc::vim
