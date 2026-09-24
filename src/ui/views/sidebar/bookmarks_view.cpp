#include "ui/views/sidebar/bookmarks_view.h"

#include "core/time/moment_format.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace oc {

namespace {
constexpr const char* kFoldKey = "bookmarks-folds";

QString shortName(Vault* vault, const QString& path)
{
    TAbstractFile* item = vault ? vault->item(path) : nullptr;
    if (!item)
        return {};
    if (item->isFolder())
        return item->name();
    auto* f = static_cast<TFile*>(item);
    return f->isMarkdown() ? f->basename() : f->name();
}

QString iconFor(const Bookmark& b)
{
    if (b.type == u"file") {
        if (b.path.endsWith(QLatin1String(".canvas"), Qt::CaseInsensitive))
            return QStringLiteral("layout-dashboard");
        if (b.subpath.startsWith(QLatin1String("#^")))
            return QStringLiteral("toy-brick");
        if (b.subpath.startsWith(u'#'))
            return QStringLiteral("heading");
        return QStringLiteral("file");
    }
    if (b.type == u"folder")
        return QStringLiteral("folder");
    if (b.type == u"search")
        return QStringLiteral("search");
    if (b.type == u"graph")
        return QStringLiteral("git-fork");
    if (b.type == u"url")
        return QStringLiteral("globe-2");
    return QString();
}

QVariant pointerData(const Bookmark* b) { return QVariant::fromValue(reinterpret_cast<quintptr>(b)); }

// A modal row (.setting-item): the name on the left, the control on the right.
QWidget* settingRow(const QString& name, QWidget* control)
{
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 8, 0, 8);
    h->setSpacing(16);
    auto* label = new QLabel(name, row);
    const ThemePtr t = currentTheme();
    label->setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13)));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(t->color(QStringLiteral("--text-normal")).name(QColor::HexArgb)));
    h->addWidget(label, 1);
    control->setParent(row);
    control->setMinimumWidth(220);
    h->addWidget(control, 1);
    return row;
}

// The bookmark group dropdown: a blank entry (no group) and every group, indented by level.
QComboBox* groupDropdown(BookmarkModel* model, Bookmark* current)
{
    auto* box = new QComboBox;
    box->addItem(QString(), pointerData(nullptr));
    std::function<void(const std::vector<std::unique_ptr<Bookmark>>&, const QString&)> add = [&](const std::vector<std::unique_ptr<Bookmark>>& list,
                                                                                                      const QString& indent) {
        for (const auto& b : list) {
            if (!b->isGroup())
                continue;
            box->addItem(indent + b->title.value_or(QString()), pointerData(b.get()));
            if (b.get() == current)
                box->setCurrentIndex(box->count() - 1);
            add(b->items, indent + QStringLiteral("    "));
        }
    };
    add(model->items(), QString());
    return box;
}

Bookmark* chosenGroup(BookmarkModel* model, QComboBox* box)
{
    auto* g = reinterpret_cast<Bookmark*>(box->currentData().value<quintptr>());
    return g && model->contains(g) ? g : nullptr;
}
} // namespace

std::unique_ptr<Bookmark> bookmarkForLeaf(WorkspaceLeaf* leaf)
{
    View* v = leaf ? leaf->view() : nullptr;
    if (!v)
        return nullptr;
    if (auto* fv = qobject_cast<FileView*>(v); fv && fv->file())
        return Bookmark::file(fv->file()->path());
    if (v->viewType() == u"search") {
        const QString q = v->state().value(QStringLiteral("query")).toString();
        return q.isEmpty() ? nullptr : Bookmark::search(q);
    }
    if (v->viewType() == u"graph") {
        auto b = std::make_unique<Bookmark>();
        b->type = QStringLiteral("graph");
        b->ctime = QDateTime::currentMSecsSinceEpoch();
        b->title = QObject::tr("Untitled Graph");
        b->extra.insert(QStringLiteral("options"), v->state().value(QStringLiteral("options")).toObject());
        return b;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// BookmarkModal
// ---------------------------------------------------------------------------------------------

namespace {
Modal* bookmarkModal(MainWindow* window, BookmarkModel* model, Bookmark* existing, std::shared_ptr<std::unique_ptr<Bookmark>> fresh,
                     Bookmark* presetGroup, std::function<void()> onSave)
{
    if (!window || !window->overlay() || !model)
        return nullptr;
    Bookmark* item = existing ? existing : fresh->get();
    if (!item)
        return nullptr;
    Vault* vault = window->session() ? window->session()->vault() : nullptr;
    auto* modal = new Modal(window->overlay());
    modal->setTitle(existing ? QObject::tr("Edit bookmark") : QObject::tr("Add bookmark"));
    auto* form = new QWidget;
    auto* v = new QVBoxLayout(form);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    if (item->type == u"file" || item->type == u"folder") {
        auto* path = new QLineEdit;
        QString shown = item->path;
        if (shown.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            shown.chop(3);
        path->setText(shown + (item->type == u"file" ? item->subpath : QString()));
        path->setEnabled(false);
        v->addWidget(settingRow(QObject::tr("Path"), path));
    } else if (item->type == u"search") {
        auto* query = new QLineEdit;
        query->setText(item->query);
        query->setEnabled(false);
        v->addWidget(settingRow(QObject::tr("Query"), query));
    } else if (item->type == u"url") {
        auto* url = new QLineEdit;
        url->setText(item->extra.value(QStringLiteral("url")).toString());
        url->setEnabled(false);
        v->addWidget(settingRow(QStringLiteral("URL"), url));
    }
    auto* title = new QLineEdit;
    title->setObjectName(QStringLiteral("ocBookmarkTitle"));
    title->setPlaceholderText(BookmarkModel::title(*item, [vault](const QString& p) { return shortName(vault, p); }));
    title->setText(item->title.value_or(QString()));
    v->addWidget(settingRow(QObject::tr("Title"), title));
    Bookmark* initialGroup = existing ? existing->parent : presetGroup;
    QComboBox* group = groupDropdown(model, initialGroup);
    group->setObjectName(QStringLiteral("ocBookmarkGroup"));
    v->addWidget(settingRow(QObject::tr("Bookmark group"), group));
    modal->addContentWidget(form);

    const int remove = existing ? modal->addButton(QObject::tr("Remove"), Modal::ButtonStyle::Destructive) : -1;
    modal->addButton(QObject::tr("Cancel"));
    const int save = modal->addButton(QObject::tr("Save"), Modal::ButtonStyle::Cta);
    modal->setDefaultButton(save);
    const bool hadTitle = item->title.has_value();
    QPointer<BookmarkModel> m = model;
    QObject::connect(modal, &Modal::buttonClicked, modal, [=](int index) {
        if (!m)
            return;
        if (index == remove && existing && m->contains(existing)) {
            m->remove(existing);
            return;
        }
        if (index != save)
            return;
        Bookmark* target = existing ? existing : fresh->get();
        if (!target)
            return;
        const QString text = title->text();
        if (hadTitle || !text.isEmpty())
            target->title = text;
        Bookmark* g = chosenGroup(m, group);
        if (existing) {
            if (!m->contains(existing))
                return;
            m->edited(existing);
            if (g != existing->parent)
                m->move(existing, g, int(m->itemsOf(g).size()));
        } else {
            m->add(std::move(*fresh), g);
        }
        if (onSave)
            onSave();
    });
    modal->open();
    title->setFocus(Qt::OtherFocusReason);
    title->selectAll();
    return modal;
}
} // namespace

Modal* BookmarkModal::edit(MainWindow* window, BookmarkModel* model, Bookmark* existing)
{
    return bookmarkModal(window, model, existing, std::make_shared<std::unique_ptr<Bookmark>>(), nullptr, {});
}

Modal* BookmarkModal::add(MainWindow* window, BookmarkModel* model, std::unique_ptr<Bookmark> item, Bookmark* group, std::function<void()> onSave)
{
    auto fresh = std::make_shared<std::unique_ptr<Bookmark>>(std::move(item));
    return bookmarkModal(window, model, nullptr, fresh, group, std::move(onSave));
}

Modal* BookmarkModal::addMany(MainWindow* window, BookmarkModel* model, std::vector<std::unique_ptr<Bookmark>> items)
{
    if (!window || !window->overlay() || !model || items.empty())
        return nullptr;
    auto shared = std::make_shared<std::vector<std::unique_ptr<Bookmark>>>(std::move(items));
    auto* modal = new Modal(window->overlay());
    modal->setTitle(QObject::tr("Add %1 bookmarks").arg(shared->size()));
    auto* form = new QWidget;
    auto* v = new QVBoxLayout(form);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    const QString placeholder = formatMoment(QDateTime::currentDateTime(), QStringLiteral("LLL"));
    auto* title = new QLineEdit;
    title->setObjectName(QStringLiteral("ocBookmarkTitle"));
    title->setPlaceholderText(placeholder);
    v->addWidget(settingRow(QObject::tr("Title"), title));
    QComboBox* group = groupDropdown(model, nullptr);
    group->setObjectName(QStringLiteral("ocBookmarkGroup"));
    v->addWidget(settingRow(QObject::tr("Bookmark group"), group));
    modal->addContentWidget(form);
    modal->addButton(QObject::tr("Cancel"));
    const int save = modal->addButton(QObject::tr("Save"), Modal::ButtonStyle::Cta);
    modal->setDefaultButton(save);
    QPointer<BookmarkModel> m = model;
    QObject::connect(modal, &Modal::buttonClicked, modal, [=](int index) {
        if (!m || index != save)
            return;
        const QString text = title->text();
        Bookmark* g = m->add(Bookmark::group(text.isEmpty() ? placeholder : text), chosenGroup(m, group));
        for (auto& b : *shared)
            m->add(std::move(b), g);
        shared->clear();
    });
    modal->open();
    title->setFocus(Qt::OtherFocusReason);
    return modal;
}

// ---------------------------------------------------------------------------------------------
// BookmarksView
// ---------------------------------------------------------------------------------------------

BookmarksView::BookmarksView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
    , m_header(new NavHeader(this))
    , m_tree(new TreeList(this))
    , m_model(sessionBookmarks(session()))
{
    setObjectName(QStringLiteral("ocBookmarksView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    m_header->addButton(QStringLiteral("bookmark-plus"), tr("Bookmark the active tab..."), [this] {
        Workspace* ws = workspace();
        if (std::unique_ptr<Bookmark> b = bookmarkForLeaf(ws ? ws->activeRootLeaf() : nullptr))
            BookmarkModal::add(sessionWindow(session()), m_model, std::move(b));
    });
    m_header->addButton(QStringLiteral("folder-plus"), tr("New group"), [this] { createNewGroup(); });
    m_collapseButton = m_header->addButton(QStringLiteral("chevrons-down-up"), tr("Collapse all"), [this] {
        m_tree->setAllCollapsed(!m_tree->isAllCollapsed());
        updateCollapseButton();
        m_foldSave.start();
    });
    m_header->addSearchFilter([this](const QString& q) { setSearchQuery(q); }, [this](bool) { emit stateChanged(); });

    // .view-content: padding 4 12 32
    m_tree->setContentMargins(12, 4, 12, 32);
    m_tree->setSelectionEnabled(true);
    m_tree->setDragEnabled(true);
    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        Bookmark* b = bookmarkOf(n);
        if (!b)
            return;
        if (b->isGroup()) {
            m_tree->toggleCollapsed(n);
            updateCollapseButton();
            m_foldSave.start();
            return;
        }
        Workspace::NewLeaf mode = Workspace::NewLeaf::No;
        if (button == Qt::MiddleButton || (mods & Qt::ControlModifier))
            mode = (mods & Qt::AltModifier) ? Workspace::NewLeaf::SplitRight : Workspace::NewLeaf::Tab;
        open(b, mode);
    };
    h.collapsedChanged = [this](TreeList::Node*, bool) {
        updateCollapseButton();
        m_foldSave.start();
    };
    h.contextMenu = [this](TreeList::Node* n, const QPoint& g) { showContextMenu(n, g); };
    h.hover = [this](TreeList::Node* n, const QRect& r) {
        Bookmark* b = bookmarkOf(n);
        if (b && b->type == u"file")
            hoverLink(this, QStringLiteral("bookmarks"), b->path + b->subpath, QString(), r, m_tree->viewport());
    };
    h.unhover = [this] { unhoverLink(this, m_tree->viewport()); };
    h.rename = [this](TreeList::Node* n) {
        if (Bookmark* b = bookmarkOf(n))
            startRename(b);
    };
    h.renamed = [this](TreeList::Node* n, const QString& text) {
        Bookmark* b = bookmarkOf(n);
        const QString t = text.trimmed();
        if (b && !t.isEmpty() && t != titleOf(*b)) {
            b->title = t;
            m_model->edited(b);
        }
    };
    h.remove = [this](const QList<TreeList::Node*>& nodes) {
        QList<Bookmark*> items;
        for (TreeList::Node* n : nodes)
            if (Bookmark* b = bookmarkOf(n))
                items << b;
        for (Bookmark* b : std::as_const(items))
            if (m_model->contains(b))
                m_model->remove(b);
    };
    h.canDrop = [this](const QList<TreeList::Node*>&, TreeList::Node* target, TreeList::DropPlacement placement) {
        Bookmark* t = bookmarkOf(target);
        return t && (placement != TreeList::DropPlacement::Inside || t->isGroup());
    };
    h.drop = [this](const QList<TreeList::Node*>& nodes, TreeList::Node* target, TreeList::DropPlacement placement) {
        dropItems(nodes, target, placement);
    };
    h.activateView = [this] {
        if (workspace() && this->leaf())
            workspace()->setActiveLeaf(this->leaf(), false);
    };

    m_update.setSingleShot(true);
    m_update.setInterval(50);
    connect(&m_update, &QTimer::timeout, this, &BookmarksView::update);
    m_foldSave.setSingleShot(true);
    m_foldSave.setInterval(200);
    connect(&m_foldSave, &QTimer::timeout, this, &BookmarksView::saveFoldsNow);
    if (m_model)
        m_connections << connect(m_model, &BookmarkModel::changed, this, [this] { m_update.start(); });
    if (VaultSession* s = session())
        if (Vault* vault = s->vault()) {
            m_connections << connect(vault, &Vault::created, this, [this] { m_update.start(); });
            m_connections << connect(vault, &Vault::deleted, this, [this] { m_update.start(); });
            m_connections << connect(vault, &Vault::renamed, this, [this](TAbstractFile* item, const QString& oldPath, bool) {
                if (m_model)
                    m_model->renamePath(oldPath, item->path());
            });
        }
    update();
}

BookmarksView::~BookmarksView()
{
    if (m_foldSave.isActive())
        saveFoldsNow();
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
}

QString BookmarksView::displayText() const { return tr("Bookmarks"); }

QJsonObject BookmarksView::state() const
{
    return QJsonObject{{QStringLiteral("showSearch"), m_header->isSearchShown()}, {QStringLiteral("searchQuery"), m_header->searchQuery()}};
}

void BookmarksView::setState(const QJsonObject& st)
{
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_header->setSearchShown(st.value(QStringLiteral("showSearch")).toBool());
    if (st.value(QStringLiteral("searchQuery")).isString()) {
        m_header->setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
        setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
    }
}

void BookmarksView::onOpen()
{
    if (Workspace* ws = workspace()) {
        m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* l) { m_tree->setLeafActive(l && l == leaf()); });
        m_connections << connect(ws, &Workspace::fileOpened, this, [this](TFile* f) {
            m_activePath = f ? f->path() : QString();
            for (TreeList::Node* n : m_tree->allNodes())
                if (Bookmark* b = bookmarkOf(n))
                    n->active = b->type == u"file" && b->subpath.isEmpty() && b->path == m_activePath;
            m_tree->viewport()->update();
        });
        TFile* f = workspaceActiveFile(ws);
        m_activePath = f ? f->path() : QString();
        m_tree->setLeafActive(ws->activeLeaf() == leaf());
    }
    update();
}

void BookmarksView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

void BookmarksView::setSearchQuery(const QString& query)
{
    if (query == m_filter.source())
        return;
    m_filter = search::parseSearchQuery(query);
    update();
    emit stateChanged();
}

QString BookmarksView::titleOf(const Bookmark& item) const
{
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    return BookmarkModel::title(item, [vault](const QString& p) { return shortName(vault, p); });
}

Bookmark* BookmarksView::bookmarkOf(const TreeList::Node* node) const
{
    if (!node || !m_model)
        return nullptr;
    auto* b = reinterpret_cast<Bookmark*>(node->data.value<quintptr>());
    return b && m_model->contains(b) ? b : nullptr;
}

TreeList::Node* BookmarksView::nodeFor(const Bookmark* item) const
{
    for (TreeList::Node* n : m_tree->allNodes())
        if (reinterpret_cast<const Bookmark*>(n->data.value<quintptr>()) == item)
            return n;
    return nullptr;
}

void BookmarksView::updateCollapseButton()
{
    if (m_tree->isAllCollapsed()) {
        m_collapseButton->setIconName(QStringLiteral("chevrons-up-down"));
        m_collapseButton->setToolTip(tr("Expand all"));
    } else {
        m_collapseButton->setIconName(QStringLiteral("chevrons-down-up"));
        m_collapseButton->setToolTip(tr("Collapse all"));
    }
}

void BookmarksView::saveFoldsNow()
{
    m_foldSave.stop();
    if (!m_foldsLoaded)
        return;
    for (const TreeList::Node* n : m_tree->allNodes()) {
        if (!n->collapsible)
            continue;
        if (n->collapsed)
            m_folded.insert(n->id);
        else
            m_folded.remove(n->id);
    }
    QStringList ids(m_folded.begin(), m_folded.end());
    ids.sort();
    saveFolds(session(), QString::fromLatin1(kFoldKey), ids);
}

void BookmarksView::update()
{
    m_update.stop();
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!m_foldsLoaded && s) {
        const QStringList saved = loadFolds(s, QString::fromLatin1(kFoldKey));
        m_folded = QSet<QString>(saved.begin(), saved.end());
        m_foldsLoaded = true;
    } else {
        for (const TreeList::Node* n : m_tree->allNodes())
            if (n->collapsible) {
                if (n->collapsed)
                    m_folded.insert(n->id);
                else
                    m_folded.remove(n->id);
            }
    }
    std::vector<TreeList::NodePtr> roots;
    const bool valid = !m_model || m_model->property("ocValid").toBool() || !m_model->property("ocValid").isValid();
    if (!valid) {
        auto msg = std::make_unique<TreeList::Node>();
        msg->kind = TreeList::Kind::Message;
        msg->centered = true;
        msg->title = tr("Failed to load bookmarks.") + QLatin1Char('\n') + tr("The data file is corrupted.");
        roots.push_back(std::move(msg));
        m_tree->setRoots(std::move(roots));
        return;
    }
    QSet<QString> ids;
    const bool filtering = !m_filter.isEmpty();
    bool anyShown = false;
    std::function<TreeList::NodePtr(const Bookmark&, bool&)> make = [&](const Bookmark& b, bool& visible) {
        auto n = std::make_unique<TreeList::Node>();
        QString id = QStringLiteral("item-%1").arg(b.ctime);
        while (ids.contains(id))
            id += QLatin1Char('+');
        ids.insert(id);
        n->id = id;
        n->data = pointerData(&b);
        n->title = titleOf(b);
        n->icon = iconFor(b);
        n->collapsible = b.isGroup();
        n->collapsed = b.isGroup() && m_folded.contains(id);
        n->active = b.type == u"file" && b.subpath.isEmpty() && b.path == m_activePath;
        bool anyChild = false;
        for (const auto& c : b.items) {
            bool cv = true;
            n->add(make(*c, cv));
            anyChild = anyChild || cv;
        }
        bool self = !filtering || filterMatch(m_filter, n->title).has_value();
        // a file bookmark whose file is gone is hidden (Obsidian toggles it with the file)
        if (b.type == u"file" && vault && !vault->file(b.path)) {
            self = false;
            anyChild = false;
        }
        visible = self || anyChild;
        n->hidden = !visible;
        return n;
    };
    if (m_model) {
        for (const auto& b : m_model->items()) {
            bool visible = true;
            roots.push_back(make(*b, visible));
            anyShown = anyShown || visible;
        }
    }
    if (!anyShown && !filtering) {
        auto msg = std::make_unique<TreeList::Node>();
        msg->kind = TreeList::Kind::Message;
        msg->centered = true;
        msg->title = tr("No bookmarks found");
        roots.push_back(std::move(msg));
    }
    m_tree->setRoots(std::move(roots));
}

void BookmarksView::open(Bookmark* item, Workspace::NewLeaf mode)
{
    Workspace* ws = workspace();
    VaultSession* s = session();
    if (!item || !ws || !s)
        return;
    if (item->type == u"file") {
        MarkdownView::openLinkText(ws, item->path + item->subpath, 0, QString(), mode != Workspace::NewLeaf::No);
    } else if (item->type == u"folder") {
        TAbstractFile* f = s->vault() ? s->vault()->item(item->path) : nullptr;
        WorkspaceLeaf* l = ws->leavesOfType(QStringLiteral("file-explorer")).value(0);
        if (auto* fe = l ? qobject_cast<FileExplorerView*>(l->view()) : nullptr; fe && f)
            fe->revealInFolder(f);
    } else if (item->type == u"search") {
        searchFor(sessionWindow(s), item->query, mode != Workspace::NewLeaf::No);
    } else if (item->type == u"graph") {
        if (ws->viewRegistry() && ws->viewRegistry()->hasView(QStringLiteral("graph")))
            if (WorkspaceLeaf* l = ws->getLeaf(mode))
                l->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("graph")}});
    } else if (item->type == u"url") {
        QDesktopServices::openUrl(QUrl(item->extra.value(QStringLiteral("url")).toString()));
    }
}

Bookmark* BookmarksView::createNewGroup(Bookmark* parentGroup)
{
    if (!m_model)
        return nullptr;
    Bookmark* g = m_model->add(Bookmark::group(tr("Untitled group")), parentGroup);
    update(); // now, so the group can be renamed in place
    startRename(g);
    return g;
}

void BookmarksView::startRename(Bookmark* item)
{
    if (TreeList::Node* n = nodeFor(item))
        m_tree->startRename(n);
}

void BookmarksView::dropItems(const QList<TreeList::Node*>& nodes, TreeList::Node* target, TreeList::DropPlacement placement)
{
    Bookmark* t = bookmarkOf(target);
    if (!t || !m_model)
        return;
    Bookmark* group = t->parent;
    int index = m_model->indexOf(t);
    if (placement == TreeList::DropPlacement::Inside) {
        group = t;
        index = target->collapsed ? int(t->items.size()) : 0;
    } else if (placement == TreeList::DropPlacement::After) {
        if (t->isGroup() && !target->collapsed && target->hasVisibleChildren()) {
            group = t;
            index = 0;
        } else {
            ++index;
        }
    }
    for (TreeList::Node* n : nodes) {
        Bookmark* b = bookmarkOf(n);
        if (!b || b == group)
            continue;
        m_model->move(b, group, index);
        index = m_model->indexOf(b) + 1; // the next one goes after it
    }
}

void BookmarksView::populateContextMenu(QMenu* menu, TreeList::Node* node)
{
    const IconProvider& icons = IconProvider::instance();
    QPointer<BookmarksView> self(this);
    if (!node) {
        QAction* a = menu->addAction(icons.icon(QStringLiteral("folder-plus"), 16, 2.0), tr("New group"));
        connect(a, &QAction::triggered, this, [self] {
            if (self)
                self->createNewGroup();
        });
        return;
    }
    Bookmark* b = bookmarkOf(node);
    if (!b)
        return;
    const QList<TreeList::Node*> selection = m_tree->selectedNodes();
    auto openAll = [self](QList<Bookmark*> items, Workspace::NewLeaf mode) {
        if (!self)
            return;
        for (Bookmark* x : items)
            if (self->m_model && self->m_model->contains(x) && (x->type == u"file" || x->type == u"graph"))
                self->open(x, mode);
    };
    if (selection.size() > 1 && selection.contains(node)) {
        QList<Bookmark*> items;
        for (TreeList::Node* n : selection)
            if (Bookmark* x = bookmarkOf(n))
                items << x;
        connect(menu->addAction(icons.icon(QStringLiteral("file-plus"), 16, 2.0), tr("Open in new tab")), &QAction::triggered, this,
                [openAll, items] { openAll(items, Workspace::NewLeaf::Tab); });
        connect(menu->addAction(icons.icon(QStringLiteral("separator-vertical"), 16, 2.0), tr("Open to the right")), &QAction::triggered, this,
                [openAll, items] { openAll(items, Workspace::NewLeaf::SplitRight); });
        menu->addSeparator();
        QAction* rm = menu->addAction(icons.icon(QStringLiteral("trash-2"), 16, 2.0), tr("Remove"));
        connect(rm, &QAction::triggered, this, [self, items] {
            for (Bookmark* x : items)
                if (self && self->m_model && self->m_model->contains(x))
                    self->m_model->remove(x);
        });
        return;
    }
    // open
    QList<Bookmark*> targets;
    if (b->isGroup()) {
        for (const auto& c : b->items)
            targets << c.get();
    } else if (b->type == u"file" || b->type == u"graph") {
        targets << b;
    }
    if (!targets.isEmpty()) {
        connect(menu->addAction(icons.icon(QStringLiteral("file-plus"), 16, 2.0), tr("Open in new tab")), &QAction::triggered, this,
                [openAll, targets] { openAll(targets, Workspace::NewLeaf::Tab); });
        connect(menu->addAction(icons.icon(QStringLiteral("separator-vertical"), 16, 2.0), tr("Open to the right")), &QAction::triggered, this,
                [openAll, targets] { openAll(targets, Workspace::NewLeaf::SplitRight); });
        menu->addSeparator();
    }
    // action
    connect(menu->addAction(icons.icon(QStringLiteral("pencil"), 16, 2.0), tr("Rename")), &QAction::triggered, this, [self, b] {
        if (self && self->m_model && self->m_model->contains(b))
            QTimer::singleShot(0, self, [self, b] {
                if (self && self->m_model && self->m_model->contains(b))
                    self->startRename(b);
            });
    });
    if (b->isGroup()) {
        connect(menu->addAction(icons.icon(QStringLiteral("bookmark-plus"), 16, 2.0), tr("Bookmark the active tab...")), &QAction::triggered, this,
                [self, b] {
                    if (!self || !self->m_model || !self->m_model->contains(b))
                        return;
                    Workspace* ws = self->workspace();
                    if (std::unique_ptr<Bookmark> item = bookmarkForLeaf(ws ? ws->activeRootLeaf() : nullptr))
                        BookmarkModal::add(sessionWindow(self->session()), self->m_model, std::move(item), b);
                });
        connect(menu->addAction(icons.icon(QStringLiteral("folder-plus"), 16, 2.0), tr("New group")), &QAction::triggered, this, [self, b] {
            if (self && self->m_model && self->m_model->contains(b))
                self->createNewGroup(b);
        });
    } else {
        connect(menu->addAction(icons.icon(QStringLiteral("wrench"), 16, 2.0), tr("Edit...")), &QAction::triggered, this, [self, b] {
            if (self && self->m_model && self->m_model->contains(b))
                BookmarkModal::edit(sessionWindow(self->session()), self->m_model, b);
        });
    }
    // view
    if (b->type == u"file" || b->type == u"folder") {
        VaultSession* s = session();
        if (s && s->vault() && s->vault()->item(b->path)) {
            menu->addSeparator();
            const QString path = b->path;
            connect(menu->addAction(icons.icon(QStringLiteral("folder-open"), 16, 2.0), tr("Reveal file in navigation")), &QAction::triggered, this,
                    [self, path] {
                        if (!self)
                            return;
                        Workspace* ws = self->workspace();
                        VaultSession* ss = self->session();
                        TAbstractFile* f = ss && ss->vault() ? ss->vault()->item(path) : nullptr;
                        WorkspaceLeaf* l = ws ? ws->leavesOfType(QStringLiteral("file-explorer")).value(0) : nullptr;
                        if (auto* fe = l ? qobject_cast<FileExplorerView*>(l->view()) : nullptr; fe && f)
                            fe->revealInFolder(f);
                    });
        }
    }
    // danger
    menu->addSeparator();
    QAction* rm = menu->addAction(icons.icon(QStringLiteral("trash-2"), 16, 2.0), tr("Remove"));
    connect(rm, &QAction::triggered, this, [self, b] {
        if (self && self->m_model && self->m_model->contains(b))
            self->m_model->remove(b);
    });
}

void BookmarksView::showContextMenu(TreeList::Node* node, const QPoint& globalPos)
{
    QMenu menu(this);
    populateContextMenu(&menu, node);
    if (menu.isEmpty())
        return;
    if (node)
        m_tree->setMenuNode(node);
    QPointer<BookmarksView> self(this);
    menu.exec(globalPos);
    if (self)
        m_tree->setMenuNode(nullptr);
}

} // namespace oc
