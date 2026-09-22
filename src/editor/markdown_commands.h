#pragma once

#include <QString>

namespace oc {

class EditorView;

// Obsidian's Markdown-aware editing behaviours and editor:* commands, operating on an EditorView.
namespace mdcmd {

struct Settings {
    bool smartIndentList = true;   // app.json "smartIndentList"
    bool autoPairBrackets = true;  // "autoPairBrackets"
    bool autoPairMarkdown = true;  // "autoPairMarkdown"
    bool useTab = true;            // "useTab"
    int tabSize = 4;               // "tabSize"
};
Settings& settings();

// Enter: continue lists/tasks/quotes, exit an empty list item. Returns true if handled.
bool newlineAndContinueMarkup(EditorView& view);
// Tab / Shift+Tab on list items: indent/outdent the item (and renumber ordered siblings). Returns true if handled.
bool indentListItem(EditorView& view, bool more);
// Backspace right after a list/quote marker removes the marker. Returns true if handled.
bool deleteMarkupBackward(EditorView& view);
// Typed-char helpers for auto-pairing; returns true if handled.
bool autoPairInput(EditorView& view, const QString& text);

// Formatting toggles (wrap/unwrap the selection or the word at the cursor).
void toggleWrap(EditorView& view, const QString& open, const QString& close);
void toggleBold(EditorView& view);          // **
void toggleItalics(EditorView& view);       // *
void toggleStrikethrough(EditorView& view); // ~~
void toggleHighlight(EditorView& view);     // ==
void toggleCode(EditorView& view);          // `
void toggleInlineMath(EditorView& view);    // $
void toggleComment(EditorView& view);       // %% (block comment around multi-line selections)
void toggleBlockquote(EditorView& view);
void toggleBulletList(EditorView& view);
void toggleNumberedList(EditorView& view);
void toggleChecklist(EditorView& view);     // Mod+L: plain -> "- [ ] " -> "- [x] " -> "- [ ] "
void setHeading(EditorView& view, int level); // 0 removes
void insertLink(EditorView& view);          // [text](url) — Mod+K
void insertWikilink(EditorView& view, bool embed = false);
void insertHorizontalRule(EditorView& view);
void insertCodeBlock(EditorView& view);
void insertMathBlock(EditorView& view);
void insertTable(EditorView& view);
void insertCallout(EditorView& view, const QString& type = QStringLiteral("note"));
void clearFormatting(EditorView& view);

} // namespace mdcmd
} // namespace oc
