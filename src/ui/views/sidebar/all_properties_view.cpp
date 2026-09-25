#include "ui/views/sidebar/all_properties_view.h"

#include "core/config/config_store.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/property_types.h"
#include "core/properties/vault_properties.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"
#include "render/theme/icons.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/workspace/workspace.h"

#include <QJsonObject>
#include <QMenu>
#include <QVBoxLayout>

namespace oc {

AllPropertiesView::AllPropertiesView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
    , m_header(new NavHeader(this))
    , m_tree(new TreeList(this))
{
    setObjectName(QStringLiteral("ocAllPropertiesView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    m_header->addSortButton(countSortGroups(tr("Property name (A to Z)"), tr("Property name (Z to A)")),
                            [this] { return countSortOrderId(m_order); },
                            [this](const QString& id) {
                                if (auto o = countSortOrderFromId(id))
                                    setSortOrder(*o);
                                emit stateChanged();
                            });
    m_header->addSearchFilter([this](const QString& q) { setSearchQuery(q); }, [this](bool) { emit stateChanged(); });

    // .view-content: padding 4 12 32
    m_tree->setContentMargins(12, 4, 12, 32);
    m_tree->setSelectionEnabled(true);
    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        activateProperty(n->data.toString(), button == Qt::MiddleButton || (mods & Qt::ControlModifier));
    };
    h.activateView = [this] {
        if (workspace() && this->leaf())
            workspace()->setActiveLeaf(this->leaf(), false);
    };
    h.contextMenu = [this](TreeList::Node* n, const QPoint& globalPos) {
        if (!n || n->kind != TreeList::Kind::Item)
            return;
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        populateMenu(menu, n->data.toString());
        menu->popup(globalPos);
    };
    h.rename = [this](TreeList::Node* n) {
        if (n && n->kind == TreeList::Kind::Item && !PropertyTypeManager::isReserved(n->data.toString()))
            m_tree->startRename(n);
    };
    h.renamed = [this](TreeList::Node* n, const QString& text) { renameProperty(n->data.toString(), text.trimmed()); };
    h.remove = [this](const QList<TreeList::Node*>& nodes) {
        QStringList names;
        for (TreeList::Node* n : nodes)
            if (n->kind == TreeList::Kind::Item && !PropertyTypeManager::isReserved(n->data.toString()))
                names << n->data.toString();
        for (const QString& name : std::as_const(names))
            deleteProperty(name);
    };

    m_update.setSingleShot(true);
    m_update.setInterval(100); // Obsidian's requestUpdate debounce
    connect(&m_update, &QTimer::timeout, this, &AllPropertiesView::update);
    if (VaultSession* s = session())
        if (MetadataCache* mc = s->metadata()) {
            m_connections << connect(mc, &MetadataCache::resolved, this, [this] { m_update.start(); });
            // during the initial index only resolved() (the end of a batch) counts
            m_connections << connect(mc, &MetadataCache::changed, this, [this, mc] {
                if (mc->isReady())
                    m_update.start();
            });
            m_connections << connect(mc, &MetadataCache::deleted, this, [this] { m_update.start(); });
        }
    if (PropertyTypeManager* types = PropertyTypeManager::of(session()))
        m_connections << connect(types, &PropertyTypeManager::changed, this, [this] { m_update.start(); });
}

AllPropertiesView::~AllPropertiesView()
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
}

QString AllPropertiesView::displayText() const { return tr("All properties"); }

QJsonObject AllPropertiesView::state() const
{
    return QJsonObject{{QStringLiteral("sortOrder"), countSortOrderId(m_order)},
                       {QStringLiteral("showSearch"), m_header->isSearchShown()},
                       {QStringLiteral("searchQuery"), m_header->searchQuery()}};
}

void AllPropertiesView::setState(const QJsonObject& st)
{
    if (auto o = countSortOrderFromId(st.value(QStringLiteral("sortOrder")).toString()))
        m_order = *o;
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_header->setSearchShown(st.value(QStringLiteral("showSearch")).toBool());
    if (st.value(QStringLiteral("searchQuery")).isString()) {
        m_header->setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
        m_filter = search::parseSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
    }
    update();
}

void AllPropertiesView::onOpen()
{
    if (Workspace* ws = workspace()) {
        m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* l) { m_tree->setLeafActive(l && l == leaf()); });
        m_tree->setLeafActive(ws->activeLeaf() == leaf());
    }
    update();
}

void AllPropertiesView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

void AllPropertiesView::setSortOrder(CountSortOrder order)
{
    if (order == m_order)
        return;
    m_order = order;
    update();
}

void AllPropertiesView::setSearchQuery(const QString& query)
{
    if (query == m_filter.source())
        return;
    m_filter = search::parseSearchQuery(query);
    update();
    emit stateChanged();
}

void AllPropertiesView::activateProperty(const QString& name, bool modifier)
{
    searchFor(sessionWindow(session()), QStringLiteral("[\"%1\"]").arg(name), modifier);
}

void AllPropertiesView::populateMenu(QMenu* menu, const QString& name)
{
    PropertyTypeManager* types = PropertyTypeManager::of(session());
    if (!types)
        return;
    const bool reserved = PropertyTypeManager::isReserved(name);
    auto icon = [](const QString& n) { return IconProvider::instance().icon(n, 16); };
    QMenu* sub = menu->addMenu(icon(QStringLiteral("menu")), tr("Property type"));
    const QString assigned = types->assignedType(name);
    if (!reserved) {
        const QString inferred = props::propertyType(types->typeInfo(name).inferred).name;
        QAction* a = sub->addAction(icon(QStringLiteral("loader")), assigned.isEmpty() ? tr("Automatic (%1)").arg(inferred) : tr("Automatic"), this,
                                    [types, name] { types->unsetType(name); });
        a->setCheckable(true);
        a->setChecked(assigned.isEmpty());
    }
    for (const props::PropertyType& t : props::menuPropertyTypes(name)) {
        QAction* a = sub->addAction(icon(t.icon), t.name, this, [types, name, id = t.id] { types->setType(name, id); });
        a->setCheckable(true);
        a->setChecked(t.id == assigned);
        a->setEnabled(!reserved);
    }
    menu->addSeparator();
    QAction* rename = menu->addAction(icon(QStringLiteral("edit-3")), tr("Rename"), this, [this, name] {
        if (TreeList::Node* n = m_tree->findNode(name.toLower()))
            m_tree->startRename(n);
    });
    rename->setEnabled(!reserved);
    menu->addSeparator();
    QAction* del = menu->addAction(icon(QStringLiteral("trash-2")), tr("Delete property"), this, [this, name] { deleteProperty(name); });
    del->setEnabled(!reserved);
    del->setProperty("ocWarning", true);
}

void AllPropertiesView::renameProperty(const QString& from, const QString& to)
{
    VaultSession* s = session();
    MainWindow* w = sessionWindow(s);
    if (to == from)
        return;
    if (to.isEmpty()) {
        if (w)
            w->notices()->show(tr("Property name cannot be empty."));
        return;
    }
    auto run = [this, from, to] {
        QString err;
        props::renameProperty(session(), from, to, &err);
        if (!err.isEmpty())
            if (MainWindow* win = sessionWindow(session()))
                win->notices()->show(err);
        m_update.start();
    };
    if (props::propertyExists(s, to) && to.compare(from, Qt::CaseInsensitive) != 0 && w && w->overlay()) {
        QPointer<AllPropertiesView> self = this;
        ConfirmModal::ask(w->overlay(), tr("Merge property “%1” with “%2”?").arg(from, to),
                          tr("In case of conflicts, the values will be merged or the value from “%1” will be used.").arg(from),
                          {{tr("Save"), Modal::ButtonStyle::Cta}, {tr("Cancel"), Modal::ButtonStyle::Normal}}, QString(),
                          [self, run](int button, bool) {
                              if (self && button == 0)
                                  run();
                          });
        return;
    }
    run();
}

void AllPropertiesView::deleteProperty(const QString& name)
{
    QString err;
    props::deleteProperty(session(), name, &err);
    if (!err.isEmpty())
        if (MainWindow* w = sessionWindow(session()))
            w->notices()->show(err);
    m_update.start();
}

void AllPropertiesView::update()
{
    m_update.stop();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    QList<const md::CachedMetadata*> caches;
    if (mc)
        for (const QString& path : mc->markdownFiles())
            caches << mc->fileCache(path);
    // Assigned types: the reserved ones plus types.json ({"types": {name: type}}) from the clone's config.
    QHash<QString, QPair<QString, QString>> assigned = reservedPropertyTypes();
    if (PropertyTypeManager* types = PropertyTypeManager::of(s))
        assigned = types->assignedTypes();
    const QHash<QString, PropertyInfo> all = collectProperties(caches, assigned);
    m_properties.clear();
    for (const PropertyInfo& p : all)
        if (m_filter.isEmpty() || filterMatch(m_filter, p.name))
            m_properties << p;
    sortProperties(m_properties, m_order);
    std::vector<TreeList::NodePtr> roots;
    for (const PropertyInfo& p : std::as_const(m_properties)) {
        auto n = std::make_unique<TreeList::Node>();
        n->id = p.name.toLower();
        n->data = p.name;
        n->icon = propertyTypeIcon(p.type);
        n->title = p.name;
        n->flair = QString::number(p.occurrences);
        n->draggable = false;
        roots.push_back(std::move(n));
    }
    if (all.isEmpty()) {
        auto msg = std::make_unique<TreeList::Node>();
        msg->kind = TreeList::Kind::Message;
        msg->centered = true;
        msg->title = tr("No properties found.");
        roots.push_back(std::move(msg));
    }
    m_tree->setRoots(std::move(roots));
}

} // namespace oc
