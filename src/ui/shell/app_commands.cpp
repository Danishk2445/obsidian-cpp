#include "ui/shell/app_commands.h"

#include "core/commands/command_registry.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/inline_title.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>

namespace oc {

namespace {
QList<Hotkey> keys(std::initializer_list<const char*> specs)
{
    QList<Hotkey> out;
    for (const char* s : specs)
        out.append(Hotkey::parse(QString::fromLatin1(s)));
    return out;
}

Command cmd(const char* id, const QString& name, const char* icon, std::function<bool(bool)> check, QList<Hotkey> hk = {}, bool repeat = false)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    c.defaultHotkeys = std::move(hk);
    c.repeatable = repeat;
    c.checkCallback = std::move(check);
    return c;
}

TFile* activeFile(Workspace* ws)
{
    WorkspaceLeaf* l = ws ? ws->activeRootLeaf() : nullptr;
    auto* fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
    return fv ? fv->file() : nullptr;
}
} // namespace

void registerAppCommands(CommandRegistry& r, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    auto ws = [w]() -> Workspace* { return w ? w->workspace() : nullptr; };

    r.add(cmd("app:go-back", QObject::tr("Navigate back"), "arrow-left", [ws](bool checking) {
        WorkspaceLeaf* l = ws() ? ws()->activeLeaf() : nullptr;
        if (!l || !l->canGoBack())
            return false;
        if (!checking)
            l->goBack();
        return true;
    }, keys({"Mod+Alt+ArrowLeft"})));
    r.add(cmd("app:go-forward", QObject::tr("Navigate forward"), "arrow-right", [ws](bool checking) {
        WorkspaceLeaf* l = ws() ? ws()->activeLeaf() : nullptr;
        if (!l || !l->canGoForward())
            return false;
        if (!checking)
            l->goForward();
        return true;
    }, keys({"Mod+Alt+ArrowRight"})));
    r.add(cmd("app:toggle-left-sidebar", QObject::tr("Toggle left sidebar"), "sidebar-left", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->toggleSide(Workspace::Side::Left);
        return true;
    }));
    r.add(cmd("app:toggle-right-sidebar", QObject::tr("Toggle right sidebar"), "sidebar-right", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->toggleSide(Workspace::Side::Right);
        return true;
    }));

    r.add(cmd("workspace:new-tab", QObject::tr("New tab"), "plus", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->newTab();
        return true;
    }, keys({"Mod+T"})));
    r.add(cmd("workspace:close", QObject::tr("Close current tab"), "x", [ws](bool checking) {
        if (!ws() || !ws()->activeLeaf())
            return false;
        if (!checking)
            ws()->closeActiveLeaf();
        return true;
    }, keys({"Mod+W"})));
    r.add(cmd("workspace:undo-close-pane", QObject::tr("Undo close tab"), "undo-2", [ws](bool checking) {
        if (!ws() || !ws()->canUndoCloseLeaf())
            return false;
        if (!checking)
            ws()->undoCloseLeaf();
        return true;
    }, keys({"Mod+Shift+T"})));
    r.add(cmd("workspace:next-tab", QObject::tr("Go to next tab"), "arrow-right", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->nextTab();
        return true;
    }, keys({"Ctrl+Tab", "Ctrl+PageDown"}), true));
    r.add(cmd("workspace:previous-tab", QObject::tr("Go to previous tab"), "arrow-left", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->previousTab();
        return true;
    }, keys({"Ctrl+Shift+Tab", "Ctrl+PageUp"}), true));
    for (int i = 1; i <= 8; ++i) {
        const QByteArray id = "workspace:goto-tab-" + QByteArray::number(i);
        const QByteArray key = "Mod+" + QByteArray::number(i);
        r.add(cmd(id.constData(), QObject::tr("Go to tab #%1").arg(i), "", [ws, i](bool checking) {
            if (!ws())
                return false;
            if (!checking)
                ws()->goToTab(i - 1);
            return true;
        }, keys({key.constData()})));
    }
    r.add(cmd("workspace:goto-last-tab", QObject::tr("Go to last tab"), "", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            ws()->goToLastTab();
        return true;
    }, keys({"Mod+9"})));
    r.add(cmd("workspace:toggle-pin", QObject::tr("Toggle pin"), "pin", [ws](bool checking) {
        if (!ws() || !ws()->activeLeaf())
            return false;
        if (!checking)
            ws()->togglePinActiveLeaf();
        return true;
    }));
    r.add(cmd("workspace:split-vertical", QObject::tr("Split right"), "separator-vertical", [ws](bool checking) {
        WorkspaceLeaf* l = ws() ? ws()->activeRootLeaf() : nullptr;
        if (!l)
            return false;
        if (!checking)
            ws()->duplicateLeaf(l, Workspace::NewLeaf::SplitRight);
        return true;
    }));
    r.add(cmd("workspace:split-horizontal", QObject::tr("Split down"), "separator-horizontal", [ws](bool checking) {
        WorkspaceLeaf* l = ws() ? ws()->activeRootLeaf() : nullptr;
        if (!l)
            return false;
        if (!checking)
            ws()->duplicateLeaf(l, Workspace::NewLeaf::SplitDown);
        return true;
    }));
    r.add(cmd("workspace:close-window", QObject::tr("Close window"), "x", [w](bool checking) {
        if (!w)
            return false;
        if (!checking)
            w->close();
        return true;
    }, keys({"Mod+Shift+W"})));
    r.add(cmd("workspace:edit-file-title", QObject::tr("Rename file"), "edit-3", [ws](bool checking) {
        WorkspaceLeaf* l = ws() ? ws()->activeRootLeaf() : nullptr;
        auto* mv = l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
        if (!mv || !mv->file() || !mv->inlineTitle()->isVisible())
            return false;
        if (!checking)
            mv->inlineTitle()->beginRename(true);
        return true;
    }, keys({"F2"})));
    r.add(cmd("workspace:copy-path", QObject::tr("Copy current file path from vault folder"), "copy", [ws](bool checking) {
        TFile* f = activeFile(ws());
        if (!f)
            return false;
        if (!checking)
            QGuiApplication::clipboard()->setText(f->path());
        return true;
    }));
    r.add(cmd("workspace:copy-full-path", QObject::tr("Copy current file path from system root"), "hard-drive", [ws, w](bool checking) {
        TFile* f = activeFile(ws());
        Vault* v = w && w->session() ? w->session()->vault() : nullptr;
        if (!f || !v)
            return false;
        if (!checking)
            QGuiApplication::clipboard()->setText(v->absolutePath(f->path()));
        return true;
    }));
}

} // namespace oc
