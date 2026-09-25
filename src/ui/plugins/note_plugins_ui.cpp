#include "ui/plugins/note_plugins_ui.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/plugins/note_plugins.h"
#include "core/time/moment_format.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QCollator>
#include <QFile>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>

namespace oc {

using namespace Qt::StringLiterals;
using namespace plugins;

namespace {
QString normalizePath(QString p)
{
    p.replace(u'\\', u'/');
    while (p.contains(u"//"_s))
        p.replace(u"//"_s, u"/"_s);
    while (p.startsWith(u'/'))
        p.remove(0, 1);
    while (p.endsWith(u'/'))
        p.chop(1);
    return p;
}

void notice(MainWindow* w, const QString& text)
{
    if (w && w->notices())
        w->notices()->show(text);
}

QJsonObject options(MainWindow* w, const QString& name)
{
    VaultSession* s = w ? w->session() : nullptr;
    return s && s->config() ? s->config()->object(name) : QJsonObject();
}

std::optional<QString> readText(VaultSession* s, const QString& path)
{
    if (DocumentManager* dm = s->documents())
        if (Document* d = dm->find(path))
            return d->text().toString();
    QFile f(s->vault()->absolutePath(path));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace(u"\r\n"_s, u"\n"_s);
    return text;
}

TFile* activeFile(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    WorkspaceLeaf* l = ws ? ws->activeRootLeaf() : nullptr;
    auto* fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
    return fv ? fv->file() : nullptr;
}

MarkdownView* activeMarkdown(MainWindow* w)
{
    Workspace* ws = w ? w->workspace() : nullptr;
    WorkspaceLeaf* l = ws ? ws->activeRootLeaf() : nullptr;
    return l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
}

Command command(const char* id, const QString& name, const char* icon)
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    return c;
}

QString parentPrefix(const TFolder* f) { return !f || f->path().isEmpty() || f->path() == u"/" ? QString() : f->path() + u'/'; }

// ---- daily notes -------------------------------------------------------------------------------
TFolder* dailyFolder(VaultSession* s, const DailyNotesOptions& o) // null: configured folder missing
{
    if (o.folder.trimmed().isEmpty())
        return s->vault()->folder(QString());
    return s->vault()->folder(normalizePath(o.folder));
}

TFile* dailyNote(MainWindow* w, const QDateTime& when)
{
    VaultSession* s = w->session();
    const DailyNotesOptions o = DailyNotesOptions::fromJson(options(w, u"daily-notes"_s));
    const QString name = dailyNoteName(o, when);
    TFolder* parent = nullptr;
    if (!o.folder.trimmed().isEmpty()) {
        parent = s->vault()->folder(normalizePath(o.folder));
        if (!parent) {
            notice(w, QObject::tr("Failed to create daily note. Folder “%1” not found.").arg(o.folder));
            return nullptr;
        }
    } else if (FileManager* fm = s->fileManager()) {
        parent = fm->newFileParent(QString());
    }
    const QString path = parentPrefix(parent) + name + u".md"_s;
    if (TFile* existing = s->vault()->file(path))
        return existing;
    QString content;
    if (!o.templatePath.trimmed().isEmpty()) {
        const QString tpath = withMdExtension(normalizePath(o.templatePath));
        const auto text = s->vault()->file(tpath) ? readText(s, tpath) : std::nullopt;
        if (!text) {
            notice(w, QObject::tr("Failed to create daily note. Template file “%1” not found.").arg(tpath));
            return nullptr;
        }
        content = applyTemplate(*text, {{u"title"_s, name}}, {}, {}, when);
    }
    QString err;
    TFile* f = s->vault()->createFile(path, content.toUtf8(), &err);
    if (!f)
        notice(w, err);
    return f;
}

std::optional<QDateTime> currentDailyDate(MainWindow* w)
{
    VaultSession* s = w ? w->session() : nullptr;
    TFile* f = activeFile(w);
    if (!s || !f || !f->isMarkdown())
        return std::nullopt;
    const DailyNotesOptions o = DailyNotesOptions::fromJson(options(w, u"daily-notes"_s));
    TFolder* folder = dailyFolder(s, o);
    if (!folder)
        return std::nullopt;
    return dailyNoteDate(f->path(), folder->path() == u"/" ? QString() : folder->path(), o);
}

void gotoAdjacentDaily(MainWindow* w, int dir)
{
    VaultSession* s = w->session();
    const auto current = currentDailyDate(w);
    if (!current)
        return;
    const DailyNotesOptions o = DailyNotesOptions::fromJson(options(w, u"daily-notes"_s));
    TFolder* folder = dailyFolder(s, o);
    if (!folder) {
        notice(w, QObject::tr("Daily note folder not found."));
        return;
    }
    const QString folderPath = folder->path() == u"/" ? QString() : folder->path();
    TFile* best = nullptr;
    QDateTime bestDate;
    for (TFile* f : s->vault()->markdownFiles()) {
        const auto d = dailyNoteDate(f->path(), folderPath, o);
        if (!d)
            continue;
        if (dir < 0 ? (*d < *current && (!best || *d > bestDate)) : (*d > *current && (!best || *d < bestDate))) {
            best = f;
            bestDate = *d;
        }
    }
    if (!best) {
        notice(w, dir < 0 ? QObject::tr("There's no daily note before this one.") : QObject::tr("There's no daily note after this one."));
        return;
    }
    w->workspace()->openFile(best, Workspace::NewLeaf::No);
}

// ---- templates ---------------------------------------------------------------------------------
void insertTemplateText(MarkdownView* mv, const QString& raw, const QJsonObject& opts)
{
    EditorView* e = mv ? mv->editor() : nullptr;
    Document* doc = e ? e->document() : nullptr;
    if (!doc)
        return;
    const QString title = mv->file() ? mv->file()->basename() : QString();
    const QString content = applyTemplate(raw, {{u"title"_s, title}}, opts.value(u"dateFormat"_s).toString(),
                                          opts.value(u"timeFormat"_s).toString(), QDateTime::currentDateTime());
    const Text& t = doc->text();
    const SelRange sel = e->selection().main();
    if (sel.from() == 0) { // at the very start the whole template (frontmatter included) goes in as is
        e->replaceSelections(content, u"input"_s);
        return;
    }
    const TemplateParts parts = splitFrontmatter(content);
    QList<ChangeSpec> changes;
    qsizetype shift = 0;
    if (parts.hasFrontmatter) {
        const QString noteText = t.toString();
        const TemplateParts note = splitFrontmatter(noteText);
        const qsizetype headerLen = noteText.size() - note.body.size();
        const QString merged = mergeFrontmatter(noteText.left(headerLen), parts.frontmatter);
        if (sel.from() >= headerLen && merged != noteText.left(headerLen)) {
            changes.append({0, headerLen, merged});
            shift = merged.size() - headerLen;
        }
    }
    changes.append({sel.from(), sel.to(), parts.body});
    Transaction tr;
    tr.changes = ChangeSet::of(changes, t.length());
    tr.selection = Selection::cursor(sel.from() + shift + parts.body.size());
    tr.userEvent = u"input"_s;
    e->dispatch(tr);
}

void openTemplatePicker(MainWindow* w)
{
    VaultSession* s = w->session();
    const QJsonObject opts = options(w, u"templates"_s);
    const QString folderOpt = opts.value(u"folder"_s).toString();
    if (folderOpt.trimmed().isEmpty()) {
        notice(w, QObject::tr("Failed to list templates. No template folder configured."));
        return;
    }
    const QString folderPath = normalizePath(folderOpt);
    TFolder* folder = s->vault()->folder(folderPath);
    if (!folder) {
        notice(w, QObject::tr("Failed to list templates. Folder “%1” not found.").arg(folderOpt));
        return;
    }
    if (folderPath.isEmpty()) {
        notice(w, QObject::tr("Failed to list templates. Template folder is invalid."));
        return;
    }
    QStringList paths;
    for (TFile* f : s->vault()->markdownFiles())
        if (f->path().startsWith(folderPath + u'/'))
            paths << f->path();
    QCollator col;
    col.setNumericMode(true);
    col.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(paths.begin(), paths.end(), [&](const QString& a, const QString& b) { return col.compare(a, b) < 0; });
    auto* modal = new SuggestModal(w->overlay());
    modal->setPlaceholder(QObject::tr("Type name of a template..."));
    modal->setEmptyText(QObject::tr("No templates found"));
    modal->setInstructions({{u"↑↓"_s, QObject::tr("to navigate")}, {u"↵"_s, QObject::tr("to insert template")}, {u"esc"_s, QObject::tr("to dismiss")}});
    const qsizetype cut = folderPath.size() + 1;
    modal->setProvider([paths, cut](const QString& query) {
        QList<Suggestion> out;
        const FuzzyQuery q(query);
        QList<QPair<double, Suggestion>> scored;
        for (const QString& p : paths) {
            Suggestion s;
            s.title = p.mid(cut, p.size() - cut - 3); // without folder and ".md"
            s.data = p;
            if (q.isEmpty()) {
                scored.append({0, s});
            } else if (const auto m = q.match(s.title)) {
                for (const MatchRange& r : matchIndicesToRanges(m->matches))
                    s.titleMatches.append({r.from, r.to});
                scored.append({m->score, s});
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [score, sug] : scored)
            out << sug;
        return out;
    });
    QPointer<MainWindow> wp = w;
    QObject::connect(modal, &SuggestModal::chosen, modal, [wp, opts](const Suggestion& sug, Qt::KeyboardModifiers) {
        if (!wp)
            return;
        MarkdownView* mv = activeMarkdown(wp);
        const auto text = readText(wp->session(), sug.data.toString());
        if (mv && text)
            insertTemplateText(mv, *text, opts);
    });
    modal->open();
}

void insertAtHead(EditorView* e, const QString& text)
{
    Document* doc = e ? e->document() : nullptr;
    if (!doc)
        return;
    const qsizetype head = e->selection().main().head;
    Transaction tr;
    tr.changes = ChangeSet::of(ChangeSpec{head, head, text}, doc->text().length());
    tr.selection = Selection::cursor(head + text.size());
    tr.userEvent = u"input"_s;
    e->dispatch(tr);
}

// ---- unique notes ------------------------------------------------------------------------------
std::optional<QString> uniqueName(MainWindow* w, const QString& suffix)
{
    VaultSession* s = w->session();
    const QString format = options(w, u"zk-prefixer"_s).value(u"format"_s).toString();
    const QList<TFile*> files = s->vault()->files();
    auto taken = [&files](const QString& prefix) {
        return std::any_of(files.cbegin(), files.cend(), [&](TFile* f) { return f->name().startsWith(prefix); });
    };
    auto name = uniqueNoteName(format, suffix, QDateTime::currentDateTime(), taken);
    if (!name)
        notice(w, QObject::tr("Failed to generate a unique note with the format “%1”").arg(format.isEmpty() ? u"YYYYMMDDHHmm"_s : format));
    return name;
}

TFile* createUniqueNote(MainWindow* w)
{
    VaultSession* s = w->session();
    const QJsonObject o = options(w, u"zk-prefixer"_s);
    const auto name = uniqueName(w, {});
    if (!name)
        return nullptr;
    TFolder* parent = nullptr;
    const QString folderOpt = o.value(u"folder"_s).toString();
    if (!folderOpt.trimmed().isEmpty()) {
        parent = s->vault()->folder(normalizePath(folderOpt));
        if (!parent) {
            notice(w, QObject::tr("Failed to create unique note. Folder “%1” not found.").arg(folderOpt));
            return nullptr;
        }
    } else if (FileManager* fm = s->fileManager()) {
        parent = fm->newFileParent(QString());
    }
    QString content;
    const QString tmpl = o.value(u"template"_s).toString();
    if (!tmpl.trimmed().isEmpty()) {
        const QString tpath = withMdExtension(normalizePath(tmpl));
        const auto text = s->vault()->file(tpath) ? readText(s, tpath) : std::nullopt;
        if (!text) {
            notice(w, QObject::tr("Failed to create unique note. Template file “%1” not found.").arg(tpath));
            return nullptr;
        }
        content = applyTemplate(*text, {{u"title"_s, *name}}, {}, {}, QDateTime::currentDateTime());
    }
    QString err;
    TFile* f = s->fileManager() ? s->fileManager()->createNewMarkdownFile(parent, *name, content.toUtf8(), &err) : nullptr;
    if (!f)
        notice(w, err);
    return f;
}
} // namespace

bool isCorePluginEnabled(MainWindow* window, const char* id)
{
    const QJsonObject cfg = options(window, u"core-plugins"_s);
    const QString key = QString::fromLatin1(id);
    if (cfg.contains(key))
        return cfg.value(key).toBool();
    // Obsidian 1.13's plugins without defaultOn (see ui/settings/settings_pages.cpp corePluginInfos()).
    static const QStringList offByDefault = {u"zk-prefixer"_s, u"random-note"_s, u"slash-command"_s, u"workspaces"_s,
                                             u"audio-recorder"_s, u"slides"_s, u"markdown-importer"_s,
                                             u"footnotes"_s, u"webviewer"_s, u"publish"_s};
    return !offByDefault.contains(key);
}

void registerNotePluginCommands(CommandRegistry& r, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    if (isCorePluginEnabled(window, "daily-notes")) {
        Command c = command("daily-notes", QObject::tr("Open today's daily note"), "calendar-days");
        c.callback = [w] {
            if (!w)
                return;
            if (TFile* f = dailyNote(w, QDateTime::currentDateTime()))
                w->workspace()->openFile(f, Workspace::NewLeaf::No);
        };
        r.add(c);
        c = command("daily-notes:goto-prev", QObject::tr("Open previous daily note"), "calendar-minus");
        c.checkCallback = [w](bool checking) {
            if (!w || !currentDailyDate(w))
                return false;
            if (!checking)
                gotoAdjacentDaily(w, -1);
            return true;
        };
        r.add(c);
        c = command("daily-notes:goto-next", QObject::tr("Open next daily note"), "calendar-plus");
        c.checkCallback = [w](bool checking) {
            if (!w || !currentDailyDate(w))
                return false;
            if (!checking)
                gotoAdjacentDaily(w, 1);
            return true;
        };
        r.add(c);
    }
    if (isCorePluginEnabled(window, "templates")) {
        Command c = command("insert-template", QObject::tr("Insert template"), "copy");
        c.callback = [w] {
            if (w)
                openTemplatePicker(w);
        };
        r.add(c);
        c = command("insert-current-date", QObject::tr("Insert current date"), "calendar-days");
        c.editorCallback = [w](EditorContext& ec) {
            const QString fmt = options(w, u"templates"_s).value(u"dateFormat"_s).toString();
            insertAtHead(qobject_cast<EditorView*>(ec.editor), formatMoment(QDateTime::currentDateTime(), fmt.isEmpty() ? u"YYYY-MM-DD"_s : fmt));
        };
        r.add(c);
        c = command("insert-current-time", QObject::tr("Insert current time"), "clock");
        c.editorCallback = [w](EditorContext& ec) {
            const QString fmt = options(w, u"templates"_s).value(u"timeFormat"_s).toString();
            insertAtHead(qobject_cast<EditorView*>(ec.editor), formatMoment(QDateTime::currentDateTime(), fmt.isEmpty() ? u"HH:mm"_s : fmt));
        };
        r.add(c);
    }
    if (isCorePluginEnabled(window, "zk-prefixer")) {
        Command c = command("zk-prefixer", QObject::tr("Create new unique note"), "box-glyph");
        c.callback = [w] {
            if (!w)
                return;
            if (TFile* f = createUniqueNote(w))
                w->workspace()->openFile(f, Workspace::NewLeaf::No, QJsonObject{{u"rename"_s, u"end"_s}});
        };
        r.add(c);
        c = command("insert-unique-link", QObject::tr("Add unique internal link"), "link");
        c.editorCallback = [w](EditorContext& ec) {
            auto* e = qobject_cast<EditorView*>(ec.editor);
            Document* doc = e ? e->document() : nullptr;
            if (!doc || !w)
                return;
            QList<ChangeSpec> changes;
            QList<SelRange> ranges;
            qsizetype delta = 0;
            for (const SelRange& sr : e->selection().ranges()) {
                const QString selected = doc->text().slice(sr.from(), sr.to());
                const auto id = uniqueName(w, selected);
                if (!id)
                    return;
                const QString link = u"[["_s + *id + u"]]"_s;
                changes.append({sr.from(), sr.to(), link});
                const qsizetype at = sr.from() + delta;
                ranges.append(sr.empty() ? SelRange(at + 2 + id->size(), at + 2 + id->size()) : SelRange(at + 2, at + 2 + id->size()));
                delta += link.size() - (sr.to() - sr.from());
            }
            Transaction tr;
            tr.changes = ChangeSet::of(changes, doc->text().length());
            tr.selection = Selection::create(ranges);
            tr.userEvent = u"input.autocomplete"_s;
            e->dispatch(tr);
        };
        r.add(c);
    }
    if (isCorePluginEnabled(window, "random-note")) {
        Command c = command("random-note", QObject::tr("Open random note"), "dice-glyph");
        c.callback = [w] {
            if (!w || !w->session() || !w->session()->vault())
                return;
            const QList<TFile*> notes = w->session()->vault()->markdownFiles();
            if (!notes.isEmpty())
                w->workspace()->openFile(notes[QRandomGenerator::global()->bounded(int(notes.size()))], Workspace::NewLeaf::No);
        };
        r.add(c);
    }
}

} // namespace oc
