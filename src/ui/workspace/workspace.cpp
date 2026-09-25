#include "ui/workspace/workspace.h"

#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/shell/empty_view.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/vault_access.h"
#include "ui/workspace/view.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace_p.h"

#include <QApplication>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QRandomGenerator>

#include <cmath>

namespace oc {

namespace {
constexpr int kMaxHistory = 50;
constexpr int kMaxClosed = 10;
constexpr int kMaxRecent = 50;
constexpr int kDefaultSideWidth = 300;

const QString kEmptyType = QStringLiteral("empty");

bool sameViewState(const QJsonObject& a, const QJsonObject& b)
{
    return a.value(QStringLiteral("type")) == b.value(QStringLiteral("type"))
        && a.value(QStringLiteral("state")) == b.value(QStringLiteral("state"));
}

QString fileViewPath(const View* v)
{
    if (auto* fv = qobject_cast<const FileView*>(v))
        return fv->state().value(QStringLiteral("file")).toString();
    return {};
}

QJsonObject without(QJsonObject o, std::initializer_list<const char*> keys)
{
    for (const char* k : keys)
        o.remove(QLatin1String(k));
    return o;
}

// Obsidian's recent-file tracker keeps at most 25 notes, 10 canvases, 10 images and 10 other files.
QString recentCategory(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"md")
        return QStringLiteral("md");
    if (ext == u"canvas")
        return QStringLiteral("canvas");
    static const QStringList images{QStringLiteral("bmp"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                    QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
    if (images.contains(ext))
        return QStringLiteral("image");
    return QStringLiteral("other");
}

QString ribbonKey(const QString& actionId, const QString& title)
{
    // Obsidian keys ribbon items by "<plugin id>:<tooltip>".
    QString plugin = actionId.section(QLatin1Char(':'), 0, 0);
    if (plugin == u"insert-template")
        plugin = QStringLiteral("templates");
    return plugin + QLatin1Char(':') + title;
}

bool explicitlyHidden(const QWidget* w)
{
    return w->testAttribute(Qt::WA_WState_ExplicitShowHide) && w->testAttribute(Qt::WA_WState_Hidden);
}

// Obsidian's WorkspaceItem.setDimension(): values outside (0, 100) mean "unset" (flex-grow 1).
double validDimension(double dim) { return dim > 0 && dim < 100 ? dim : 0; }
} // namespace

QString fileBaseName(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    QString name = slash >= 0 ? path.mid(slash + 1) : path;
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        name.truncate(dot);
    return name;
}

// ---------------------------------------------------------------------------------------------
// Internal QObjects
// ---------------------------------------------------------------------------------------------

// Clicking or focusing anything inside a leaf makes it the active leaf (Obsidian listens for
// mousedown/focusin on the leaf container).
class ActivationFilter : public QObject {
    Q_OBJECT
public:
    ActivationFilter(WorkspaceImpl* d, QObject* parent)
        : QObject(parent)
        , d(d)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        const QEvent::Type t = ev->type();
        if (t == QEvent::KeyPress && d->drag.active && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
            d->endDrag({}, false); // Escape cancels a tab drag (like an HTML drag in Obsidian)
            return true;
        }
        if (t != QEvent::MouseButtonPress && t != QEvent::FocusIn)
            return false;
        if (!obj->isWidgetType() || !d->widget || d->destroying)
            return false;
        auto* w = static_cast<QWidget*>(obj);
        if (w->window() != d->widget->window() || !d->widget->isAncestorOf(w))
            return false;
        for (QWidget* p = w; p && p != d->widget; p = p->parentWidget()) {
            if (auto* lw = qobject_cast<LeafWidget*>(p)) {
                if (lw->leaf() && lw->leaf()->tabs())
                    d->q->setActiveLeaf(lw->leaf(), false);
                break;
            }
        }
        return false;
    }

private:
    WorkspaceImpl* d;
};

// Receives the Vault's signals. Connected with string-based connect() so this library doesn't need
// the Vault's moc output at link time (the Vault implementation lives in another module).
class VaultBridge : public QObject {
    Q_OBJECT
public:
    VaultBridge(WorkspaceImpl* d, QObject* parent)
        : QObject(parent)
        , d(d)
    {
    }

public slots:
    void renamed(oc::TAbstractFile* item, const QString& oldPath, bool external)
    {
        Q_UNUSED(external);
        d->onVaultRenamed(item, oldPath);
    }
    void deleted(oc::TAbstractFile* item, bool external)
    {
        Q_UNUSED(external);
        d->onVaultDeleted(item);
    }
    void created(oc::TAbstractFile* item, bool external)
    {
        Q_UNUSED(external);
        d->onVaultCreated(item);
    }

private:
    WorkspaceImpl* d;
};

// ---------------------------------------------------------------------------------------------
// Model items
// ---------------------------------------------------------------------------------------------

WorkspaceSplit* WorkspaceItem::asSplit() { return isSplit() ? static_cast<WorkspaceSplit*>(this) : nullptr; }
WorkspaceTabs* WorkspaceItem::asTabs() { return isTabs() ? static_cast<WorkspaceTabs*>(this) : nullptr; }

WorkspaceSplit::WorkspaceSplit(WorkspaceImpl* d, Role role, bool vertical)
    : WorkspaceItem(Kind::Split)
    , role(role)
    , vertical(vertical)
    , w(new SplitWidget(d, this))
{
}

WorkspaceSplit::~WorkspaceSplit()
{
    if (w)
        w->detachModel();
}

QWidget* WorkspaceSplit::widget() const { return w; }

WorkspaceTabs::WorkspaceTabs(Workspace::Impl* d)
    : WorkspaceItem(Kind::Tabs)
    , w(new TabGroupWidget(d, this))
{
}

WorkspaceTabs::~WorkspaceTabs()
{
    if (w)
        w->detachModel();
}

QWidget* WorkspaceTabs::widget() const { return w; }

// ---------------------------------------------------------------------------------------------
// WorkspaceLeaf
// ---------------------------------------------------------------------------------------------

WorkspaceLeaf::WorkspaceLeaf(Workspace* ws, QString id)
    : QObject(ws)
    , m_id(std::move(id))
    , m_workspace(ws)
{
}

WorkspaceLeaf::~WorkspaceLeaf() = default;

QJsonObject WorkspaceLeaf::viewState() const
{
    if (auto* ph = qobject_cast<PlaceholderView*>(m_view))
        return ph->originalViewState(m_pinned);
    QJsonObject o;
    o.insert(QStringLiteral("type"), m_view ? m_view->viewType() : kEmptyType);
    o.insert(QStringLiteral("state"), m_view ? m_view->state() : QJsonObject());
    if (m_pinned)
        o.insert(QStringLiteral("pinned"), true);
    if (m_view) {
        const QString icon = m_view->icon();
        if (!icon.isEmpty())
            o.insert(QStringLiteral("icon"), icon);
        const QString title = m_view->displayText().trimmed();
        if (!title.isEmpty())
            o.insert(QStringLiteral("title"), title);
    }
    return o;
}

void WorkspaceLeaf::setViewState(const QJsonObject& viewState, const QJsonObject& eState, bool recordHistory)
{
    m_workspace->d->setViewState(this, viewState, eState, recordHistory);
}

void WorkspaceLeaf::openFile(TFile* file, const QJsonObject& state, const QJsonObject& eState)
{
    if (!file)
        return;
    const QString ext = file->extension().toLower();
    ViewRegistry* reg = m_workspace->viewRegistry();
    QString type = reg ? reg->typeForExtension(ext) : QString();
    if (auto* fv = qobject_cast<FileView*>(m_view); fv && fv->canAcceptExtension(ext))
        type = fv->viewType();
    if (type.isEmpty())
        return; // Obsidian hands unknown extensions to the system; nothing to show here
    QJsonObject st = state;
    st.insert(QStringLiteral("file"), file->path());
    Workspace::Impl* d = m_workspace->d;
    TFile* const previous = d->openingFile;
    d->openingFile = file;
    setViewState(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("state"), st}}, eState);
    if (d->active == this)
        d->updateActiveFile(file);
    d->openingFile = previous;
}

QString WorkspaceLeaf::displayText() const { return m_view ? m_view->displayText() : QString(); }
QString WorkspaceLeaf::icon() const { return m_view ? m_view->icon() : QStringLiteral("file"); }

void WorkspaceLeaf::setPinned(bool pinned)
{
    if (m_pinned == pinned)
        return;
    m_pinned = pinned;
    emit pinnedChanged();
    m_workspace->d->leafTitleChanged(this);
}

bool WorkspaceLeaf::canGoBack() const { return !m_back.isEmpty(); }
bool WorkspaceLeaf::canGoForward() const { return !m_forward.isEmpty(); }
void WorkspaceLeaf::goBack() { m_workspace->d->goHistory(this, -1); }
void WorkspaceLeaf::goForward() { m_workspace->d->goHistory(this, 1); }

bool WorkspaceLeaf::isInSidedock() const
{
    WorkspaceSplit* top = m_workspace->d->topSplit(this);
    return top && top->isSidedock();
}

void WorkspaceLeaf::detach() { m_workspace->d->closeLeaf(this, true); }

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — helpers
// ---------------------------------------------------------------------------------------------

Workspace::Impl::Impl(Workspace* q)
    : q(q)
{
}

Workspace::Impl::~Impl() = default;

QString Workspace::Impl::genId()
{
    // Obsidian uses 16 random hex digits.
    const quint64 v = QRandomGenerator::global()->generate64();
    return QStringLiteral("%1").arg(v, 16, 16, QLatin1Char('0'));
}

bool Workspace::Impl::frameless() const { return window() && window()->isFrameless(); }

int Workspace::Impl::topRightReserve() const { return window() ? window()->topRightReserve() : 0; }

bool Workspace::Impl::windowFocused() const
{
    if (!widget)
        return false;
    QWidget* w = widget->window();
    return w && w->isActiveWindow();
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — views
// ---------------------------------------------------------------------------------------------

WorkspaceLeaf* Workspace::Impl::createLeaf(const QString& id)
{
    auto* leaf = new WorkspaceLeaf(q, id.isEmpty() ? genId() : id);
    LeafData& ld = leafData[leaf];
    ld.widget = new LeafWidget(this, leaf);
    setLeafView(leaf, createView(kEmptyType, leaf));
    return leaf;
}

View* Workspace::Impl::createView(const QString& type, WorkspaceLeaf* leaf)
{
    if (type.isEmpty() || type == kEmptyType)
        return new EmptyView(leaf);
    View* v = registry() ? registry()->create(type, leaf) : nullptr;
    if (!v)
        v = new PlaceholderView(leaf, type); // "Plugin no longer active"
    return v;
}

LeafWidget* Workspace::Impl::leafWidget(const WorkspaceLeaf* leaf) const
{
    auto it = leafData.constFind(leaf);
    return it == leafData.cend() ? nullptr : it->widget.data();
}

void Workspace::Impl::disposeView(View* view)
{
    if (!view)
        return;
    view->hide();
    view->deleteLater();
}

void Workspace::Impl::setLeafView(WorkspaceLeaf* leaf, View* view)
{
    View* old = leaf->m_view;
    if (old == view)
        return;
    if (old) {
        QObject::disconnect(old, nullptr, leaf, nullptr);
        old->onClose();
        disposeView(old);
    }
    leaf->m_view = view;
    if (view) {
        QObject::connect(view, &View::titleChanged, leaf, [this, leaf] { leafTitleChanged(leaf); });
        QObject::connect(view, &View::stateChanged, leaf, [this, leaf] {
            q->requestSaveLayout();
            if (active == leaf)
                updateActiveFile();
        });
        if (LeafWidget* lw = leafWidget(leaf))
            lw->setView(view);
        view->onOpen();
    }
    emit leaf->viewChanged();
}

void Workspace::Impl::setViewState(WorkspaceLeaf* leaf, const QJsonObject& viewState, const QJsonObject& eState, bool recordHistory)
{
    auto it = leafData.find(leaf);
    if (it == leafData.end() || it->working)
        return;
    it->working = true;

    QString type = viewState.value(QStringLiteral("type")).toString();
    if (type.isEmpty())
        type = kEmptyType;
    const QJsonObject state = viewState.value(QStringLiteral("state")).toObject();

    View* cur = leaf->m_view;
    const bool haveBefore = cur && cur->viewType() != kEmptyType;
    WorkspaceLeaf::HistoryEntry before;
    if (haveBefore)
        before = {leaf->viewState(), cur->ephemeralState()};
    const QString oldFile = fileViewPath(cur);
    const bool placeholderNowAvailable = qobject_cast<PlaceholderView*>(cur) && registry() && registry()->hasView(type);
    const bool typeChanged = !cur || cur->viewType() != type || placeholderNowAvailable;

    if (typeChanged)
        setLeafView(leaf, createView(type, leaf));
    View* v = leaf->m_view;
    if (auto* ph = qobject_cast<PlaceholderView*>(v))
        ph->setOriginalViewState(viewState);
    v->setState(state);
    // Obsidian closes a file view whose file doesn't exist (the leaf shows the empty state instead).
    if (auto* fv = qobject_cast<FileView*>(v); fv && !fv->file() && hasVault(q->m_session)) {
        setLeafView(leaf, createView(kEmptyType, leaf));
        v = leaf->m_view;
    }
    if (!eState.isEmpty())
        v->setEphemeralState(eState);

    const bool changed = typeChanged || oldFile != fileViewPath(v);
    bool historyChanged = false;
    if (recordHistory && haveBefore && changed && v->navigation() && v->viewType() != kEmptyType) {
        if (leaf->m_back.isEmpty() || !sameViewState(leaf->m_back.last().viewState, before.viewState)) {
            leaf->m_back.push_back(before);
            while (leaf->m_back.size() > kMaxHistory)
                leaf->m_back.removeFirst();
        }
        leaf->m_forward.clear();
        historyChanged = true;
    }
    leafData[leaf].working = false;

    if (viewState.value(QStringLiteral("active")).toBool())
        q->setActiveLeaf(leaf, true);
    if (historyChanged)
        emit leaf->historyChanged();
    leafTitleChanged(leaf);
    if (LeafWidget* lw = leafWidget(leaf))
        lw->refresh();
    if (active == leaf)
        updateActiveFile();
    if (leaf->m_view != cur && leaf->m_tabs)
        structureChanged(); // Obsidian: a new view is a layout change ("layout-change", saved)
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — structure
// ---------------------------------------------------------------------------------------------

void Workspace::Impl::releaseWidget(QWidget* w)
{
    // Move child pane widgets that still belong to live items out of a widget about to be deleted.
    if (!w || destroying)
        return;
    const QObjectList kids = w->children();
    for (QObject* o : kids) {
        if (qobject_cast<SplitWidget*>(o) || qobject_cast<TabGroupWidget*>(o) || qobject_cast<LeafWidget*>(o)) {
            auto* cw = static_cast<QWidget*>(o);
            cw->hide();
            cw->setParent(widget);
        }
    }
}

void Workspace::Impl::insertLeaf(WorkspaceTabs* tabs, int index, WorkspaceLeaf* leaf)
{
    index = std::clamp(index < 0 ? int(tabs->leaves.size()) : index, 0, int(tabs->leaves.size()));
    WorkspaceLeaf* cur = tabs->current();
    tabs->leaves.insert(index, leaf);
    leaf->m_tabs = tabs;
    if (cur)
        tabs->currentTab = int(tabs->leaves.indexOf(cur));
    else
        tabs->currentTab = index;
    if (LeafWidget* lw = leafWidget(leaf))
        lw->refresh();
    structureChanged();
}

void Workspace::Impl::takeLeaf(WorkspaceLeaf* leaf)
{
    WorkspaceTabs* tabs = leaf->m_tabs;
    if (!tabs)
        return;
    const int idx = int(tabs->leaves.indexOf(leaf));
    WorkspaceLeaf* cur = tabs->current();
    tabs->leaves.removeAt(idx);
    leaf->m_tabs = nullptr;
    if (tabs->leaves.isEmpty()) {
        removeItem(tabs);
    } else if (cur == leaf) {
        tabs->currentTab = std::min(idx, int(tabs->leaves.size()) - 1);
    } else {
        tabs->currentTab = int(tabs->leaves.indexOf(cur));
    }
    structureChanged();
}

void Workspace::Impl::removeItem(WorkspaceItem* item)
{
    WorkspaceSplit* parent = item->parent;
    if (parent) {
        parent->children.removeOne(item);
        item->parent = nullptr;
    }
    if (QWidget* w = item->widget()) {
        releaseWidget(w);
        w->hide();
        w->deleteLater();
    }
    delete item;
    if (parent && parent->role == WorkspaceSplit::Role::Nested) {
        if (parent->children.size() == 1) {
            // A split with a single child is replaced by that child (Obsidian's removeChild()).
            WorkspaceItem* child = parent->children.takeFirst();
            WorkspaceSplit* gp = parent->parent;
            const qsizetype pi = gp->children.indexOf(parent);
            child->dimension = parent->dimension;
            child->parent = gp;
            gp->children[pi] = child;
            parent->parent = nullptr;
            releaseWidget(parent->w);
            parent->w->hide();
            parent->w->deleteLater();
            delete parent;
        } else if (parent->children.isEmpty()) {
            removeItem(parent);
        }
    }
    structureChanged();
}

void Workspace::Impl::insertItem(WorkspaceSplit* parent, int index, WorkspaceItem* item)
{
    index = std::clamp(index < 0 ? int(parent->children.size()) : index, 0, int(parent->children.size()));
    parent->children.insert(index, item);
    item->parent = parent;
    structureChanged();
}

void Workspace::Impl::splitItem(WorkspaceItem* target, WorkspaceItem* item, bool vertical, bool before)
{
    WorkspaceSplit* r = target->parent;
    if (!r) {
        // target is a top-level split (root / sidedock): split inside it.
        WorkspaceSplit* s = target->asSplit();
        if (!s)
            return;
        if (s->vertical == vertical || s->children.size() <= 1) {
            if (s->role == WorkspaceSplit::Role::Nested || s->children.size() <= 1)
                s->vertical = s->isSidedock() ? false : vertical;
            insertItem(s, before ? 0 : int(s->children.size()), item);
            return;
        }
        // Wrap the current children in a nested split so the new item can sit next to them.
        auto* inner = new WorkspaceSplit(this, WorkspaceSplit::Role::Nested, s->vertical);
        inner->id = genId();
        inner->children = s->children;
        for (WorkspaceItem* c : std::as_const(inner->children))
            c->parent = inner;
        inner->parent = s;
        s->children = {inner};
        s->vertical = vertical;
        insertItem(s, before ? 0 : 1, item);
        return;
    }
    const qsizetype o = r->children.indexOf(target);
    const double dim = target->dimension;
    if (r->vertical != vertical) {
        target->dimension = 0;
        auto* ns = new WorkspaceSplit(this, WorkspaceSplit::Role::Nested, vertical);
        ns->id = genId();
        ns->dimension = dim;
        r->children[o] = ns;
        ns->parent = r;
        ns->children = before ? QList<WorkspaceItem*>{item, target} : QList<WorkspaceItem*>{target, item};
        target->parent = ns;
        item->parent = ns;
    } else {
        if (dim > 0) {
            target->dimension = dim / 2;
            item->dimension = dim / 2;
        }
        r->children.insert(before ? o : o + 1, item);
        item->parent = r;
    }
    structureChanged();
}

void Workspace::Impl::ensureRootLeaf()
{
    if (!root || destroying)
        return;
    bool any = false;
    forEachLeaf(root, [&](WorkspaceLeaf*) { any = true; });
    if (any)
        return;
    beginBatch();
    while (!root->children.isEmpty()) {
        WorkspaceItem* c = root->children.takeLast();
        c->parent = nullptr;
        deleteTree(c);
    }
    auto* t = new WorkspaceTabs(this);
    t->id = genId();
    insertItem(root, 0, t);
    insertLeaf(t, 0, createLeaf());
    endBatch();
}

void Workspace::Impl::closeLeaf(WorkspaceLeaf* leaf, bool recordUndo)
{
    if (!leaf || !leaf->m_tabs || destroying)
        return;
    beginBatch();
    WorkspaceTabs* tabs = leaf->m_tabs;
    const bool wasActive = active == leaf;
    const bool groupSurvives = tabs->leaves.size() > 1;
    if (recordUndo && leaf->m_view && leaf->m_view->viewType() != kEmptyType) {
        ClosedLeaf c;
        c.leafId = leaf->m_id;
        c.parentId = tabs->id;
        if (WorkspaceSplit* top = topSplit(tabs))
            c.rootId = top->id;
        c.index = int(tabs->leaves.indexOf(leaf));
        c.viewState = leaf->viewState();
        c.eState = leaf->m_view->ephemeralState();
        c.back = leaf->m_back;
        c.forward = leaf->m_forward;
        closed.prepend(c);
        while (closed.size() > kMaxClosed)
            closed.removeLast();
    }
    takeLeaf(leaf);
    View* v = leaf->m_view;
    leaf->m_view = nullptr;
    if (v) {
        QObject::disconnect(v, nullptr, leaf, nullptr);
        v->onClose();
        v->hide();
    }
    if (LeafWidget* lw = leafWidget(leaf)) {
        lw->hide();
        lw->deleteLater(); // also deletes the view (its child)
    } else if (v) {
        v->deleteLater();
    }
    leafData.remove(leaf);
    if (active == leaf)
        active = nullptr;
    leaf->deleteLater();
    ensureRootLeaf();
    if (wasActive) {
        WorkspaceLeaf* next = groupSurvives ? tabs->current() : nullptr;
        if (!next)
            next = mostRecentLeaf(root);
        if (next)
            q->setActiveLeaf(next, true);
    }
    endBatch();
    updateActiveFile();
}

void Workspace::Impl::deleteTree(WorkspaceItem* item)
{
    if (!item)
        return;
    if (WorkspaceSplit* s = item->asSplit()) {
        for (WorkspaceItem* c : std::as_const(s->children))
            deleteTree(c);
        s->children.clear();
    } else if (WorkspaceTabs* t = item->asTabs()) {
        for (WorkspaceLeaf* leaf : std::as_const(t->leaves)) {
            if (View* v = leaf->m_view) {
                QObject::disconnect(v, nullptr, leaf, nullptr);
                if (!destroying)
                    v->onClose();
            }
            leaf->m_view = nullptr;
            leaf->m_tabs = nullptr;
            if (LeafWidget* lw = leafWidget(leaf))
                delete lw;
            leafData.remove(leaf);
            if (active == leaf)
                active = nullptr;
            delete leaf;
        }
        t->leaves.clear();
    }
    delete item->widget();
    delete item;
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — queries
// ---------------------------------------------------------------------------------------------

WorkspaceSplit* Workspace::Impl::topSplit(const WorkspaceItem* item) const
{
    const WorkspaceItem* it = item;
    while (it && it->parent)
        it = it->parent;
    return it && it->isSplit() ? const_cast<WorkspaceItem*>(it)->asSplit() : nullptr;
}

WorkspaceSplit* Workspace::Impl::topSplit(const WorkspaceLeaf* leaf) const { return leaf && leaf->m_tabs ? topSplit(leaf->m_tabs) : nullptr; }

bool Workspace::Impl::isRootLeaf(const WorkspaceLeaf* leaf) const { return root && topSplit(leaf) == root; }

void Workspace::Impl::forEachLeaf(const WorkspaceItem* item, const std::function<void(WorkspaceLeaf*)>& fn) const
{
    if (!item)
        return;
    if (item->isSplit()) {
        const auto* s = static_cast<const WorkspaceSplit*>(item);
        for (const WorkspaceItem* c : s->children)
            forEachLeaf(c, fn);
    } else {
        const auto* t = static_cast<const WorkspaceTabs*>(item);
        const QList<WorkspaceLeaf*> leaves = t->leaves; // fn may modify the group
        for (WorkspaceLeaf* l : leaves)
            fn(l);
    }
}

void Workspace::Impl::forEachTabs(const WorkspaceItem* item, const std::function<void(WorkspaceTabs*)>& fn) const
{
    if (!item)
        return;
    if (item->isSplit()) {
        const auto* s = static_cast<const WorkspaceSplit*>(item);
        const QList<WorkspaceItem*> children = s->children;
        for (const WorkspaceItem* c : children)
            forEachTabs(c, fn);
    } else {
        fn(const_cast<WorkspaceItem*>(item)->asTabs());
    }
}

QList<WorkspaceLeaf*> Workspace::Impl::leavesUnder(const WorkspaceItem* item) const
{
    QList<WorkspaceLeaf*> out;
    forEachLeaf(item, [&](WorkspaceLeaf* l) { out.push_back(l); });
    return out;
}

WorkspaceLeaf* Workspace::Impl::mostRecentLeaf(const WorkspaceItem* under) const
{
    // Obsidian's getMostRecentLeaf(): the most recently active visible leaf, else the first one.
    WorkspaceLeaf* first = nullptr;
    WorkspaceLeaf* best = nullptr;
    quint64 bestTime = 0;
    forEachTabs(under, [&](WorkspaceTabs* t) {
        if (!first)
            first = t->leaves.value(0);
        WorkspaceLeaf* c = t->current();
        const quint64 time = leafData.value(c).activeTime;
        if (c && (!best || time > bestTime)) {
            best = c;
            bestTime = time;
        }
    });
    return best ? best : first;
}

WorkspaceLeaf* Workspace::Impl::mostRecentInGroup(const WorkspaceTabs* tabs) const
{
    if (!tabs || tabs->leaves.isEmpty())
        return nullptr;
    WorkspaceLeaf* best = tabs->leaves.last();
    for (WorkspaceLeaf* l : tabs->leaves)
        if (leafData.value(l).activeTime > leafData.value(best).activeTime)
            best = l;
    return best;
}

bool Workspace::Impl::canNavigate(const WorkspaceLeaf* leaf) const
{
    return leaf && !leaf->m_pinned && leaf->m_view && leaf->m_view->navigation();
}

WorkspaceLeaf* Workspace::Impl::leafById(const QString& id) const
{
    if (id.isEmpty())
        return nullptr;
    WorkspaceLeaf* found = nullptr;
    for (const WorkspaceSplit* s : {root, left, right})
        forEachLeaf(s, [&](WorkspaceLeaf* l) {
            if (!found && l->m_id == id)
                found = l;
        });
    return found;
}

WorkspaceTabs* Workspace::Impl::tabsById(const WorkspaceItem* under, const QString& id) const
{
    WorkspaceTabs* found = nullptr;
    forEachTabs(under, [&](WorkspaceTabs* t) {
        if (!found && t->id == id)
            found = t;
    });
    return found;
}

bool Workspace::Impl::isGroupActive(const WorkspaceTabs* tabs) const { return active && active->m_tabs == tabs; }

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — operations
// ---------------------------------------------------------------------------------------------

void Workspace::Impl::selectTab(WorkspaceTabs* tabs, int index)
{
    if (!tabs || index < 0 || index >= tabs->leaves.size() || tabs->currentTab == index)
        return;
    tabs->currentTab = index;
    tabs->w->sync();
    refreshChrome();
    q->requestSaveLayout();
    emit q->layoutChanged();
}

void Workspace::Impl::moveLeaf(WorkspaceLeaf* leaf, WorkspaceTabs* target, int index)
{
    if (!leaf || !target || !leaf->m_tabs)
        return;
    beginBatch();
    if (leaf->m_tabs == target) {
        const int from = int(target->leaves.indexOf(leaf));
        int to = std::clamp(index < 0 ? int(target->leaves.size()) : index, 0, int(target->leaves.size()));
        if (to > from)
            --to;
        if (to != from) {
            target->leaves.move(from, to);
            target->currentTab = to;
            structureChanged();
        }
    } else {
        takeLeaf(leaf); // may remove the source group (never the target: it has other leaves)
        insertLeaf(target, index, leaf);
        target->currentTab = int(target->leaves.indexOf(leaf));
        structureChanged();
    }
    endBatch();
}

bool Workspace::Impl::splitLeafTo(WorkspaceLeaf* leaf, WorkspaceTabs* target, DropEdge edge)
{
    if (!leaf || !target || !leaf->m_tabs || edge == DropEdge::Center)
        return false;
    if (leaf->m_tabs == target && target->leaves.size() == 1)
        return false; // splitting a group with its only leaf is a no-op
    beginBatch();
    takeLeaf(leaf);
    auto* t = new WorkspaceTabs(this);
    t->id = genId();
    t->leaves.push_back(leaf);
    leaf->m_tabs = t;
    const bool vertical = edge == DropEdge::Left || edge == DropEdge::Right;
    const bool before = edge == DropEdge::Left || edge == DropEdge::Top;
    splitItem(target, t, vertical, before);
    if (LeafWidget* lw = leafWidget(leaf))
        lw->refresh();
    endBatch();
    return true;
}

void Workspace::Impl::moveLeafToSide(WorkspaceLeaf* leaf, WorkspaceSplit* side)
{
    if (!leaf || !side || !leaf->m_tabs)
        return;
    beginBatch();
    WorkspaceTabs* target = side->children.isEmpty() ? nullptr : side->children.first()->asTabs();
    if (target && target == leaf->m_tabs) {
        endBatch();
        return;
    }
    takeLeaf(leaf);
    if (!target) {
        target = new WorkspaceTabs(this);
        target->id = genId();
        insertItem(side, int(side->children.size()), target);
    }
    insertLeaf(target, -1, leaf);
    target->currentTab = int(target->leaves.indexOf(leaf));
    endBatch();
}

WorkspaceLeaf* Workspace::Impl::createLeafInGroup(WorkspaceTabs* tabs)
{
    if (!tabs)
        return nullptr;
    WorkspaceLeaf* recent = mostRecentInGroup(tabs);
    if (recent && recent->m_view && recent->m_view->viewType() == kEmptyType)
        return recent; // reuse an empty tab
    WorkspaceLeaf* leaf = createLeaf();
    insertLeaf(tabs, recent ? int(tabs->leaves.indexOf(recent)) + 1 : -1, leaf);
    return leaf;
}

WorkspaceLeaf* Workspace::Impl::appendNewTab(WorkspaceTabs* tabs)
{
    if (!tabs)
        return nullptr;
    WorkspaceLeaf* leaf = createLeaf();
    beginBatch();
    insertLeaf(tabs, -1, leaf);
    tabs->currentTab = int(tabs->leaves.size()) - 1;
    endBatch();
    q->setActiveLeaf(leaf, true);
    return leaf;
}

WorkspaceLeaf* Workspace::Impl::createLeafBySplit(WorkspaceLeaf* leaf, bool vertical, bool before)
{
    if (!leaf || !leaf->m_tabs)
        leaf = mostRecentLeaf(root);
    if (!leaf || !leaf->m_tabs)
        return nullptr;
    beginBatch();
    WorkspaceLeaf* nl = createLeaf();
    auto* t = new WorkspaceTabs(this);
    t->id = genId();
    t->leaves.push_back(nl);
    nl->m_tabs = t;
    splitItem(leaf->m_tabs, t, vertical, before);
    endBatch();
    q->setActiveLeaf(nl, false);
    return nl;
}

void Workspace::Impl::onFileViewDeleted(FileView* view)
{
    // Obsidian's FileView.onDelete(): go back if possible, else show the empty state; an empty tab
    // with siblings is then closed.
    WorkspaceLeaf* leaf = view->leaf();
    if (!leaf || !leaf->m_tabs)
        return;
    QPointer<WorkspaceLeaf> guard(leaf);
    if (leaf->canGoBack())
        goHistory(leaf, -1);
    else
        setViewState(leaf, QJsonObject{{QStringLiteral("type"), kEmptyType}}, {}, false);
    if (guard && leaf->m_tabs && leaf->m_view && leaf->m_view->viewType() == kEmptyType && leaf->m_tabs->leaves.size() > 1)
        closeLeaf(leaf, false);
}

void Workspace::Impl::closeOthers(WorkspaceLeaf* leaf)
{
    if (!leaf || !leaf->m_tabs)
        return;
    beginBatch();
    const QList<WorkspaceLeaf*> others = leaf->m_tabs->leaves;
    for (WorkspaceLeaf* l : others)
        if (l != leaf && !l->m_pinned)
            closeLeaf(l, true);
    endBatch();
}

void Workspace::Impl::closeAfter(WorkspaceLeaf* leaf)
{
    if (!leaf || !leaf->m_tabs)
        return;
    beginBatch();
    const QList<WorkspaceLeaf*> all = leaf->m_tabs->leaves;
    for (qsizetype i = all.indexOf(leaf) + 1; i < all.size(); ++i)
        if (!all[i]->m_pinned)
            closeLeaf(all[i], true);
    endBatch();
}

void Workspace::Impl::closeAll(WorkspaceTabs* tabs)
{
    if (!tabs)
        return;
    beginBatch();
    const QList<WorkspaceLeaf*> all = tabs->leaves;
    for (WorkspaceLeaf* l : all)
        if (!l->m_pinned)
            closeLeaf(l, true);
    endBatch();
}

void Workspace::Impl::goHistory(WorkspaceLeaf* leaf, int delta)
{
    if (!leaf || delta == 0 || leafData.value(leaf).working)
        return;
    WorkspaceLeaf::HistoryEntry target{leaf->viewState(), leaf->m_view ? leaf->m_view->ephemeralState() : QJsonObject()};
    bool moved = false;
    while (delta > 0 && !leaf->m_forward.isEmpty()) {
        leaf->m_back.push_back(target);
        target = leaf->m_forward.takeLast();
        --delta;
        moved = true;
    }
    while (delta < 0 && !leaf->m_back.isEmpty()) {
        leaf->m_forward.push_back(target);
        target = leaf->m_back.takeLast();
        ++delta;
        moved = true;
    }
    if (!moved)
        return;
    setViewState(leaf, target.viewState, target.eState, false);
    emit leaf->historyChanged();
}

QList<QPair<QString, QString>> Workspace::Impl::historyItems(const WorkspaceLeaf* leaf, bool back) const
{
    QList<QPair<QString, QString>> out;
    if (!leaf)
        return out;
    const QList<WorkspaceLeaf::HistoryEntry>& list = back ? leaf->m_back : leaf->m_forward;
    for (qsizetype i = list.size() - 1; i >= 0; --i) {
        const QJsonObject& vs = list[i].viewState;
        QString title = vs.value(QStringLiteral("title")).toString();
        if (title.isEmpty())
            title = vs.value(QStringLiteral("type")).toString();
        out.push_back({title, vs.value(QStringLiteral("icon")).toString()});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — notifications
// ---------------------------------------------------------------------------------------------

void Workspace::Impl::endBatch()
{
    if (--batchDepth == 0 && batchDirty) {
        batchDirty = false;
        structureChanged();
    }
}

void Workspace::Impl::structureChanged()
{
    if (destroying)
        return;
    if (batchDepth > 0) {
        batchDirty = true;
        return;
    }
    // Obsidian's updateLayout(): whatever emptied the root (closing, dragging the last tab into a
    // sidedock), it gets a tab group with an empty leaf again.
    if (root) {
        bool any = false;
        forEachLeaf(root, [&](WorkspaceLeaf*) { any = true; });
        if (!any) {
            ensureRootLeaf(); // re-enters structureChanged() when its batch ends
            return;
        }
    }
    normalizeDimensions();
    syncWidgets();
    updateTopFlags();
    refreshChrome();
    q->requestSaveLayout();
    emit q->layoutChanged();
}

void Workspace::Impl::normalizeDimensions()
{
    std::function<void(WorkspaceSplit*)> visit = [&](WorkspaceSplit* s) {
        double sum = 0;
        bool all = !s->children.isEmpty();
        for (const WorkspaceItem* c : std::as_const(s->children)) {
            all = all && c->dimension > 0;
            sum += c->dimension;
        }
        // Rescale only when needed so stable layouts serialize unchanged (no floating point drift).
        const bool rescale = all && std::abs(sum - 100) > 1e-6;
        for (WorkspaceItem* c : std::as_const(s->children)) {
            c->dimension = all ? validDimension(rescale ? c->dimension * 100 / sum : c->dimension) : 0;
            if (WorkspaceSplit* cs = c->asSplit())
                visit(cs);
        }
    };
    for (WorkspaceSplit* s : {root, left, right})
        if (s)
            visit(s);
}

void Workspace::Impl::syncWidgets()
{
    if (!widget || destroying)
        return;
    std::function<void(WorkspaceItem*)> sync = [&](WorkspaceItem* item) {
        if (WorkspaceSplit* s = item->asSplit()) {
            s->w->sync();
            for (WorkspaceItem* c : std::as_const(s->children))
                sync(c);
        } else if (WorkspaceTabs* t = item->asTabs()) {
            t->w->sync();
        }
    };
    for (WorkspaceSplit* s : {root, left, right}) {
        if (!s)
            continue;
        if (s->w->parentWidget() != widget)
            s->w->setParent(widget);
        sync(s);
    }
    widget->relayout();
}

void Workspace::Impl::updateTopFlags()
{
    if (!root || destroying)
        return;
    // Obsidian's updateFrameless(): tab groups touching the top edge ("mod-top") use the title bar
    // colors and double as window drag areas; the rightmost one leaves room for the window buttons.
    QList<WorkspaceTabs*> tops;
    std::function<void(WorkspaceItem*)> collect = [&](WorkspaceItem* it) {
        if (WorkspaceTabs* t = it->asTabs()) {
            tops.push_back(t);
        } else if (WorkspaceSplit* s = it->asSplit()) {
            if (s->vertical) {
                for (WorkspaceItem* c : std::as_const(s->children))
                    collect(c);
            } else if (!s->children.isEmpty()) {
                collect(s->children.first());
            }
        }
    };
    for (WorkspaceSplit* s : {root, left, right})
        forEachTabs(s, [](WorkspaceTabs* t) {
            t->w->header()->setTop(false);
            t->w->header()->setRightReserve(0);
        });
    collect(root);
    for (WorkspaceTabs* t : std::as_const(tops))
        t->w->header()->setTop(true);
    WorkspaceTabs* leftTop = left && !left->children.isEmpty() ? left->children.first()->asTabs() : nullptr;
    WorkspaceTabs* rightTop = right && !right->children.isEmpty() ? right->children.first()->asTabs() : nullptr;
    if (leftTop)
        leftTop->w->header()->setTop(true);
    if (rightTop)
        rightTop->w->header()->setTop(true);
    WorkspaceTabs* topRight = rightTop && !right->collapsed ? rightTop : (tops.isEmpty() ? nullptr : tops.last());
    if (topRight)
        topRight->w->header()->setRightReserve(topRightReserve());
}

void Workspace::Impl::refreshChrome()
{
    if (!widget || destroying)
        return;
    for (WorkspaceSplit* s : {root, left, right})
        forEachTabs(s, [&](WorkspaceTabs* t) {
            t->w->header()->update();
            for (WorkspaceLeaf* l : std::as_const(t->leaves))
                if (LeafWidget* lw = leafWidget(l); lw && lw->header())
                    lw->header()->refresh();
        });
}

void Workspace::Impl::leafTitleChanged(WorkspaceLeaf* leaf)
{
    if (destroying)
        return;
    if (leaf->m_tabs) {
        leaf->m_tabs->w->header()->relayout();
        leaf->m_tabs->w->header()->update();
    }
    if (LeafWidget* lw = leafWidget(leaf); lw && lw->header())
        lw->header()->refresh();
    emit leaf->titleChanged();
    q->requestSaveLayout();
}

void Workspace::Impl::updateActiveFile(TFile* hint)
{
    if (destroying)
        return;
    // Obsidian's getActiveFile(): the active leaf's file when it's a navigable view, else the file of
    // the most recently active navigable leaf.
    WorkspaceLeaf* l = active;
    if (!l || !l->m_view || !l->m_view->navigation()) {
        l = nullptr;
        quint64 best = 0;
        for (const WorkspaceSplit* s : {root, left, right})
            forEachLeaf(s, [&](WorkspaceLeaf* c) {
                const quint64 t = leafData.value(c).activeTime;
                if (c->m_view && c->m_view->navigation() && t > 0 && (!l || t > best)) {
                    l = c;
                    best = t;
                }
            });
    }
    auto* fv = l ? qobject_cast<FileView*>(l->m_view) : nullptr;
    const QString path = fv ? fv->state().value(QStringLiteral("file")).toString() : QString();
    if (path == activeFilePath)
        return;
    activeFilePath = path;
    TFile* f = fv ? fv->file() : nullptr;
    if (!hint)
        hint = openingFile;
    if (!f && hint && hint->path() == path)
        f = hint;
    if (!path.isEmpty())
        recordRecent(path);
    emit q->fileOpened(f);
}

void Workspace::Impl::recordRecent(const QString& path)
{
    if (path.isEmpty() || (!lastOpenFiles.isEmpty() && lastOpenFiles.first() == path))
        return;
    QHash<QString, int> caps{{QStringLiteral("md"), 25}, {QStringLiteral("canvas"), 10}, {QStringLiteral("image"), 10}, {QStringLiteral("other"), 10}};
    QStringList out{path};
    for (const QString& p : std::as_const(lastOpenFiles)) {
        if (p == path)
            continue;
        int& cap = caps[recentCategory(p)];
        if (cap <= 0)
            continue;
        out.push_back(p);
        --cap;
    }
    while (out.size() > kMaxRecent)
        out.removeLast();
    lastOpenFiles = out;
    q->requestSaveLayout();
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — persistence
// ---------------------------------------------------------------------------------------------

QJsonObject Workspace::Impl::serializeLeaf(const WorkspaceLeaf* leaf) const
{
    QJsonObject o = leafData.value(leaf).extra;
    o.insert(QStringLiteral("id"), leaf->m_id);
    o.insert(QStringLiteral("type"), QStringLiteral("leaf"));
    if (leaf->m_pinned)
        o.insert(QStringLiteral("pinned"), true);
    o.insert(QStringLiteral("state"), leaf->viewState());
    return o;
}

QJsonObject Workspace::Impl::serializeItem(const WorkspaceItem* item) const
{
    QJsonObject o = item->extra;
    o.insert(QStringLiteral("id"), item->id);
    if (item->dimension > 0)
        o.insert(QStringLiteral("dimension"), item->dimension);
    QJsonArray children;
    if (item->isSplit()) {
        const auto* s = static_cast<const WorkspaceSplit*>(item);
        o.insert(QStringLiteral("type"), QStringLiteral("split"));
        for (const WorkspaceItem* c : s->children)
            children.append(serializeItem(c));
        o.insert(QStringLiteral("children"), children);
        o.insert(QStringLiteral("direction"), s->vertical ? QStringLiteral("vertical") : QStringLiteral("horizontal"));
        if (s->isSidedock()) {
            o.insert(QStringLiteral("width"), s->width);
            if (s->collapsed)
                o.insert(QStringLiteral("collapsed"), true);
        }
    } else {
        const auto* t = static_cast<const WorkspaceTabs*>(item);
        o.insert(QStringLiteral("type"), QStringLiteral("tabs"));
        for (const WorkspaceLeaf* l : t->leaves)
            children.append(serializeLeaf(l));
        o.insert(QStringLiteral("children"), children);
        if (t->currentTab > 0)
            o.insert(QStringLiteral("currentTab"), t->currentTab);
        if (t->stacked)
            o.insert(QStringLiteral("stacked"), true);
    }
    return o;
}

WorkspaceItem* Workspace::Impl::parseItem(const QJsonObject& o, WorkspaceSplit::Role role, QList<QPair<WorkspaceLeaf*, QJsonObject>>& leaves)
{
    const QString type = o.value(QStringLiteral("type")).toString();
    auto idOf = [&](const QJsonObject& n) {
        const QString id = n.value(QStringLiteral("id")).toString();
        return id.isEmpty() ? genId() : id;
    };
    auto parseLeaf = [&](const QJsonObject& n, WorkspaceTabs* into) {
        WorkspaceLeaf* leaf = new WorkspaceLeaf(q, idOf(n));
        LeafData& ld = leafData[leaf];
        ld.widget = new LeafWidget(this, leaf);
        ld.extra = without(n, {"id", "type", "state", "pinned", "dimension"});
        leaf->m_pinned = n.value(QStringLiteral("pinned")).toBool();
        leaf->m_tabs = into;
        into->leaves.push_back(leaf);
        leaves.push_back({leaf, n.value(QStringLiteral("state")).toObject()});
    };
    if (type == u"split") {
        const bool vertical = o.value(QStringLiteral("direction")).toString() != u"horizontal";
        auto* s = new WorkspaceSplit(this, role, vertical);
        s->id = idOf(o);
        s->dimension = validDimension(o.value(QStringLiteral("dimension")).toDouble(0));
        s->extra = without(o, {"id", "type", "dimension", "children", "direction", "width", "collapsed"});
        if (s->isSidedock()) {
            s->width = std::max(1, int(std::lround(o.value(QStringLiteral("width")).toDouble(kDefaultSideWidth))));
            s->collapsed = o.value(QStringLiteral("collapsed")).toBool();
        }
        const QJsonArray children = o.value(QStringLiteral("children")).toArray();
        for (const QJsonValue& cv : children) {
            if (WorkspaceItem* c = parseItem(cv.toObject(), WorkspaceSplit::Role::Nested, leaves)) {
                c->parent = s;
                s->children.push_back(c);
            }
        }
        if (role == WorkspaceSplit::Role::Nested && s->children.isEmpty()) {
            delete s->w.data();
            delete s;
            return nullptr;
        }
        return s;
    }
    if (type == u"tabs") {
        auto* t = new WorkspaceTabs(this);
        t->id = idOf(o);
        t->dimension = validDimension(o.value(QStringLiteral("dimension")).toDouble(0));
        t->extra = without(o, {"id", "type", "dimension", "children", "currentTab", "stacked"});
        t->stacked = o.value(QStringLiteral("stacked")).toBool();
        const QJsonArray children = o.value(QStringLiteral("children")).toArray();
        for (const QJsonValue& cv : children) {
            const QJsonObject n = cv.toObject();
            if (n.value(QStringLiteral("type")).toString() == u"leaf")
                parseLeaf(n, t);
        }
        if (t->leaves.isEmpty()) {
            delete t->w.data();
            delete t;
            return nullptr;
        }
        t->currentTab = std::clamp(o.value(QStringLiteral("currentTab")).toInt(0), 0, int(t->leaves.size()) - 1);
        return t;
    }
    if (type == u"leaf") {
        // Pre-tabs layouts put leaves directly into splits: wrap them in a tab group.
        auto* t = new WorkspaceTabs(this);
        t->id = genId();
        t->dimension = validDimension(o.value(QStringLiteral("dimension")).toDouble(0));
        parseLeaf(o, t);
        return t;
    }
    return nullptr;
}

void Workspace::Impl::clearLayout()
{
    for (WorkspaceSplit** s : {&root, &left, &right}) {
        if (*s) {
            deleteTree(*s);
            *s = nullptr;
        }
    }
    active = nullptr;
    activeFilePath.clear();
    closed.clear();
}

void Workspace::Impl::buildEmptyLayout()
{
    beginBatch();
    root = new WorkspaceSplit(this, WorkspaceSplit::Role::Root, true);
    root->id = genId();
    left = new WorkspaceSplit(this, WorkspaceSplit::Role::Left, false);
    left->id = genId();
    right = new WorkspaceSplit(this, WorkspaceSplit::Role::Right, false);
    right->id = genId();
    for (WorkspaceSplit* s : {root, left, right})
        s->w->setParent(widget);
    ensureRootLeaf();
    endBatch();
}

void Workspace::Impl::buildDefaultLayout()
{
    beginBatch();
    clearLayout();
    buildEmptyLayout();
    right->collapsed = true; // Obsidian starts new vaults with the right sidebar collapsed
    auto addSide = [&](WorkspaceSplit* side, const QStringList& types) {
        WorkspaceTabs* t = nullptr;
        for (const QString& type : types) {
            if (!registry() || !registry()->hasView(type))
                continue; // the "plugin" providing this view isn't available
            if (!t) {
                t = new WorkspaceTabs(this);
                t->id = genId();
                insertItem(side, int(side->children.size()), t);
            }
            WorkspaceLeaf* leaf = createLeaf();
            insertLeaf(t, -1, leaf);
            setViewState(leaf, QJsonObject{{QStringLiteral("type"), type}}, {}, false);
        }
        if (t)
            t->currentTab = 0;
    };
    addSide(left, {QStringLiteral("file-explorer"), QStringLiteral("search"), QStringLiteral("bookmarks")});
    addSide(right, {QStringLiteral("backlink"), QStringLiteral("outgoing-link"), QStringLiteral("tag"), QStringLiteral("outline"),
                    QStringLiteral("all-properties")});
    WorkspaceLeaf* main = mostRecentLeaf(root);
    // Like Obsidian, a fresh layout reopens the most recent file.
    if (main && !lastOpenFiles.isEmpty()) {
        if (TFile* f = vaultFile(q->m_session, lastOpenFiles.first()))
            main->openFile(f);
    }
    endBatch();
    if (main)
        q->setActiveLeaf(main, false);
}

QJsonObject Workspace::Impl::ribbonHiddenItems() const
{
    QJsonObject o = ribbonHidden;
    Ribbon* r = window() ? window()->ribbon() : nullptr;
    if (!r)
        return o;
    for (const auto& [id, button] : r->actions()) {
        if (id.startsWith(u"app:"))
            continue; // help / settings aren't plugin ribbon items
        o.insert(ribbonKey(id, button->toolTip()), explicitlyHidden(button));
    }
    return o;
}

void Workspace::Impl::applyRibbonHidden()
{
    Ribbon* r = window() ? window()->ribbon() : nullptr;
    if (!r)
        return;
    for (const auto& [id, button] : r->actions()) {
        const QString key = ribbonKey(id, button->toolTip());
        if (ribbonHidden.contains(key))
            r->setActionHidden(id, ribbonHidden.value(key).toBool());
    }
}

QString Workspace::Impl::layoutPath() const
{
    return q->m_session ? ClonePaths::vaultConfigDir(q->m_session->id()) + QStringLiteral("/workspace.json") : QString();
}

void Workspace::Impl::saveNow()
{
    saveTimer.stop();
    if (!layoutReady || !q->m_session)
        return;
    QString error;
    if (!JsonFile::write(layoutPath(), q->serialize(), &error))
        qWarning("oc: failed to save workspace layout: %s", qPrintable(error));
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — vault events
// ---------------------------------------------------------------------------------------------

void Workspace::Impl::connectVault()
{
    QObject* vo = vaultObject(q->m_session);
    if (!vo || connectedVault == vo)
        return;
    if (connectedVault)
        QObject::disconnect(connectedVault, nullptr, vaultBridge, nullptr);
    if (!vaultBridge)
        vaultBridge = new VaultBridge(this, q);
    QObject::connect(vo, SIGNAL(renamed(oc::TAbstractFile*, QString, bool)), vaultBridge, SLOT(renamed(oc::TAbstractFile*, QString, bool)));
    QObject::connect(vo, SIGNAL(deleted(oc::TAbstractFile*, bool)), vaultBridge, SLOT(deleted(oc::TAbstractFile*, bool)));
    QObject::connect(vo, SIGNAL(created(oc::TAbstractFile*, bool)), vaultBridge, SLOT(created(oc::TAbstractFile*, bool)));
    connectedVault = vo;
}

void Workspace::Impl::onVaultRenamed(TAbstractFile* item, const QString& oldPath)
{
    if (!item || destroying)
        return;
    const QString newPath = item->path();
    if (!item->isFolder()) {
        for (QString& p : lastOpenFiles)
            if (p == oldPath)
                p = newPath;
        if (activeFilePath == oldPath)
            activeFilePath = newPath;
        auto* file = static_cast<TFile*>(item);
        QList<QPointer<FileView>> views;
        for (const WorkspaceSplit* s : {root, left, right})
            forEachLeaf(s, [&](WorkspaceLeaf* l) {
                if (auto* fv = qobject_cast<FileView*>(l->m_view); fv && fv->file() == file)
                    views.push_back(fv);
            });
        for (const QPointer<FileView>& fv : std::as_const(views))
            if (fv)
                fv->onRename(file, oldPath);
    } else {
        const QString prefix = oldPath + QLatin1Char('/');
        auto remap = [&](QString& p) {
            if (p.startsWith(prefix))
                p = newPath + QLatin1Char('/') + p.mid(prefix.size());
        };
        for (QString& p : lastOpenFiles)
            remap(p);
        remap(activeFilePath);
        // Breadcrumbs of files inside the folder changed.
        for (const WorkspaceSplit* s : {root, left, right})
            forEachLeaf(s, [&](WorkspaceLeaf* l) {
                if (auto* fv = qobject_cast<FileView*>(l->m_view); fv && fv->file() && fv->file()->path().startsWith(newPath + QLatin1Char('/')))
                    leafTitleChanged(l);
            });
    }
    q->requestSaveLayout();
}

void Workspace::Impl::onVaultDeleted(TAbstractFile* item)
{
    if (!item || item->isFolder() || destroying)
        return;
    auto* file = static_cast<TFile*>(item);
    QList<QPointer<FileView>> views;
    for (const WorkspaceSplit* s : {root, left, right})
        forEachLeaf(s, [&](WorkspaceLeaf* l) {
            if (auto* fv = qobject_cast<FileView*>(l->m_view); fv && fv->file() == file)
                views.push_back(fv);
        });
    for (const QPointer<FileView>& fv : std::as_const(views))
        if (fv)
            fv->onDelete(file);
}

void Workspace::Impl::onVaultCreated(TAbstractFile* item)
{
    // Obsidian's recent-file tracker also records newly created files.
    if (item && !item->isFolder() && layoutReady && !destroying)
        recordRecent(item->path());
}

// ---------------------------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------------------------

Workspace::Workspace(VaultSession* session, ViewRegistry* registry, MainWindow* window)
    : QObject(window)
    , d(nullptr)
    , m_session(session)
    , m_registry(registry)
    , m_window(window)
{
    // TFile is incomplete where the signals are declared: register the pointer type so queued
    // connections and QSignalSpy can carry fileOpened(oc::TFile*).
    qRegisterMetaType<oc::TFile*>("oc::TFile*");
    d = new Impl(this);
    d->widget = new WorkspaceWidget(d);
    d->buildEmptyLayout();
    d->saveTimer.setSingleShot(true);
    d->saveTimer.setInterval(1000);
    connect(&d->saveTimer, &QTimer::timeout, this, [this] { d->saveNow(); });
    d->activationFilter = new ActivationFilter(d, this);
    qApp->installEventFilter(d->activationFilter);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            d->syncWidgets();
            d->updateTopFlags();
            if (d->widget)
                d->widget->update();
        });
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(app, &QGuiApplication::focusWindowChanged, this, [this] { d->refreshChrome(); });
    // Quitting without closing the window (session end, QApplication::quit()) flushes a pending save.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (d->layoutReady && d->saveTimer.isActive())
            d->saveNow();
    });
    if (window) {
        connect(window, &MainWindow::closing, this, [this] {
            if (d->layoutReady)
                d->saveNow();
        });
        connect(window, &MainWindow::topRightReserveChanged, this, [this] { d->updateTopFlags(); });
    }
    d->connectVault();
}

Workspace::~Workspace()
{
    if (d->layoutReady && d->saveTimer.isActive())
        d->saveNow();
    d->saveTimer.stop();
    if (d->drag.active)
        d->endDrag({}, false);
    if (d->activationFilter)
        qApp->removeEventFilter(d->activationFilter);
    // Views are closed while the session and the rest of the window are still alive.
    for (WorkspaceSplit* s : {d->root, d->left, d->right})
        d->forEachLeaf(s, [](WorkspaceLeaf* l) {
            if (View* v = l->m_view) {
                QObject::disconnect(v, nullptr, l, nullptr);
                v->onClose();
            }
        });
    d->destroying = true;
    delete d->vaultBridge;
    d->vaultBridge = nullptr;
    delete d->overlay.data();
    delete d->widget.data();
    // Model items no longer own live widgets (deleted with the widget tree above).
    std::function<void(WorkspaceItem*)> drop = [&](WorkspaceItem* item) {
        if (!item)
            return;
        if (WorkspaceSplit* s = item->asSplit())
            for (WorkspaceItem* c : std::as_const(s->children))
                drop(c);
        delete item;
    };
    drop(d->root);
    drop(d->left);
    drop(d->right);
    delete d;
    d = nullptr;
    // Leaves are QObject children and are deleted by ~QObject.
}

QWidget* Workspace::widget() const { return d->widget; }

WorkspaceLeaf* Workspace::activeLeaf() const { return d->active; }

WorkspaceLeaf* Workspace::activeRootLeaf() const
{
    if (d->active && d->isRootLeaf(d->active))
        return d->active;
    return d->mostRecentLeaf(d->root);
}

void Workspace::setActiveLeaf(WorkspaceLeaf* leaf, bool focus)
{
    if (!leaf || !leaf->m_tabs || d->destroying)
        return;
    WorkspaceLeaf* old = d->active;
    if (old == leaf && !focus)
        return;
    d->leafData[leaf].activeTime = ++d->activeCounter;
    d->active = leaf;
    WorkspaceTabs* t = leaf->m_tabs;
    const int idx = int(t->leaves.indexOf(leaf));
    if (t->currentTab != idx) {
        t->currentTab = idx;
        t->w->sync();
    }
    if (focus && leaf->m_view)
        leaf->m_view->focusContent();
    if (old != leaf) {
        d->refreshChrome();
        emit activeLeafChanged(leaf);
        requestSaveLayout();
    }
    d->updateActiveFile();
}

WorkspaceLeaf* Workspace::getLeaf(NewLeaf mode)
{
    switch (mode) {
    case NewLeaf::Tab:
    case NewLeaf::Window: { // popout windows are not implemented: open a tab instead
        WorkspaceLeaf* recent = activeRootLeaf();
        WorkspaceTabs* t = recent ? recent->m_tabs : nullptr;
        WorkspaceLeaf* leaf = d->createLeafInGroup(t);
        setActiveLeaf(leaf, false);
        return leaf;
    }
    case NewLeaf::SplitRight:
        return d->createLeafBySplit(activeRootLeaf(), true, false);
    case NewLeaf::SplitDown:
        return d->createLeafBySplit(activeRootLeaf(), false, false);
    case NewLeaf::No:
        break;
    }
    // Obsidian's getUnpinnedLeaf(): the active root leaf when it can navigate, else the most recently
    // active navigable visible leaf of the root, else a new tab.
    WorkspaceLeaf* a = d->active;
    if (a && d->isRootLeaf(a) && d->canNavigate(a))
        return a;
    WorkspaceLeaf* best = nullptr;
    d->forEachTabs(d->root, [&](WorkspaceTabs* t) {
        WorkspaceLeaf* c = t->current();
        if (c && d->canNavigate(c) && (!best || d->leafData.value(c).activeTime > d->leafData.value(best).activeTime))
            best = c;
    });
    if (!best) {
        WorkspaceLeaf* recent = activeRootLeaf();
        WorkspaceTabs* t = recent ? recent->m_tabs : nullptr;
        if (!t)
            return nullptr;
        best = d->createLeaf();
        d->insertLeaf(t, -1, best);
    }
    setActiveLeaf(best, false);
    return best;
}

WorkspaceLeaf* Workspace::splitLeaf(WorkspaceLeaf* leaf, Qt::Orientation orientation, bool before)
{
    return d->createLeafBySplit(leaf, orientation == Qt::Horizontal, before);
}

WorkspaceLeaf* Workspace::ensureSideLeaf(const QString& type, Side side, bool reveal)
{
    WorkspaceLeaf* leaf = leavesOfType(type).value(0);
    if (!leaf) {
        WorkspaceSplit* s = side == Side::Left ? d->left : d->right;
        d->beginBatch();
        WorkspaceTabs* t = s->children.isEmpty() ? nullptr : s->children.first()->asTabs();
        if (!t) {
            t = new WorkspaceTabs(d);
            t->id = Impl::genId();
            d->insertItem(s, 0, t);
        }
        leaf = d->createLeaf();
        d->insertLeaf(t, -1, leaf);
        d->setViewState(leaf, QJsonObject{{QStringLiteral("type"), type}}, {}, false);
        d->endBatch();
    }
    if (reveal)
        revealLeaf(leaf);
    return leaf;
}

void Workspace::revealLeaf(WorkspaceLeaf* leaf)
{
    if (!leaf || !leaf->m_tabs)
        return;
    WorkspaceSplit* top = d->topSplit(leaf);
    if (top && top->isSidedock() && top->collapsed)
        setSideCollapsed(top == d->left ? Side::Left : Side::Right, false);
    d->selectTab(leaf->m_tabs, int(leaf->m_tabs->leaves.indexOf(leaf)));
}

void Workspace::openFile(TFile* file, NewLeaf mode, const QJsonObject& eState)
{
    if (!file || !m_registry)
        return;
    if (m_registry->typeForExtension(file->extension()).isEmpty())
        return;
    WorkspaceLeaf* leaf = getLeaf(mode);
    if (!leaf)
        return;
    leaf->openFile(file, {}, eState);
    setActiveLeaf(leaf, true);
    d->updateActiveFile(file);
}

QList<WorkspaceLeaf*> Workspace::leavesOfType(const QString& type) const
{
    QList<WorkspaceLeaf*> out;
    iterateAllLeaves([&](WorkspaceLeaf* l) {
        if (l->view() && l->view()->viewType() == type)
            out.push_back(l);
    });
    return out;
}

void Workspace::iterateAllLeaves(const std::function<void(WorkspaceLeaf*)>& fn) const
{
    for (const WorkspaceSplit* s : {d->root, d->left, d->right})
        d->forEachLeaf(s, fn);
}

void Workspace::iterateRootLeaves(const std::function<void(WorkspaceLeaf*)>& fn) const { d->forEachLeaf(d->root, fn); }

bool Workspace::isSideCollapsed(Side side) const
{
    const WorkspaceSplit* s = side == Side::Left ? d->left : d->right;
    return s && s->collapsed;
}

void Workspace::setSideCollapsed(Side side, bool collapsed)
{
    WorkspaceSplit* s = side == Side::Left ? d->left : d->right;
    if (!s || s->collapsed == collapsed)
        return;
    s->collapsed = collapsed;
    if (collapsed && d->active && d->topSplit(d->active) == s) {
        if (WorkspaceLeaf* next = d->mostRecentLeaf(d->root))
            setActiveLeaf(next, false);
    }
    d->structureChanged();
}

void Workspace::toggleSide(Side side) { setSideCollapsed(side, !isSideCollapsed(side)); }

int Workspace::sideWidth(Side side) const
{
    const WorkspaceSplit* s = side == Side::Left ? d->left : d->right;
    return s ? s->width : kDefaultSideWidth;
}

void Workspace::setSideWidth(Side side, int px)
{
    WorkspaceSplit* s = side == Side::Left ? d->left : d->right;
    if (!s || px <= 0 || s->width == px)
        return;
    s->width = px;
    if (d->widget)
        d->widget->relayout();
    requestSaveLayout();
    emit layoutChanged();
}

QStringList Workspace::lastOpenFiles() const { return d->lastOpenFiles; }

void Workspace::undoCloseLeaf()
{
    if (d->closed.isEmpty())
        return;
    const Impl::ClosedLeaf c = d->closed.takeFirst();
    WorkspaceSplit* top = d->root;
    if (d->left && c.rootId == d->left->id)
        top = d->left;
    else if (d->right && c.rootId == d->right->id)
        top = d->right;
    WorkspaceLeaf* leaf = nullptr;
    d->beginBatch();
    if (WorkspaceTabs* t = d->tabsById(top, c.parentId)) {
        leaf = d->createLeaf(d->leafById(c.leafId) ? QString() : c.leafId);
        d->insertLeaf(t, std::clamp(c.index, 0, int(t->leaves.size())), leaf);
    } else {
        WorkspaceLeaf* a = d->active;
        if (a && a->m_view && a->m_view->viewType() == kEmptyType)
            leaf = a;
        else
            leaf = d->createLeafBySplit(d->mostRecentLeaf(d->root), true, false);
        if (leaf && leaf->m_id != c.leafId && !d->leafById(c.leafId))
            leaf->m_id = c.leafId;
    }
    d->endBatch();
    if (!leaf)
        return;
    leaf->m_back = c.back;
    leaf->m_forward = c.forward;
    d->setViewState(leaf, c.viewState, c.eState, false);
    setActiveLeaf(leaf, true);
    emit leaf->historyChanged();
}

bool Workspace::canUndoCloseLeaf() const { return !d->closed.isEmpty(); }

QJsonObject Workspace::serialize() const
{
    QJsonObject o = d->extraTop;
    o.insert(QStringLiteral("main"), d->serializeItem(d->root));
    o.insert(QStringLiteral("left"), d->serializeItem(d->left));
    o.insert(QStringLiteral("right"), d->serializeItem(d->right));
    o.insert(QStringLiteral("left-ribbon"), QJsonObject{{QStringLiteral("hiddenItems"), d->ribbonHiddenItems()}});
    if (d->active)
        o.insert(QStringLiteral("active"), d->active->m_id);
    else
        o.remove(QStringLiteral("active"));
    o.insert(QStringLiteral("lastOpenFiles"), QJsonArray::fromStringList(d->lastOpenFiles));
    return o;
}

bool Workspace::restore(const QJsonObject& json)
{
    const QJsonValue mainV = json.value(QStringLiteral("main"));
    if (!mainV.isObject())
        return false;
    QList<QPair<WorkspaceLeaf*, QJsonObject>> leaves;
    WorkspaceItem* mainItem = d->parseItem(mainV.toObject(), WorkspaceSplit::Role::Root, leaves);
    if (!mainItem) {
        for (auto& [leaf, state] : leaves) {
            if (LeafWidget* lw = d->leafWidget(leaf))
                delete lw;
            d->leafData.remove(leaf);
            delete leaf;
        }
        return false;
    }
    auto asTop = [&](WorkspaceItem* item, WorkspaceSplit::Role role, bool vertical) -> WorkspaceSplit* {
        WorkspaceSplit* s = item ? item->asSplit() : nullptr;
        if (item && !s) {
            // A bare tab group at the top: wrap it.
            s = new WorkspaceSplit(d, role, vertical);
            s->id = Impl::genId();
            s->children.push_back(item);
            item->parent = s;
        } else if (!s) {
            s = new WorkspaceSplit(d, role, vertical);
            s->id = Impl::genId();
        }
        s->role = role;
        s->vertical = vertical; // Obsidian forces the root vertical and the sidedocks horizontal
        s->parent = nullptr;
        return s;
    };
    const QJsonObject leftO = json.value(QStringLiteral("left")).toObject();
    const QJsonObject rightO = json.value(QStringLiteral("right")).toObject();
    WorkspaceItem* leftItem = leftO.isEmpty() ? nullptr : d->parseItem(leftO, WorkspaceSplit::Role::Left, leaves);
    WorkspaceItem* rightItem = rightO.isEmpty() ? nullptr : d->parseItem(rightO, WorkspaceSplit::Role::Right, leaves);

    if (d->drag.active)
        d->endDrag({}, false);
    d->beginBatch();
    // The parsed tree was built directly (not through insertItem()/insertLeaf()), so nothing else
    // marks the batch dirty: without this the widgets are never synced and the layout renders blank.
    d->structureChanged();
    d->clearLayout();
    d->root = asTop(mainItem, WorkspaceSplit::Role::Root, true);
    d->left = asTop(leftItem, WorkspaceSplit::Role::Left, false);
    d->right = asTop(rightItem, WorkspaceSplit::Role::Right, false);
    if (!rightItem)
        d->right->collapsed = true; // like Obsidian when the layout has no right sidedock
    for (WorkspaceSplit* s : {d->root, d->left, d->right})
        s->w->setParent(d->widget);
    for (auto& [leaf, state] : leaves)
        d->setViewState(leaf, state, {}, false);
    d->ensureRootLeaf();

    d->ribbonHidden = json.value(QStringLiteral("left-ribbon")).toObject().value(QStringLiteral("hiddenItems")).toObject();
    d->applyRibbonHidden();
    d->lastOpenFiles.clear();
    for (const QJsonValue& v : json.value(QStringLiteral("lastOpenFiles")).toArray())
        if (v.isString() && !d->lastOpenFiles.contains(v.toString()))
            d->lastOpenFiles.push_back(v.toString());
    d->extraTop = without(json, {"main", "left", "right", "left-ribbon", "active", "lastOpenFiles"});
    d->endBatch();

    WorkspaceLeaf* act = d->leafById(json.value(QStringLiteral("active")).toString());
    if (!act)
        act = d->mostRecentLeaf(d->root);
    if (act)
        setActiveLeaf(act, false);
    d->activeFilePath.clear();
    d->updateActiveFile();
    return true;
}

void Workspace::loadLayout()
{
    d->layoutReady = false;
    d->connectVault();
    const QString path = d->layoutPath();
    const QJsonObject o = path.isEmpty() ? QJsonObject() : JsonFile::read(path);
    if (!restore(o)) {
        // Like Obsidian, the recent files survive a layout that can't be restored and the fresh layout
        // reopens the most recent one.
        d->lastOpenFiles.clear();
        for (const QJsonValue& v : o.value(QStringLiteral("lastOpenFiles")).toArray())
            if (v.isString() && !d->lastOpenFiles.contains(v.toString()))
                d->lastOpenFiles.push_back(v.toString());
        d->buildDefaultLayout();
    }
    d->layoutReady = true;
    d->saveTimer.stop();
}

void Workspace::requestSaveLayout()
{
    if (!d || !d->layoutReady || d->destroying)
        return;
    d->saveTimer.start();
}

// ---- commands ---------------------------------------------------------------------------------

WorkspaceLeaf* Workspace::duplicateLeaf(WorkspaceLeaf* leaf, NewLeaf mode)
{
    if (!leaf || !leaf->m_view)
        return nullptr;
    const QJsonObject vs = leaf->viewState();
    const QJsonObject es = leaf->m_view->ephemeralState();
    const auto back = leaf->m_back;
    const auto forward = leaf->m_forward;
    WorkspaceLeaf* nl = nullptr;
    if (mode == NewLeaf::SplitRight || mode == NewLeaf::SplitDown) {
        if (leaf->isInSidedock() && mode == NewLeaf::SplitRight)
            return nullptr; // Obsidian disables "Split right" in the sidebars
        nl = d->createLeafBySplit(leaf, mode == NewLeaf::SplitRight, false);
    } else {
        nl = getLeaf(mode);
    }
    if (!nl)
        return nullptr;
    QJsonObject state = vs;
    state.remove(QStringLiteral("pinned"));
    d->setViewState(nl, state, es, false);
    nl->m_back = back;
    nl->m_forward = forward;
    emit nl->historyChanged();
    setActiveLeaf(nl, true);
    return nl;
}

WorkspaceLeaf* Workspace::newTab()
{
    WorkspaceLeaf* recent = activeRootLeaf();
    return recent ? d->appendNewTab(recent->m_tabs) : nullptr;
}

void Workspace::nextTab()
{
    WorkspaceLeaf* a = d->active;
    if (!a || !a->m_tabs)
        return;
    const auto& leaves = a->m_tabs->leaves;
    const int next = (int(leaves.indexOf(a)) + 1) % int(leaves.size());
    setActiveLeaf(leaves[next], true);
}

void Workspace::previousTab()
{
    WorkspaceLeaf* a = d->active;
    if (!a || !a->m_tabs)
        return;
    const auto& leaves = a->m_tabs->leaves;
    const int n = int(leaves.size());
    const int prev = (int(leaves.indexOf(a)) - 1 + n) % n;
    setActiveLeaf(leaves[prev], true);
}

void Workspace::goToTab(int index)
{
    WorkspaceLeaf* a = d->active;
    if (!a || !a->m_tabs || index < 0 || index >= a->m_tabs->leaves.size())
        return;
    setActiveLeaf(a->m_tabs->leaves[index], true);
}

void Workspace::goToLastTab()
{
    WorkspaceLeaf* a = d->active;
    if (!a || !a->m_tabs)
        return;
    setActiveLeaf(a->m_tabs->leaves.last(), true);
}

void Workspace::closeActiveLeaf()
{
    WorkspaceLeaf* a = d->active;
    if (!a)
        a = activeRootLeaf();
    if (!a)
        return;
    if (a->isPinned())
        a->setPinned(false); // Obsidian: Ctrl+W on a pinned tab unpins it
    else
        a->detach();
}

QList<WorkspaceLeaf*> Workspace::groupLeaves(WorkspaceLeaf* leaf) const
{
    return leaf && leaf->m_tabs ? leaf->m_tabs->leaves : QList<WorkspaceLeaf*>();
}

void Workspace::closeLeaves(const QList<WorkspaceLeaf*>& leaves)
{
    d->beginBatch();
    for (const QPointer<WorkspaceLeaf>& l : QList<QPointer<WorkspaceLeaf>>(leaves.begin(), leaves.end()))
        if (l && l->m_tabs)
            d->closeLeaf(l, true);
    d->endBatch();
}

void Workspace::togglePinActiveLeaf()
{
    WorkspaceLeaf* a = d->active;
    if (a && a->m_view && a->m_view->viewType() != kEmptyType)
        a->setPinned(!a->isPinned());
}

} // namespace oc

#include "workspace.moc"
