#include "ui/navigation/quick_switcher.h"

#include "core/config/config_store.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/workspace/view_registry.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <memory>

namespace oc {

namespace {

const char* const kContext = "oc::QuickSwitcher";
QString tr(const char* s) { return QCoreApplication::translate(kContext, s); }

// Obsidian's image extensions (shown with "Show attachments").
bool isImageExtension(const QString& ext)
{
    static const QSet<QString> images{QStringLiteral("bmp"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                      QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
    return images.contains(ext);
}

QString extensionOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    // "x/.hidden" and "x/name." have no extension (Obsidian's rule: a dot neither first nor last)
    if (dot <= slash + 1 || dot == path.size() - 1)
        return {};
    return path.mid(dot + 1).toLower();
}

// Case-insensitive fuzzy or (large vaults) simple-search matcher with a common result shape.
class Matcher {
public:
    Matcher(const QString& query, bool simple)
        : m_simple(simple)
    {
        if (simple)
            m_words = SimpleSearch(query);
        else
            m_fuzzy = FuzzyQuery(query);
    }
    std::optional<std::pair<double, QList<MatchRange>>> match(const QString& text, bool isPath) const
    {
        if (m_simple) {
            if (auto r = m_words.match(text))
                return std::make_pair(r->score, r->matches);
            return std::nullopt;
        }
        const std::optional<FuzzyResult> r = isPath ? m_fuzzy.matchPath(text) : m_fuzzy.match(text);
        if (!r)
            return std::nullopt;
        return std::make_pair(r->score, matchIndicesToRanges(r->matches));
    }

private:
    bool m_simple;
    FuzzyQuery m_fuzzy;
    SimpleSearch m_words;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// options / source
// ---------------------------------------------------------------------------------------------

QuickSwitcherOptions QuickSwitcherOptions::fromJson(const QJsonObject& json)
{
    QuickSwitcherOptions o;
    o.showExistingOnly = json.value(QStringLiteral("showExistingOnly")).toBool(o.showExistingOnly);
    o.showAttachments = json.value(QStringLiteral("showAttachments")).toBool(o.showAttachments);
    o.showAllFileTypes = json.value(QStringLiteral("showAllFileTypes")).toBool(o.showAllFileTypes);
    return o;
}

QJsonObject QuickSwitcherOptions::toJson() const
{
    // Obsidian saves the whole options object once changed; the defaults are {false, true, false}.
    return QJsonObject{{QStringLiteral("showExistingOnly"), showExistingOnly},
                       {QStringLiteral("showAttachments"), showAttachments},
                       {QStringLiteral("showAllFileTypes"), showAllFileTypes}};
}

SwitcherSource SwitcherSource::fromSession(VaultSession* session)
{
    SwitcherSource s;
    if (!session)
        return s;
    if (Vault* v = session->vault()) {
        const QList<TFile*> files = v->files();
        s.files.reserve(files.size());
        for (const TFile* f : files)
            s.files << f->path();
    }
    if (Workspace* ws = session->workspace())
        s.recent = ws->lastOpenFiles();
    if (MetadataCache* mc = session->metadata()) {
        s.aliases = mc->aliases();
        QSet<QString> seen;
        const auto& unresolved = mc->unresolvedLinks();
        for (auto it = unresolved.cbegin(); it != unresolved.cend(); ++it)
            for (auto t = it.value().cbegin(); t != it.value().cend(); ++t)
                if (!seen.contains(t.key())) {
                    seen.insert(t.key());
                    s.unresolved << t.key();
                }
        s.unresolved.sort();
    }
    if (ViewRegistry* reg = session->viewRegistry())
        s.canOpen = [reg](const QString& ext) { return reg->isExtensionRegistered(ext); };
    s.ignore = UserIgnoreFilters(session->vaultConfig());
    return s;
}

// ---------------------------------------------------------------------------------------------
// suggestions
// ---------------------------------------------------------------------------------------------

QList<SwitcherItem> quickSwitcherItems(const QString& rawQuery, const SwitcherSource& source, const QuickSwitcherOptions& options)
{
    const QString query = rawQuery.trimmed();
    auto shown = [&](const QString& path) {
        const QString ext = extensionOf(path);
        if (ext == u"md" || ext == u"canvas" || ext == u"base")
            return true;
        if (isImageExtension(ext))
            return options.showAttachments;
        if (options.showAllFileTypes)
            return true;
        return options.showAttachments && source.canOpen && source.canOpen(ext);
    };

    QList<SwitcherItem> out;
    if (query.isEmpty()) {
        const QSet<QString> existing(source.files.cbegin(), source.files.cend());
        for (const QString& path : source.recent) {
            if (!existing.contains(path) || source.ignore.matches(path) || !shown(path))
                continue;
            SwitcherItem it;
            it.type = SwitcherItem::Type::File;
            it.path = path;
            it.text = displayPath(path);
            out.append(it);
        }
        return out;
    }

    QStringList files;
    files.reserve(source.files.size());
    for (const QString& p : source.files)
        if (shown(p))
            files << p;
    files.sort();
    const Matcher matcher(query, files.size() >= 10000);
    for (const QString& path : std::as_const(files)) {
        const bool ignored = source.ignore.matches(path);
        const QString text = displayPath(path);
        if (auto m = matcher.match(text, true)) {
            SwitcherItem it;
            it.type = SwitcherItem::Type::File;
            it.path = path;
            it.text = text;
            it.score = m->first - (ignored ? 10 : 0);
            it.matches = m->second;
            it.downranked = ignored;
            out.append(it);
        }
        const auto al = source.aliases.constFind(path);
        if (al == source.aliases.cend())
            continue;
        for (const QString& alias : *al) {
            if (auto m = matcher.match(alias, false)) {
                SwitcherItem it;
                it.type = SwitcherItem::Type::Alias;
                it.path = path;
                it.text = alias;
                it.score = m->first - (ignored ? 10 : 0);
                it.matches = m->second;
                it.downranked = ignored;
                out.append(it);
            }
        }
    }
    if (!options.showExistingOnly) {
        for (const QString& link : source.unresolved) {
            if (auto m = matcher.match(link, false)) {
                SwitcherItem it;
                it.type = SwitcherItem::Type::Unresolved;
                it.text = link;
                it.score = m->first;
                it.matches = m->second;
                out.append(it);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SwitcherItem& a, const SwitcherItem& b) { return a.score > b.score; });
    if (out.isEmpty()) {
        SwitcherItem create;
        create.type = SwitcherItem::Type::Create;
        create.text = rawQuery;
        out.append(create);
    }
    return out;
}

QString quickSwitcherCompletion(const SwitcherItem& item, const QString& query)
{
    QString text;
    switch (item.type) {
    case SwitcherItem::Type::File:
    case SwitcherItem::Type::Alias:
        text = displayPath(item.path); // Obsidian completes an alias to its file's path, too
        break;
    case SwitcherItem::Type::Unresolved:
        text = item.text;
        break;
    case SwitcherItem::Type::Create:
        return query;
    }
    if (!item.matches.isEmpty()) {
        const qsizetype slash = text.indexOf(QLatin1Char('/'), item.matches.last().to);
        if (slash >= 0)
            text = text.left(slash + 1);
    }
    if (text == query)
        text += QLatin1Char('/');
    return text;
}

// ---------------------------------------------------------------------------------------------
// opening / creating
// ---------------------------------------------------------------------------------------------

TFile* openOrCreateLinkText(VaultSession* session, const QString& linktext, const QString& sourcePath, Workspace::NewLeaf mode,
                            QString* error)
{
    Vault* vault = session ? session->vault() : nullptr;
    Workspace* ws = session ? session->workspace() : nullptr;
    if (!vault || !ws)
        return nullptr;
    QString path = linktext.trimmed();
    QString subpath;
    if (const qsizetype hash = path.indexOf(QLatin1Char('#')); hash >= 0) {
        subpath = path.mid(hash);
        path = path.left(hash).trimmed();
    }
    if (path.isEmpty())
        return nullptr;

    TFile* file = nullptr;
    if (MetadataCache* mc = session->metadata()) {
        const QString dest = mc->firstLinkpathDest(path, sourcePath);
        if (!dest.isEmpty())
            file = vault->file(dest);
    } else {
        file = vault->file(path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) ? path : path + QStringLiteral(".md"));
    }
    if (!file) {
        QString folderPath;
        QString name = path;
        if (const qsizetype slash = path.lastIndexOf(QLatin1Char('/')); slash >= 0) {
            folderPath = path.left(slash);
            name = path.mid(slash + 1);
        }
        while (folderPath.startsWith(QLatin1Char('/')))
            folderPath.remove(0, 1);
        while (folderPath.endsWith(QLatin1Char('/')))
            folderPath.chop(1);
        if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            name.chop(3);
        static const QString forbidden = QStringLiteral("*\"\\/<>:|?");
        const bool badName = name.trimmed().isEmpty() || std::any_of(name.cbegin(), name.cend(), [](QChar c) { return forbidden.contains(c); })
            || std::any_of(folderPath.cbegin(), folderPath.cend(), [](QChar c) { return c != u'/' && forbidden.contains(c); });
        if (badName) {
            if (error)
                *error = tr("File name cannot contain any of the following characters: * \" \\ / < > : | ?");
            return nullptr;
        }
        FileManager* fm = session->fileManager();
        TFolder* parent = nullptr;
        if (!folderPath.isEmpty()) {
            parent = vault->folder(folderPath);
            if (!parent)
                parent = vault->createFolder(folderPath, error);
            if (!parent)
                return nullptr;
        } else {
            parent = fm ? fm->newFileParent(sourcePath) : vault->rootFolder();
        }
        if (fm) {
            file = fm->createNewMarkdownFile(parent, name, {}, error);
        } else {
            const QString folder = parent ? parent->path() : QString();
            file = vault->createFile(vault->availablePath(folder, name, QStringLiteral("md")), {}, error);
        }
        if (!file)
            return nullptr;
    }
    QJsonObject eState;
    if (!subpath.isEmpty())
        eState.insert(QStringLiteral("subpath"), subpath);
    ws->openFile(file, mode, eState);
    return file;
}

// ---------------------------------------------------------------------------------------------
// the modal
// ---------------------------------------------------------------------------------------------

namespace {

Suggestion toSuggestion(const SwitcherItem& it, int index)
{
    Suggestion s;
    s.title = it.text;
    for (const MatchRange& m : it.matches)
        s.titleMatches.append({m.from, m.to});
    s.downranked = it.downranked;
    s.data = index;
    switch (it.type) {
    case SwitcherItem::Type::File:
        break;
    case SwitcherItem::Type::Alias:
        s.note = displayPath(it.path);
        s.auxIcon = QStringLiteral("forward");
        break;
    case SwitcherItem::Type::Unresolved:
        s.auxIcon = QStringLiteral("file-plus");
        break;
    case SwitcherItem::Type::Create:
        s.auxText = tr("Enter to create");
        break;
    }
    return s;
}

struct SwitcherState {
    QList<SwitcherItem> items;
    QString query;
    QPointer<QWidget> previousFocus;
};

} // namespace

SuggestModal* openQuickSwitcher(VaultSession* session, OverlayLayer* layer, const SwitcherSource* sourceOverride)
{
    if (!layer)
        return nullptr;
    if (auto* open = layer->findChild<SuggestModal*>(QStringLiteral("ocQuickSwitcher"), Qt::FindDirectChildrenOnly); open && open->isOpen())
        return open;
    auto source = std::make_shared<const SwitcherSource>(sourceOverride ? *sourceOverride : SwitcherSource::fromSession(session));
    const QuickSwitcherOptions options =
        QuickSwitcherOptions::fromJson(session && session->config() ? session->config()->object(QStringLiteral("switcher")) : QJsonObject());
    auto state = std::make_shared<SwitcherState>();
    state->previousFocus = QApplication::focusWidget();
    QPointer<VaultSession> sess = session;
    QPointer<OverlayLayer> lay = layer;

    auto* modal = new SuggestModal(layer);
    modal->setObjectName(QStringLiteral("ocQuickSwitcher"));
    modal->setPlaceholder(tr("Find or create a note..."));
    modal->setInstructions({{QStringLiteral("↑↓"), tr("to navigate")},
                            {QStringLiteral("↵"), tr("to open")},
                            {QStringLiteral("ctrl ↵"), tr("to open in new tab")},
                            {QStringLiteral("ctrl alt ↵"), tr("to open to the right")},
                            {QStringLiteral("shift ↵"), tr("to create")},
                            {QStringLiteral("esc"), tr("to dismiss")}});
    modal->setLimit(20); // Obsidian's file suggest limit
    QPointer<SuggestModal> m = modal;
    modal->setProvider([state, source, options, m](const QString& q) {
        state->query = q;
        state->items = quickSwitcherItems(q, *source, options);
        if (m)
            m->setEmptyText(q.trimmed().isEmpty() ? tr("No recent files found. Type to search...") : tr("No notes found. Enter to create a new one."));
        QList<Suggestion> out;
        out.reserve(state->items.size());
        for (int i = 0; i < state->items.size(); ++i)
            out.append(toSuggestion(state->items[i], i));
        return out;
    });

    auto createFromQuery = [sess, lay](const QString& query, Qt::KeyboardModifiers mods) {
        if (!sess || query.trimmed().isEmpty())
            return;
        QString err;
        if (!openOrCreateLinkText(sess, query, activeFilePath(sess), newLeafForModifiers(mods), &err) && !err.isEmpty())
            showNotice(lay, err);
    };
    modal->setKeyHandler([state, m, createFromQuery](QKeyEvent* e) {
        if (!m)
            return false;
        const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (mods & Qt::ShiftModifier)) {
            const QString query = m->query();
            m->close();
            createFromQuery(query, mods);
            return true;
        }
        if (e->key() == Qt::Key_Tab && mods == Qt::NoModifier) {
            const int sel = m->selectedIndex();
            const QList<Suggestion>& shown = m->suggestions();
            if (sel >= 0 && sel < shown.size()) {
                const int index = shown[sel].data.toInt();
                if (index >= 0 && index < state->items.size() && state->items[index].type != SwitcherItem::Type::Create)
                    m->setQuery(quickSwitcherCompletion(state->items[index], m->query()));
            }
            return true;
        }
        return false;
    });

    QObject::connect(modal, &SuggestModal::closed, modal, [state] {
        if (state->previousFocus)
            state->previousFocus->setFocus(Qt::OtherFocusReason);
    });
    QObject::connect(modal, &SuggestModal::chosen, modal, [state, sess, createFromQuery](const Suggestion& s, Qt::KeyboardModifiers mods) {
        const int index = s.data.toInt();
        if (!sess || index < 0 || index >= state->items.size())
            return;
        const SwitcherItem& it = state->items[index];
        const Workspace::NewLeaf mode = newLeafForModifiers(mods);
        switch (it.type) {
        case SwitcherItem::Type::File:
        case SwitcherItem::Type::Alias:
            if (Vault* v = sess->vault(); v && sess->workspace())
                if (TFile* f = v->file(it.path))
                    sess->workspace()->openFile(f, mode);
            break;
        case SwitcherItem::Type::Unresolved:
            createFromQuery(it.text, mods);
            break;
        case SwitcherItem::Type::Create:
            createFromQuery(state->query, mods);
            break;
        }
    });
    modal->open();
    return modal;
}

} // namespace oc
