#include "ui/plugins/core_file_menu.h"

#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/text_prompt_modal.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace oc {

namespace {
MainWindow* windowOf(QWidget* origin)
{
    return origin ? qobject_cast<MainWindow*>(origin->window()) : nullptr;
}

void copied(MainWindow* w)
{
    if (w && w->notices())
        w->notices()->show(QObject::tr("Copied to your clipboard"));
}

// Obsidian's splitLeafOrActive(leaf, direction).setViewState({type, state: {file}}): a linked view of
// the note next to (or under) its leaf.
void openLinked(MainWindow* w, const QString& leafId, const QString& type, const QString& path, Qt::Orientation orientation)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    if (!ws)
        return;
    WorkspaceLeaf* source = nullptr;
    ws->iterateAllLeaves([&](WorkspaceLeaf* l) {
        if (l->id() == leafId)
            source = l;
    });
    WorkspaceLeaf* leaf = ws->splitLeaf(source ? source : ws->activeRootLeaf(), orientation);
    if (!leaf)
        return;
    leaf->setViewState(QJsonObject{{u"type"_s, type}, {u"state"_s, QJsonObject{{u"file"_s, path}}}});
    ws->setActiveLeaf(leaf, true);
}
} // namespace

void installCoreFileMenu()
{
    static const bool once = [] {
        FileMenuHooks::add([](QList<MenuEntry>& items, TAbstractFile* item, const QString& source, WorkspaceLeaf* leaf, QWidget* origin) {
            if (!item || source == u"file-explorer-context-menu")
                return; // the explorer builds these itself
            QPointer<MainWindow> w = windowOf(origin);
            VaultSession* s = w ? w->session() : nullptr;
            Vault* vault = s ? s->vault() : nullptr;
            if (!vault)
                return;
            const QString path = item->path();
            const bool isFile = !item->isFolder();
            const bool isRoot = item->isFolder() && !item->parent();
            auto itemOf = [w, path]() -> TAbstractFile* {
                return w && w->session() && w->session()->vault() ? w->session()->vault()->item(path) : nullptr;
            };
            if (isFile) {
                items << MenuEntry{u"info.copy"_s, QObject::tr("as Obsidian URL"), u"link"_s, [w, itemOf] {
                                       if (TAbstractFile* f = itemOf()) {
                                           QGuiApplication::clipboard()->setText(obsidianUrl(w->session()->vault(), static_cast<TFile*>(f)));
                                           copied(w);
                                       }
                                   }};
                items << MenuEntry{u"system"_s, QObject::tr("Open in default app"), u"arrow-up-right"_s, [w, itemOf] {
                                       if (TAbstractFile* f = itemOf())
                                           openWithDefaultApp(w->session()->vault(), f);
                                   }};
            }
            if (!isRoot) {
                items << MenuEntry{u"action"_s, isFile ? QObject::tr("Move file to...") : QObject::tr("Move folder to..."), u"folder-tree"_s,
                                   [w, itemOf] {
                                       if (TAbstractFile* f = itemOf())
                                           openMoveModal(w->session(), {f});
                                   }};
                items << MenuEntry{u"info.copy"_s, QObject::tr("from vault folder"), u"vault"_s, [w, path] {
                                       QGuiApplication::clipboard()->setText(path);
                                       copied(w);
                                   }};
                items << MenuEntry{u"info.copy"_s, QObject::tr("from system root"), u"hard-drive"_s, [w, path] {
                                       if (w && w->session() && w->session()->vault()) {
                                           QGuiApplication::clipboard()->setText(w->session()->vault()->absolutePath(path));
                                           copied(w);
                                       }
                                   }};
                items << MenuEntry{u"system"_s, QObject::tr("Show in system explorer"), u"arrow-up-right"_s, [w, itemOf] {
                                       if (TAbstractFile* f = itemOf())
                                           showInSystemExplorer(w->session()->vault(), f);
                                   }};
            }
            if (isFile && isCorePluginEnabled(w, "file-explorer"))
                items << MenuEntry{u"system"_s, QObject::tr("Reveal file in navigation"), u"folder-open"_s, [w, itemOf] {
                                       Workspace* ws = w ? w->workspace() : nullptr;
                                       TAbstractFile* f = itemOf();
                                       if (!ws || !f)
                                           return;
                                       WorkspaceLeaf* l = ws->leavesOfType(u"file-explorer"_s).value(0);
                                       if (!l)
                                           l = ws->ensureSideLeaf(u"file-explorer"_s, Workspace::Side::Left, false);
                                       if (auto* v = l ? qobject_cast<FileExplorerView*>(l->view()) : nullptr)
                                           v->revealInFolder(f);
                                   }};
            // FileView.onPaneMenu: the view's own menus rename or delete the file it shows.
            if (source == u"more-options" || source == u"tab-header") {
                items << MenuEntry{u"action"_s, QObject::tr("Rename..."), u"edit-3"_s, [w, itemOf] {
                                       if (TAbstractFile* f = itemOf())
                                           promptForFileRename(w, f);
                                   }};
                MenuEntry del{u"danger"_s, QObject::tr("Delete file"), u"trash-2"_s, [w, itemOf] {
                                  if (TAbstractFile* f = itemOf())
                                      promptForDeletion(w->session(), f);
                              }};
                del.warning = true;
                items << del;
            }
            // Linked views of a note shown in a leaf (the core plugins' "file-menu" handlers).
            const bool md = isFile && static_cast<TFile*>(item)->extension() == u"md";
            if (leaf && source != u"sidebar-context-menu" && isFile) {
                const QString leafId = leaf->id();
                struct Linked {
                    const char* plugin;
                    QString title;
                    QString icon;
                    QString type;
                    Qt::Orientation orientation;
                    bool markdownOnly;
                };
                const Linked linked[] = {
                    {"backlink", QObject::tr("Open backlinks"), u"links-coming-in"_s, u"backlink"_s, Qt::Vertical, false},
                    {"outgoing-link", QObject::tr("Open outgoing links"), u"links-going-out"_s, u"outgoing-link"_s, Qt::Vertical, true},
                    {"outline", QObject::tr("Open outline"), u"list"_s, u"outline"_s, Qt::Horizontal, true},
                    {"graph", QObject::tr("Open local graph"), u"git-fork"_s, u"localgraph"_s, Qt::Horizontal, true},
                    {"properties", QObject::tr("Open file properties"), u"info"_s, u"file-properties"_s, Qt::Vertical, true},
                };
                for (const Linked& l : linked) {
                    if ((l.markdownOnly && !md) || !isCorePluginEnabled(w, l.plugin))
                        continue;
                    if (w->session()->viewRegistry() && !w->session()->viewRegistry()->hasView(l.type))
                        continue;
                    items << MenuEntry{u"view.linked"_s, l.title, l.icon, [w, leafId, type = l.type, path, o = l.orientation] {
                                           openLinked(w, leafId, type, path, o);
                                       }};
                }
            }
        });
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace oc
