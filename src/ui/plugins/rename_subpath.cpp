#include "ui/plugins/rename_subpath.h"

#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/text_prompt_modal.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/file_menu.h"

#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace oc::rename_subpath {

namespace {
MainWindow* windowOf(const QWidget* w)
{
    return w ? MainWindow::of(w) : nullptr;
}

QString collapse(QString s)
{
    static const QRegularExpression ws(u"\\s+"_s);
    return s.replace(ws, u" "_s).trimmed();
}

// Obsidian's wD(): the link path before the first '#', the subpath from it.
std::pair<QString, QString> splitLink(const QString& link)
{
    const qsizetype hash = link.indexOf(u'#');
    return hash < 0 ? std::pair{link, QString()} : std::pair{link.left(hash), link.mid(hash)};
}

// The reference's link target is `filePath` (a bare "#subpath" means the source note itself).
bool pointsAt(const LinkUpdater::Reference& r, const QString& linkPath, const QString& filePath)
{
    if (linkPath.isEmpty())
        return r.sourcePath == filePath;
    return !r.resolvedPaths.isEmpty() && r.resolvedPaths.first() == filePath;
}

template <typename Match>
QHash<QString, QList<LinkUpdater::Edit>> collect(const QList<LinkUpdater::Reference>& refs, const QString& filePath, Match match)
{
    QHash<QString, QList<LinkUpdater::Edit>> out;
    for (const LinkUpdater::Reference& r : refs) {
        const auto [path, subpath] = splitLink(r.link.link);
        if (subpath.isEmpty() || !pointsAt(r, path, filePath))
            continue;
        const QString newSubpath = match(subpath);
        if (newSubpath.isNull())
            continue;
        out[r.sourcePath].append(LinkUpdater::Edit{r.link.position.start.offset, r.link.position.end.offset, r.link.original,
                                                   LinkUpdater::rewrite(r.link, path + newSubpath)});
    }
    return out;
}

QString countText(int n, const char* one, const char* many)
{
    return n == 1 ? QObject::tr(one).arg(n) : QObject::tr(many).arg(n);
}

// Obsidian's mR.submit(): with no links to update, only the note changes (in its editor); otherwise
// the note's own link edits plus `ownChange` go through one file edit and the other notes follow.
void applyRename(MainWindow* w, MarkdownView* view, const QHash<QString, QList<LinkUpdater::Edit>>& all, const ChangeSpec& ownChange)
{
    VaultSession* s = w ? w->session() : nullptr;
    FileManager* fm = s ? s->fileManager() : nullptr;
    const QString path = view->file()->path();
    int links = 0;
    for (const auto& list : all)
        links += int(list.size());
    auto others = all;
    const QList<LinkUpdater::Edit> own = others.take(path);
    if (links == 0 || own.isEmpty()) {
        EditorView* e = view->editor();
        Document* doc = e ? e->document() : nullptr;
        if (doc) {
            Transaction tr;
            tr.changes = ChangeSet::of({ownChange}, doc->text().length());
            tr.userEvent = u"input"_s;
            e->dispatch(tr);
        }
    } else if (fm) {
        fm->editFile(path, [own, ownChange](const Text& t) {
            QList<ChangeSpec> specs{ownChange};
            for (const LinkUpdater::Edit& e : own)
                if (e.from >= 0 && e.to <= t.length() && t.slice(e.from, e.to) == e.expected && (e.to <= ownChange.from || e.from >= ownChange.to))
                    specs.push_back({e.from, e.to, e.text});
            return specs;
        });
    }
    if (links == 0 || !fm)
        return;
    if (!others.isEmpty())
        fm->updateInternalLinks(others);
    if (w->notices())
        w->notices()->show(QObject::tr("Updated %1 in %2.")
                               .arg(countText(links, "%1 link", "%1 links"), countText(int(all.size()), "%1 file", "%1 files")));
}

// Saved notes and a clean metadata cache, then every reference (Obsidian reads its cache the same way)
// plus the note's links to itself ("[[#Heading]]"), which a rename snapshot leaves out.
QList<LinkUpdater::Reference> references(VaultSession* s, const QString& notePath)
{
    if (DocumentManager* dm = s ? s->documents() : nullptr)
        dm->saveAll();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    if (!mc)
        return {};
    mc->waitUntilClean();
    QList<LinkUpdater::Reference> refs = LinkUpdater::snapshot(*mc);
    if (const md::CachedMetadata* meta = mc->fileCache(notePath))
        for (const QList<md::LinkCache>* list : {&meta->frontmatterLinks, &meta->links, &meta->embeds})
            for (const md::LinkCache& l : *list)
                if (l.link.startsWith(u'#'))
                    refs.append(LinkUpdater::Reference{notePath, l, {notePath}});
    return refs;
}
} // namespace

QString normalizeHeading(const QString& heading)
{
    static const QRegularExpression special(u"[!\"#$%&()*+,.:;<=>?@^`{|}~/\\[\\]\\\\\\r\\n]"_s);
    return collapse(QString(heading).replace(special, u" "_s));
}

QString sanitizeSubpath(const QString& heading)
{
    static const QRegularExpression bad(u"([:#|^\\\\\\r\\n]|%%|\\[\\[|]])"_s);
    return collapse(QString(heading).replace(bad, u" "_s));
}

QString headingTextOf(const QString& line)
{
    static const QRegularExpression re(u"^#{1,6} (.*)"_s);
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString validateBlockId(const QString& id)
{
    static const QRegularExpression ok(u"^[a-zA-Z0-9\\-]+$"_s);
    if (id.isEmpty())
        return QObject::tr("Block ID cannot be empty");
    if (!ok.match(id).hasMatch())
        return QObject::tr("Block ID can only contain alphanumeric characters or dash");
    return {};
}

QHash<QString, QList<LinkUpdater::Edit>> headingLinkEdits(const QList<LinkUpdater::Reference>& refs, const QString& filePath,
                                                          const QString& oldHeading, const QString& newHeading)
{
    const QString old = normalizeHeading(oldHeading).toLower();
    const QString replacement = u"#"_s + sanitizeSubpath(newHeading);
    return collect(refs, filePath, [&](const QString& subpath) {
        return normalizeHeading(subpath.mid(1)).toLower() == old ? replacement : QString();
    });
}

QHash<QString, QList<LinkUpdater::Edit>> blockLinkEdits(const QList<LinkUpdater::Reference>& refs, const QString& filePath,
                                                        const QString& oldId, const QString& newId)
{
    const QString old = (u"#^"_s + oldId).toLower();
    const QString replacement = u"#^"_s + newId;
    return collect(refs, filePath, [&](const QString& subpath) { return subpath.toLower() == old ? replacement : QString(); });
}

bool blockIdAt(const MarkdownView* view, qsizetype pos, qsizetype* from, qsizetype* to)
{
    Document* doc = view ? view->document() : nullptr;
    if (!doc)
        return false;
    const Text& t = doc->text();
    const int line = t.lineAt(pos);
    static const QRegularExpression re(u"(?:^|\\s)(\\^[a-zA-Z0-9\\-]+)\\s*$"_s);
    const QRegularExpressionMatch m = re.match(t.line(line));
    if (!m.hasMatch())
        return false;
    const qsizetype a = t.lineStart(line) + m.capturedStart(1), b = t.lineStart(line) + m.capturedEnd(1);
    if (pos < a || pos > b)
        return false;
    *from = a;
    *to = b;
    return true;
}

TextPromptModal* promptHeadingRename(MainWindow* window, MarkdownView* view, int line)
{
    Document* doc = view ? view->document() : nullptr;
    if (!window || !doc || !view->file() || line < 0 || line >= doc->text().lineCount())
        return nullptr;
    const QString oldHeading = headingTextOf(doc->text().line(line));
    if (oldHeading.isNull())
        return nullptr;
    auto* m = new TextPromptModal(window->overlay(), QObject::tr("Rename heading"), oldHeading);
    m->validate = [](const QString& text) { return text.isEmpty() ? QObject::tr("Heading cannot be empty") : QString(); };
    QPointer<MainWindow> w = window;
    QPointer<MarkdownView> v = view;
    m->onSubmit = [w, v, line, oldHeading](const QString& text) {
        if (!w || !v || !v->file() || !v->document())
            return;
        const QList<LinkUpdater::Reference> refs = references(w->session(), v->file()->path());
        Document* doc = v->document();
        const Text& t = doc->text();
        if (line >= t.lineCount())
            return;
        // replaceHeadingText(): keep the "#… " marker, replace the rest of the line.
        static const QRegularExpression marker(u"^(#{1,6} ).*"_s);
        const QString lineText = t.line(line);
        const QRegularExpressionMatch mm = marker.match(lineText);
        if (!mm.hasMatch())
            return;
        const ChangeSpec own{t.lineStart(line) + mm.capturedLength(1), t.lineEnd(line), text};
        applyRename(w, v, headingLinkEdits(refs, v->file()->path(), oldHeading, text), own);
    };
    m->open();
    return m;
}

TextPromptModal* promptBlockIdRename(MainWindow* window, MarkdownView* view, qsizetype from, qsizetype to)
{
    Document* doc = view ? view->document() : nullptr;
    if (!window || !doc || !view->file() || from < 0 || to > doc->text().length() || to - from < 2)
        return nullptr;
    const QString oldId = doc->text().slice(from + 1, to);
    auto* m = new TextPromptModal(window->overlay(), QObject::tr("Rename block ID"), oldId);
    m->validate = &validateBlockId;
    QPointer<MainWindow> w = window;
    QPointer<MarkdownView> v = view;
    m->onSubmit = [w, v, from, to, oldId](const QString& id) {
        if (!w || !v || !v->file() || !v->document())
            return;
        const QList<LinkUpdater::Reference> refs = references(w->session(), v->file()->path());
        const Text& t = v->document()->text();
        if (to > t.length() || t.slice(from, to) != u"^"_s + oldId)
            return; // the note changed meanwhile
        applyRename(w, v, blockLinkEdits(refs, v->file()->path(), oldId, id), ChangeSpec{from, to, u"^"_s + id});
    };
    m->open();
    return m;
}

void registerCommands(CommandRegistry& registry, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    Command c;
    c.id = u"editor:rename-heading"_s;
    c.name = QObject::tr("Rename this heading...");
    c.icon = u"edit-3"_s;
    c.editorCheckCallback = [w](bool checking, EditorContext& ec) {
        auto* e = qobject_cast<EditorView*>(ec.editor);
        auto* v = qobject_cast<MarkdownView*>(ec.view);
        Document* doc = e ? e->document() : nullptr;
        if (!w || !v || !doc || !v->file())
            return false;
        const int line = doc->text().lineAt(e->cursorPosition());
        if (headingTextOf(doc->text().line(line)).isNull())
            return false;
        if (!checking)
            promptHeadingRename(w, v, line);
        return true;
    };
    registry.add(c);
    installEditorMenus();
}

void installEditorMenus()
{
    static const bool once = [] {
        EditorMenuHooks::add([](QList<MenuEntry>& items, MarkdownView* view, qsizetype) {
            MainWindow* w = windowOf(view);
            Document* doc = view ? view->document() : nullptr;
            if (!w || !doc || !view->file())
                return;
            QPointer<MainWindow> pw = w;
            QPointer<MarkdownView> pv = view;
            const qsizetype cursor = view->editor()->cursorPosition();
            const int line = doc->text().lineAt(cursor);
            if (!headingTextOf(doc->text().line(line)).isNull())
                items << MenuEntry{u"action"_s, QObject::tr("Rename this heading..."), u"edit-3"_s, [pw, pv, line] {
                                       if (pw && pv)
                                           promptHeadingRename(pw, pv, line);
                                   }};
            qsizetype from = 0, to = 0;
            if (blockIdAt(view, cursor, &from, &to))
                items << MenuEntry{u"selection"_s, QObject::tr("Rename this block ID..."), u"edit-3"_s, [pw, pv, from, to] {
                                       if (pw && pv)
                                           promptBlockIdRename(pw, pv, from, to);
                                   }};
        });
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace oc::rename_subpath
