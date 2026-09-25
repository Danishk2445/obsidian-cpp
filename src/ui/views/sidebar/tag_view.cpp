#include "ui/views/sidebar/tag_view.h"

#include "core/metadata/link_suggest.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h" // file_explorer_view.h needs the complete type
#include "core/vault_session.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/workspace/workspace.h"

#include <QVBoxLayout>

#include <functional>

namespace oc {

namespace {
constexpr const char* kFoldKey = "tag-pane-fold";
}

TagView::TagView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
    , m_header(new NavHeader(this))
    , m_tree(new TreeList(this))
{
    setObjectName(QStringLiteral("ocTagView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    m_header->addSortButton(countSortGroups(tr("Tag name (A to Z)"), tr("Tag name (Z to A)")), [this] { return countSortOrderId(m_order); },
                            [this](const QString& id) {
                                if (auto o = countSortOrderFromId(id))
                                    setSortOrder(*o);
                                emit stateChanged();
                            });
    m_nestedButton = m_header->addButton(QStringLiteral("folder-tree"), tr("Show nested tags"), [this] {
        setUseHierarchy(!m_nested);
        emit stateChanged();
    });
    m_collapseButton = m_header->addButton(QStringLiteral("chevrons-up-down"), tr("Expand all"), [this] {
        m_tree->setAllCollapsed(!m_tree->isAllCollapsed());
        updateCollapseButton();
        m_foldSave.start();
        emit stateChanged();
    });
    m_header->addSearchFilter(
        [this](const QString& q) {
            setSearchQuery(q);
            emit stateChanged();
        },
        [this](bool) { emit stateChanged(); }, 300);
    m_nestedButton->setActive(m_nested);

    // .tag-container: padding 12 12 32
    m_tree->setContentMargins(12, 12, 12, 32);
    m_tree->setAllCollapsedFlag(true); // Obsidian's tree starts "all collapsed" (the button offers Expand all)
    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        activateTag(n->data.toString(), button == Qt::MiddleButton || (mods & Qt::ControlModifier));
        m_tree->setFocusedNode(nullptr, false);
    };
    h.collapsedChanged = [this](TreeList::Node*, bool) {
        updateCollapseButton();
        m_foldSave.start();
    };
    h.activateView = [this] {
        if (workspace() && this->leaf())
            workspace()->setActiveLeaf(this->leaf(), false);
    };

    m_update.setSingleShot(true);
    m_update.setInterval(10);
    connect(&m_update, &QTimer::timeout, this, &TagView::updateTags);
    m_foldSave.setSingleShot(true);
    m_foldSave.setInterval(200);
    connect(&m_foldSave, &QTimer::timeout, this, &TagView::saveFoldsNow);
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
    updateCollapseButton();
}

TagView::~TagView()
{
    if (m_foldSave.isActive())
        saveFoldsNow();
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
}

QString TagView::displayText() const { return tr("Tags"); }

QJsonObject TagView::state() const
{
    return QJsonObject{{QStringLiteral("sortOrder"), countSortOrderId(m_order)},
                       {QStringLiteral("useHierarchy"), m_nested},
                       {QStringLiteral("showSearch"), m_header->isSearchShown()},
                       {QStringLiteral("searchQuery"), m_header->searchQuery()}};
}

void TagView::setState(const QJsonObject& st)
{
    if (auto o = countSortOrderFromId(st.value(QStringLiteral("sortOrder")).toString()))
        m_order = *o;
    if (st.value(QStringLiteral("useHierarchy")).isBool())
        setUseHierarchy(st.value(QStringLiteral("useHierarchy")).toBool());
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_header->setSearchShown(st.value(QStringLiteral("showSearch")).toBool());
    if (st.value(QStringLiteral("searchQuery")).isString()) {
        m_header->setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
        m_filter = search::parseSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
    }
    m_update.start();
}

void TagView::onOpen()
{
    if (Workspace* ws = workspace()) {
        m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* l) { m_tree->setLeafActive(l && l == leaf()); });
        m_tree->setLeafActive(ws->activeLeaf() == leaf());
    }
    updateTags();
}

void TagView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

void TagView::setSortOrder(CountSortOrder order)
{
    if (order == m_order)
        return;
    m_order = order;
    m_update.start();
}

void TagView::setUseHierarchy(bool nested)
{
    if (nested == m_nested)
        return;
    m_nested = nested;
    m_nestedButton->setActive(nested);
    m_collapseButton->setEnabled(nested); // aria-disabled without the hierarchy
    m_update.start();
}

void TagView::setSearchQuery(const QString& query)
{
    if (query == m_filter.source())
        return;
    m_filter = search::parseSearchQuery(query);
    updateTags();
}

void TagView::updateCollapseButton()
{
    // Obsidian's setIsAllCollapsed(): the button offers the opposite.
    if (m_tree->isAllCollapsed()) {
        m_collapseButton->setIconName(QStringLiteral("chevrons-up-down"));
        m_collapseButton->setToolTip(tr("Expand all"));
    } else {
        m_collapseButton->setIconName(QStringLiteral("chevrons-down-up"));
        m_collapseButton->setToolTip(tr("Collapse all"));
    }
}

void TagView::activateTag(const QString& tag, bool modifier)
{
    searchFor(sessionWindow(session()), QStringLiteral("tag:") + tag, modifier);
}

void TagView::saveFoldsNow()
{
    m_foldSave.stop();
    if (!m_foldsLoaded)
        return;
    // Nodes present now: their state; others keep what was saved (e.g. hidden by the filter).
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

void TagView::updateTags()
{
    m_update.stop();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    if (!m_foldsLoaded && s) {
        const QStringList saved = loadFolds(s, QString::fromLatin1(kFoldKey));
        m_folded = QSet<QString>(saved.begin(), saved.end());
        m_foldsLoaded = true;
    } else {
        // remember the current folds before rebuilding
        for (const TreeList::Node* n : m_tree->allNodes())
            if (n->collapsible) {
                if (n->collapsed)
                    m_folded.insert(n->id);
                else
                    m_folded.remove(n->id);
            }
    }
    const QHash<QString, int> counts = mc ? vaultTags(*mc) : QHash<QString, int>();
    const QList<TagNode> tree = buildTagTree(counts, m_nested, m_order);
    const bool filtering = !m_filter.isEmpty();
    std::function<TreeList::NodePtr(const TagNode&, bool&)> make = [&](const TagNode& t, bool& visible) {
        auto n = std::make_unique<TreeList::Node>();
        n->id = t.tag.toLower();
        n->data = t.tag;
        // flat list: "parent/" before the name; nested children hide it (.tree-item-children .tag-pane-tag-parent)
        n->prefix = t.parent;
        n->title = t.name;
        n->flair = QString::number(t.count);
        n->selectable = false;
        n->draggable = false;
        n->collapsible = !t.children.isEmpty();
        n->collapsed = n->collapsible && m_folded.contains(n->id);
        bool anyChild = false;
        for (const TagNode& c : t.children) {
            bool childVisible = true;
            TreeList::NodePtr cn = make(c, childVisible);
            cn->prefix.clear();
            anyChild = anyChild || childVisible;
            n->add(std::move(cn));
        }
        const bool self = !filtering || filterMatch(m_filter, t.tag).has_value();
        visible = self || anyChild;
        n->hidden = !visible;
        return n;
    };
    std::vector<TreeList::NodePtr> roots;
    for (const TagNode& t : tree) {
        bool visible = true;
        roots.push_back(make(t, visible));
    }
    if (roots.empty()) {
        auto msg = std::make_unique<TreeList::Node>();
        msg->kind = TreeList::Kind::Message;
        msg->centered = true;
        msg->title = tr("No tags found.");
        roots.push_back(std::move(msg));
    }
    m_tree->setRoots(std::move(roots), false);
}

} // namespace oc
