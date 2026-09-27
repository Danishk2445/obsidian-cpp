#include "ui/views/file_explorer/file_actions.h"

#include "core/config/vault_config.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_tree.h"
#include "ui/views/markdown/inline_title.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/view.h"

#include <KIO/OpenFileManagerWindowJob>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace oc {

namespace {
QString tr(const char* s) { return QCoreApplication::translate("oc::FileExplorer", s); }

// Characters a file name can't contain (Obsidian on Linux: \ / :), and the ones that break links.
const QString kInvalidChars = QStringLiteral("\\/:");
const QString kUnsafeChars = QStringLiteral("#^[]|");

QString spaced(const QString& chars)
{
    QStringList out;
    for (QChar c : chars)
        out << QString(c);
    return out.join(QLatin1Char(' '));
}

QString joinPath(const QString& folder, const QString& name) { return folder.isEmpty() ? name : folder + QLatin1Char('/') + name; }

bool isInside(const TAbstractFile* item, const TAbstractFile* folder)
{
    for (const TAbstractFile* p = item; p; p = p->parent())
        if (p == folder)
            return true;
    return false;
}

// Obsidian's SA(): `item` may be moved into `target`.
bool canMoveInto(const TAbstractFile* item, const TFolder* target)
{
    if (!item || !target || item == target)
        return false;
    if (item->isFolder() && static_cast<const TFolder*>(item)->isRoot())
        return false;
    return !(item->isFolder() && isInside(target, item));
}

TFolder* copyFolderTree(Vault* vault, TFolder* src, const QString& dst, QString* error)
{
    TFolder* out = vault->createFolder(dst, error);
    if (!out)
        return nullptr;
    const QList<TAbstractFile*> kids = src->children(); // snapshot
    for (TAbstractFile* c : kids) {
        const QString path = joinPath(dst, c->name());
        if (c->isFolder()) {
            if (!copyFolderTree(vault, static_cast<TFolder*>(c), path, error))
                return nullptr;
        } else if (!vault->copy(static_cast<TFile*>(c), path, error)) {
            return nullptr;
        }
    }
    return out;
}
} // namespace

MainWindow* sessionWindow(VaultSession* session)
{
    Workspace* ws = session ? session->workspace() : nullptr;
    return ws ? ws->window() : nullptr;
}

OverlayLayer* sessionOverlay(VaultSession* session)
{
    MainWindow* w = sessionWindow(session);
    return w ? w->overlay() : nullptr;
}

void showNotice(VaultSession* session, const QString& message)
{
    if (MainWindow* w = sessionWindow(session))
        w->notices()->show(message);
    else
        qWarning("oc: %s", qPrintable(message));
}

TFile* workspaceActiveFile(Workspace* ws)
{
    if (!ws)
        return nullptr;
    WorkspaceLeaf* l = ws->activeLeaf();
    auto* fv = l && l->view() && l->view()->navigation() ? qobject_cast<FileView*>(l->view()) : nullptr;
    if (!fv) {
        l = ws->activeRootLeaf();
        fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
    }
    return fv ? fv->file() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// names
// ---------------------------------------------------------------------------------------------

QString pathAfterRename(const TAbstractFile* item, const QString& rawName)
{
    if (!item || !item->parent())
        return {};
    QString name = rawName;
    for (QChar& c : name)
        if (c.unicode() < 0x20)
            c = QLatin1Char(' ');
    name = name.trimmed();
    if (!item->isFolder()) {
        const QString ext = static_cast<const TFile*>(item)->extension();
        if (!ext.isEmpty())
            name += QLatin1Char('.') + ext;
    }
    return joinPath(item->parent()->path(), name);
}

QString validateFileName(Vault* vault, const TAbstractFile* item, const QString& name, bool final)
{
    for (QChar c : name)
        if (kInvalidChars.contains(c))
            return tr("File name cannot contain any of these characters: ") + spaced(kInvalidChars);
    if (final && name.isEmpty())
        return tr("File name cannot be empty.");
    if (name.startsWith(QLatin1Char('.')))
        return tr("File name must not start with a dot.");
    if (vault && item && !name.isEmpty()) {
        const TAbstractFile* existing = vault->item(pathAfterRename(item, name));
        if (existing && existing != item)
            return tr("There's already a file with the same name");
    }
    for (QChar c : name)
        if (kUnsafeChars.contains(c))
            return tr("Links will not work with file names containing any of these characters: ") + spaced(kUnsafeChars);
    return {};
}

// ---------------------------------------------------------------------------------------------
// create / copy / move
// ---------------------------------------------------------------------------------------------

TFolder* createNewFolder(VaultSession* session, TFolder* parent, QString* error)
{
    Vault* vault = session ? session->vault() : nullptr;
    if (!vault)
        return nullptr;
    const QString folder = parent && !parent->isRoot() ? parent->path() : QString();
    return vault->createFolder(vault->availablePath(folder, tr("Untitled"), {}), error);
}

TAbstractFile* makeCopy(VaultSession* session, TAbstractFile* item, QString* error)
{
    if (!item || !item->parent())
        return nullptr;
    return copyInto(session, item, item->parent(), error);
}

TAbstractFile* copyInto(VaultSession* session, TAbstractFile* item, TFolder* target, QString* error)
{
    Vault* vault = session ? session->vault() : nullptr;
    if (!vault || !item || !target)
        return nullptr;
    if (item->isFolder() && isInside(target, item)) {
        if (error)
            *error = tr("A folder cannot be copied into itself.");
        return nullptr;
    }
    const QString folder = target->isRoot() ? QString() : target->path();
    if (item->isFolder())
        return copyFolderTree(vault, static_cast<TFolder*>(item), vault->availablePath(folder, item->name(), {}), error);
    auto* f = static_cast<TFile*>(item);
    return vault->copy(f, vault->availablePath(folder, f->extension().isEmpty() ? f->name() : f->basename(), f->extension()), error);
}

QList<TAbstractFile*> importFiles(VaultSession* session, const QList<QUrl>& urls, TFolder* target)
{
    QList<TAbstractFile*> out;
    Vault* vault = session ? session->vault() : nullptr;
    if (!vault || !target)
        return out;
    const QString targetPath = target->path();
    std::function<TAbstractFile*(const QFileInfo&, const QString&)> import = [&](const QFileInfo& fi, const QString& folder) -> TAbstractFile* {
        QString err;
        if (fi.isDir()) {
            TFolder* dir = vault->createFolder(vault->availablePath(folder, fi.fileName(), {}), &err);
            if (!dir) {
                showNotice(session, err);
                return nullptr;
            }
            const QString dirPath = dir->path();
            for (const QFileInfo& c : QDir(fi.absoluteFilePath()).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))
                if (!Vault::isIgnoredName(c.fileName()))
                    import(c, dirPath);
            return vault->item(dirPath);
        }
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) {
            showNotice(session, tr("Failed to import “%1”: %2").arg(fi.fileName(), f.errorString()));
            return nullptr;
        }
        const QString ext = fi.suffix();
        const QString base = ext.isEmpty() ? fi.fileName() : fi.completeBaseName();
        TFile* created = vault->createFile(vault->availablePath(folder, base, ext), f.readAll(), &err);
        if (!created)
            showNotice(session, err);
        return created;
    };
    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo fi(url.toLocalFile());
        if (!fi.exists())
            continue;
        TFolder* dest = vault->folder(targetPath);
        if (!dest)
            break;
        if (TAbstractFile* item = import(fi, dest->isRoot() ? QString() : dest->path()))
            out << item;
    }
    return out;
}

QList<TAbstractFile*> outermostItems(const QList<TAbstractFile*>& items)
{
    QList<TAbstractFile*> out;
    for (TAbstractFile* i : items) {
        bool inside = false;
        for (TAbstractFile* o : items)
            if (o != i && o->isFolder() && isInside(i, o))
                inside = true;
        if (!inside && !out.contains(i))
            out << i;
    }
    return out;
}

int moveItems(VaultSession* session, const QList<TAbstractFile*>& items, TFolder* target)
{
    Vault* vault = session ? session->vault() : nullptr;
    if (!vault || !target)
        return 0;
    FileManager* fm = session->fileManager();
    // Paths first: renaming a folder moves (and re-paths) what's inside it.
    QStringList paths;
    for (TAbstractFile* i : outermostItems(items))
        if (canMoveInto(i, target))
            paths << i->path();
    const QString targetPath = target->path();
    int moved = 0;
    for (const QString& path : std::as_const(paths)) {
        TAbstractFile* item = vault->item(path);
        TFolder* dest = vault->folder(targetPath);
        if (!item || !dest || item->parent() == dest)
            continue;
        const QString folder = dest->isRoot() ? QString() : dest->path();
        QString newPath;
        if (item->isFolder()) {
            newPath = vault->availablePath(folder, item->name(), {});
        } else {
            const auto* f = static_cast<const TFile*>(item);
            newPath = vault->availablePath(folder, f->extension().isEmpty() ? f->name() : f->basename(), f->extension());
        }
        QString err;
        const bool ok = fm ? fm->renameFile(item, newPath, &err) : vault->rename(item, newPath, &err);
        if (ok)
            ++moved;
        else
            showNotice(session, err.isEmpty() ? tr("Failed to move “%1”.").arg(path) : err);
    }
    return moved;
}

WorkspaceLeaf* openNewFile(Workspace* ws, TFile* file, Workspace::NewLeaf mode)
{
    if (!ws || !file)
        return nullptr;
    WorkspaceLeaf* leaf = ws->getLeaf(mode);
    if (!leaf)
        return nullptr;
    leaf->openFile(file, QJsonObject{{QStringLiteral("mode"), QStringLiteral("source")}});
    ws->setActiveLeaf(leaf, true);
    // The rename request goes last: activating the leaf focuses its editor.
    const QJsonObject rename{{QStringLiteral("rename"), QStringLiteral("all")}};
    QPointer<View> view = leaf->view();
    if (view)
        view->setEphemeralState(rename);
    QTimer::singleShot(0, view, [view, rename] {
        auto* mv = qobject_cast<MarkdownView*>(view.data());
        if (mv && !mv->inlineTitle()->hasFocus())
            mv->setEphemeralState(rename);
    });
    return leaf;
}

// ---------------------------------------------------------------------------------------------
// deletion
// ---------------------------------------------------------------------------------------------

namespace {

// The existing attachments (anything but notes, canvases and bases) that `path` links to.
QStringList linkedAttachments(VaultSession* s, const QString& path)
{
    MetadataCache* mc = s->metadata();
    Vault* v = s->vault();
    QStringList out;
    if (!mc || !v)
        return out;
    static const QStringList notAttachments{QStringLiteral("md"), QStringLiteral("canvas"), QStringLiteral("base")};
    const QHash<QString, int> targets = mc->resolvedLinks().value(path);
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        TFile* f = v->file(it.key());
        if (f && !notAttachments.contains(f->extension().toLower()))
            out << f->path();
    }
    out.sort();
    return out;
}

void trashPaths(VaultSession* s, const QStringList& paths)
{
    for (const QString& p : paths) {
        Vault* v = s ? s->vault() : nullptr;
        TAbstractFile* it = v ? v->item(p) : nullptr;
        if (!it)
            continue;
        QString err;
        FileManager* fm = s->fileManager();
        if (!(fm ? fm->trashFile(it, &err) : v->trash(it, Vault::TrashMode::System, &err)))
            showNotice(s, err.isEmpty() ? tr("Failed to delete “%1”.").arg(p) : err);
    }
}

// Obsidian's promptForDeletion() tail ("deleteUnlinkedAttachments"): after `deleted` is gone, the attachments it
// linked that no other file links to are deleted too ("always"), offered in a checklist ("ask", all checked) or
// kept ("never"). `then` runs once that's settled.
void deleteOrphanAttachments(VaultSession* s, const QString& deleted, QStringList candidates, const std::function<void()>& then)
{
    VaultConfig* cfg = s ? s->vaultConfig() : nullptr;
    const QString mode = cfg ? cfg->string(QStringLiteral("deleteUnlinkedAttachments")) : QStringLiteral("ask");
    if (s && s->metadata() && mode != u"never") {
        const auto& resolved = s->metadata()->resolvedLinks();
        for (auto it = resolved.cbegin(); it != resolved.cend() && !candidates.isEmpty(); ++it)
            if (it.key() != deleted)
                for (auto t = it->cbegin(); t != it->cend(); ++t)
                    candidates.removeAll(t.key());
    }
    OverlayLayer* layer = s ? sessionOverlay(s) : nullptr;
    if (mode == u"never" || candidates.isEmpty() || (mode == u"ask" && !layer) || (mode != u"ask" && mode != u"always")) {
        if (then)
            then();
        return;
    }
    if (mode == u"always") {
        trashPaths(s, candidates);
        if (then)
            then();
        return;
    }
    auto* m = new Modal(layer);
    m->setObjectName(QStringLiteral("ocOrphanAttachments"));
    m->setTitle(tr("Delete file attachments?"));
    m->addParagraph(candidates.size() == 1
                        ? tr("This attachment is no longer used in any notes. Would you like to delete it?")
                        : tr("These %1 attachments are no longer used in any notes. Would you like to delete them?")
                              .arg(candidates.size()));
    auto* list = new QWidget;
    auto* col = new QVBoxLayout(list);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    QList<QPointer<QCheckBox>> boxes;
    for (const QString& p : std::as_const(candidates)) {
        auto* box = new QCheckBox(p, list);
        box->setChecked(true);
        col->addWidget(box);
        boxes << box;
    }
    m->addContentWidget(list);
    m->addButton(tr("Cancel"));
    const int del = m->addButton(tr("Delete"), Modal::ButtonStyle::Warning);
    QPointer<VaultSession> guard = s;
    QObject::connect(m, &Modal::closed, m, [guard, boxes, del, then](int result) {
        if (result == del && guard) {
            QStringList checked;
            for (const auto& b : boxes)
                if (b && b->isChecked())
                    checked << b->text();
            trashPaths(guard, checked);
        }
        if (then)
            then();
    });
    m->open();
}

} // namespace

void promptForDeletion(VaultSession* session, TAbstractFile* item, std::function<void(bool)> done)
{
    Vault* vault = session ? session->vault() : nullptr;
    if (!vault || !item || (item->isFolder() && static_cast<TFolder*>(item)->isRoot())) {
        if (done)
            done(false);
        return;
    }
    const QString path = item->path();
    QPointer<VaultSession> s = session;
    auto doDelete = [s, path, done] {
        Vault* v = s ? s->vault() : nullptr;
        TAbstractFile* it = v ? v->item(path) : nullptr;
        bool ok = false;
        QStringList attachments;
        if (it) {
            if (!it->isFolder())
                attachments = linkedAttachments(s, path);
            QString err;
            FileManager* fm = s->fileManager();
            ok = fm ? fm->trashFile(it, &err) : v->trash(it, Vault::TrashMode::System, &err);
            if (!ok)
                showNotice(s, err.isEmpty() ? tr("Failed to delete “%1”.").arg(path) : err);
        }
        if (ok && !attachments.isEmpty()) {
            deleteOrphanAttachments(s, path, attachments, [done] {
                if (done)
                    done(true);
            });
            return;
        }
        if (done)
            done(ok);
    };
    VaultConfig* cfg = session->vaultConfig();
    if (cfg && !cfg->flag(QStringLiteral("promptDelete"))) {
        doDelete();
        return;
    }
    OverlayLayer* layer = sessionOverlay(session);
    if (!layer) { // nowhere to ask: don't delete unasked
        if (done)
            done(false);
        return;
    }
    auto* m = new Modal(layer);
    m->setTitle(item->isFolder() ? tr("Delete folder") : tr("Delete file"));
    m->addParagraph(tr("Are you sure you want to delete “%1”?").arg(item->name()));
    const QString option = cfg ? cfg->string(QStringLiteral("trashOption")) : QStringLiteral("system");
    if (option == u"local")
        m->addParagraph(tr("It will be moved to your Obsidian trash, which is located in the “.trash” hidden folder in your vault."));
    else if (option == u"none")
        m->addParagraph(tr("The file will be permanent deleted."), true);
    else
        m->addParagraph(tr("It will be moved to your system trash."));
    if (item->isFolder() && !static_cast<TFolder*>(item)->children().isEmpty()) {
        m->addParagraph(tr("This folder is not empty."), true);
        m->addParagraph(tr("If you continue, all files inside this folder will be deleted."), true);
    }
    if (!item->isFolder()) {
        if (MetadataCache* mc = session->metadata()) {
            const int n = mc->backlinkCount(path);
            if (n == 1)
                m->addParagraph(tr("There is currently %1 link pointing to this file.").arg(n), true);
            else if (n > 1)
                m->addParagraph(tr("There are currently %1 links pointing to this file.").arg(n), true);
        }
    }
    m->setCheckbox(tr("Don't ask again"));
    m->addButton(tr("Cancel"));
    const int del = m->addButton(tr("Delete"), Modal::ButtonStyle::Warning);
    QObject::connect(m, &Modal::closed, m, [m, s, del, doDelete, done](int result) {
        if (result != del) {
            if (done)
                done(false);
            return;
        }
        if (m->isChecked() && s && s->vaultConfig())
            s->vaultConfig()->set(QStringLiteral("promptDelete"), false);
        doDelete();
    });
    m->open();
}

void promptForDeletion(VaultSession* session, const QList<TAbstractFile*>& items, std::function<void()> done)
{
    QStringList paths;
    for (TAbstractFile* i : outermostItems(items))
        paths << i->path();
    QPointer<VaultSession> s = session;
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [s, paths, done, next](int index) {
        Vault* v = s ? s->vault() : nullptr;
        while (v && index < paths.size() && !v->item(paths[index]))
            ++index; // already gone (e.g. inside a deleted folder)
        if (!v || index >= paths.size()) {
            if (done)
                done();
            *next = nullptr; // break the cycle
            return;
        }
        promptForDeletion(s, v->item(paths[index]), [next, index](bool) {
            if (*next)
                (*next)(index + 1);
        });
    };
    (*next)(0);
}

FileManager::LinkUpdateChoice promptLinkUpdate(OverlayLayer* overlay, int links, int files)
{
    if (!overlay)
        return FileManager::LinkUpdateChoice::DoNotUpdate;
    auto* m = new Modal(overlay);
    m->setTitle(tr("Update links"));
    m->addParagraph(tr("Do you want to update internal links that link to this file?"));
    const QString l = links == 1 ? tr("%1 link").arg(links) : tr("%1 links").arg(links);
    const QString f = files == 1 ? tr("%1 file").arg(files) : tr("%1 files").arg(files);
    m->addParagraph(tr("This will affect %1 in %2.").arg(l, f));
    const int always = m->addButton(tr("Always update"));
    const int once = m->addButton(tr("Just once"));
    m->addButton(tr("Do not update"));
    const int r = m->exec();
    if (r == always)
        return FileManager::LinkUpdateChoice::AlwaysUpdate;
    if (r == once)
        return FileManager::LinkUpdateChoice::JustOnce;
    return FileManager::LinkUpdateChoice::DoNotUpdate;
}

// ---------------------------------------------------------------------------------------------
// move modal
// ---------------------------------------------------------------------------------------------

void openMoveModal(VaultSession* session, const QList<TAbstractFile*>& items)
{
    OverlayLayer* layer = sessionOverlay(session);
    Vault* vault = session ? session->vault() : nullptr;
    if (!layer || !vault || items.isEmpty())
        return;
    QStringList paths;
    for (TAbstractFile* i : outermostItems(items))
        paths << i->path();
    QPointer<VaultSession> s = session;
    // Candidate folders: all of them (the root as "/"), minus the moved folders and their contents.
    auto candidates = [s, paths]() {
        QList<TFolder*> out;
        Vault* v = s ? s->vault() : nullptr;
        if (!v)
            return out;
        QList<TAbstractFile*> moving;
        for (const QString& p : paths)
            if (TAbstractFile* i = v->item(p))
                moving << i;
        QList<TFolder*> all = v->folders();
        if (!all.contains(v->rootFolder()))
            all.prepend(v->rootFolder());
        for (TFolder* f : std::as_const(all)) {
            bool ok = true;
            for (TAbstractFile* m : std::as_const(moving))
                if (m->isFolder() && !canMoveInto(m, f))
                    ok = false;
            if (ok)
                out << f;
        }
        std::sort(out.begin(), out.end(), [](TFolder* a, TFolder* b) {
            if (a->isRoot() != b->isRoot())
                return a->isRoot();
            return naturalCompare(a->path(), b->path()) < 0;
        });
        return out;
    };
    auto* m = new SuggestModal(layer);
    m->setPlaceholder(tr("Type a folder"));
    m->setInstructions({{QStringLiteral("↑↓"), tr("to navigate")},
                        {QStringLiteral("↵"), tr("to move")},
                        {QStringLiteral("shift ↵"), tr("to create")},
                        {QStringLiteral("esc"), tr("to dismiss")}});
    m->setLimit(20);
    m->setEmptyText(tr("No existing folders found."));
    m->setProvider([candidates](const QString& query) {
        QList<Suggestion> out;
        const QList<TFolder*> folders = candidates();
        const FuzzyQuery fq(query);
        struct Hit {
            TFolder* folder;
            FuzzyResult r;
        };
        QList<Hit> hits;
        for (TFolder* f : folders) {
            const QString text = f->isRoot() ? QStringLiteral("/") : f->path();
            if (fq.isEmpty()) {
                hits.append(Hit{f, FuzzyResult{}});
            } else if (auto r = fq.match(text)) {
                hits.append(Hit{f, *r});
            }
        }
        if (!fq.isEmpty())
            std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.r.score > b.r.score; });
        for (const Hit& h : std::as_const(hits)) {
            Suggestion sg;
            sg.title = h.folder->isRoot() ? QStringLiteral("/") : h.folder->path();
            for (const MatchRange& mr : matchIndicesToRanges(h.r.matches))
                sg.titleMatches.append(qMakePair(mr.from, mr.to));
            sg.data = h.folder->path();
            out << sg;
        }
        if (out.isEmpty() && !query.trimmed().isEmpty()) {
            // Obsidian's empty match: the typed name, "Enter to create".
            Suggestion sg;
            sg.title = query;
            sg.auxText = tr("Enter to create");
            out << sg;
        }
        return out;
    });
    auto moveTo = [s, paths](TFolder* target) {
        Vault* v = s ? s->vault() : nullptr;
        if (!v || !target)
            return;
        QList<TAbstractFile*> items;
        for (const QString& p : paths)
            if (TAbstractFile* i = v->item(p))
                items << i;
        moveItems(s, items, target);
    };
    auto createAndMove = [s, moveTo](const QString& raw) {
        Vault* v = s ? s->vault() : nullptr;
        QString path = raw.trimmed();
        while (path.startsWith(QLatin1Char('/')))
            path.remove(0, 1);
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (!v)
            return;
        TFolder* target = path.isEmpty() ? v->rootFolder() : v->folder(path);
        if (!target) {
            QString err;
            target = v->createFolder(path, &err);
            if (!target) {
                showNotice(s, err);
                return;
            }
        }
        moveTo(target);
    };
    QPointer<SuggestModal> guard = m;
    m->setKeyHandler([guard, createAndMove](QKeyEvent* e) {
        if (!guard)
            return false;
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ShiftModifier)) {
            const QString q = guard->query();
            guard->close();
            createAndMove(q);
            return true;
        }
        if (e->key() == Qt::Key_Tab && e->modifiers() == Qt::NoModifier) {
            // Complete the selected folder's path up to the next "/" after the matched part.
            const int i = guard->selectedIndex();
            const QList<Suggestion>& list = guard->suggestions();
            if (i < 0 || i >= list.size() || list[i].data.isNull())
                return true;
            const Suggestion& sg = list[i];
            if (sg.title == u"/")
                return true;
            QString completion = sg.title;
            if (!sg.titleMatches.isEmpty()) {
                const qsizetype slash = sg.title.indexOf(QLatin1Char('/'), sg.titleMatches.last().second);
                if (slash >= 0)
                    completion = sg.title.left(slash + 1);
            }
            if (completion == guard->query())
                completion += QLatin1Char('/');
            guard->setQuery(completion);
            return true;
        }
        return false;
    });
    QObject::connect(m, &SuggestModal::chosen, m, [s, moveTo, createAndMove, guard](const Suggestion& sg, Qt::KeyboardModifiers) {
        Vault* v = s ? s->vault() : nullptr;
        if (!v)
            return;
        if (sg.data.isNull()) {
            createAndMove(sg.title);
            return;
        }
        moveTo(v->folder(sg.data.toString()));
    });
    m->open();
}

// ---------------------------------------------------------------------------------------------
// system
// ---------------------------------------------------------------------------------------------

QString obsidianUrl(Vault* vault, const TFile* file)
{
    if (!vault || !file)
        return {};
    QString path = file->path();
    if (file->isMarkdown())
        path.chop(3);
    const QByteArray keep("!~*'()");
    return QStringLiteral("obsidian://open?vault=") + QString::fromLatin1(QUrl::toPercentEncoding(vault->name(), keep))
        + QStringLiteral("&file=") + QString::fromLatin1(QUrl::toPercentEncoding(path, keep));
}

DesktopHandlers& desktopHandlers()
{
    static DesktopHandlers h{
        [](const QUrl& url) { QDesktopServices::openUrl(url); },
        [](const QString& absPath) { KIO::highlightInFileManager({QUrl::fromLocalFile(absPath)}); },
    };
    return h;
}

void openWithDefaultApp(Vault* vault, const TAbstractFile* item)
{
    if (vault && item && desktopHandlers().openUrl)
        desktopHandlers().openUrl(QUrl::fromLocalFile(vault->absolutePath(item->path())));
}

void showInSystemExplorer(Vault* vault, const TAbstractFile* item)
{
    if (vault && item && desktopHandlers().showInFolder)
        desktopHandlers().showInFolder(vault->absolutePath(item->path()));
}

} // namespace oc
