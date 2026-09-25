#include "ui/views/sidebar/all_properties_view.h"

#include "core/config/config_store.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/workspace/workspace.h"

#include <QJsonObject>
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
    if (s && s->config()) {
        const QJsonObject types = s->config()->object(QStringLiteral("types")).value(QStringLiteral("types")).toObject();
        for (auto it = types.begin(); it != types.end(); ++it)
            if (!assigned.contains(it.key().toLower()))
                assigned.insert(it.key().toLower(), {it.key(), it.value().toString()});
    }
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
