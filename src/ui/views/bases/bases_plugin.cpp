#include "ui/views/bases/bases_plugin.h"

#include "core/commands/command_registry.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/navigation/navigation_util.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/main_window.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_embed.h"
#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_view.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QPointer>

namespace oc {

using namespace oc::basesui;

namespace {

QString tr(const char* s) { return QCoreApplication::translate("oc::BasesPlugin", s); }

BasesFileView* activeBase(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    WorkspaceLeaf* leaf = ws ? ws->activeLeaf() : nullptr;
    return leaf ? qobject_cast<BasesFileView*>(leaf->view()) : nullptr;
}

Command basesCommand(const char* id, const QString& name, const char* icon)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    return c;
}

// Obsidian's createAndEmbedBase(): a new base next to the note, embedded at the cursor.
void createAndEmbedBase(MarkdownView* mv, EditorView* ev)
{
    VaultSession* s = mv ? mv->session() : nullptr;
    if (!s || !ev || !ev->document() || ev->isReadOnly())
        return;
    FileManager* fm = s->fileManager();
    TFolder* parent = fm ? fm->newFileParent(mv->file() ? mv->file()->path() : QString()) : nullptr;
    TFile* file = createNewBase(s, Workspace::NewLeaf::No, parent ? parent->path() : QString(), true, false);
    if (!file)
        return;
    const Text& text = ev->document()->text();
    const qsizetype from = ev->selection().main().from();
    const qsizetype to = ev->selection().main().to();
    const int line = text.lineAt(from);
    const QString link = QStringLiteral("![[") + file->path() + QStringLiteral("]]");
    const bool midLine = from > text.lineStart(line);
    const QString insert = midLine ? QString(u'\n' + link + u'\n') : QString(link + u'\n');
    Transaction tr;
    tr.changes = ChangeSet::of({ChangeSpec{from, to, insert}}, text.length());
    tr.selection = Selection::cursor(from + (midLine ? 1 : 0) + link.size());
    tr.userEvent = QStringLiteral("input.paste");
    ev->dispatch(tr);
}

} // namespace

TFile* createNewBase(VaultSession* session, Workspace::NewLeaf mode, const QString& folder, bool useFolder, bool open)
{
    Vault* vault = session ? session->vault() : nullptr;
    Workspace* ws = session ? session->workspace() : nullptr;
    if (!vault || !vault->isOnline())
        return nullptr;
    QString parent;
    if (useFolder) {
        parent = folder;
    } else if (FileManager* fm = session->fileManager()) {
        if (TFolder* f = fm->newFileParent(activeFilePath(session)))
            parent = f->path();
    }
    const QString path = vault->availablePath(parent, tr("Untitled"), QStringLiteral("base"));
    QString err;
    TFile* file = vault->createFile(path, QByteArray(), &err); // Obsidian creates new bases empty
    if (!file) {
        if (ws && ws->window())
            ws->window()->notices()->show(err);
        return nullptr;
    }
    if (open && ws)
        ws->openFile(file, mode, QJsonObject{{QStringLiteral("rename"), QStringLiteral("all")}});
    return file;
}

void registerBases(MainWindow* window)
{
    VaultSession* session = window ? window->session() : nullptr;
    if (!session)
        return;
    if (ViewRegistry* registry = session->viewRegistry()) {
        registry->registerView(QStringLiteral("bases"), [](WorkspaceLeaf* leaf) -> View* { return new BasesFileView(leaf); });
        registry->registerExtensions({QStringLiteral("base")}, QStringLiteral("bases"));
    }
    registerBaseEmbeds();
    addEmbedSession(session);
    CommandRegistry* commands = session->commands();
    if (!commands)
        return;
    QPointer<MainWindow> w = window;
    {
        Command c = basesCommand("bases:new-file", tr("Create new base"), "layout-list");
        c.callback = [w] {
            if (w)
                createNewBase(w->session(), Workspace::NewLeaf::No);
        };
        commands->add(c);
    }
    {
        Command c = basesCommand("bases:insert", tr("Insert new base"), "layout-list");
        c.editorCheckCallback = [](bool checking, EditorContext& ec) {
            auto* ev = qobject_cast<EditorView*>(ec.editor);
            auto* mv = qobject_cast<MarkdownView*>(ec.view);
            if (!ev || !mv || ev->isReadOnly())
                return false;
            if (!checking)
                createAndEmbedBase(mv, ev);
            return true;
        };
        commands->add(c);
    }
    {
        Command c = basesCommand("bases:copy-table", tr("Copy table to clipboard"), "copy");
        c.checkCallback = [w](bool checking) {
            BasesFileView* v = activeBase(w);
            if (!v || !v->controller()->layoutWidget() || v->controller()->layoutWidget()->type() != u"table")
                return false;
            if (!checking)
                v->controller()->copyTable();
            return true;
        };
        commands->add(c);
    }
    {
        Command c = basesCommand("bases:change-view", tr("Switch view..."), "shapes");
        c.checkCallback = [w](bool checking) {
            BasesFileView* v = activeBase(w);
            if (!v || v->controller()->base().views.size() <= 1)
                return false;
            if (!checking) {
                QPointer<BasesController> ctrl = v->controller();
                const QStringList names = ctrl->base().viewNames();
                auto* m = new SuggestModal(w->overlay());
                m->setObjectName(QStringLiteral("ocBasesSwitchView"));
                m->setProvider([names, ctrl](const QString& query) {
                    QList<Suggestion> out;
                    const FuzzyQuery q(query);
                    for (const QString& n : names) {
                        const std::optional<FuzzyResult> r = q.match(n);
                        if (!r)
                            continue;
                        Suggestion s;
                        s.title = n;
                        for (int i : r->matches)
                            s.titleMatches.push_back({i, i + 1});
                        if (ctrl) {
                            const bases::BaseView* view = ctrl->base().view(n);
                            if (const ViewTypeInfo* info = viewType(view ? view->type : QString()))
                                s.icon = info->icon;
                        }
                        s.data = n;
                        out << s;
                    }
                    return out;
                });
                QObject::connect(m, &SuggestModal::chosen, m, [ctrl](const Suggestion& s, Qt::KeyboardModifiers) {
                    if (ctrl)
                        ctrl->selectView(s.data.toString());
                });
                m->open();
            }
            return true;
        };
        commands->add(c);
    }
    {
        Command c = basesCommand("bases:add-view", tr("Add view"), "shapes");
        c.checkCallback = [w](bool checking) {
            BasesFileView* v = activeBase(w);
            if (!v)
                return false;
            if (!checking)
                v->controller()->promptAddView();
            return true;
        };
        commands->add(c);
    }
    {
        Command c = basesCommand("bases:add-item", tr("Add item"), "shapes");
        c.checkCallback = [w](bool checking) {
            BasesFileView* v = activeBase(w);
            if (!v)
                return false;
            if (!checking)
                v->controller()->createNewItem();
            return true;
        };
        commands->add(c);
    }
    // "New base" in the file explorer's folder menu and in the editor menu (process-wide hooks).
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        FileMenuHooks::add([](QList<MenuEntry>& items, TAbstractFile* file, const QString& source, WorkspaceLeaf*, QWidget* origin) {
            if (source != u"file-explorer-context-menu" || !file || !file->isFolder() || !origin)
                return;
            QPointer<MainWindow> win = qobject_cast<MainWindow*>(origin->window());
            const QString folder = file->path();
            items << MenuEntry{QStringLiteral("action-primary"), tr("New base"), QStringLiteral("layout-list"), [win, folder] {
                                   if (win)
                                       createNewBase(win->session(), Workspace::NewLeaf::No, folder, true);
                               }};
        });
        EditorMenuHooks::add([](QList<MenuEntry>& items, MarkdownView* view, qsizetype) {
            if (!view || !view->editor() || view->editor()->isReadOnly())
                return;
            QPointer<MarkdownView> mv = view;
            items << MenuEntry{QStringLiteral("selection.insert.advanced"), tr("New base"), QStringLiteral("layout-list"), [mv] {
                                   if (mv)
                                       createAndEmbedBase(mv, mv->editor());
                               }};
        });
    }
}

} // namespace oc
