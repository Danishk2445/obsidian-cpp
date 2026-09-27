#include "ui/shell/app_commands.h"

#include "core/commands/command_registry.h"
#include "core/config/vault_config.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/shell/popout_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/inline_title.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSysInfo>
#include <QUrl>

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

// Obsidian's "Debug info" modal: a read-only text area with the system info, Copy / Done.
QString debugInfoText(MainWindow* w)
{
    VaultSession* s = w->session();
    VaultConfig* cfg = s ? s->vaultConfig() : nullptr;
    QString t = QStringLiteral("SYSTEM INFO:\n");
    t += QStringLiteral("\tObsidian C++ version: %1\n").arg(QCoreApplication::applicationVersion());
    t += QStringLiteral("\tQt version: %1\n").arg(QString::fromLatin1(qVersion()));
    t += QStringLiteral("\tOperating system: %1 %2\n").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    t += QStringLiteral("\tPlatform: %1\n").arg(QGuiApplication::platformName());
    t += QStringLiteral("\tLanguage: %1\n").arg(QLocale().name().section(u'_', 0, 0));
    if (cfg) {
        t += QStringLiteral("\tLive preview: %1\n").arg(cfg->flag(QStringLiteral("livePreview")) ? QStringLiteral("on") : QStringLiteral("off"));
        const QString theme = cfg->string(QStringLiteral("theme"));
        t += QStringLiteral("\tBase theme: %1\n").arg(theme == u"system" ? QStringLiteral("adapt to system") : theme == u"obsidian" ? QStringLiteral("dark") : QStringLiteral("light"));
        const QJsonArray snippets = cfg->get(QStringLiteral("enabledCssSnippets")).toArray();
        t += QStringLiteral("\tCommunity theme: none\n\tSnippets enabled: %1\n").arg(snippets.size());
    }
    t += QStringLiteral("\tRestricted mode: on\n");
    t += QStringLiteral("\nRECOMMENDATIONS:\n\tnone\n");
    return t;
}

void openDebugInfoModal(MainWindow* w)
{
    auto* m = new Modal(w->overlay());
    m->setTitle(QObject::tr("Debug info"));
    m->setDialogWidth(800); // .modal.mod-lg
    auto* text = new QPlainTextEdit;
    text->setObjectName(QStringLiteral("ocDebugInfo"));
    text->setProperty("ocInput", true);
    text->setReadOnly(true);
    text->setPlainText(debugInfoText(w));
    text->setMinimumHeight(260);
    m->addContentWidget(text);
    const int copy = m->addButton(QObject::tr("Copy to clipboard"), Modal::ButtonStyle::Normal, false);
    m->addButton(QObject::tr("Done"));
    QPointer<MainWindow> pw = w;
    QObject::connect(m, &Modal::buttonClicked, m, [pw, text, copy](int index) {
        if (index != copy)
            return;
        QGuiApplication::clipboard()->setText(text->toPlainText());
        if (pw && pw->notices())
            pw->notices()->show(QObject::tr("%1 copied to your clipboard").arg(QObject::tr("Debug info")));
    });
    m->open();
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
    // Obsidian closes the other unpinned leaves of the active leaf's container (the root split, or the
    // active leaf's pop-out window).
    r.add(cmd("workspace:close-others", QObject::tr("Close all other tabs"), "x", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || a->isInSidedock())
            return false;
        QList<WorkspaceLeaf*> others;
        for (WorkspaceLeaf* l : ws()->containerLeaves(a))
            if (l != a && !l->isPinned())
                others.push_back(l);
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
    // Obsidian: the active leaf's tab group, unless it is in a sidebar (a pop-out's groups stack too).
    r.add(cmd("workspace:toggle-stacked-tabs", QObject::tr("Toggle stacked tabs"), "layers", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || !a->tabs() || a->isInSidedock())
            return false;
        if (!checking)
            ws()->setStacked(a, !ws()->isStacked(a));
        return true;
    }));
    // Obsidian's activeWindow.close(): the focused window, a pop-out (its tabs close) or the main window.
    r.add(cmd("workspace:close-window", QObject::tr("Close window"), "x", [w](bool checking) {
        if (!w)
            return false;
        if (!checking) {
            auto* popout = qobject_cast<PopoutWindow*>(QApplication::activeWindow());
            if (popout && popout->mainWindow() == w)
                popout->close();
            else
                w->close();
        }
        return true;
    }, keys({"Mod+Shift+W"})));
    // Pop-out windows (Obsidian's canPopoutWindow commands).
    r.add(cmd("workspace:new-window", QObject::tr("New window"), "", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking)
            if (WorkspaceLeaf* leaf = ws()->openPopoutLeaf())
                ws()->setActiveLeaf(leaf, true);
        return true;
    }));
    r.add(cmd("workspace:open-in-new-window", QObject::tr("Open current tab in new window"), "maximize", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || !a->view())
            return false;
        if (!checking) {
            // Obsidian: openPopoutLeaf().setViewState(leaf.getViewState(), {focus: true}).
            QJsonObject state = a->viewState();
            state.remove(QStringLiteral("pinned"));
            if (WorkspaceLeaf* leaf = ws()->openPopoutLeaf()) {
                leaf->setViewState(state, QJsonObject{{QStringLiteral("focus"), true}});
                ws()->setActiveLeaf(leaf, true);
            }
        }
        return true;
    }));
    r.add(cmd("workspace:move-to-new-window", QObject::tr("Move current tab to new window"), "maximize", [ws](bool checking) {
        WorkspaceLeaf* a = ws() ? ws()->activeLeaf() : nullptr;
        if (!a || !a->view())
            return false;
        if (!checking)
            ws()->moveLeafToPopout(a);
        return true;
    }));
    // Obsidian's window:toggle-always-on-top: the focused pop-out only (never the main window), unless it
    // is maximized. No menu shows it. (Wayland compositors ignore the hint, see PopoutWindow.)
    r.add(cmd("window:toggle-always-on-top", QObject::tr("Toggle window always on top"), "", [w](bool checking) {
        auto* popout = qobject_cast<PopoutWindow*>(QApplication::activeWindow());
        if (!w || !popout || popout->mainWindow() != w || popout->isMaximized())
            return false;
        if (!checking)
            popout->setAlwaysOnTop(!popout->isAlwaysOnTop());
        return true;
    }));
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
    // F1: Obsidian opens its help; the clone's behaviour is Obsidian's, so its documentation applies.
    r.add(cmd("app:open-help", QObject::tr("Open help"), "circle-help", [](bool checking) {
        if (!checking && desktopHandlers().openUrl)
            desktopHandlers().openUrl(QUrl(QStringLiteral("https://help.obsidian.md/")));
        return true;
    }, keys({"F1"})));
    r.add(cmd("app:show-debug-info", QObject::tr("Show debug info"), "", [w](bool checking) {
        if (!w)
            return false;
        if (!checking)
            openDebugInfoModal(w);
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
