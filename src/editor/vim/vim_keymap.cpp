// The default key map: the same entries, order and arguments as the vim.js Obsidian 1.13 ships
// (plus za/zo/zc/zR/zM for the editor's folding). Order matters: the first full match wins.
#include "editor/vim/vim_engine.h"

namespace oc::vim {

namespace {
// "forward linewise insertAt=eol" -> Args
Args A(const char* spec = "")
{
    Args a;
    for (const QString& tok : QString::fromLatin1(spec).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const qsizetype eq = tok.indexOf(QLatin1Char('='));
        const QString k = eq < 0 ? tok : tok.left(eq);
        const QString v = eq < 0 ? QString() : tok.mid(eq + 1);
        if (k == u"forward") a.forward = true;
        else if (k == u"linewise") a.linewise = true;
        else if (k == u"inclusive") a.inclusive = true;
        else if (k == u"toJumplist") a.toJumplist = true;
        else if (k == u"explicitRepeat") a.explicitRepeat = true;
        else if (k == u"wordEnd") a.wordEnd = true;
        else if (k == u"bigWord") a.bigWord = true;
        else if (k == u"toFirstChar") a.toFirstChar = true;
        else if (k == u"sameLine") a.sameLine = true;
        else if (k == u"textObjectInner") a.textObjectInner = true;
        else if (k == u"indentRight") a.indentRight = true;
        else if (k == u"keepCursor") a.keepCursor = true;
        else if (k == u"shouldMoveCursor") a.shouldMoveCursor = true;
        else if (k == u"fullLine") a.fullLine = true;
        else if (k == u"toLower") a.toLower = true;
        else if (k == u"toUpper") a.toLower = false;
        else if (k == u"after") a.after = true;
        else if (k == u"isEdit") a.isEdit = true;
        else if (k == u"matchIndent") a.matchIndent = true;
        else if (k == u"keepSpaces") a.keepSpaces = true;
        else if (k == u"blockwise") a.blockwise = true;
        else if (k == u"backtrack") a.backtrack = true;
        else if (k == u"increase") a.increase = true;
        else if (k == u"replace") a.replace = true;
        else if (k == u"visualLine") a.visualLine = true;
        else if (k == u"wholeWordOnly") a.wholeWordOnly = true;
        else if (k == u"insertAt") a.insertAt = v;
        else if (k == u"position") a.position = v;
        else if (k == u"querySrc") a.querySrc = v;
        else if (k == u"fold") a.fold = v;
        else if (k == u"repeatOffset") a.repeatOffset = v.toInt();
        else qFatal("vim keymap: unknown arg %s", qPrintable(k));
    }
    return a;
}

KeyCommand K2K(const char* keys, const char* to, const char* ctx = "")
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::KeyToKey;
    c.toKeys = QString::fromLatin1(to);
    c.context = QString::fromLatin1(ctx);
    return c;
}

KeyCommand M(const char* keys, Motion m, const char* args = "", const char* ctx = "")
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::Motion;
    c.motion = m;
    c.motionArgs = A(args);
    c.context = QString::fromLatin1(ctx);
    return c;
}

KeyCommand O(const char* keys, Operator op, const char* args = "", const char* ctx = "", bool isEdit = false, bool exitVisualBlock = false)
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::Operator;
    c.op = op;
    c.operatorArgs = A(args);
    c.context = QString::fromLatin1(ctx);
    c.isEdit = isEdit;
    c.exitVisualBlock = exitVisualBlock;
    return c;
}

KeyCommand OM(const char* keys, Operator op, Motion m, const char* motionArgs, const char* opArgs, const char* opMotionArgs, const char* ctx = "")
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::OperatorMotion;
    c.op = op;
    c.motion = m;
    c.motionArgs = A(motionArgs);
    c.operatorArgs = A(opArgs);
    c.operatorMotionArgs = A(opMotionArgs);
    c.context = QString::fromLatin1(ctx);
    return c;
}

KeyCommand ACT(const char* keys, Action act, const char* args = "", const char* ctx = "", bool isEdit = false, bool interlace = false)
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::Action;
    c.action = act;
    c.actionArgs = A(args);
    c.context = QString::fromLatin1(ctx);
    c.isEdit = isEdit;
    c.interlaceInsertRepeat = interlace;
    return c;
}

KeyCommand S(const char* keys, const char* args)
{
    KeyCommand c;
    c.keys = QString::fromLatin1(keys);
    c.type = CmdType::Search;
    c.searchArgs = A(args);
    return c;
}

QList<KeyCommand> build()
{
    using Mo = Motion;
    using Op = Operator;
    using Ac = Action;
    QList<KeyCommand> m;
    // Key to key mapping. This goes first to make it possible to override existing mappings.
    m << K2K("<Left>", "h") << K2K("<Right>", "l") << K2K("<Up>", "k") << K2K("<Down>", "j") << K2K("g<Up>", "gk")
      << K2K("g<Down>", "gj") << K2K("<Space>", "l") << K2K("<BS>", "h") << K2K("<Del>", "x") << K2K("<C-Space>", "W")
      << K2K("<C-BS>", "B") << K2K("<S-Space>", "w") << K2K("<S-BS>", "b") << K2K("<C-n>", "j") << K2K("<C-p>", "k")
      << K2K("<C-[>", "<Esc>") << K2K("<C-c>", "<Esc>") << K2K("<C-[>", "<Esc>", "insert") << K2K("<C-c>", "<Esc>", "insert")
      << K2K("<C-Esc>", "<Esc>") << K2K("<C-Esc>", "<Esc>", "insert") << K2K("s", "cl", "normal") << K2K("s", "c", "visual")
      << K2K("S", "cc", "normal") << K2K("S", "VdO", "visual") << K2K("<Home>", "0") << K2K("<End>", "$")
      << K2K("<PageUp>", "<C-b>") << K2K("<PageDown>", "<C-f>") << K2K("<CR>", "j^", "normal") << K2K("<Ins>", "i", "normal")
      << ACT("<Ins>", Ac::ToggleOverwrite, "", "insert");
    // Motions
    m << M("H", Mo::MoveToTopLine, "linewise toJumplist") << M("M", Mo::MoveToMiddleLine, "linewise toJumplist")
      << M("L", Mo::MoveToBottomLine, "linewise toJumplist") << M("h", Mo::MoveByCharacters, "") << M("l", Mo::MoveByCharacters, "forward")
      << M("j", Mo::MoveByLines, "forward linewise") << M("k", Mo::MoveByLines, "linewise") << M("gj", Mo::MoveByDisplayLines, "forward")
      << M("gk", Mo::MoveByDisplayLines, "") << M("w", Mo::MoveByWords, "forward") << M("W", Mo::MoveByWords, "forward bigWord")
      << M("e", Mo::MoveByWords, "forward wordEnd inclusive") << M("E", Mo::MoveByWords, "forward wordEnd bigWord inclusive")
      << M("b", Mo::MoveByWords, "") << M("B", Mo::MoveByWords, "bigWord") << M("ge", Mo::MoveByWords, "wordEnd inclusive")
      << M("gE", Mo::MoveByWords, "wordEnd bigWord inclusive") << M("{", Mo::MoveByParagraph, "toJumplist")
      << M("}", Mo::MoveByParagraph, "forward toJumplist") << M("(", Mo::MoveBySentence, "") << M(")", Mo::MoveBySentence, "forward")
      << M("<C-f>", Mo::MoveByPage, "forward") << M("<C-b>", Mo::MoveByPage, "") << M("<C-d>", Mo::MoveByScroll, "forward explicitRepeat")
      << M("<C-u>", Mo::MoveByScroll, "explicitRepeat")
      << M("gg", Mo::MoveToLineOrEdgeOfDocument, "explicitRepeat linewise toJumplist")
      << M("G", Mo::MoveToLineOrEdgeOfDocument, "forward explicitRepeat linewise toJumplist")
      << M("g_", Mo::MoveToLastNonBlank, "inclusive") << M("g$", Mo::MoveToEndOfDisplayLine) << M("g^", Mo::MoveToStartOfDisplayLine) << M("g0", Mo::MoveToStartOfDisplayLine)
      << M("0", Mo::MoveToStartOfLine) << M("^", Mo::MoveToFirstNonWhiteSpaceCharacter) << M("+", Mo::MoveByLines, "forward toFirstChar")
      << M("-", Mo::MoveByLines, "toFirstChar") << M("_", Mo::MoveByLines, "forward toFirstChar repeatOffset=-1")
      << M("$", Mo::MoveToEol, "inclusive") << M("%", Mo::MoveToMatchedSymbol, "inclusive toJumplist")
      << M("f<character>", Mo::MoveToCharacter, "forward inclusive") << M("F<character>", Mo::MoveToCharacter, "")
      << M("t<character>", Mo::MoveTillCharacter, "forward inclusive") << M("T<character>", Mo::MoveTillCharacter, "")
      << M(";", Mo::RepeatLastCharacterSearch, "forward") << M(",", Mo::RepeatLastCharacterSearch, "")
      << M("'<register>", Mo::GoToMark, "toJumplist linewise") << M("`<register>", Mo::GoToMark, "toJumplist")
      << M("]`", Mo::JumpToMark, "forward") << M("[`", Mo::JumpToMark, "") << M("]'", Mo::JumpToMark, "forward linewise")
      << M("['", Mo::JumpToMark, "linewise")
      // the next two aren't motions but must come before more general motion declarations
      << ACT("]p", Ac::Paste, "after isEdit matchIndent", "", true) << ACT("[p", Ac::Paste, "isEdit matchIndent", "", true)
      << M("]<character>", Mo::MoveToSymbol, "forward toJumplist") << M("[<character>", Mo::MoveToSymbol, "toJumplist")
      << M("|", Mo::MoveToColumn) << M("o", Mo::MoveToOtherHighlightedEnd, "", "visual")
      << M("O", Mo::MoveToOtherHighlightedEnd, "sameLine", "visual");
    // Operators
    m << O("d", Op::Delete) << O("y", Op::Yank) << O("c", Op::Change) << O("=", Op::IndentAuto) << O(">", Op::Indent, "indentRight")
      << O("<", Op::Indent) << O("g~", Op::ChangeCase) << O("gu", Op::ChangeCase, "toLower", "", true)
      << O("gU", Op::ChangeCase, "toUpper", "", true) << M("n", Mo::FindNext, "forward toJumplist") << M("N", Mo::FindNext, "toJumplist")
      << M("gn", Mo::FindAndSelectNextInclusive, "forward") << M("gN", Mo::FindAndSelectNextInclusive, "")
      << O("gq", Op::HardWrap) << O("gw", Op::HardWrap, "keepCursor");
    // Operator-Motion dual commands
    m << OM("x", Op::Delete, Mo::MoveByCharacters, "forward", "", "")
      << OM("X", Op::Delete, Mo::MoveByCharacters, "", "", "visualLine")
      << OM("D", Op::Delete, Mo::MoveToEol, "inclusive", "", "", "normal") << O("D", Op::Delete, "linewise", "visual")
      << OM("Y", Op::Yank, Mo::ExpandToLine, "linewise", "", "", "normal") << O("Y", Op::Yank, "linewise", "visual")
      << OM("C", Op::Change, Mo::MoveToEol, "inclusive", "", "", "normal") << O("C", Op::Change, "linewise", "visual")
      << OM("~", Op::ChangeCase, Mo::MoveByCharacters, "forward", "shouldMoveCursor", "", "normal") << O("~", Op::ChangeCase, "", "visual")
      << OM("<C-u>", Op::Delete, Mo::MoveToStartOfLine, "", "", "", "insert")
      << OM("<C-w>", Op::Delete, Mo::MoveByWords, "", "", "", "insert");
    // Actions
    m << ACT("<C-i>", Ac::JumpListWalk, "forward") << ACT("<C-o>", Ac::JumpListWalk, "") << ACT("<C-e>", Ac::Scroll, "forward linewise")
      << ACT("<C-y>", Ac::Scroll, "linewise") << ACT("a", Ac::EnterInsertMode, "insertAt=charAfter", "normal", true)
      << ACT("A", Ac::EnterInsertMode, "insertAt=eol", "normal", true) << ACT("A", Ac::EnterInsertMode, "insertAt=endOfSelectedArea", "visual", true)
      << ACT("i", Ac::EnterInsertMode, "insertAt=inplace", "normal", true) << ACT("gi", Ac::EnterInsertMode, "insertAt=lastEdit", "normal", true)
      << ACT("I", Ac::EnterInsertMode, "insertAt=firstNonBlank", "normal", true) << ACT("gI", Ac::EnterInsertMode, "insertAt=bol", "normal", true)
      << ACT("I", Ac::EnterInsertMode, "insertAt=startOfSelectedArea", "visual", true)
      << ACT("o", Ac::NewLineAndEnterInsertMode, "after", "normal", true, true) << ACT("O", Ac::NewLineAndEnterInsertMode, "", "normal", true, true)
      << ACT("v", Ac::ToggleVisualMode) << ACT("V", Ac::ToggleVisualMode, "linewise") << ACT("<C-v>", Ac::ToggleVisualMode, "blockwise")
      << ACT("<C-q>", Ac::ToggleVisualMode, "blockwise") << ACT("gv", Ac::ReselectLastSelection) << ACT("J", Ac::JoinLines, "", "", true)
      << ACT("gJ", Ac::JoinLines, "keepSpaces", "", true) << ACT("p", Ac::Paste, "after isEdit", "", true) << ACT("P", Ac::Paste, "isEdit", "", true)
      << ACT("r<character>", Ac::Replace, "", "", true) << ACT("@<register>", Ac::ReplayMacro) << ACT("q<register>", Ac::EnterMacroRecordMode)
      // Replace mode is a special case of insert mode.
      << ACT("R", Ac::EnterInsertMode, "replace", "normal", true) << O("R", Op::Change, "linewise fullLine", "visual", false, true)
      << ACT("u", Ac::Undo, "", "normal") << O("u", Op::ChangeCase, "toLower", "visual", true) << O("U", Op::ChangeCase, "toUpper", "visual", true)
      << ACT("<C-r>", Ac::Redo) << ACT("m<register>", Ac::SetMark) << ACT("\"<register>", Ac::SetRegister)
      << ACT("<C-r><register>", Ac::InsertRegister, "", "insert", true) << ACT("<C-o>", Ac::OneNormalCommand, "", "insert")
      << ACT("zz", Ac::ScrollToCursor, "position=center");
    {
        KeyCommand c = ACT("z.", Ac::ScrollToCursor, "position=center");
        c.motion = Mo::MoveToFirstNonWhiteSpaceCharacter;
        m << c;
    }
    m << ACT("zt", Ac::ScrollToCursor, "position=top");
    {
        KeyCommand c = ACT("z<CR>", Ac::ScrollToCursor, "position=top");
        c.motion = Mo::MoveToFirstNonWhiteSpaceCharacter;
        m << c;
    }
    m << ACT("zb", Ac::ScrollToCursor, "position=bottom");
    {
        KeyCommand c = ACT("z-", Ac::ScrollToCursor, "position=bottom");
        c.motion = Mo::MoveToFirstNonWhiteSpaceCharacter;
        m << c;
    }
    m << ACT("za", Ac::Fold, "fold=za", "normal") << ACT("zo", Ac::Fold, "fold=zo", "normal") << ACT("zc", Ac::Fold, "fold=zc", "normal")
      << ACT("zR", Ac::Fold, "fold=zR", "normal") << ACT("zM", Ac::Fold, "fold=zM", "normal")
      << ACT(".", Ac::RepeatLastEdit) << ACT("<C-a>", Ac::IncrementNumberToken, "increase", "", true)
      << ACT("<C-x>", Ac::IncrementNumberToken, "", "", true) << ACT("<C-t>", Ac::Indent, "indentRight", "insert")
      << ACT("<C-d>", Ac::Indent, "", "insert");
    // Text object motions
    m << M("a<register>", Mo::TextObjectManipulation) << M("i<register>", Mo::TextObjectManipulation, "textObjectInner");
    // Search
    m << S("/", "forward querySrc=prompt toJumplist") << S("?", "querySrc=prompt toJumplist")
      << S("*", "forward querySrc=wordUnderCursor wholeWordOnly toJumplist") << S("#", "querySrc=wordUnderCursor wholeWordOnly toJumplist")
      << S("g*", "forward querySrc=wordUnderCursor toJumplist") << S("g#", "querySrc=wordUnderCursor toJumplist");
    // Ex command
    {
        KeyCommand c;
        c.keys = QStringLiteral(":");
        c.type = CmdType::Ex;
        m << c;
    }
    return m;
}
} // namespace

const QList<KeyCommand>& defaultKeymap()
{
    static const QList<KeyCommand> map = build();
    return map;
}

} // namespace oc::vim
