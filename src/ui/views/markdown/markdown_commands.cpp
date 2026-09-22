#include "ui/views/markdown/markdown_commands.h"

#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "editor/editor_view.h"
#include "editor/live_preview_decorator.h"
#include "editor/markdown_commands.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QPointer>

namespace oc {

namespace {
struct Ctx {
    EditorView* editor = nullptr;
    MarkdownView* view = nullptr;
};
Ctx ctxOf(EditorContext& c) { return {qobject_cast<EditorView*>(c.editor), qobject_cast<MarkdownView*>(c.view)}; }

QList<Hotkey> keys(std::initializer_list<const char*> specs)
{
    QList<Hotkey> out;
    for (const char* s : specs)
        out.append(Hotkey::parse(QString::fromLatin1(s)));
    return out;
}

// An editing command: available when a markdown view is active and not in Reading view.
Command editing(const char* id, const QString& name, const char* icon, std::function<void(EditorView&, MarkdownView&)> run,
                QList<Hotkey> hotkeys = {}, bool repeatable = false)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    c.defaultHotkeys = std::move(hotkeys);
    c.repeatable = repeatable;
    c.editorCheckCallback = [run = std::move(run)](bool checking, EditorContext& ec) {
        const Ctx x = ctxOf(ec);
        if (!x.editor || !x.view || x.view->mode() == EditorView::Mode::Reading || !x.editor->document())
            return false;
        if (!checking)
            run(*x.editor, *x.view);
        return true;
    };
    return c;
}

// A command for any markdown view (Reading view included).
Command viewing(const char* id, const QString& name, const char* icon, std::function<void(MarkdownView&)> run, QList<Hotkey> hotkeys = {})
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    c.defaultHotkeys = std::move(hotkeys);
    c.editorCheckCallback = [run = std::move(run)](bool checking, EditorContext& ec) {
        const Ctx x = ctxOf(ec);
        if (!x.view)
            return false;
        if (!checking)
            run(*x.view);
        return true;
    };
    return c;
}

// Follow the link under the (main) cursor.
bool followLinkAtCursor(EditorView& e, bool newLeaf)
{
    LivePreviewDecorator* lp = e.livePreview();
    if (!lp || !e.document())
        return false;
    const auto hit = lp->linkAt(e.cursorPosition());
    if (!hit)
        return false;
    emit e.linkActivated(hit->target, int(hit->kind), newLeaf);
    return true;
}
} // namespace

void registerMarkdownCommands(CommandRegistry& r, Workspace* workspace)
{
    QPointer<Workspace> ws = workspace;
    r.setEditorProvider([ws]() -> EditorContext {
        if (!ws)
            return {};
        auto* mv = ws->activeViewOfType<MarkdownView>();
        if (!mv) {
            // A sidebar may have focus: fall back to the most recent root leaf (Obsidian's activeEditor).
            if (WorkspaceLeaf* l = ws->activeRootLeaf())
                mv = qobject_cast<MarkdownView*>(l->view());
        }
        return mv ? EditorContext{mv->editor(), mv} : EditorContext{};
    });

    using E = EditorView;
    using V = MarkdownView;
    // formatting
    r.add(editing("editor:toggle-bold", QObject::tr("Toggle bold"), "bold", [](E& e, V&) { mdcmd::toggleBold(e); }, keys({"Mod+B"})));
    r.add(editing("editor:toggle-italics", QObject::tr("Toggle italic"), "italic", [](E& e, V&) { mdcmd::toggleItalics(e); }, keys({"Mod+I"})));
    r.add(editing("editor:toggle-strikethrough", QObject::tr("Toggle strikethrough"), "strikethrough", [](E& e, V&) { mdcmd::toggleStrikethrough(e); }));
    r.add(editing("editor:toggle-highlight", QObject::tr("Toggle highlight"), "highlighter", [](E& e, V&) { mdcmd::toggleHighlight(e); }));
    r.add(editing("editor:toggle-code", QObject::tr("Toggle code"), "code-2", [](E& e, V&) { mdcmd::toggleCode(e); }));
    r.add(editing("editor:toggle-inline-math", QObject::tr("Toggle inline math"), "sigma", [](E& e, V&) { mdcmd::toggleInlineMath(e); }));
    r.add(editing("editor:toggle-comments", QObject::tr("Toggle comment"), "percent", [](E& e, V&) { mdcmd::toggleComment(e); }, keys({"Mod+/"})));
    r.add(editing("editor:toggle-blockquote", QObject::tr("Toggle blockquote"), "quote", [](E& e, V&) { mdcmd::toggleBlockquote(e); }));
    r.add(editing("editor:toggle-bullet-list", QObject::tr("Toggle bullet list"), "list", [](E& e, V&) { mdcmd::toggleBulletList(e); }));
    r.add(editing("editor:toggle-numbered-list", QObject::tr("Toggle numbered list"), "list-ordered", [](E& e, V&) { mdcmd::toggleNumberedList(e); }));
    r.add(editing("editor:toggle-checklist-status", QObject::tr("Toggle checkbox status"), "check-square", [](E& e, V&) { mdcmd::toggleChecklist(e); },
                  keys({"Mod+L"})));
    r.add(editing("editor:clear-formatting", QObject::tr("Clear formatting"), "eraser", [](E& e, V&) { mdcmd::clearFormatting(e); }));
    r.add(editing("editor:set-heading-0", QObject::tr("Remove heading"), "heading-glyph", [](E& e, V&) { mdcmd::setHeading(e, 0); }));
    for (int level = 1; level <= 6; ++level) {
        const QByteArray id = "editor:set-heading-" + QByteArray::number(level);
        Command c = editing(id.constData(), QObject::tr("Set as heading %1").arg(level), "heading-glyph",
                            [level](E& e, V&) { mdcmd::setHeading(e, level); });
        r.add(c);
    }
    // inserting
    r.add(editing("editor:insert-link", QObject::tr("Insert Markdown link"), "link", [](E& e, V&) { mdcmd::insertLink(e); }, keys({"Mod+K"})));
    r.add(editing("editor:insert-wikilink", QObject::tr("Add internal link"), "bracket-glyph", [](E& e, V&) { mdcmd::insertWikilink(e, false); }));
    r.add(editing("editor:insert-embed", QObject::tr("Add embed"), "sticky-note", [](E& e, V&) { mdcmd::insertWikilink(e, true); }));
    r.add(editing("editor:insert-horizontal-rule", QObject::tr("Insert horizontal rule"), "minus", [](E& e, V&) { mdcmd::insertHorizontalRule(e); }));
    r.add(editing("editor:insert-codeblock", QObject::tr("Insert code block"), "code", [](E& e, V&) { mdcmd::insertCodeBlock(e); }));
    r.add(editing("editor:insert-mathblock", QObject::tr("Insert math block"), "sigma-square", [](E& e, V&) { mdcmd::insertMathBlock(e); }));
    r.add(editing("editor:insert-table", QObject::tr("Insert table"), "table", [](E& e, V&) { mdcmd::insertTable(e); }));
    r.add(editing("editor:insert-callout", QObject::tr("Insert callout"), "quote", [](E& e, V&) { mdcmd::insertCallout(e); }));
    // lines and cursors
    r.add(editing("editor:swap-line-up", QObject::tr("Move line up"), "corner-right-up", [](E& e, V&) { e.moveLines(-1); }, {}, true));
    r.add(editing("editor:swap-line-down", QObject::tr("Move line down"), "corner-right-down", [](E& e, V&) { e.moveLines(1); }, {}, true));
    r.add(editing("editor:delete-paragraph", QObject::tr("Delete paragraph"), "scissors", [](E& e, V&) { e.deleteLines(); }, keys({"Mod+D"}), true));
    r.add(editing("editor:add-cursor-above", QObject::tr("Add cursor above"), "mouse-pointer-click", [](E& e, V&) { e.addCursorVertical(-1); }, {}, true));
    r.add(editing("editor:add-cursor-below", QObject::tr("Add cursor below"), "mouse-pointer-click", [](E& e, V&) { e.addCursorVertical(1); }, {}, true));
    r.add(editing("editor:indent-list", QObject::tr("Indent list item"), "indent", [](E& e, V&) {
        if (!mdcmd::indentListItem(e, true))
            e.indentSelection(true);
    }));
    r.add(editing("editor:unindent-list", QObject::tr("Unindent list item"), "outdent", [](E& e, V&) {
        if (!mdcmd::indentListItem(e, false))
            e.indentSelection(false);
    }));
    r.add(editing("editor:undo", QObject::tr("Undo"), "undo-2", [](E& e, V&) { e.undo(); }, {}, true));
    r.add(editing("editor:redo", QObject::tr("Redo"), "redo-2", [](E& e, V&) { e.redo(); }, {}, true));
    r.add(editing("editor:cut", QObject::tr("Cut"), "scissors", [](E& e, V&) { e.cut(); }));
    r.add(editing("editor:copy", QObject::tr("Copy"), "copy", [](E& e, V&) { e.copy(); }));
    r.add(editing("editor:paste", QObject::tr("Paste"), "clipboard-type", [](E& e, V&) { e.paste(); }));
    r.add(viewing("editor:go-start", QObject::tr("Go to first line"), "chevrons-up", [](V& v) { v.editor()->moveDocBoundary(false, false); }));
    r.add(viewing("editor:go-end", QObject::tr("Go to last line"), "chevrons-down", [](V& v) { v.editor()->moveDocBoundary(true, false); }));
    // folding
    r.add(editing("editor:toggle-fold", QObject::tr("Toggle fold on the current line"), "diff", [](E& e, V&) {
        if (e.document())
            e.toggleFoldLine(e.document()->text().lineAt(e.cursorPosition()));
    }));
    r.add(editing("editor:fold-all", QObject::tr("Fold all headings and lists"), "minimize-2", [](E& e, V&) { e.foldAll(); }));
    r.add(editing("editor:unfold-all", QObject::tr("Unfold all headings and lists"), "maximize-2", [](E& e, V&) { e.unfoldAll(); }));
    r.add(editing("editor:fold-more", QObject::tr("Fold more"), "fold-vertical", [](E& e, V&) { e.foldMore(); }));
    r.add(editing("editor:fold-less", QObject::tr("Fold less"), "unfold-vertical", [](E& e, V&) { e.foldLess(); }));
    // links
    r.add(editing("editor:follow-link", QObject::tr("Follow link under cursor"), "link", [](E& e, V&) { followLinkAtCursor(e, false); },
                  keys({"Alt+Enter"})));
    r.add(editing("editor:open-link-in-new-leaf", QObject::tr("Open link under cursor in new tab"), "link",
                  [](E& e, V&) { followLinkAtCursor(e, true); }, keys({"Mod+Enter"})));
    // views
    r.add(viewing("editor:open-search", QObject::tr("Search current file"), "search", [](V& v) { v.showSearch(false); }, keys({"Mod+F"})));
    r.add(editing("editor:open-search-replace", QObject::tr("Search & replace in current file"), "search", [](E&, V& v) { v.showSearch(true); },
                  keys({"Mod+H", "Mod+Alt+F"})));
    r.add(viewing("editor:save-file", QObject::tr("Save current file"), "save", [](V& v) { v.save(); }, keys({"Mod+S"})));
    r.add(editing("editor:toggle-source", QObject::tr("Toggle Live Preview/Source mode"), "code-2", [](E&, V& v) { v.toggleSource(); }));
    r.add(viewing("markdown:toggle-preview", QObject::tr("Toggle reading view"), "book-open", [](V& v) { v.toggleReading(); }, keys({"Mod+E"})));
    r.add(viewing("editor:toggle-readable-line-length", QObject::tr("Toggle readable line length"), "ruler", [](V& v) {
        v.editor()->setReadableLineLength(!v.editor()->readableLineLength());
    }));
}

} // namespace oc
