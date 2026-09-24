#include "ui/views/sidebar/backlink_view.h"

#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/mentions.h"
#include "core/metadata/metadata_cache.h"
#include "core/search/search_document.h"
#include "core/search/search_engine.h"
#include "core/search/search_match.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "text/codec.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/file_explorer/file_tree.h"
#include "ui/workspace/workspace.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>

namespace oc {

namespace {

enum NodeType { kHeader = 1, kResult, kMatch };

Workspace::NewLeaf leafModeFor(Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    // Obsidian's Keymap.isModEvent(): Mod = tab, Mod+Alt = split, Mod+Alt+Shift = window; middle = tab.
    if (button == Qt::MiddleButton)
        return Workspace::NewLeaf::Tab;
    if (mods & Qt::ControlModifier) {
        if (mods & Qt::AltModifier)
            return (mods & Qt::ShiftModifier) ? Workspace::NewLeaf::Window : Workspace::NewLeaf::SplitRight;
        return Workspace::NewLeaf::Tab;
    }
    return Workspace::NewLeaf::No;
}

QString quoted(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QJsonObject matchEState(const QString& content, const QList<MatchRange>& matches)
{
    QJsonArray arr;
    for (const MatchRange& m : matches)
        arr.append(QJsonArray{m.from, m.to});
    return QJsonObject{{QStringLiteral("match"), QJsonObject{{QStringLiteral("content"), content}, {QStringLiteral("matches"), arr}}}};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// BacklinkPane
// ---------------------------------------------------------------------------------------------

BacklinkPane::BacklinkPane(View* owner, QWidget* parent)
    : QWidget(parent)
    , m_owner(owner)
    , m_header(new NavHeader(this))
    , m_tree(new TreeList(this))
    , m_engine(std::make_unique<search::SearchEngine>())
{
    setObjectName(QStringLiteral("ocBacklinkPane"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    m_collapseButton = m_header->addButton(QStringLiteral("list"), tr("Collapse results"), [this] {
        setCollapseAll(!m_collapseAll);
        emit stateChanged();
    });
    m_contextButton = m_header->addButton(QStringLiteral("move-vertical"), tr("Show more context"), [this] {
        setExtraContext(!m_extraContext);
        emit stateChanged();
    });
    m_header->addSortButton(fileSortGroups(), [this] { return m_sortOrder; }, [this](const QString& id) {
        setSortOrder(id);
        emit stateChanged();
    });
    m_header->addSearchFilter(
        [this](const QString& q) {
            setSearchQuery(q);
            emit stateChanged();
        },
        [this](bool) { emit stateChanged(); }, 300);

    // .backlink-pane: padding 4 12 32
    m_tree->setContentMargins(12, 4, 12, 32);
    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        if (n->type == kHeader) {
            if (n->id == u"linked")
                setBacklinkCollapsed(!m_backlinkCollapsed);
            else
                setUnlinkedCollapsed(!m_unlinkedCollapsed);
            emit stateChanged();
            return;
        }
        openMatch(n, button, mods);
    };
    h.contextMenu = [this](TreeList::Node* n, const QPoint& g) { showContextMenu(n, g); };
    h.hover = [this](TreeList::Node* n, const QRect& r) {
        if (n->type == kResult || n->type == kMatch)
            hoverLink(m_owner, QStringLiteral("backlink"), n->data.toString(), QString(), r, m_tree->viewport());
    };
    h.unhover = [this] { unhoverLink(m_owner, m_tree->viewport()); };
    h.moreContext = [this](TreeList::Node* n, bool before) {
        if (!n->content)
            return;
        VaultSession* s = session();
        const md::CachedMetadata* meta = s && s->metadata() ? s->metadata()->fileCache(n->data.toString()) : nullptr;
        if (before) {
            n->from = moreContextBefore(*n->content, meta, n->from);
            n->ellipsisBefore = false;
        } else {
            n->to = moreContextAfter(*n->content, meta, n->to);
            n->ellipsisAfter = false;
        }
        n->moreBefore = n->from > 0;
        n->moreAfter = n->to < n->content->size();
        m_tree->nodeChanged(n);
    };
    h.button = [this](TreeList::Node* n) {
        if (n->type != kMatch || n->matches.isEmpty())
            return;
        const QString source = n->data.toString();
        const auto it = m_unlinked.constFind(source);
        if (it == m_unlinked.cend())
            return;
        const int index = int(it->matches.indexOf(n->matches.first()));
        if (index >= 0)
            linkUnlinkedMention(source, index);
    };
    h.activateView = [this] {
        if (m_owner && m_owner->workspace() && m_owner->leaf())
            m_owner->workspace()->setActiveLeaf(m_owner->leaf(), false);
    };

    m_rebuild.setSingleShot(true);
    m_rebuild.setInterval(0);
    connect(&m_rebuild, &QTimer::timeout, this, &BacklinkPane::rebuild);
    m_linkedTimer.setSingleShot(true);
    m_linkedTimer.setInterval(50);
    connect(&m_linkedTimer, &QTimer::timeout, this, &BacklinkPane::recomputeLinked);

    if (VaultSession* s = session()) {
        if (MetadataCache* mc = s->metadata()) {
            m_connections << connect(mc, &MetadataCache::changed, this, &BacklinkPane::onMetadataChanged);
            m_connections << connect(mc, &MetadataCache::deleted, this, [this](const QString& path) {
                const bool a = m_linked.remove(path) > 0;
                const bool b = m_unlinked.remove(path) > 0;
                if (a || b)
                    scheduleRebuild();
            });
            m_connections << connect(mc, &MetadataCache::resolved, this, [this] { m_linkedTimer.start(); });
        }
    }
    rebuild();
}

BacklinkPane::~BacklinkPane()
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    cancelUnlinked();
    m_engine.reset(); // waits for the workers before the callbacks' context goes away
}

VaultSession* BacklinkPane::session() const { return m_owner ? m_owner->session() : nullptr; }

void BacklinkPane::setLeafActive(bool active) { m_tree->setLeafActive(active); }

void BacklinkPane::setFile(const QString& path)
{
    if (path == m_path && !path.isEmpty())
        return;
    m_path = path;
    recomputeLinked();
    recomputeUnlinked();
}

// ---- state ----------------------------------------------------------------------------------------

QJsonObject BacklinkPane::state() const
{
    return QJsonObject{{QStringLiteral("collapseAll"), m_collapseAll},
                       {QStringLiteral("extraContext"), m_extraContext},
                       {QStringLiteral("sortOrder"), m_sortOrder},
                       {QStringLiteral("showSearch"), m_header->isSearchShown()},
                       {QStringLiteral("searchQuery"), m_header->searchQuery()},
                       {QStringLiteral("backlinkCollapsed"), m_backlinkCollapsed},
                       {QStringLiteral("unlinkedCollapsed"), m_unlinkedCollapsed}};
}

void BacklinkPane::setState(const QJsonObject& st)
{
    if (st.value(QStringLiteral("collapseAll")).isBool())
        setCollapseAll(st.value(QStringLiteral("collapseAll")).toBool());
    if (st.value(QStringLiteral("extraContext")).isBool())
        setExtraContext(st.value(QStringLiteral("extraContext")).toBool());
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_header->setSearchShown(st.value(QStringLiteral("showSearch")).toBool());
    if (st.value(QStringLiteral("sortOrder")).isString())
        setSortOrder(st.value(QStringLiteral("sortOrder")).toString());
    if (st.value(QStringLiteral("backlinkCollapsed")).isBool())
        setBacklinkCollapsed(st.value(QStringLiteral("backlinkCollapsed")).toBool());
    if (st.value(QStringLiteral("unlinkedCollapsed")).isBool())
        setUnlinkedCollapsed(st.value(QStringLiteral("unlinkedCollapsed")).toBool());
    if (st.value(QStringLiteral("searchQuery")).isString()) {
        m_header->setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
        setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
    }
}

void BacklinkPane::setCollapseAll(bool on)
{
    if (on == m_collapseAll)
        return;
    m_collapseAll = on;
    m_collapseButton->setActive(on);
    for (TreeList::Node* n : m_tree->allNodes())
        if (n->type == kResult)
            n->collapsed = on;
    m_tree->layoutChanged();
}

void BacklinkPane::setExtraContext(bool on)
{
    if (on == m_extraContext)
        return;
    m_extraContext = on;
    m_contextButton->setActive(on);
    scheduleRebuild();
}

void BacklinkPane::setSortOrder(const QString& order)
{
    if (!sortOrderFromId(order) || order == m_sortOrder)
        return;
    m_sortOrder = order;
    scheduleRebuild();
}

void BacklinkPane::setBacklinkCollapsed(bool collapsed)
{
    if (collapsed == m_backlinkCollapsed)
        return;
    m_backlinkCollapsed = collapsed;
    recomputeLinked();
}

void BacklinkPane::setUnlinkedCollapsed(bool collapsed)
{
    if (collapsed == m_unlinkedCollapsed)
        return;
    m_unlinkedCollapsed = collapsed;
    recomputeUnlinked();
}

void BacklinkPane::setSearchQuery(const QString& query)
{
    if (query == m_filter.source())
        return;
    m_filter = search::parseSearchQuery(query);
    recomputeLinked();
    recomputeUnlinked();
}

// ---- computing --------------------------------------------------------------------------------------

bool BacklinkPane::passesFilter(const QString& path, const QString& content) const
{
    if (m_filter.isEmpty())
        return true;
    VaultSession* s = session();
    const md::CachedMetadata* meta = s && s->metadata() ? s->metadata()->fileCache(path) : nullptr;
    const search::SearchDocument doc = meta ? search::SearchDocument::fromMetadata(path, content, *meta) : search::SearchDocument{path, true, content, {}, {}, {}, {}, {}};
    return search::documentMatches(m_filter, doc);
}

std::optional<BacklinkPane::Result> BacklinkPane::linkedResult(const QString& source) const
{
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    if (!mc || source == m_path)
        return std::nullopt;
    const md::CachedMetadata* meta = mc->fileCache(source);
    if (!meta)
        return std::nullopt;
    const QList<MatchRange> ranges = linkedMentionRanges(*meta, [&](const QString& linkpath) {
        return !linkpath.isEmpty() && mc->firstLinkpathDest(linkpath, source) == m_path;
    });
    if (ranges.isEmpty())
        return std::nullopt;
    const std::optional<QString> content = readNoteText(s, source);
    if (!content || !passesFilter(source, *content))
        return std::nullopt;
    return Result{source, std::make_shared<const QString>(*content), ranges};
}

void BacklinkPane::recomputeLinked()
{
    m_linkedTimer.stop();
    m_linked.clear();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    if (!m_backlinkCollapsed && !m_path.isEmpty() && mc) {
        for (const QString& source : mc->backlinksOf(m_path))
            if (std::optional<Result> r = linkedResult(source))
                m_linked.insert(source, *r);
    }
    scheduleRebuild();
}

QStringList BacklinkPane::mentionNames() const
{
    QStringList names;
    VaultSession* s = session();
    TFile* f = s && s->vault() ? s->vault()->file(m_path) : nullptr;
    if (!f)
        return names;
    names << f->basename();
    if (const md::CachedMetadata* meta = s->metadata() ? s->metadata()->fileCache(m_path) : nullptr)
        names << meta->aliases();
    return names;
}

std::optional<BacklinkPane::Result> BacklinkPane::unlinkedResult(const QString& source, const QString* text) const
{
    VaultSession* s = session();
    if (!s || source == m_path || !source.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        return std::nullopt;
    std::optional<QString> content = text ? std::optional<QString>(*text) : readNoteText(s, source);
    if (!content)
        return std::nullopt;
    const md::CachedMetadata* meta = s->metadata() ? s->metadata()->fileCache(source) : nullptr;
    const QList<MatchRange> ranges = unlinkedMentionRanges(*content, mentionNames(), meta);
    if (ranges.isEmpty() || !passesFilter(source, *content))
        return std::nullopt;
    return Result{source, std::make_shared<const QString>(*content), ranges};
}

void BacklinkPane::cancelUnlinked()
{
    if (m_job) {
        m_job->cancel();
        m_job.reset();
    }
}

void BacklinkPane::recomputeUnlinked()
{
    cancelUnlinked();
    m_unlinked.clear();
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    const QStringList names = mentionNames();
    m_unlinkedAliases = names.mid(1).join(QLatin1Char('\n'));
    if (m_unlinkedCollapsed || m_path.isEmpty() || !vault || names.isEmpty()) {
        scheduleRebuild();
        return;
    }
    // The search engine finds the notes containing a name (substring, any case); the whole-word,
    // link and frontmatter checks run on the results.
    QStringList terms;
    for (const QString& n : names)
        if (!n.trimmed().isEmpty() && !terms.contains(quoted(n), Qt::CaseInsensitive))
            terms << quoted(n);
    if (terms.isEmpty()) {
        scheduleRebuild();
        return;
    }
    const search::SearchQuery query = search::parseSearchQuery(QStringLiteral("content:(%1)").arg(terms.join(QStringLiteral(" OR "))));
    QStringList files;
    for (TFile* f : vault->markdownFiles())
        if (f->path() != m_path)
            files << f->path();
    // Open documents may hold newer text than the disk.
    QHash<QString, QString> open;
    if (DocumentManager* dm = s->documents())
        for (Document* d : dm->documents())
            open.insert(d->path(), d->text().toString());
    const QString root = vault->root();
    search::SearchSource source;
    source.count = files.size();
    source.load = [files, open, root](qsizetype i) -> std::shared_ptr<const search::SearchDocument> {
        auto doc = std::make_shared<search::SearchDocument>();
        doc->path = files[i];
        if (auto it = open.constFind(doc->path); it != open.cend()) {
            doc->content = *it;
        } else {
            QFile f(root + QLatin1Char('/') + doc->path);
            if (!f.open(QIODevice::ReadOnly))
                return nullptr;
            doc->content = decodeFile(f.readAll()).text.toString();
        }
        return doc;
    };
    search::SearchCallbacks cb;
    cb.context = this;
    cb.onResults = [this](QList<search::SearchResult> batch) {
        for (const search::SearchResult& r : batch)
            if (std::optional<Result> res = unlinkedResult(r.path))
                m_unlinked.insert(r.path, *res);
        scheduleRebuild();
    };
    cb.onFinished = [this](const search::SearchStats&) { scheduleRebuild(); };
    m_job = m_engine->search(query, source, cb);
    scheduleRebuild();
}

bool BacklinkPane::isSearchingUnlinked() const { return m_job && !m_job->isFinished(); }

void BacklinkPane::onMetadataChanged(const QString& path)
{
    if (m_path.isEmpty())
        return;
    if (path == m_path) {
        // new aliases: the unlinked mentions change
        const QStringList names = mentionNames();
        if (names.mid(1).join(QLatin1Char('\n')) != m_unlinkedAliases)
            recomputeUnlinked();
        return;
    }
    bool changed = false;
    if (!m_backlinkCollapsed) {
        if (std::optional<Result> r = linkedResult(path))
            m_linked.insert(path, *r);
        else
            m_linked.remove(path);
        changed = true;
    }
    if (!m_unlinkedCollapsed && !isSearchingUnlinked()) {
        if (std::optional<Result> r = unlinkedResult(path))
            m_unlinked.insert(path, *r);
        else
            m_unlinked.remove(path);
        changed = true;
    }
    if (changed)
        scheduleRebuild();
}

bool BacklinkPane::linkUnlinkedMention(const QString& sourcePath, int matchIndex)
{
    VaultSession* s = session();
    const auto it = m_unlinked.find(sourcePath);
    if (!s || it == m_unlinked.end() || matchIndex < 0 || matchIndex >= it->matches.size())
        return false;
    const MatchRange m = it->matches[matchIndex];
    const QString mention = it->content->mid(m.from, m.to - m.from);
    FileManager* fm = s->fileManager();
    const QString link = fm ? fm->generateMarkdownLink(m_path, sourcePath, QString(), mention) : QStringLiteral("[[%1]]").arg(mention);
    if (!replaceNoteText(s, sourcePath, m.from, m.to, mention, link))
        return false;
    // Obsidian drops the match right away; the metadata update refreshes both sections.
    it->matches.removeAt(matchIndex);
    if (it->matches.isEmpty())
        m_unlinked.erase(it);
    scheduleRebuild();
    return true;
}

// ---- tree -------------------------------------------------------------------------------------------

void BacklinkPane::scheduleRebuild() { m_rebuild.start(); }

QList<const BacklinkPane::Result*> BacklinkPane::sorted(const QHash<QString, Result>& results) const
{
    QList<const Result*> out;
    for (const Result& r : results)
        out << &r;
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    const ExplorerSortOrder order = sortOrderFromId(m_sortOrder).value_or(ExplorerSortOrder::Alphabetical);
    auto base = [](const QString& p) {
        QString b = p.mid(p.lastIndexOf(u'/') + 1);
        if (b.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            b.chop(3);
        return b;
    };
    auto times = [vault](const QString& p, bool created) -> qint64 {
        TFile* f = vault ? vault->file(p) : nullptr;
        return f ? (created ? f->ctimeMs() : f->mtimeMs()) : 0;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Result* a, const Result* b) {
        const int byName = naturalCompare(base(a->path), base(b->path));
        const int byPath = byName != 0 ? byName : QString::compare(a->path, b->path);
        switch (order) {
        case ExplorerSortOrder::Alphabetical: return byPath < 0;
        case ExplorerSortOrder::AlphabeticalReverse: return byPath > 0;
        case ExplorerSortOrder::ByModifiedTime:
        case ExplorerSortOrder::ByModifiedTimeReverse:
        case ExplorerSortOrder::ByCreatedTime:
        case ExplorerSortOrder::ByCreatedTimeReverse: {
            const bool created = order == ExplorerSortOrder::ByCreatedTime || order == ExplorerSortOrder::ByCreatedTimeReverse;
            const bool reverse = order == ExplorerSortOrder::ByModifiedTimeReverse || order == ExplorerSortOrder::ByCreatedTimeReverse;
            const qint64 ta = times(a->path, created), tb = times(b->path, created);
            if (ta != tb)
                return reverse ? ta < tb : ta > tb;
            return byPath < 0;
        }
        }
        return byPath < 0;
    });
    return out;
}

void BacklinkPane::addSection(std::vector<TreeList::NodePtr>& roots, bool unlinked)
{
    const bool collapsed = unlinked ? m_unlinkedCollapsed : m_backlinkCollapsed;
    auto header = std::make_unique<TreeList::Node>();
    header->kind = TreeList::Kind::Header;
    header->type = kHeader;
    header->id = unlinked ? QStringLiteral("unlinked") : QStringLiteral("linked");
    header->title = unlinked ? tr("Unlinked mentions") : tr("Linked mentions");
    header->collapsed = collapsed;
    header->tooltip = collapsed ? tr("Click to expand") : tr("Click to collapse");
    header->draggable = false;
    header->selectable = false;
    if (unlinked && !m_backlinkCollapsed)
        header->spaceBefore = 16; // the linked results' container padding-bottom
    if (!collapsed) {
        const QHash<QString, Result>& results = unlinked ? m_unlinked : m_linked;
        int total = 0;
        for (const Result& r : results)
            total += int(r.matches.size());
        header->flair = QString::number(total);
        header->spaceAfter = 4; // .search-result-container padding-top
        VaultSession* s = session();
        MetadataCache* mc = s ? s->metadata() : nullptr;
        for (const Result* r : sorted(results)) {
            TreeList::Node* item = header->add(TreeList::Kind::Item, (unlinked ? QStringLiteral("u:") : QStringLiteral("l:")) + r->path);
            item->type = kResult;
            item->data = r->path;
            QString title = r->path.mid(r->path.lastIndexOf(u'/') + 1);
            if (title.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
                title.chop(3);
            item->title = title;
            item->flair = QString::number(r->matches.size());
            item->collapsible = true;
            item->collapsed = m_collapseAll;
            item->selectable = false;
            item->draggable = false;
            const md::CachedMetadata* meta = mc ? mc->fileCache(r->path) : nullptr;
            for (const MatchSnippet& sn : buildMatchSnippets(*r->content, r->matches, m_extraContext, meta, unlinked)) {
                TreeList::Node* m = item->add(TreeList::Kind::Match);
                m->type = kMatch;
                m->data = r->path;
                m->content = r->content;
                m->from = sn.from;
                m->to = sn.to;
                m->ellipsisBefore = sn.ellipsisBefore;
                m->ellipsisAfter = sn.ellipsisAfter;
                m->moreBefore = sn.from > 0;
                m->moreAfter = sn.to < r->content->size();
                m->matches = sn.matches;
                m->selectable = false;
                if (unlinked)
                    m->button = tr("Link");
            }
        }
        const bool searching = unlinked && isSearchingUnlinked();
        if (results.isEmpty() && !searching) {
            TreeList::Node* msg = header->add(TreeList::Kind::Message);
            msg->title = unlinked ? tr("No unlinked mentions found.") : tr("No backlinks found.");
        }
    }
    roots.push_back(std::move(header));
}

void BacklinkPane::rebuild()
{
    m_rebuild.stop();
    std::vector<TreeList::NodePtr> roots;
    if (!m_path.isEmpty()) {
        addSection(roots, false);
        addSection(roots, true);
    }
    m_tree->setContentMargins(12, 4, 12, m_unlinkedCollapsed ? 32 : 48);
    m_tree->setRoots(std::move(roots));
    emit resultsChanged();
}

QStringList BacklinkPane::linkedFiles() const
{
    QStringList out;
    for (const Result* r : sorted(m_linked))
        out << r->path;
    return out;
}

QStringList BacklinkPane::unlinkedFiles() const
{
    QStringList out;
    for (const Result* r : sorted(m_unlinked))
        out << r->path;
    return out;
}

int BacklinkPane::linkedCount() const
{
    int n = 0;
    for (const Result& r : m_linked)
        n += int(r.matches.size());
    return n;
}

int BacklinkPane::unlinkedCount() const
{
    int n = 0;
    for (const Result& r : m_unlinked)
        n += int(r.matches.size());
    return n;
}

void BacklinkPane::openMatch(TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    VaultSession* s = session();
    Workspace* ws = m_owner ? m_owner->workspace() : nullptr;
    TFile* f = s && s->vault() ? s->vault()->file(n->data.toString()) : nullptr;
    if (!ws || !f)
        return;
    QJsonObject eState;
    if (n->type == kMatch && n->content) {
        eState = matchEState(*n->content, n->matches);
    } else if (n->type == kResult) {
        const QHash<QString, Result>& results = n->id.startsWith(u"u:") ? m_unlinked : m_linked;
        const auto it = results.constFind(n->data.toString());
        if (it != results.cend())
            eState = matchEState(*it->content, it->matches);
    }
    ws->openFile(f, leafModeFor(button, mods), eState);
}

void BacklinkPane::showContextMenu(TreeList::Node* n, const QPoint& globalPos)
{
    if (!n || (n->type != kResult && n->type != kMatch))
        return;
    VaultSession* s = session();
    Workspace* ws = m_owner ? m_owner->workspace() : nullptr;
    if (!s || !ws)
        return;
    const QString path = n->data.toString();
    QMenu menu(this);
    populateLinkMenu(&menu, ws, path, QString());
    if (n->type == kResult) {
        menu.addSeparator();
        QAction* del = menu.addAction(IconProvider::instance().icon(QStringLiteral("trash-2"), 16, 2.0), tr("Delete file"));
        del->setProperty("ocWarning", true);
        QPointer<VaultSession> ps = s;
        connect(del, &QAction::triggered, this, [ps, path] {
            if (ps && ps->vault())
                if (TFile* f = ps->vault()->file(path))
                    promptForDeletion(ps, f);
        });
    }
    m_tree->setMenuNode(n);
    QPointer<BacklinkPane> self(this);
    menu.exec(globalPos);
    if (self)
        m_tree->setMenuNode(nullptr);
}

// ---------------------------------------------------------------------------------------------
// BacklinkView
// ---------------------------------------------------------------------------------------------

BacklinkView::BacklinkView(WorkspaceLeaf* leaf, QWidget* parent)
    : LinkedFileView(leaf, parent)
    , m_pane(new BacklinkPane(this, this))
{
    setObjectName(QStringLiteral("ocBacklinkView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_pane);
    connect(m_pane, &BacklinkPane::stateChanged, this, &View::stateChanged);
}

QString BacklinkView::baseTitle() const { return tr("Backlinks"); }
QString BacklinkView::boundTitle() const { return tr("Backlinks for %1"); }

QJsonObject BacklinkView::state() const
{
    QJsonObject o = LinkedFileView::state();
    const QJsonObject p = m_pane->state();
    for (auto it = p.begin(); it != p.end(); ++it)
        o.insert(it.key(), it.value());
    return o;
}

void BacklinkView::setState(const QJsonObject& st)
{
    m_pane->setState(st);
    LinkedFileView::setState(st);
}

void BacklinkView::focusContent() { m_pane->tree()->setFocus(Qt::OtherFocusReason); }

void BacklinkView::fileChanged() { m_pane->setFile(filePath()); }

void BacklinkView::leafActiveChanged(bool active) { m_pane->setLeafActive(active); }

} // namespace oc
