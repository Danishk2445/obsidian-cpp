#include "ui/views/sidebar/sidebar_common.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/metadata/link_updater.h"
#include "core/metadata/metadata_cache.h"
#include "core/search/search_document.h"
#include "core/search/search_match.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "text/codec.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/file_explorer/file_tree.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/widgets/search_input.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QVBoxLayout>

namespace oc {

// ---------------------------------------------------------------------------------------------
// NavHeader
// ---------------------------------------------------------------------------------------------

namespace {
// Escape in the filter input hides it (Obsidian's search filter keydown handler).
class EscapeFilter : public QObject {
public:
    EscapeFilter(QObject* parent, std::function<void()> onEscape)
        : QObject(parent)
        , m_onEscape(std::move(onEscape))
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() == QEvent::ShortcutOverride || ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier) {
                if (ev->type() == QEvent::KeyPress)
                    m_onEscape();
                ke->accept();
                return true;
            }
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    std::function<void()> m_onEscape;
};
} // namespace

NavHeader::NavHeader(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocNavHeader"));
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(0);
    m_row = new QHBoxLayout;
    m_row->setContentsMargins(0, 0, 0, 0);
    m_row->setSpacing(2);
    m_row->addStretch(1);
    m_row->addStretch(1);
    m_layout->addLayout(m_row);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (m_changed)
            m_changed(searchQuery());
    });
}

NavActionButton* NavHeader::addButton(const QString& icon, const QString& tooltip, std::function<void()> onClick)
{
    auto* b = new NavActionButton(icon, tooltip, this);
    m_row->insertWidget(m_row->count() - 1, b);
    m_buttons << b;
    connect(b, &QAbstractButton::clicked, this, [onClick = std::move(onClick)] {
        if (onClick)
            onClick();
    });
    return b;
}

NavActionButton* NavHeader::addSortButton(const QList<QList<QPair<QString, QString>>>& groups, std::function<QString()> current,
                                          std::function<void(const QString&)> choose)
{
    auto* b = new NavActionButton(QStringLiteral("sort-asc"), tr("Change sort order"), this);
    m_row->insertWidget(m_row->count() - 1, b);
    m_buttons << b;
    connect(b, &QAbstractButton::clicked, this, [this, b, groups, current, choose] {
        QMenu menu(this);
        const QString cur = current ? current() : QString();
        for (int g = 0; g < groups.size(); ++g) {
            for (const auto& [id, label] : groups[g]) {
                QAction* a = menu.addAction(label);
                a->setCheckable(true);
                a->setChecked(id == cur);
                a->setData(id);
                connect(a, &QAction::triggered, this, [choose, id] {
                    if (choose)
                        choose(id);
                });
            }
            menu.addSeparator();
        }
        b->setActive(true); // .has-active-menu
        menu.exec(b->mapToGlobal(QPoint(0, b->height() + 2)));
        b->setActive(false);
    });
    return b;
}

NavActionButton* NavHeader::addSearchFilter(std::function<void(const QString&)> changed, std::function<void(bool)> toggled, int debounceMs)
{
    m_changed = std::move(changed);
    m_toggled = std::move(toggled);
    m_debounce.setInterval(debounceMs);
    m_searchButton = addButton(QStringLiteral("search"), tr("Show search filter"), [this] { setSearchShown(!m_searchShown, true); });
    m_input = new SearchInput(this);
    m_input->setPlaceholderText(tr("Search..."));
    m_input->setClearButtonEnabled(true);
    m_input->hide();
    // .nav-header .search-input-container: margin 4px auto
    m_layout->addSpacing(4);
    m_layout->addWidget(m_input);
    connect(m_input, &QLineEdit::textEdited, this, [this] { m_debounce.start(); });
    connect(m_input, &SearchInput::cleared, this, [this] {
        m_debounce.stop();
        if (m_changed)
            m_changed(searchQuery());
    });
    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        m_debounce.stop();
        if (m_changed)
            m_changed(searchQuery());
    });
    m_input->installEventFilter(new EscapeFilter(m_input, [this] { setSearchShown(false); }));
    return m_searchButton;
}

void NavHeader::setSearchShown(bool shown, bool focus)
{
    if (!m_input)
        return;
    if (shown != m_searchShown) {
        m_searchShown = shown;
        m_input->setVisible(shown);
        m_searchButton->setActive(shown);
        if (!shown && !m_input->text().isEmpty()) {
            m_input->clear();
            m_debounce.stop();
            if (m_changed)
                m_changed(QString());
        }
        if (m_toggled)
            m_toggled(shown);
    }
    if (shown && focus) {
        m_input->setFocus(Qt::OtherFocusReason);
        m_input->selectAll();
    }
}

QString NavHeader::searchQuery() const { return m_input ? m_input->text() : QString(); }

void NavHeader::setSearchQuery(const QString& query)
{
    if (m_input && m_input->text() != query)
        m_input->setText(query);
}

// ---------------------------------------------------------------------------------------------
// LinkedFileView
// ---------------------------------------------------------------------------------------------

LinkedFileView::LinkedFileView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
{
}

LinkedFileView::~LinkedFileView()
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    disconnect(m_linkedConn);
}

TFile* LinkedFileView::file() const
{
    VaultSession* s = session();
    return s && s->vault() && !m_path.isEmpty() ? s->vault()->file(m_path) : nullptr;
}

bool LinkedFileView::isBound() const
{
    WorkspaceLeaf* l = leaf();
    return (l && l->isPinned()) || m_linked;
}

QString LinkedFileView::displayText() const
{
    if (TFile* f = file(); f && isBound())
        return boundTitle().arg(f->basename());
    return baseTitle();
}

QJsonObject LinkedFileView::state() const
{
    QJsonObject o;
    if (!m_path.isEmpty())
        o.insert(QStringLiteral("file"), m_path);
    return o;
}

void LinkedFileView::setState(const QJsonObject& st)
{
    // Obsidian's linked view: unless pinned (or linked), the view shows the active file.
    TFile* f = nullptr;
    if (isBound() && st.contains(QStringLiteral("file"))) {
        VaultSession* s = session();
        f = s && s->vault() ? s->vault()->file(st.value(QStringLiteral("file")).toString()) : nullptr;
    } else {
        f = activeWorkspaceFile();
    }
    loadFile(f);
}

TFile* LinkedFileView::activeWorkspaceFile() const { return workspaceActiveFile(workspace()); }

void LinkedFileView::loadFile(TFile* f)
{
    const QString path = f ? f->path() : QString();
    if (path == m_path && !path.isEmpty())
        return;
    m_path = path;
    emit titleChanged();
    fileChanged();
}

void LinkedFileView::linkTo(WorkspaceLeaf* source)
{
    disconnect(m_linkedConn);
    m_linked = source;
    if (source) {
        m_linkedConn = connect(source, &WorkspaceLeaf::titleChanged, this, &LinkedFileView::followLinked);
        connect(source, &WorkspaceLeaf::viewChanged, this, &LinkedFileView::followLinked, Qt::UniqueConnection);
        connect(source, &QObject::destroyed, this, [this] {
            m_linked = nullptr;
            emit titleChanged();
            loadFile(activeWorkspaceFile());
        });
        followLinked();
    }
    emit titleChanged();
}

void LinkedFileView::followLinked()
{
    if (!m_linked)
        return;
    auto* fv = qobject_cast<FileView*>(m_linked->view());
    loadFile(fv ? fv->file() : nullptr);
}

void LinkedFileView::attach()
{
    if (m_attached)
        return;
    VaultSession* s = session();
    Workspace* ws = workspace();
    if (!s || !ws || !s->vault())
        return;
    m_attached = true;
    m_connections << connect(ws, &Workspace::fileOpened, this, [this](TFile* f) {
        if (!isBound())
            loadFile(f);
    });
    m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* l) { leafActiveChanged(l && l == leaf()); });
    if (WorkspaceLeaf* l = leaf())
        m_connections << connect(l, &WorkspaceLeaf::pinnedChanged, this, [this] {
            if (!isBound())
                loadFile(activeWorkspaceFile());
            emit titleChanged();
        });
    Vault* vault = s->vault();
    m_connections << connect(vault, &Vault::renamed, this, [this](TAbstractFile* item, const QString& oldPath, bool) {
        if (oldPath == m_path || (item->isFolder() && m_path.startsWith(oldPath + QLatin1Char('/')))) {
            m_path = item->isFolder() ? item->path() + m_path.mid(oldPath.size()) : item->path();
            emit titleChanged();
            fileChanged();
        }
    });
    m_connections << connect(vault, &Vault::deleted, this, [this](TAbstractFile* item, bool) {
        if (item->path() == m_path || (item->isFolder() && m_path.startsWith(item->path() + QLatin1Char('/')))) {
            m_path.clear();
            emit titleChanged();
            fileChanged();
        }
    });
}

void LinkedFileView::onOpen()
{
    attach();
    if (!isBound())
        loadFile(activeWorkspaceFile());
    if (Workspace* ws = workspace())
        leafActiveChanged(ws->activeLeaf() == leaf());
}

void LinkedFileView::onClose()
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_attached = false;
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

std::optional<QString> readNoteText(VaultSession* session, const QString& path)
{
    if (!session)
        return std::nullopt;
    if (DocumentManager* dm = session->documents())
        if (Document* doc = dm->find(path))
            return doc->text().toString();
    Vault* vault = session->vault();
    if (!vault)
        return std::nullopt;
    const std::optional<QByteArray> bytes = vault->read(path, nullptr, nullptr);
    if (!bytes)
        return std::nullopt;
    return decodeFile(*bytes).text.toString();
}

bool replaceNoteText(VaultSession* session, const QString& path, int from, int to, const QString& expected, const QString& replacement,
                     const QString& userEvent)
{
    if (!session)
        return false;
    if (DocumentManager* dm = session->documents()) {
        if (Document* doc = dm->find(path)) {
            if (doc->isReadOnly())
                return false;
            const Text& t = doc->text();
            if (from < 0 || to > t.length() || t.slice(from, to) != expected)
                return false;
            Transaction tr;
            tr.changes = ChangeSet::of(ChangeSpec{from, to, replacement}, t.length());
            tr.userEvent = userEvent;
            tr.scrollIntoView = false;
            doc->dispatch(tr, nullptr, Selection::cursor(0));
            doc->save();
            return true;
        }
    }
    Vault* vault = session->vault();
    if (!vault)
        return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        DiskState state;
        QString err;
        const std::optional<QByteArray> bytes = vault->read(path, &state, &err);
        if (!bytes)
            return false;
        const DecodedFile dec = decodeFile(*bytes);
        if (dec.hasNul)
            return false;
        Text t = dec.text;
        if (LinkUpdater::apply(t, {LinkUpdater::Edit{from, to, expected, replacement}}) == 0)
            return false;
        const DocumentStorage::WriteResult r = vault->write(path, encodeFile(t, dec.bom), state, nullptr, &err);
        if (r == DocumentStorage::WriteResult::Ok)
            return true;
        if (r == DocumentStorage::WriteResult::Error)
            return false;
    }
    return false;
}

std::optional<QList<MatchRange>> filterMatch(const search::SearchQuery& query, const QString& text)
{
    if (query.isEmpty())
        return QList<MatchRange>{};
    search::SearchDocument doc;
    doc.content = text;
    const std::optional<search::SearchResult> r = search::matchDocument(query, doc);
    if (!r)
        return std::nullopt;
    QList<MatchRange> out;
    for (const search::ContentMatch& m : r->contentMatches)
        out.push_back({m.from, m.to});
    return out;
}

void hoverLink(View* view, const QString& source, const QString& linktext, const QString& sourcePath, const QRect& globalRect, QWidget* hoverParent)
{
    if (MainWindow* w = view ? sessionWindow(view->session()) : nullptr)
        PagePreview::of(w)->onLinkHover(source, linktext, sourcePath, globalRect, hoverParent);
}

void unhoverLink(View* view, QWidget* hoverParent)
{
    if (MainWindow* w = view ? sessionWindow(view->session()) : nullptr)
        PagePreview::of(w)->onLinkUnhover(hoverParent);
}

GlobalSearchHooks& globalSearchHooks()
{
    static GlobalSearchHooks hooks;
    return hooks;
}

namespace {
void openGlobalSearch(MainWindow* window, const QString& query)
{
    GlobalSearchHooks& h = globalSearchHooks();
    if (h.openSearch) {
        h.openSearch(window, query);
        return;
    }
    Workspace* ws = window ? window->workspace() : nullptr;
    if (!ws || !ws->viewRegistry() || !ws->viewRegistry()->hasView(QStringLiteral("search")))
        return;
    WorkspaceLeaf* l = ws->ensureSideLeaf(QStringLiteral("search"), Workspace::Side::Left, true);
    if (!l)
        return;
    QJsonObject st = l->viewState().value(QStringLiteral("state")).toObject();
    st.insert(QStringLiteral("query"), query);
    l->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("search")}, {QStringLiteral("state"), st}}, {}, false);
    ws->setActiveLeaf(l, true);
}
} // namespace

void searchFor(MainWindow* window, const QString& term, bool modifier)
{
    const GlobalSearchHooks& h = globalSearchHooks();
    const QString current = h.currentQuery ? h.currentQuery(window) : QString();
    if (!modifier || current.isEmpty()) {
        openGlobalSearch(window, term);
        return;
    }
    if (current.contains(term)) {
        QString q = current;
        q.replace(q.indexOf(term), term.size(), QString());
        openGlobalSearch(window, q.trimmed());
    } else {
        openGlobalSearch(window, current.trimmed() + QLatin1Char(' ') + term);
    }
}

void populateLinkMenu(QMenu* menu, Workspace* ws, const QString& linktext, const QString& sourcePath)
{
    QPointer<Workspace> w = ws;
    const IconProvider& icons = IconProvider::instance();
    QAction* tab = menu->addAction(icons.icon(QStringLiteral("file-plus"), 16, 2.0), QObject::tr("Open in new tab"));
    QObject::connect(tab, &QAction::triggered, menu, [w, linktext, sourcePath] {
        if (w)
            MarkdownView::openLinkText(w, linktext, 0, sourcePath, true);
    });
    QAction* right = menu->addAction(icons.icon(QStringLiteral("separator-vertical"), 16, 2.0), QObject::tr("Open to the right"));
    QObject::connect(right, &QAction::triggered, menu, [w, linktext, sourcePath] {
        if (!w)
            return;
        VaultSession* s = w->session();
        const QString dest = s && s->vault() ? LinkResolver::splitSubpath(linktext).first : QString();
        TFile* f = nullptr;
        if (s && s->metadata() && s->vault())
            f = s->vault()->file(s->metadata()->resolver().resolve(dest, sourcePath));
        if (f) {
            QJsonObject eState;
            const QString sub = LinkResolver::splitSubpath(linktext).second;
            if (!sub.isEmpty())
                eState.insert(QStringLiteral("subpath"), sub);
            w->openFile(f, Workspace::NewLeaf::SplitRight, eState);
        }
    });
}

QList<QList<QPair<QString, QString>>> countSortGroups(const QString& nameAToZ, const QString& nameZToA)
{
    return {
        {{QStringLiteral("alphabetical"), nameAToZ}, {QStringLiteral("alphabeticalReverse"), nameZToA}},
        {{QStringLiteral("frequency"), QObject::tr("Frequency (high to low)")}, {QStringLiteral("frequencyReverse"), QObject::tr("Frequency (low to high)")}},
    };
}

namespace {
QString foldsFile(VaultSession* session)
{
    if (!session || session->id().isEmpty())
        return {};
    return ClonePaths::vaultDataDir(session->id()) + QStringLiteral("/sidebar-folds.json");
}
} // namespace

QStringList loadFolds(VaultSession* session, const QString& key)
{
    const QString file = foldsFile(session);
    QStringList out;
    if (file.isEmpty())
        return out;
    for (const QJsonValue& v : JsonFile::read(file).value(key).toArray())
        out << v.toString();
    return out;
}

void saveFolds(VaultSession* session, const QString& key, const QStringList& ids)
{
    const QString file = foldsFile(session);
    if (file.isEmpty())
        return;
    QJsonObject o = JsonFile::read(file);
    const QJsonArray arr = QJsonArray::fromStringList(ids);
    if (o.value(key).toArray() == arr)
        return;
    o.insert(key, arr);
    JsonFile::write(file, o);
}

QList<QList<QPair<QString, QString>>> fileSortGroups()
{
    return {
        {{QStringLiteral("alphabetical"), QObject::tr("File name (A to Z)")}, {QStringLiteral("alphabeticalReverse"), QObject::tr("File name (Z to A)")}},
        {{QStringLiteral("byModifiedTime"), QObject::tr("Modified time (new to old)")},
         {QStringLiteral("byModifiedTimeReverse"), QObject::tr("Modified time (old to new)")}},
        {{QStringLiteral("byCreatedTime"), QObject::tr("Created time (new to old)")},
         {QStringLiteral("byCreatedTimeReverse"), QObject::tr("Created time (old to new)")}},
    };
}

} // namespace oc
