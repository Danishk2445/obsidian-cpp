#pragma once
// Internal: the vim state machine behind VimController. A port of the behaviour of the
// CodeMirror-vim build Obsidian 1.13 ships (lib/codemirror/vim.js on top of its CodeMirror 6
// adapter): keymap, command dispatcher, motions, operators, actions, text objects, registers,
// marks, search, ex commands, dot repeat and macros. Positions are {line, ch} like vim.js (ch in
// UTF-16 code units) and are converted to document offsets at the edges.

#include "editor/vim/vim_registers.h"
#include "text/selection.h"
#include "text/text.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>

#include <climits>
#include <functional>
#include <memory>
#include <optional>

namespace oc {
class EditorView;
class Document;
} // namespace oc

namespace oc::vim {

class VimPanel;

// "Infinity" / Number.MAX_VALUE for columns (end of line). Large, but safe to add small offsets to.
constexpr int kEol = INT_MAX / 4;

struct Pos {
    int line = 0;
    int ch = 0;
    Pos() = default;
    Pos(int l, int c) : line(l), ch(c) {}
    bool operator==(const Pos&) const = default;
};
inline bool cursorIsBefore(Pos a, Pos b) { return a.line < b.line || (a.line == b.line && a.ch < b.ch); }
inline Pos cursorMin(Pos a, Pos b) { return cursorIsBefore(a, b) ? a : b; }
inline Pos cursorMax(Pos a, Pos b) { return cursorIsBefore(a, b) ? b : a; }
inline bool cursorIsBetween(Pos a, Pos b, Pos c) { return cursorIsBefore(a, b) && cursorIsBefore(b, c); }
inline Pos offsetCursor(Pos p, int dl, int dc) { return Pos(p.line + dl, p.ch + dc); }

struct Range {
    Pos anchor;
    Pos head;
};

enum class CmdType { KeyToKey, Motion, Operator, OperatorMotion, Action, Search, Ex, KeyToEx };

enum class Motion {
    None,
    MoveToTopLine,
    MoveToMiddleLine,
    MoveToBottomLine,
    ExpandToLine,
    FindNext,
    FindAndSelectNextInclusive,
    GoToMark,
    MoveToOtherHighlightedEnd,
    JumpToMark,
    MoveByCharacters,
    MoveByLines,
    MoveByDisplayLines,
    MoveByPage,
    MoveByParagraph,
    MoveBySentence,
    MoveByScroll,
    MoveByWords,
    MoveTillCharacter,
    MoveToCharacter,
    MoveToSymbol,
    MoveToColumn,
    MoveToEol,
    MoveToFirstNonWhiteSpaceCharacter,
    MoveToMatchedSymbol,
    MoveToStartOfLine,
    MoveToLineOrEdgeOfDocument,
    MoveToStartOfDisplayLine,
    MoveToEndOfDisplayLine,
    TextObjectManipulation,
    RepeatLastCharacterSearch,
};

enum class Operator { None, Change, Delete, Indent, IndentAuto, HardWrap, ChangeCase, Yank };

enum class Action {
    None,
    JumpListWalk,
    Scroll,
    ScrollToCursor,
    ReplayMacro,
    EnterMacroRecordMode,
    ToggleOverwrite,
    EnterInsertMode,
    ToggleVisualMode,
    ReselectLastSelection,
    JoinLines,
    NewLineAndEnterInsertMode,
    Paste,
    Undo,
    Redo,
    SetRegister,
    InsertRegister,
    OneNormalCommand,
    SetMark,
    Replace,
    IncrementNumberToken,
    RepeatLastEdit,
    Indent,
    Fold, // za / zo / zc / zR / zM (not in Obsidian's keymap; the editor's folding)
};

// The last visual selection an operator ran on (replayed by ".").
struct LastSel {
    Pos anchor, head;
    bool visualBlock = false;
    bool visualLine = false;
};

// Arguments of motions, operators, actions and searches (vim.js's untyped args objects, merged).
struct Args {
    // motion
    bool forward = false;
    bool linewise = false;
    bool inclusive = false;
    bool toJumplist = false;
    bool explicitRepeat = false;
    bool wordEnd = false;
    bool bigWord = false;
    bool toFirstChar = false;
    bool sameLine = false;
    bool textObjectInner = false;
    bool noRepeat = false;
    int repeatOffset = 0;
    // operator
    bool indentRight = false;
    bool keepCursor = false;
    bool shouldMoveCursor = false;
    bool fullLine = false;
    std::optional<bool> toLower;
    // action
    bool after = false;
    bool isEdit = false;
    bool matchIndent = false;
    bool keepSpaces = false;
    bool blockwise = false;
    bool backtrack = false;
    bool increase = false;
    bool replace = false;
    QString insertAt;
    QString position;
    QString fold;
    std::optional<Pos> head;
    // operatorMotion
    bool visualLine = false;
    bool hasVisualLine = false;
    // search
    QString querySrc;
    bool wholeWordOnly = false;
    // filled in while running
    int repeat = 0;
    bool repeatIsExplicit = false;
    QString selectedCharacter;
    QString registerName;
    std::optional<LastSel> lastSel;
};

struct KeyCommand {
    QString keys;
    CmdType type = CmdType::Motion;
    Motion motion = Motion::None;
    Operator op = Operator::None;
    Action action = Action::None;
    Args motionArgs;
    Args operatorArgs;
    Args actionArgs;
    Args searchArgs;
    Args operatorMotionArgs;
    QString context; // "", "normal", "visual", "insert", "operatorPending"
    bool isEdit = false;
    bool interlaceInsertRepeat = false;
    bool exitVisualBlock = false;
    QString toKeys;      // KeyToKey
    bool noremap = false; // user mappings
    bool user = false;
    QString exInput;     // KeyToEx
    std::optional<int> repeatOverride; // ex ":{n}" runs a motion with this count
};

// Vim's input state while a command is being typed.
struct InputState {
    QString prefixRepeat;
    QString motionRepeat;
    Operator op = Operator::None;
    Args operatorArgs;
    Motion motion = Motion::None;
    Args motionArgs;
    QStringList keyBuffer;
    QString registerName;
    QString selectedCharacter;
    std::optional<int> repeatOverride;
    QString operatorShortcut;
    // insert-mode multi-key mappings ("jk" -> <Esc>): what the typed keys replaced
    bool hasChangeQueue = false;
    QStringList changeQueueRemoved;
    QString changeQueueInserted;

    void pushRepeatDigit(const QString& n);
    int getRepeat() const;
};

// A position that follows edits (CodeMirror bookmark with TrackDel mapping).
struct Bookmark {
    qsizetype offset = -1; // -1: deleted
    int assoc = -1;
};

struct LastSelection {
    int anchorMark = -1; // bookmark ids
    int headMark = -1;
    Pos anchor, head;
    bool visualMode = false, visualLine = false, visualBlock = false;
};

struct MacroModeState {
    QString latestRegister;
    bool isPlaying = false;
    bool isRecording = false;
    QStringList replaySearchQueries;
    InsertChanges lastInsertModeChanges;
};

struct HistoryController {
    QStringList historyBuffer;
    int iterator = 0;
    std::optional<QString> initialPrefix;
    QString nextMatch(const QString& input, bool up);
    void pushInput(const QString& input);
    void reset();
};

// Search query in effect (vim.js keeps a JS RegExp): the pattern and whether it ignores case.
struct Query {
    QString source;
    bool ignoreCase = false;
    QRegularExpression re;
    bool isValid() const { return !source.isEmpty() && re.isValid(); }
    bool operator==(const Query& o) const { return source == o.source && ignoreCase == o.ignoreCase; }
};

// Process-wide state shared by every vim editor (vim.js's vimGlobalState + options).
struct VimGlobal {
    std::optional<Query> query;
    bool isReversed = false;
    std::optional<QString> lastSubstituteReplacePart;
    MacroModeState macroModeState;
    struct {
        int increment = 0;
        bool forward = true;
        QString selectedCharacter;
    } lastCharacterSearch;
    RegisterController registerController;
    HistoryController searchHistoryController;
    HistoryController exCommandHistoryController;
    // options (":set")
    bool pcre = true;
    int textwidth = 80;
    int insertModeEscKeysTimeout = 200;
    // user mappings (":map" & co), most recent first, searched before the defaults
    QList<KeyCommand> userKeymap;
    QHash<QString, KeyCommand> userExCommands; // ":map :x :y"

    static VimGlobal& instance();
    static void reset(); // tests: registers, search, macros, mappings, options
};

// A parsed ex command line (vim.js's params object).
struct ExParams {
    QString input;
    QString commandName;
    QString argString;
    QStringList args;
    std::optional<int> line;
    std::optional<int> lineEnd;
    std::optional<int> selectionLine;
    std::optional<int> selectionLineEnd;
    std::function<void()> callback;
    QString setScope; // "local" / "global" for :setlocal / :setglobal
};

class VimEngine {
public:
    explicit VimEngine(EditorView* view);
    ~VimEngine();

    // --- entry points -------------------------------------------------------------------------
    // Handle one vim key (from a key event, a mapping, a macro or :normal); origin: "user",
    // "mapping", "macro". Returns whether it was consumed.
    bool handleKey(const QString& key, const QString& origin = QStringLiteral("user"));
    // Would handleKey consume `key` in the current state? (no side effects; for ShortcutOverride)
    bool wouldHandle(const QString& key) const;
    void handleEx(const QString& input);
    void enterVimMode();
    void leaveVimMode();

    // --- state for the controller -------------------------------------------------------------
    bool insertMode() const { return m_insertMode; }
    bool overwrite() const { return m_overwrite; }
    bool visualMode() const { return m_visualMode; }
    bool visualLine() const { return m_visualLine; }
    bool visualBlock() const { return m_visualBlock; }
    bool hasPendingInput() const;
    QString pendingKeys() const;
    EditorView* view() const { return m_view; }
    VimPanel* panel() const { return m_panel; }
    void setPanel(VimPanel* panel) { m_panel = panel; }
    std::function<bool(const QString& commandId)> runCommand;
    std::function<void()> modeChanged; // notified after every mode change
    std::function<void()> highlightChanged;

    // Called by the controller for editor events vim tracks.
    void onDocumentChanged(const ChangeDesc& changes, const ChangeSet& set, qsizetype headBefore, bool fromThisView, bool userInput);
    void onSelectionChanged(bool causedByEdit);
    void onFocusChanged(bool focused);
    bool executing() const { return m_busy > 0; }

    // Search highlight (the decoration layer paints these).
    const std::optional<Query>& highlightQuery() const { return m_highlight; }
    // Replace mode typing (the controller routes printable keys here).
    void overwriteType(const QString& text);
    // Ctrl+C with a selection: the editor copies, then vim leaves visual mode / collapses.
    void afterCopy();

    // --- tests & diagnostics --------------------------------------------------------------------
    QString modeName() const;
    const QHash<QString, int>& marks() const { return m_marks; }
    std::optional<Pos> markPos(const QString& name) const;

private:
    friend class VimPanel;
    // document access (vim.js's CodeMirror adapter API)
    Document* doc() const;
    const Text& text() const;
    int lineCount() const;
    int firstLine() const { return 0; }
    int lastLine() const { return lineCount() - 1; }
    QString getLine(int line) const;
    int lineLength(int line) const;
    qsizetype indexFromPos(Pos p) const;
    Pos posFromIndex(qsizetype i) const;
    Pos clipPos(Pos p) const;
    QString getRange(Pos a, Pos b) const;
    Pos getCursor(const char* which = "head") const;
    void setCursor(Pos p);
    void setCursor(int line, int ch) { setCursor(Pos(line, ch)); }
    void setSelection(Pos anchor, Pos head);
    void setSelections(const QList<Range>& ranges, int primary = 0);
    QList<Range> listSelections() const;
    QString getSelection() const;
    QStringList getSelections() const;
    bool somethingSelected() const;
    void replaceRange(const QString& text, Pos from, std::optional<Pos> to = std::nullopt);
    void replaceSelection(const QString& text);
    void replaceSelections(const QStringList& texts);
    void applyChanges(const QList<ChangeSpec>& changes, std::optional<Selection> selection = std::nullopt,
                      const QString& userEvent = QStringLiteral("vim"));
    bool readOnly() const;

    // bookmarks
    int setBookmark(Pos p, bool insertLeft = false);
    std::optional<Pos> findBookmark(int id) const;
    void clearBookmark(int id);

    // key handling (findKey)
    enum class MatchType { None, Partial, Full, Clear };
    struct Match {
        MatchType type = MatchType::None;
        const KeyCommand* command = nullptr;
        bool expectLiteralNext = false;
    };
    Match matchCommand(const QString& keys, const QString& context, InputState& inputState) const;
    bool commandMatches(const QString& keys, const QString& context, const InputState& st, QList<const KeyCommand*>* partial,
                        QList<const KeyCommand*>* full) const;
    const QList<KeyCommand>& keymap() const;
    bool handleKeyInsertMode(const QString& key, const KeyCommand** command, bool* consumed);
    bool handleKeyNonInsertMode(const QString& key, const QString& origin, const KeyCommand** command, bool* consumed);
    void runCommand_(const KeyCommand& command);
    void doKeyToKey(const QString& keys, const KeyCommand* fromKey);
    void clearInputState();
    void logKey(const QString& key);

    // command dispatcher
    void processCommand(const KeyCommand& command);
    void processMotion(const KeyCommand& command);
    void processOperator(const KeyCommand& command);
    void processOperatorMotion(const KeyCommand& command);
    void processAction(const KeyCommand& command);
    void processSearch(const KeyCommand& command);
    void processEx(const KeyCommand& command);
    void evalInput();
    void recordLastEdit(const std::shared_ptr<InputState>& inputState, const KeyCommand* actionCommand = nullptr);

    // motions (vim_motions.cpp)
    struct MotionResult {
        bool valid = false;
        bool isRange = false;
        Pos anchor, head;
        static MotionResult none() { return {}; }
        static MotionResult pos(Pos p) { return {true, false, p, p}; }
        static MotionResult range(Pos a, Pos h) { return {true, true, a, h}; }
    };
    MotionResult runMotion(Motion m, Pos head, Args& args, const InputState& prevInputState);
    Pos moveByCharacters(Pos head, const Args& args) const;
    Pos moveByLines(Pos head, Args& args);
    Pos moveByDisplayLines(Pos head, Args& args);
    Pos moveToStartOfLine(Pos head) const { return Pos(head.line, 0); }
    Pos moveToFirstNonWhiteSpaceCharacter(Pos head) const;
    Pos moveToEolMotion(Pos head, const Args& args, bool keepHPos);
    MotionResult textObjectManipulation(Pos head, Args& args);
    Pos moveToMatchedSymbol(Pos head) const;
    Pos findSymbol(int repeat, bool forward, const QString& symb) const;
    struct Word {
        int from, to, line;
    };
    std::optional<Word> findWord(Pos cur, bool forward, bool bigWord, bool emptyLineIsWord) const;
    Pos moveToWord(Pos cur, int repeat, bool forward, bool wordEnd, bool bigWord) const;
    std::optional<Pos> moveToCharacter(int repeat, bool forward, const QString& character, Pos head) const;
    struct Span {
        Pos start, end;
    };
    Span findParagraph(Pos head, int repeat, int dir, bool inclusive) const;
    Pos findParagraphMotion(Pos head, int repeat, int dir) const;
    Pos getSentence(Pos cur, int repeat, int dir, bool inclusive) const;
    Pos findSentence(Pos cur, int repeat, int dir) const;
    std::optional<Span> selectCompanionObject(Pos head, QChar symb, bool inclusive) const;
    Span findBeginningAndEnd(Pos head, QChar symb, bool inclusive) const;
    std::optional<Span> expandWordUnderCursor(bool inclusive, bool innerWord, bool bigWord, bool noSymbol, bool multiline,
                                             std::optional<Pos> cursor = std::nullopt) const;
    Span expandTagUnderCursor(Pos head, bool inclusive) const;
    std::optional<Pos> scanForBracket(Pos where, int dir, const QString& bracketChars) const;
    std::optional<Pos> findMatchingBracket(Pos where) const;
    QPair<Pos, Pos> expandSelection(Pos start, Pos end, bool move) const;
    int getUserVisibleTop() const;
    int getUserVisibleBottom() const;
    double charCoordsLeft(Pos p) const;
    Pos coordsChar(double x, double y) const;
    void recordLastCharacterSearch(int increment, const Args& args);
    void recordJumpPosition(Pos oldCur, Pos newCur);

    // operators & actions (vim_actions.cpp)
    std::optional<Pos> runOperator(Operator op, Args& args, const QList<Range>& ranges, Pos oldAnchor, Pos newHead);
    std::optional<Pos> opChange(Args& args, const QList<Range>& ranges);
    std::optional<Pos> opDelete(Args& args, const QList<Range>& ranges);
    std::optional<Pos> opIndent(Args& args, const QList<Range>& ranges);
    std::optional<Pos> opChangeCase(Args& args, const QList<Range>& ranges, Pos oldAnchor, Pos newHead);
    std::optional<Pos> opYank(Args& args, const QList<Range>& ranges, Pos oldAnchor);
    std::optional<Pos> opHardWrap(Args& args, const QList<Range>& ranges, Pos oldAnchor);
    void runAction(Action a, Args& args);
    void actJumpListWalk(Args& args);
    void actScroll(Args& args);
    void actScrollToCursor(Args& args);
    void actReplayMacro(Args& args);
    void actEnterMacroRecordMode(Args& args);
    void actToggleOverwrite();
    void actEnterInsertMode(Args& args);
    void actToggleVisualMode(Args& args);
    void actReselectLastSelection();
    void actJoinLines(Args& args);
    void actNewLineAndEnterInsertMode(Args& args);
    void actPaste(Args& args);
    void actUndo(Args& args);
    void actRedo(Args& args);
    void actInsertRegister(Args& args);
    void actOneNormalCommand();
    void actReplace(Args& args);
    void actIncrementNumberToken(Args& args);
    void actRepeatLastEdit(Args& args);
    void actIndentLine(Args& args);
    void actFold(Args& args);
    void newlineAndIndent();
    void newlineAndIndentBefore();
    int hardWrap(int from, int to, int column);

    // modes
    void enterInsertMode(); // vim.js enterInsertMode(cm)
    void exitInsertMode(bool keepCursor = false);
    void exitVisualMode(bool moveHead = true);
    void updateCmSelection();
    struct CmSelection {
        QList<Range> ranges;
        int primary = 0;
    };
    CmSelection makeCmSelection(Range sel, const QString& mode, bool exclusive = false) const;
    void updateLastSelection();
    void updateMark(const QString& name, Pos pos);
    std::optional<Pos> getMarkPos(const QString& name) const;
    std::optional<Pos> getLastEditPos() const;
    Pos clipCursorToContent(Pos cur, std::optional<Pos> oldCur = std::nullopt) const;
    void selectForInsert(Pos head, int height);
    Pos selectBlock(Pos selectionEnd);
    QPair<Pos, Pos> getSelectedAreaRange();
    QPair<Pos, Pos> updateSelectionForSurrogateCharacters(Pos start, Pos end) const;
    void handleExternalSelection();
    void signalModeChange();

    // dot repeat & macros
    void repeatLastEdit(int repeat, bool repeatForInsert);
    void repeatInsertModeChanges(const QList<InsertOp>& changes, int repeat);
    void executeMacroRegister(const QString& registerName);
    void logInsertModeChange();

    // search & ex (vim_ex.cpp)
    std::optional<Query> parseQuery(const QString& query, bool ignoreCase, bool smartCase);
    std::optional<Query> updateSearchQuery(const QString& rawQuery, bool ignoreCase, bool smartCase, bool* invalid = nullptr);
    void highlightSearchMatches(const Query& query);
    void clearSearchHighlight();
    struct SearchHit {
        qsizetype from = 0, to = 0;
        QRegularExpressionMatch match;
    };
    std::optional<SearchHit> searchForward(const Query& q, qsizetype from) const;
    std::optional<SearchHit> searchBackward(const Query& q, qsizetype endAt) const;
    std::optional<Pos> findNext(bool prev, const Query& query, int repeat = 1);
    std::optional<QPair<Pos, Pos>> findNextFromAndToInclusive(bool prev, const Query& query, int repeat);
    void showConfirm(const QString& text, bool isLong = false);
    using PromptClose = std::function<void(const std::optional<QString>&)>;
    using PromptKeyFn = std::function<bool(const QString& key, const QString& value, const PromptClose& close)>;
    void showPrompt(const QString& prefix, const QString& desc, const QString& value, bool selectValue, std::function<void(const QString&)> onClose,
                    PromptKeyFn onKeyDown, PromptKeyFn onKeyUp, bool richPrefix = false);
    void processExCommand(const QString& input, std::function<void()> callback = {});
    bool exCommand(const QString& name, ExParams& params);
    void doReplace(bool confirm, bool global, int lineStart, int lineEnd, Pos startPos, const Query& query, const QString& replaceWith,
                   std::function<void()> callback);
    void exMap(ExParams& params, const QString& ctx, bool noremap);
    bool exUnmap(const QString& lhs, const QString& ctx);
    void mapClear(const QString& ctx);

    // undo grouping
    void beginEdit();
    void endEdit();
    void ensureCursorVisible();

    // state -----------------------------------------------------------------------------------
    EditorView* m_view;
    VimPanel* m_panel = nullptr;
    std::shared_ptr<InputState> m_inputState;
    std::shared_ptr<InputState> m_lastEditInputState;
    std::optional<KeyCommand> m_lastEditActionCommand;
    int m_lastHPos = -1;
    double m_lastHSPos = -1;
    Motion m_lastMotion = Motion::None;
    QHash<QString, int> m_marks; // name -> bookmark id
    bool m_insertMode = false;
    bool m_insertModeReturn = false;
    int m_insertModeRepeat = 0;
    bool m_visualMode = false;
    bool m_visualLine = false;
    bool m_visualBlock = false;
    bool m_wasInVisualBlock = false;
    std::optional<LastSelection> m_lastSelection;
    std::optional<QString> m_lastPastedText;
    Range m_sel;
    bool m_expectLiteralNext = false;
    bool m_overwrite = false;
    int m_insertEnd = -1; // bookmark
    bool m_exMode = false;
    QString m_status;
    std::optional<Query> m_highlight;
    bool m_insertEditGroup = false; // an undo group spans the insert session
    bool m_recordingInsert = false; // insert-mode edits are recorded for "." (vim.js onChange attached)

    // bookmarks
    QHash<int, Bookmark> m_bookmarks;
    int m_nextBookmark = 1;
    // jump list (per editor)
    struct JumpList {
        static constexpr int size = 100;
        int pointer = -1, head = 0, tail = 0;
        QList<int> buffer = QList<int>(size, -1);
        std::optional<Pos> cachedCursor;
    } m_jumpList;
    void jumpListAdd(Pos oldCur, Pos newCur);
    int jumpListMove(int offset);
    std::optional<Pos> jumpListFind(int offset);

    // operation bookkeeping
    std::optional<qsizetype> m_changeStart; // start of the changed text (undo/redo put the cursor there)
    bool m_trackChangeStart = false;
    int m_busy = 0;          // > 0 while vim itself changes the editor
    int m_editDepth = 0;
    qsizetype m_lastChangeEnd = 0;
    bool m_hasLastChangeEnd = false;
    int m_keyToKeyDepth = 0;
    QList<KeyCommand> m_keyToKeyStack; // mappings being expanded (recursion guard)
    bool m_noremap = false;
    bool m_closeNotificationOnKey = false;
    QElapsedTimer m_insertKeyTimer;
    // a prompt opened while replaying keys (":normal /foo<CR>", mappings) takes the following keys
    struct VirtualPrompt {
        QString prefix, desc, value;
        std::function<void(const QString&)> onClose;
        PromptKeyFn onKeyDown, onKeyUp;
    };
    std::optional<VirtualPrompt> m_virtualPrompt;
    void sendKeyToPrompt(const QString& key);
};

} // namespace oc::vim
