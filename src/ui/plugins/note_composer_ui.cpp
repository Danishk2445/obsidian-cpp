#include "ui/plugins/note_composer_ui.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/plugins/note_composer.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/navigation/quick_switcher.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/workspace.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>

#include <memory>

namespace oc {

using namespace Qt::StringLiterals;

namespace {

void notice(MainWindow* w, const QString& text)
{
    if (w && w->notices())
        w->notices()->show(text);
}

plugins::NoteComposerOptions options(VaultSession* s)
{
    return plugins::NoteComposerOptions::fromJson(s && s->config() ? s->config()->object(u"note-composer"_s) : QJsonObject());
}

MainWindow* windowOf(QWidget* w)
{
    return w ? MainWindow::of(w) : nullptr;
}

TFile* activeMarkdownFile(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    WorkspaceLeaf* leaf = ws ? ws->activeRootLeaf() : nullptr;
    auto* fv = leaf ? qobject_cast<FileView*>(leaf->view()) : nullptr;
    TFile* f = fv ? fv->file() : nullptr;
    return f && f->extension() == u"md"_s ? f : nullptr;
}

MarkdownView* viewOf(const EditorContext& ctx)
{
    return qobject_cast<MarkdownView*>(ctx.view);
}

// Obsidian's createNewMarkdownFileFromLinktext() (`linktext`: a name with folders is relative to the
// vault root) and createNewMarkdownFile(getNewFileParent(source), name).
TFile* createNote(VaultSession* s, QString name, const QString& sourcePath, bool linktext, QString* error)
{
    FileManager* fm = s ? s->fileManager() : nullptr;
    Vault* v = s ? s->vault() : nullptr;
    if (!fm || !v)
        return nullptr;
    name = name.trimmed();
    if (name.endsWith(u".md"_s))
        name.chop(3);
    static const QRegularExpression invalid(u"[*\"\\\\<>:|?]"_s);
    if (invalid.match(name).hasMatch()) {
        if (error)
            *error = QObject::tr("File name cannot contain any of the following characters: * \" \\ / < > : | ?");
        return nullptr;
    }
    TFolder* parent = linktext && name.contains(u'/') ? v->rootFolder() : fm->newFileParent(sourcePath);
    QString full = (parent && !parent->path().isEmpty() ? parent->path() + u'/' : QString()) + name;
    const qsizetype slash = full.lastIndexOf(u'/');
    if (slash > 0) {
        parent = v->createFolder(full.left(slash), error);
        if (!parent)
            return nullptr;
        full = full.mid(slash + 1);
    }
    return fm->createNewMarkdownFile(parent, full, {}, error);
}

// The notes a composer picker offers (Obsidian's file suggest without attachments).
struct PickerState {
    SwitcherSource source;
    QList<SwitcherItem> items;
    bool showUnresolved = false;
    bool allowCreate = false;
};

QList<Suggestion> pickerSuggestions(PickerState& st, const QString& query)
{
    QuickSwitcherOptions o;
    o.showExistingOnly = !st.showUnresolved;
    o.showAttachments = false;
    QList<SwitcherItem> items = quickSwitcherItems(query, st.source, o);
    items.removeIf([&](const SwitcherItem& it) {
        if (it.type == SwitcherItem::Type::Create)
            return !st.allowCreate;
        return (it.type == SwitcherItem::Type::File || it.type == SwitcherItem::Type::Alias) && !it.path.endsWith(u".md"_s);
    });
    st.items = items;
    QList<Suggestion> out;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const SwitcherItem& it = items[i];
        Suggestion s;
        s.title = it.text;
        for (const MatchRange& m : it.matches)
            s.titleMatches.append({m.from, m.to});
        s.downranked = it.downranked;
        s.data = int(i);
        if (it.type == SwitcherItem::Type::Alias) {
            s.note = it.path.chopped(3);
            s.auxIcon = u"forward"_s;
        } else if (it.type == SwitcherItem::Type::Unresolved) {
            s.auxIcon = u"file-plus"_s;
        } else if (it.type == SwitcherItem::Type::Create) {
            s.auxText = QObject::tr("Enter to create");
        }
        out << s;
    }
    return out;
}

SuggestModal* openPicker(MainWindow* w, const std::shared_ptr<PickerState>& st, const QString& placeholder,
                         const QList<SuggestModal::Instruction>& instructions)
{
    auto* modal = new SuggestModal(w->overlay());
    modal->setPlaceholder(placeholder);
    modal->setInstructions(instructions);
    modal->setEmptyText(QObject::tr("No files found."));
    modal->setLimit(100);
    modal->setProvider([st](const QString& q) { return pickerSuggestions(*st, q); });
    return modal;
}

void runMerge(QPointer<MainWindow> w, const QString& fromPath, const QString& intoPath, bool prepend)
{
    VaultSession* s = w ? w->session() : nullptr;
    Vault* v = s ? s->vault() : nullptr;
    TFile* from = v ? v->file(fromPath) : nullptr;
    TFile* into = v ? v->file(intoPath) : nullptr;
    if (!from || !into)
        return;
    QString err;
    if (!plugins::mergeNotes(s, from, into, prepend, &err)) {
        if (!err.isEmpty())
            notice(w, err);
        return;
    }
    if (w && w->workspace())
        if (TFile* f = v->file(intoPath))
            w->workspace()->openFile(f, Workspace::NewLeaf::No);
}

} // namespace

SuggestModal* openMergeModal(MainWindow* window, TFile* current)
{
    VaultSession* s = window ? window->session() : nullptr;
    if (!s || !current || !window->overlay())
        return nullptr;
    auto st = std::make_shared<PickerState>();
    st->source = SwitcherSource::fromSession(s);
    const QString ctrl = u"ctrl ↵"_s;
    SuggestModal* modal = openPicker(window, st, QObject::tr("Select file to merge into..."),
                                     {{u"↑↓"_s, QObject::tr("to navigate")},
                                      {u"↵"_s, QObject::tr("to merge")},
                                      {ctrl, QObject::tr("to create new")},
                                      {u"shift ↵"_s, QObject::tr("to merge at top")},
                                      {u"esc"_s, QObject::tr("to dismiss")}});
    QPointer<MainWindow> w = window;
    const QString currentPath = current->path();
    QObject::connect(modal, &SuggestModal::chosen, modal, [w, st, currentPath, modal](const Suggestion& sug, Qt::KeyboardModifiers mods) {
        VaultSession* s = w ? w->session() : nullptr;
        Vault* v = s ? s->vault() : nullptr;
        if (!v)
            return;
        const int index = sug.data.toInt();
        const SwitcherItem item = st->items.value(index);
        TFile* dest = nullptr;
        if ((mods & Qt::ControlModifier) || item.type == SwitcherItem::Type::Create || item.type == SwitcherItem::Type::Unresolved) {
            QString err;
            dest = createNote(s, item.type == SwitcherItem::Type::Unresolved ? item.text : modal->query(), currentPath, false, &err);
            if (!dest && !err.isEmpty())
                notice(w, err);
        } else {
            dest = v->file(item.path);
        }
        if (!dest || dest->path() == currentPath)
            return;
        const QString destPath = dest->path();
        const bool prepend = mods & Qt::ShiftModifier;
        if (!options(s).askBeforeMerging) {
            runMerge(w, currentPath, destPath, prepend);
            return;
        }
        const QString fromName = QFileInfo(currentPath).completeBaseName(), toName = QFileInfo(destPath).completeBaseName();
        ConfirmModal::ask(w->overlay(), QObject::tr("Merge file"),
                          QObject::tr("Are you sure you want to merge “%1” into “%2”? “%1” will be deleted.").arg(fromName, toName),
                          {{QObject::tr("Merge"), Modal::ButtonStyle::Warning}, {QObject::tr("Cancel"), Modal::ButtonStyle::Normal}},
                          QObject::tr("Don't ask again"), [w, currentPath, destPath, prepend](int button, bool dontAsk) {
                              if (button != 0 || !w)
                                  return;
                              VaultSession* s = w->session();
                              if (dontAsk && s && s->config()) {
                                  QJsonObject o = s->config()->object(u"note-composer"_s);
                                  o.insert(u"askBeforeMerging"_s, false);
                                  s->config()->setObject(u"note-composer"_s, o);
                              }
                              runMerge(w, currentPath, destPath, prepend);
                          });
    });
    modal->open();
    return modal;
}

SuggestModal* openExtractModal(MainWindow* window, MarkdownView* view, const QString& defaultName)
{
    VaultSession* s = window ? window->session() : nullptr;
    if (!s || !view || !view->file() || !window->overlay() || !view->editor()->document())
        return nullptr;
    QString name = defaultName;
    const SelRange sel = view->editor()->selection().main();
    const qsizetype from = sel.from(), to = sel.to();
    if (name.isEmpty()) {
        const QString selected = view->editor()->document()->text().slice(from, to);
        name = plugins::headingTextOf(selected.section(u'\n', 0, 0)).value_or(QString());
    }
    auto st = std::make_shared<PickerState>();
    st->source = SwitcherSource::fromSession(s);
    st->showUnresolved = true;
    st->allowCreate = true;
    SuggestModal* modal = openPicker(window, st, QObject::tr("Select file to merge into..."),
                                     {{u"↑↓"_s, QObject::tr("to navigate")},
                                      {u"↵"_s, QObject::tr("to move to bottom")},
                                      {u"ctrl ↵"_s, QObject::tr("to create new")},
                                      {u"shift ↵"_s, QObject::tr("to move to top")},
                                      {u"esc"_s, QObject::tr("to dismiss")}});
    QPointer<MainWindow> w = window;
    const QString sourcePath = view->file()->path();
    QObject::connect(modal, &SuggestModal::chosen, modal,
                     [w, st, sourcePath, from, to, modal](const Suggestion& sug, Qt::KeyboardModifiers mods) {
                         VaultSession* s = w ? w->session() : nullptr;
                         Vault* v = s ? s->vault() : nullptr;
                         if (!v)
                             return;
                         const SwitcherItem item = st->items.value(sug.data.toInt());
                         TFile* dest = nullptr;
                         QString err;
                         if ((mods & Qt::ControlModifier) || item.type == SwitcherItem::Type::Create)
                             dest = createNote(s, modal->query(), sourcePath, true, &err);
                         else if (item.type == SwitcherItem::Type::Unresolved)
                             dest = createNote(s, item.text, sourcePath, true, &err);
                         else
                             dest = v->file(item.path);
                         if (!dest) {
                             if (!err.isEmpty())
                                 notice(w, err);
                             return;
                         }
                         TFile* source = v->file(sourcePath);
                         if (!source || dest == source)
                             return;
                         if (!plugins::extractToNote(s, source, from, to, dest, mods & Qt::ShiftModifier, &err) && !err.isEmpty())
                             notice(w, err);
                     });
    modal->open();
    modal->setQuery(name);
    return modal;
}

bool extractHeading(MainWindow* window, MarkdownView* view)
{
    VaultSession* s = window ? window->session() : nullptr;
    MetadataCache* mc = s ? s->metadata() : nullptr;
    Document* doc = view ? view->editor()->document() : nullptr;
    if (!mc || !doc || !view->file())
        return false;
    if (DocumentManager* dm = s->documents())
        dm->saveAll();
    mc->waitUntilClean();
    const md::CachedMetadata* meta = mc->fileCache(view->file()->path());
    const Text& text = doc->text();
    const int line = text.lineAt(view->editor()->cursorPosition());
    const std::optional<plugins::HeadingRange> r = meta ? plugins::headingRangeAt(*meta, text.toString(), line) : std::nullopt;
    if (!r) {
        notice(window, QObject::tr("Failed to find heading"));
        return false;
    }
    view->editor()->setSelection(Selection::single(r->start, r->end));
    return openExtractModal(window, view, r->heading) != nullptr;
}

void registerNoteComposerCommands(CommandRegistry& registry, MainWindow* window)
{
    if (!isCorePluginEnabled(window, "note-composer"))
        return;
    installNoteComposerMenus();
    QPointer<MainWindow> w = window;
    Command merge;
    merge.id = u"note-composer:merge-file"_s;
    merge.name = QObject::tr("Merge current file with another file...");
    merge.icon = u"git-merge"_s;
    merge.checkCallback = [w](bool checking) {
        TFile* f = activeMarkdownFile(w);
        if (!f)
            return false;
        if (!checking)
            openMergeModal(w, f);
        return true;
    };
    registry.add(merge);

    Command split;
    split.id = u"note-composer:split-file"_s;
    split.name = QObject::tr("Extract current selection...");
    split.icon = u"scissors"_s;
    split.editorCheckCallback = [w](bool checking, EditorContext& ctx) {
        MarkdownView* v = viewOf(ctx);
        if (!v || !v->file() || v->editor()->selection().main().empty())
            return false;
        if (!checking)
            openExtractModal(w, v);
        return true;
    };
    registry.add(split);

    Command heading;
    heading.id = u"note-composer:extract-heading"_s;
    heading.name = QObject::tr("Extract this heading...");
    heading.icon = u"scissors"_s;
    heading.editorCheckCallback = [w](bool checking, EditorContext& ctx) {
        MarkdownView* v = viewOf(ctx);
        Document* doc = v ? v->editor()->document() : nullptr;
        if (!doc || !v->file())
            return false;
        const Text& t = doc->text();
        if (!plugins::headingTextOf(t.line(t.lineAt(v->editor()->cursorPosition()))))
            return false;
        if (!checking)
            extractHeading(w, v);
        return true;
    };
    registry.add(heading);
}

void installNoteComposerMenus()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    FileMenuHooks::add([](QList<MenuEntry>& items, TAbstractFile* item, const QString& source, WorkspaceLeaf*, QWidget* origin) {
        MainWindow* w = windowOf(origin);
        if (!w || !item || item->isFolder() || source == u"link-context-menu" || !isCorePluginEnabled(w, "note-composer"))
            return;
        auto* file = static_cast<TFile*>(item);
        if (file->extension() != u"md"_s)
            return;
        QPointer<MainWindow> pw = w;
        const QString path = file->path();
        items << MenuEntry{u"action"_s, QObject::tr("Merge entire file with..."), u"git-merge"_s, [pw, path] {
                               VaultSession* s = pw ? pw->session() : nullptr;
                               if (TFile* f = s && s->vault() ? s->vault()->file(path) : nullptr)
                                   openMergeModal(pw, f);
                           }};
    });
    EditorMenuHooks::add([](QList<MenuEntry>& items, MarkdownView* view, qsizetype) {
        MainWindow* w = windowOf(view);
        Document* doc = view ? view->editor()->document() : nullptr;
        if (!w || !doc || !view->file() || !isCorePluginEnabled(w, "note-composer"))
            return;
        QPointer<MainWindow> pw = w;
        QPointer<MarkdownView> pv = view;
        const SelRange sel = view->editor()->selection().main();
        if (!doc->text().slice(sel.from(), sel.to()).trimmed().isEmpty())
            items << MenuEntry{u"selection"_s, QObject::tr("Extract current selection..."), u"git-branch-plus"_s, [pw, pv] {
                                   if (pw && pv)
                                       openExtractModal(pw, pv);
                               }};
        const Text& t = doc->text();
        if (plugins::headingTextOf(t.line(t.lineAt(view->editor()->cursorPosition()))))
            items << MenuEntry{u"action"_s, QObject::tr("Extract this heading..."), u"git-branch-plus"_s, [pw, pv] {
                                   if (pw && pv)
                                       extractHeading(pw, pv);
                               }};
    });
}

} // namespace oc
