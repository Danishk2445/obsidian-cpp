#include "ui/views/canvas/canvas_registration.h"

#include "core/canvas/canvas_model.h"
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/canvas/canvas_modals.h"
#include "ui/views/canvas/canvas_view.h"
#include "ui/views/canvas/canvas_widget.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QHash>
#include <QPointer>
#include <QTimer>

namespace oc {

using namespace Qt::StringLiterals;
using namespace oc::canvas;

namespace {

QString tr(const char* s) { return QCoreApplication::translate("oc::CanvasView", s); }

CanvasView* activeCanvas(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    return ws ? ws->activeViewOfType<CanvasView>() : nullptr;
}

QString parentFolderOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

// Obsidian's canvas plugin rename handling: file cards and group backgrounds that show a renamed file
// follow it, in every canvas of the vault ("Canvas detected file renames affecting N canvas files").
class CanvasRenameUpdater : public QObject {
public:
    explicit CanvasRenameUpdater(MainWindow* window)
        : QObject(window)
        , m_window(window)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(500);
        connect(&m_timer, &QTimer::timeout, this, &CanvasRenameUpdater::process);
        if (VaultSession* s = window->session(); s && s->vault())
            connect(s->vault(), &Vault::renamed, this, [this](TAbstractFile* item, const QString& oldPath, bool) {
                if (item && !item->isFolder()) {
                    m_renames.append({oldPath, item->path()});
                    m_timer.start();
                }
            });
    }

private:
    void process()
    {
        const QList<std::pair<QString, QString>> renames = std::exchange(m_renames, {});
        VaultSession* s = m_window ? m_window->session() : nullptr;
        Vault* vault = s ? s->vault() : nullptr;
        if (!vault || renames.isEmpty())
            return;
        QHash<QString, QString> map;
        for (const auto& [from, to] : renames)
            map.insert(from, to);
        // Open canvases are updated through their view (undoable, saved like an edit).
        QHash<QString, CanvasView*> open;
        if (Workspace* ws = m_window->workspace())
            for (WorkspaceLeaf* l : ws->leavesOfType(u"canvas"_s))
                if (auto* cv = qobject_cast<CanvasView*>(l->view()); cv && cv->file())
                    open.insert(cv->file()->path(), cv);
        int files = 0, cards = 0;
        for (TFile* f : vault->files()) {
            if (f->extension().compare(u"canvas", Qt::CaseInsensitive) != 0)
                continue;
            if (CanvasView* cv = open.value(f->path())) {
                CanvasDocument& doc = cv->canvas()->document();
                int changed = 0;
                for (const Node& n : doc.data().nodes) {
                    if (n.type == NodeType::File && map.contains(n.file))
                        changed += doc.setFile(n.id, map.value(n.file), n.subpath) ? 1 : 0;
                    else if (n.type == NodeType::Group && map.contains(n.background))
                        changed += doc.setGroupBackground(n.id, map.value(n.background), n.backgroundStyle) ? 1 : 0;
                }
                if (changed) {
                    ++files;
                    cards += changed;
                    cv->canvas()->commit();
                    cv->save();
                }
                continue;
            }
            DiskState state;
            const std::optional<QByteArray> bytes = vault->read(f->path(), &state, nullptr);
            if (!bytes)
                continue;
            bool mentions = false;
            for (auto it = map.cbegin(); it != map.cend() && !mentions; ++it)
                mentions = bytes->contains(jsonQuote(it.key()));
            if (!mentions)
                continue;
            std::optional<CanvasData> data = parseCanvas(*bytes);
            if (!data)
                continue;
            int changed = 0;
            for (Node& n : data->nodes) {
                if (n.type == NodeType::File && map.contains(n.file)) {
                    n.file = map.value(n.file);
                    ++changed;
                } else if (n.type == NodeType::Group && map.contains(n.background)) {
                    n.background = map.value(n.background);
                    ++changed;
                }
            }
            if (!changed)
                continue;
            DiskState newState;
            QString err;
            if (vault->write(f->path(), serializeCanvas(*data, FileFormat::detect(*bytes)), state, &newState, &err) == DocumentStorage::WriteResult::Ok) {
                ++files;
                cards += changed;
            }
        }
        if (files > 0 && m_window)
            m_window->notices()->show(cards == 1 ? tr("Canvas updated %1 embed card").arg(cards) : tr("Canvas updated %1 embed cards").arg(cards));
    }

    QPointer<MainWindow> m_window;
    QList<std::pair<QString, QString>> m_renames;
    QTimer m_timer;
};

Command canvasCommand(const char* id, const QString& name, const char* icon)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    return c;
}

} // namespace

void registerCanvasView(ViewRegistry& registry)
{
    registry.registerView(u"canvas"_s, [](WorkspaceLeaf* leaf) -> View* { return new CanvasView(leaf); });
    registry.registerExtensions({u"canvas"_s}, u"canvas"_s);
}

TFile* createNewCanvas(VaultSession* session, Workspace::NewLeaf mode, const QString& folder, bool useFolder)
{
    Vault* vault = session ? session->vault() : nullptr;
    Workspace* ws = session ? session->workspace() : nullptr;
    if (!vault || !ws || !vault->isOnline())
        return nullptr;
    QString parent;
    if (useFolder) {
        parent = folder;
    } else {
        // Obsidian's getNewFileParent(): the canvas plugin's own "Default location for new canvas files".
        const CanvasWidget::Options o = CanvasWidget::Options::fromJson(session->config() ? session->config()->object(u"canvas"_s) : QJsonObject());
        if (o.newFileLocation == u"folder" && vault->folder(o.newFileFolderPath)) {
            parent = o.newFileFolderPath;
        } else if (o.newFileLocation == u"current") {
            WorkspaceLeaf* leaf = ws->activeRootLeaf();
            auto* fv = leaf ? qobject_cast<FileView*>(leaf->view()) : nullptr;
            if (fv && fv->file())
                parent = parentFolderOf(fv->file()->path());
        }
    }
    const QString path = vault->availablePath(parent, tr("Untitled"), u"canvas"_s);
    QString err;
    TFile* file = vault->createFile(path, QByteArray(), &err); // Obsidian creates new canvases empty
    if (!file) {
        if (MainWindow* w = ws->window())
            w->notices()->show(err);
        return nullptr;
    }
    ws->openFile(file, mode, QJsonObject{{u"rename"_s, u"all"_s}});
    return file;
}

void registerCanvasCommands(CommandRegistry& registry, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    {
        Command c = canvasCommand("canvas:new-file", tr("Create new canvas"), "layout-dashboard");
        c.callback = [w] {
            if (w)
                createNewCanvas(w->session(), Workspace::NewLeaf::No);
        };
        registry.add(c);
    }
    {
        Command c = canvasCommand("canvas:export-as-image", tr("Export as image"), "image");
        c.checkCallback = [w](bool checking) {
            CanvasView* cv = activeCanvas(w);
            if (!cv)
                return false;
            if (!checking) {
                QPointer<CanvasView> view = cv;
                auto* dlg = new QFileDialog(cv, tr("Export as image"), (cv->file() ? cv->file()->basename() : u"canvas"_s) + u".png"_s,
                                            tr("PNG Files (*.png)"));
                dlg->setAcceptMode(QFileDialog::AcceptSave);
                dlg->setOption(QFileDialog::DontUseNativeDialog);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                QObject::connect(dlg, &QFileDialog::fileSelected, cv, [view](const QString& path) {
                    if (view)
                        view->renderImage(0).save(path, "PNG");
                });
                dlg->open();
            }
            return true;
        };
        registry.add(c);
    }
    {
        Command c = canvasCommand("canvas:jump-to-group", tr("Jump to group"), "move");
        c.checkCallback = [w](bool checking) {
            CanvasView* cv = activeCanvas(w);
            if (!cv)
                return false;
            if (!checking) {
                QPointer<CanvasView> view = cv;
                canvasui::openGroupPicker(w->overlay(), cv->canvas()->data(), [view](const QString& id) {
                    if (!view)
                        return;
                    if (const Node* n = view->canvas()->data().node(id))
                        view->canvas()->zoomToRect(n->rect());
                });
            }
            return true;
        };
        registry.add(c);
    }
    {
        Command c = canvasCommand("canvas:convert-to-file", tr("Convert to file..."), "file-input");
        c.checkCallback = [w](bool checking) {
            CanvasView* cv = activeCanvas(w);
            if (!cv || cv->canvas()->isReadOnly() || cv->canvas()->selection().size() != 1)
                return false;
            const QString id = *cv->canvas()->selection().begin();
            const Node* n = cv->canvas()->data().node(id);
            if (!n || n->type != NodeType::Text)
                return false;
            if (!checking)
                cv->canvas()->convertToFile(id);
            return true;
        };
        registry.add(c);
    }
    if (window)
        new CanvasRenameUpdater(window);
    // The file explorer's folder menu: "New canvas" (Obsidian's file-menu handler of the plugin).
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        FileMenuHooks::add([](QList<MenuEntry>& items, TAbstractFile* file, const QString& source, WorkspaceLeaf*, QWidget* origin) {
            if (source != u"file-explorer-context-menu" || !file || !file->isFolder() || !origin)
                return;
            auto* mw = qobject_cast<MainWindow*>(origin->window());
            QPointer<MainWindow> win = mw;
            const QString folder = file->path();
            items << MenuEntry{u"action-primary"_s, tr("New canvas"), u"layout-dashboard"_s, [win, folder] {
                                   if (win)
                                       createNewCanvas(win->session(), Workspace::NewLeaf::No, folder, true);
                               }};
        });
    }
}

} // namespace oc
