#include "ui/views/sidebar/sidebar_registration.h"

#include "core/bookmarks/bookmarks.h"
#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/metadata/link_suggest.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/outline.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "markdown/metadata.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/sidebar/all_properties_view.h"
#include "ui/views/sidebar/backlink_view.h"
#include "ui/views/sidebar/bookmarks_view.h"
#include "ui/views/sidebar/outgoing_link_view.h"
#include "ui/views/sidebar/outline_view.h"
#include "ui/views/sidebar/tag_view.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>

namespace oc {

void registerSidebarViews(ViewRegistry& registry)
{
    registry.registerView(QStringLiteral("backlink"), [](WorkspaceLeaf* leaf) -> View* { return new BacklinkView(leaf); });
    registry.registerView(QStringLiteral("outgoing-link"), [](WorkspaceLeaf* leaf) -> View* { return new OutgoingLinkView(leaf); });
    registry.registerView(QStringLiteral("tag"), [](WorkspaceLeaf* leaf) -> View* { return new TagView(leaf); });
    registry.registerView(QStringLiteral("outline"), [](WorkspaceLeaf* leaf) -> View* { return new OutlineView(leaf); });
    registry.registerView(QStringLiteral("bookmarks"), [](WorkspaceLeaf* leaf) -> View* { return new BookmarksView(leaf); });
    registry.registerView(QStringLiteral("all-properties"), [](WorkspaceLeaf* leaf) -> View* { return new AllPropertiesView(leaf); });
}

namespace {

QString tr(const char* s) { return QCoreApplication::translate("oc::Sidebar", s); }

Command makeCommand(const char* id, const QString& name, const char* icon)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    return c;
}

// Obsidian's ensureSideLeaf(type, side, {active: true}).
bool showSideView(MainWindow* w, const QString& type, Workspace::Side side, bool checking)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    if (!ws || !ws->viewRegistry() || !ws->viewRegistry()->hasView(type))
        return false;
    if (!checking)
        if (WorkspaceLeaf* l = ws->ensureSideLeaf(type, side, true))
            ws->setActiveLeaf(l, true);
    return true;
}

// Obsidian's "... for the current file": a new leaf next to the active note, linked to it.
bool openForCurrent(MainWindow* w, const QString& type, bool checking)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    TFile* f = ws ? workspaceActiveFile(ws) : nullptr;
    if (!f || !ws->viewRegistry() || !ws->viewRegistry()->hasView(type))
        return false;
    if (checking)
        return true;
    WorkspaceLeaf* source = ws->activeRootLeaf();
    WorkspaceLeaf* leaf = ws->splitLeaf(source, Qt::Horizontal);
    if (!leaf)
        return true;
    leaf->setViewState(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("state"), QJsonObject{{QStringLiteral("file"), f->path()}}}}, {}, false);
    if (auto* v = qobject_cast<LinkedFileView*>(leaf->view()))
        v->linkTo(source);
    ws->setActiveLeaf(leaf, true);
    return true;
}

MarkdownView* editorView(EditorContext& ec) { return qobject_cast<MarkdownView*>(ec.view); }

// The bookmark for the section under the cursor (Obsidian's "Bookmark block under cursor..."): the
// heading, the list item or the section; blocks without an id get a generated one, written into the
// note when the bookmark is saved.
bool bookmarkCurrentSection(MainWindow* w, EditorContext& ec, bool checking)
{
    MarkdownView* mv = editorView(ec);
    VaultSession* s = w ? w->session() : nullptr;
    if (!mv || !mv->file() || !mv->document() || !s || !s->metadata())
        return false;
    if (checking)
        return true;
    const md::CachedMetadata* meta = s->metadata()->fileCache(mv->file()->path());
    if (!meta)
        return true;
    EditorView* e = mv->editor();
    Document* doc = mv->document();
    const Text& text = doc->text();
    const int line = text.lineAt(e->selection().main().to());
    const md::SectionCache* section = nullptr;
    for (const md::SectionCache& sc : meta->sections)
        if (sc.position.start.line <= line && sc.position.end.line >= line)
            section = &sc;
    if (!section)
        return true;
    const QString path = mv->file()->path();
    std::unique_ptr<Bookmark> item;
    std::function<void()> onSave;
    if (section->type == u"heading") {
        for (const md::HeadingCache& h : meta->headings)
            if (h.position.start.line <= line && h.position.end.line >= line)
                item = Bookmark::file(path, QLatin1Char('#') + headingSubpathText(h.heading));
    } else {
        QString id;
        LinkSuggestion block;
        block.content = text.toString();
        if (section->type == u"list") {
            int found = -1;
            for (int i = 0; i < meta->listItems.size(); ++i)
                if (meta->listItems[i].position.start.line <= line && meta->listItems[i].position.end.line >= line)
                    found = i;
            if (found < 0)
                return true;
            const md::ListItemCache& li = meta->listItems[found];
            id = li.id;
            block.blockType = QStringLiteral("listItem");
            block.blockFrom = int(li.position.start.offset);
            block.blockTo = int(li.position.end.offset);
            for (int j = found + 1; j < meta->listItems.size(); ++j)
                if (meta->listItems[j].parent == li.position.start.line) {
                    block.blockParagraphEnd = int(text.lineEnd(std::max(li.position.start.line, meta->listItems[j].position.start.line - 1)));
                    break;
                }
        } else {
            id = section->id;
            block.blockType = section->type;
            block.blockFrom = int(section->position.start.offset);
            block.blockTo = int(section->position.end.offset);
        }
        if (id.isEmpty()) {
            id = generateBlockId();
            QPointer<Document> d = doc;
            onSave = [d, block, id] {
                if (!d || d->text().toString() != block.content)
                    return; // edited meanwhile: don't guess
                const BlockIdInsertion ins = blockIdInsertion(block, id);
                Transaction tr;
                tr.changes = ChangeSet::of(ChangeSpec{ins.at, ins.at, ins.text}, d->text().length());
                tr.userEvent = QStringLiteral("input");
                tr.scrollIntoView = false;
                d->dispatch(tr, nullptr, Selection::cursor(0));
            };
        }
        item = Bookmark::file(path, QStringLiteral("#^") + id);
    }
    if (!item)
        return true;
    if (!e->selectedText().trimmed().isEmpty())
        item->title = e->selectedText().trimmed();
    BookmarkModal::add(w, sessionBookmarks(s), std::move(item), nullptr, onSave);
    return true;
}

MainWindow* windowOf(QWidget* origin) { return origin ? MainWindow::of(origin) : nullptr; }

// A linked view (backlinks, outgoing links, outline) of `leaf`'s note in a split of it (Obsidian's
// splitLeafOrActive + setViewState({group: leaf})).
void openLinkedView(WorkspaceLeaf* leaf, const QString& type, Qt::Orientation orientation)
{
    Workspace* ws = leaf ? leaf->workspace() : nullptr;
    auto* fv = leaf ? qobject_cast<FileView*>(leaf->view()) : nullptr;
    if (!ws || !fv || !fv->file())
        return;
    const QString path = fv->file()->path();
    WorkspaceLeaf* target = ws->splitLeaf(leaf, orientation);
    if (!target)
        return;
    target->setViewState(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("state"), QJsonObject{{QStringLiteral("file"), path}}}}, {}, false);
    if (auto* v = qobject_cast<LinkedFileView*>(target->view()))
        v->linkTo(leaf);
    ws->setActiveLeaf(target, true);
}

// ATX heading text of a line (Obsidian's XE()), empty when the line isn't one.
QString atxHeadingText(const QString& line)
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}#{1,6}[ \\t]+(.*?)(?:[ \\t]+#+)?[ \\t]*$"));
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

} // namespace

void installSidebarMenus()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    // Obsidian's file-menu handlers of the bookmarks, backlink, outgoing-link and outline plugins.
    FileMenuHooks::add([](QList<MenuEntry>& items, TAbstractFile* item, const QString& source, WorkspaceLeaf* leaf, QWidget* origin) {
        MainWindow* w = windowOf(origin);
        if (!w && leaf && leaf->workspace())
            w = leaf->workspace()->window();
        if (!w || !item)
            return;
        QPointer<MainWindow> pw = w;
        const QString path = item->path();
        const bool folder = item->isFolder();
        if (!(folder && static_cast<TFolder*>(item)->isRoot())) {
            BookmarkModel* model = sessionBookmarks(w->session());
            const bool existing = model && model->findByPath(path);
            items << MenuEntry{QStringLiteral("action"), existing ? tr("Edit bookmark") : tr("Bookmark..."), QStringLiteral("bookmark"), [pw, path, folder] {
                                   BookmarkModel* m = pw ? sessionBookmarks(pw->session()) : nullptr;
                                   if (!m)
                                       return;
                                   if (Bookmark* b = m->findByPath(path))
                                       BookmarkModal::edit(pw, m, b);
                                   else
                                       BookmarkModal::add(pw, m, folder ? Bookmark::folder(path) : Bookmark::file(path));
                               }};
        }
        if (folder || !leaf || source == u"sidebar-context-menu")
            return;
        QPointer<WorkspaceLeaf> pl = leaf;
        const bool markdown = static_cast<TFile*>(item)->isMarkdown();
        items << MenuEntry{QStringLiteral("view.linked"), tr("Open backlinks"), QStringLiteral("links-coming-in"),
                           [pl] { openLinkedView(pl, QStringLiteral("backlink"), Qt::Vertical); }};
        if (!markdown)
            return;
        items << MenuEntry{QStringLiteral("view.linked"), tr("Open outgoing links"), QStringLiteral("links-going-out"),
                           [pl] { openLinkedView(pl, QStringLiteral("outgoing-link"), Qt::Vertical); }};
        items << MenuEntry{QStringLiteral("view.linked"), tr("Open outline"), QStringLiteral("list"),
                           [pl] { openLinkedView(pl, QStringLiteral("outline"), Qt::Horizontal); }};
    });
    // Obsidian's bookmarks editor-menu handler: "Bookmark this heading..." on a heading line.
    EditorMenuHooks::add([](QList<MenuEntry>& items, MarkdownView* view, qsizetype) {
        MainWindow* w = windowOf(view);
        Document* doc = view ? view->document() : nullptr;
        if (!w || !doc || !view->file())
            return;
        const Text& t = doc->text();
        const QString heading = atxHeadingText(t.line(t.lineAt(view->editor()->cursorPosition())));
        if (heading.isEmpty())
            return;
        QPointer<MainWindow> pw = w;
        const QString path = view->file()->path();
        const QString subpath = QLatin1Char('#') + headingSubpathText(heading);
        items << MenuEntry{QStringLiteral("action"), tr("Bookmark this heading..."), QStringLiteral("bookmark"), [pw, path, subpath] {
                               if (pw)
                                   BookmarkModal::add(pw, sessionBookmarks(pw->session()), Bookmark::file(path, subpath));
                           }};
    });
}

void registerSidebarCommands(CommandRegistry& r, MainWindow* window)
{
    installSidebarMenus();
    QPointer<MainWindow> w = window;

    // ---- backlink ----
    Command c = makeCommand("backlink:open", tr("Backlinks: Show backlinks"), "link");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("backlink"), Workspace::Side::Right, checking); };
    r.add(c);
    c = makeCommand("backlink:open-backlinks", tr("Backlinks: Open backlinks for the current note"), "link");
    c.checkCallback = [w](bool checking) { return openForCurrent(w, QStringLiteral("backlink"), checking); };
    r.add(c);
    c = makeCommand("backlink:toggle-backlinks-in-document", tr("Backlinks: Toggle backlinks in document"), "link");
    c.checkCallback = [w](bool checking) {
        // Obsidian: the active markdown view's own toggle (not the option for new views)
        WorkspaceLeaf* leaf = w && w->workspace() ? w->workspace()->activeLeaf() : nullptr;
        auto* mv = leaf ? qobject_cast<MarkdownView*>(leaf->view()) : nullptr;
        if (!mv || !mv->canToggleBacklinks())
            return false;
        if (!checking)
            mv->setShowBacklinks(!mv->showBacklinks());
        return true;
    };
    r.add(c);

    // ---- outgoing-link ----
    c = makeCommand("outgoing-links:open", tr("Outgoing links: Show outgoing links"), "link");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("outgoing-link"), Workspace::Side::Right, checking); };
    r.add(c);
    c = makeCommand("outgoing-links:open-for-current", tr("Outgoing links: Open outgoing links for the current file"), "link");
    c.checkCallback = [w](bool checking) { return openForCurrent(w, QStringLiteral("outgoing-link"), checking); };
    r.add(c);

    // ---- tag-pane ----
    c = makeCommand("tag-pane:open", tr("Tags view: Show tags"), "tag");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("tag"), Workspace::Side::Right, checking); };
    r.add(c);

    // ---- outline ----
    c = makeCommand("outline:open", tr("Outline: Show outline"), "list");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("outline"), Workspace::Side::Right, checking); };
    r.add(c);
    c = makeCommand("outline:open-for-current", tr("Outline: Open outline of the current file"), "list");
    c.checkCallback = [w](bool checking) { return openForCurrent(w, QStringLiteral("outline"), checking); };
    r.add(c);

    // ---- properties ----
    c = makeCommand("properties:open", tr("Properties view: Show all properties"), "archive");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("all-properties"), Workspace::Side::Right, checking); };
    r.add(c);

    // ---- bookmarks ----
    c = makeCommand("bookmarks:open", tr("Bookmarks: Show bookmarks"), "bookmark");
    c.checkCallback = [w](bool checking) { return showSideView(w, QStringLiteral("bookmarks"), Workspace::Side::Left, checking); };
    r.add(c);
    c = makeCommand("bookmarks:bookmark-current-view", tr("Bookmarks: Bookmark..."), "bookmark-plus");
    c.checkCallback = [w](bool checking) {
        Workspace* ws = w ? w->workspace() : nullptr;
        WorkspaceLeaf* leaf = ws ? ws->activeRootLeaf() : nullptr;
        BookmarkModel* model = w ? sessionBookmarks(w->session()) : nullptr;
        if (!leaf || !model)
            return false;
        auto* fv = qobject_cast<FileView*>(leaf->view());
        Bookmark* existing = fv && fv->file() ? model->findByPath(fv->file()->path()) : nullptr;
        std::unique_ptr<Bookmark> fresh = existing ? nullptr : bookmarkForLeaf(leaf);
        if (!existing && !fresh)
            return false;
        if (!checking) {
            if (existing)
                BookmarkModal::edit(w, model, existing);
            else
                BookmarkModal::add(w, model, std::move(fresh));
        }
        return true;
    };
    r.add(c);
    c = makeCommand("bookmarks:bookmark-current-search", tr("Bookmarks: Bookmark current search..."), "search");
    c.checkCallback = [w](bool checking) {
        Workspace* ws = w ? w->workspace() : nullptr;
        if (!ws || !ws->viewRegistry() || !ws->viewRegistry()->hasView(QStringLiteral("search")))
            return false; // the global search plugin
        if (checking)
            return true;
        const QString query = currentGlobalSearchQuery(w);
        if (query.isEmpty()) {
            if (w && w->notices())
                w->notices()->show(tr("No search query to bookmark."));
            return true;
        }
        BookmarkModal::add(w, sessionBookmarks(w->session()), Bookmark::search(query));
        return true;
    };
    r.add(c);
    c = makeCommand("bookmarks:unbookmark-current-view", tr("Bookmarks: Remove bookmark for the current file"), "bookmark-plus");
    c.checkCallback = [w](bool checking) {
        Workspace* ws = w ? w->workspace() : nullptr;
        WorkspaceLeaf* leaf = ws ? ws->activeRootLeaf() : nullptr;
        BookmarkModel* model = w ? sessionBookmarks(w->session()) : nullptr;
        auto* fv = leaf ? qobject_cast<FileView*>(leaf->view()) : nullptr;
        Bookmark* existing = fv && fv->file() && model ? model->findByPath(fv->file()->path()) : nullptr;
        if (!existing)
            return false;
        if (!checking)
            model->remove(existing);
        return true;
    };
    r.add(c);
    c = makeCommand("bookmarks:bookmark-current-section", tr("Bookmarks: Bookmark block under cursor..."), "bookmark-plus");
    c.editorCheckCallback = [w](bool checking, EditorContext& ec) { return bookmarkCurrentSection(w, ec, checking); };
    r.add(c);
    c = makeCommand("bookmarks:bookmark-current-heading", tr("Bookmarks: Bookmark heading under cursor..."), "bookmark-plus");
    c.editorCheckCallback = [w](bool checking, EditorContext& ec) {
        MarkdownView* mv = editorView(ec);
        VaultSession* s = w ? w->session() : nullptr;
        if (!mv || !mv->file() || !mv->document() || !s || !s->metadata())
            return false;
        if (checking)
            return true;
        const md::CachedMetadata* meta = s->metadata()->fileCache(mv->file()->path());
        if (!meta)
            return true;
        const int line = mv->document()->text().lineAt(mv->editor()->selection().main().to());
        const int h = headingIndexAtLine(meta->headings, line);
        if (h >= 0)
            BookmarkModal::add(w, sessionBookmarks(s), Bookmark::file(mv->file()->path(), QLatin1Char('#') + headingSubpathText(meta->headings[h].heading)));
        return true;
    };
    r.add(c);
    c = makeCommand("bookmarks:bookmark-all-tabs", tr("Bookmarks: Bookmark all tabs..."), "bookmark-plus");
    c.callback = [w] {
        Workspace* ws = w ? w->workspace() : nullptr;
        if (!ws)
            return;
        std::vector<std::unique_ptr<Bookmark>> items;
        ws->iterateRootLeaves([&](WorkspaceLeaf* leaf) {
            if (std::unique_ptr<Bookmark> b = bookmarkForLeaf(leaf))
                items.push_back(std::move(b));
        });
        if (!items.empty())
            BookmarkModal::addMany(w, sessionBookmarks(w->session()), std::move(items));
    };
    r.add(c);
}

} // namespace oc
