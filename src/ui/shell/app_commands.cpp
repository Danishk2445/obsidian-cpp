#include "ui/shell/app_commands.h"

#include "core/commands/command_registry.h"
#include "core/config/vault_config.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
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
    // Obsidian closes the other unpinned leaves of the active leaf's container (here: the root split).
    r.add(cmd("workspace:close-others", QObject::tr("Close all other tabs"), "x", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || a->isInSidedock())
            return false;
        QList<WorkspaceLeaf*> others;
        ws()->iterateRootLeaves([&](WorkspaceLeaf* l) {
            if (l != a && !l->isPinned())
                others.push_back(l);
        });
        if (!checking)
            ws()->closeLeaves(others);
        return !others.isEmpty();
    }));
    r.add(cmd("workspace:close-tab-group", QObject::tr("Close this tab group"), "x", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || a->isInSidedock())
            return false;
        if (!checking) {
            QList<WorkspaceLeaf*> group = ws()->groupLeaves(a);
            group.removeIf([](WorkspaceLeaf* l) { return l->isPinned(); });
            ws()->closeLeaves(group);
        }
        return true;
    }));
    r.add(cmd("workspace:close-others-tab-group", QObject::tr("Close others in tab group"), "x", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || a->isInSidedock() || ws()->groupLeaves(a).size() < 2)
            return false;
        if (!checking) {
            QList<WorkspaceLeaf*> group = ws()->groupLeaves(a);
            group.removeIf([a](WorkspaceLeaf* l) { return l == a || l->isPinned(); });
            ws()->closeLeaves(group);
        }
        return true;
    }));
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
    r.add(cmd("workspace:copy-url", QObject::tr("Copy Obsidian URL for current file"), "vault", [ws, w](bool checking) {
        TFile* f = activeFile(ws());
        Vault* v = w && w->session() ? w->session()->vault() : nullptr;
        if (!f || !v)
            return false;
        if (!checking) {
            QGuiApplication::clipboard()->setText(obsidianUrl(v, f));
            if (w->notices())
                w->notices()->show(QObject::tr("%1 copied to your clipboard").arg(QStringLiteral("URL")));
        }
        return true;
    }));
    r.add(cmd("app:delete-file", QObject::tr("Delete current file"), "trash-2", [ws, w](bool checking) {
        TFile* f = activeFile(ws());
        if (!f || !w || !w->session())
            return false;
        if (!checking)
            promptForDeletion(w->session(), f);
        return true;
    }));
    r.add(cmd("workspace:show-trash", QObject::tr("Show trash"), "", [w](bool checking) {
        VaultSession* s = w ? w->session() : nullptr;
        if (!s || !s->vaultConfig() || !s->vault() || s->vaultConfig()->string(QStringLiteral("trashOption")) != u"local")
            return false;
        if (!checking && desktopHandlers().showInFolder)
            desktopHandlers().showInFolder(s->vault()->absolutePath(QStringLiteral(".trash")));
        return true;
    }));
    auto configFlip = [w](const char* id, const QString& name, const char* key) {
        return cmd(id, name, "", [w, k = QString::fromLatin1(key)](bool checking) {
            VaultConfig* cfg = w && w->session() ? w->session()->vaultConfig() : nullptr;
            if (!cfg)
                return false;
            if (!checking)
                cfg->set(k, !cfg->flag(k));
            return true;
        });
    };
    r.add(configFlip("app:toggle-ribbon", QObject::tr("Toggle ribbon"), "showRibbon"));
    r.add(cmd("app:toggle-default-new-pane-mode", QObject::tr("Toggle default mode for new tabs"), "", [w](bool checking) {
        VaultConfig* cfg = w && w->session() ? w->session()->vaultConfig() : nullptr;
        if (!cfg)
            return false;
        if (!checking) {
            const QString mode = cfg->string(QStringLiteral("defaultViewMode"));
            const bool toReading = mode == u"source";
            if (mode == u"source" || mode == u"preview") {
                cfg->set(QStringLiteral("defaultViewMode"), toReading ? QStringLiteral("preview") : QStringLiteral("source"));
                if (w->notices())
                    w->notices()->show(toReading ? QObject::tr("Switched default view mode to reading view.")
                                                 : QObject::tr("Switched default view mode to editing view."));
            }
        }
        return true;
    }));
    // "system": flip this session's scheme only (Obsidian toggles the body class); otherwise switch
    // between the stored "obsidian" (dark) and "moonstone" (light) themes.
    r.add(cmd("theme:toggle-light-dark", QObject::tr("Toggle light/dark mode"), "", [w](bool checking) {
        VaultConfig* cfg = w && w->session() ? w->session()->vaultConfig() : nullptr;
        ThemeManager* tm = ThemeManager::instance();
        if (!cfg || !tm)
            return false;
        if (!checking) {
            const bool dark = tm->effectiveScheme() == Theme::Scheme::Dark;
            if (cfg->string(QStringLiteral("theme")) == u"system") {
                tm->setMode(dark ? ThemeManager::Mode::Light : ThemeManager::Mode::Dark);
                tm->apply();
            } else {
                cfg->set(QStringLiteral("theme"), dark ? QStringLiteral("moonstone") : QStringLiteral("obsidian"));
            }
        }
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
