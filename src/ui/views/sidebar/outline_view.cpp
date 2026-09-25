#include "ui/views/sidebar/outline_view.h"

#include "core/documents/document.h"
#include "core/metadata/link_suggest.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/outline.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "markdown/metadata.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QJsonObject>
#include <QVBoxLayout>

#include <functional>

namespace oc {

OutlineView::OutlineView(WorkspaceLeaf* leaf, QWidget* parent)
    : LinkedFileView(leaf, parent)
    , m_header(new NavHeader(this))
    , m_tree(new TreeList(this))
{
    setObjectName(QStringLiteral("ocOutlineView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    m_header->addSearchFilter([this](const QString& q) { setSearchQuery(q); }, [this](bool) { emit stateChanged(); });
    m_followButton = m_header->addButton(QStringLiteral("gallery-vertical"), tr("Auto-scroll to current section"), [this] {
        setFollowCursor(!m_followCursor);
        updateActiveHeading(); // scrolls to the current section when following

        emit stateChanged();
    });
    m_collapseButton = m_header->addButton(QStringLiteral("chevrons-down-up"), tr("Collapse all"), [this] {
        m_tree->setAllCollapsed(!m_tree->isAllCollapsed());
        updateCollapseButton();
    });

    // .view-content: padding 4 12 32
    m_tree->setContentMargins(12, 4, 12, 32);
    m_tree->setDragEnabled(true);
    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        navigateTo(n->data.toInt(), button == Qt::MiddleButton || (mods & Qt::ControlModifier));
    };
    h.collapsedChanged = [this](TreeList::Node*, bool) { updateCollapseButton(); };
    h.hover = [this](TreeList::Node* n, const QRect& r) {
        VaultSession* s = session();
        const md::CachedMetadata* meta = s && s->metadata() ? s->metadata()->fileCache(filePath()) : nullptr;
        const int i = n->data.toInt();
        if (meta && i >= 0 && i < meta->headings.size())
            hoverLink(this, QStringLiteral("outline"), filePath() + QLatin1Char('#') + headingSubpathText(meta->headings[i].heading), QString(), r,
                      m_tree->viewport());
    };
    h.unhover = [this] { unhoverLink(this, m_tree->viewport()); };
    h.drop = [this](const QList<TreeList::Node*>& nodes, TreeList::Node* target, TreeList::DropPlacement placement) {
        if (nodes.size() != 1 || !target)
            return;
        const int t = target->data.toInt();
        moveSection(nodes.first()->data.toInt(), placement == TreeList::DropPlacement::Before ? t : t + 1);
    };
    h.activateView = [this] {
        if (workspace() && this->leaf())
            workspace()->setActiveLeaf(this->leaf(), false);
    };

    m_update.setSingleShot(true);
    m_update.setInterval(10);
    connect(&m_update, &QTimer::timeout, this, &OutlineView::update);
    m_selection.setSingleShot(true);
    m_selection.setInterval(20); // Obsidian debounces the selection change by 20 ms
    connect(&m_selection, &QTimer::timeout, this, &OutlineView::updateActiveHeading);
    if (VaultSession* s = session())
        if (MetadataCache* mc = s->metadata())
            m_connections << connect(mc, &MetadataCache::changed, this, [this](const QString& path) {
                if (path == filePath())
                    m_update.start();
            });
    if (Workspace* ws = workspace()) {
        m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this] {
            watchOwner();
            m_selection.start();
        });
        m_connections << connect(ws, &Workspace::layoutChanged, this, [this] { watchOwner(); });
    }
    updateCollapseButton();
}

OutlineView::~OutlineView()
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    for (const auto& c : std::as_const(m_ownerConnections))
        disconnect(c);
}

QString OutlineView::baseTitle() const { return tr("Outline"); }
QString OutlineView::boundTitle() const { return tr("Outline of %1"); }

QJsonObject OutlineView::state() const
{
    QJsonObject o = LinkedFileView::state();
    o.insert(QStringLiteral("followCursor"), m_followCursor);
    o.insert(QStringLiteral("showSearch"), m_header->isSearchShown());
    o.insert(QStringLiteral("searchQuery"), m_header->searchQuery());
    return o;
}

void OutlineView::setState(const QJsonObject& st)
{
    LinkedFileView::setState(st);
    if (st.value(QStringLiteral("followCursor")).isBool())
        setFollowCursor(st.value(QStringLiteral("followCursor")).toBool());
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_header->setSearchShown(st.value(QStringLiteral("showSearch")).toBool());
    if (st.value(QStringLiteral("searchQuery")).isString()) {
        m_header->setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
        setSearchQuery(st.value(QStringLiteral("searchQuery")).toString());
    }
}

void OutlineView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

void OutlineView::fileChanged()
{
    watchOwner();
    update();
}

void OutlineView::leafActiveChanged(bool active) { m_tree->setLeafActive(active); }

void OutlineView::setFollowCursor(bool on)
{
    if (on == m_followCursor)
        return;
    m_followCursor = on;
    m_followButton->setActive(on);
}

void OutlineView::setSearchQuery(const QString& query)
{
    if (query == m_filter.source())
        return;
    m_filter = search::parseSearchQuery(query);
    update();
    emit stateChanged();
}

void OutlineView::updateCollapseButton()
{
    if (m_tree->isAllCollapsed()) {
        m_collapseButton->setIconName(QStringLiteral("chevrons-up-down"));
        m_collapseButton->setToolTip(tr("Expand all"));
    } else {
        m_collapseButton->setIconName(QStringLiteral("chevrons-down-up"));
        m_collapseButton->setToolTip(tr("Collapse all"));
    }
}

void OutlineView::update()
{
    m_update.stop();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    const QString path = filePath();
    const md::CachedMetadata* meta = mc && path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) ? mc->fileCache(path) : nullptr;
    const QList<md::HeadingCache> headings = meta ? meta->headings : QList<md::HeadingCache>();
    const bool sameFile = path == m_builtFor;
    m_builtFor = path;
    const bool filtering = !m_filter.isEmpty();
    std::function<TreeList::NodePtr(const OutlineNode&, bool&)> make = [&](const OutlineNode& o, bool& visible) {
        auto n = std::make_unique<TreeList::Node>();
        n->id = QStringLiteral("%1:%2:%3").arg(o.index).arg(o.level).arg(o.heading);
        n->title = o.text;
        n->data = o.index;
        n->selectable = false;
        n->collapsible = !o.children.isEmpty();
        n->boldMatches = true;
        bool anyChild = false;
        for (const OutlineNode& c : o.children) {
            bool cv = true;
            n->add(make(c, cv));
            anyChild = anyChild || cv;
        }
        bool self = true;
        if (filtering) {
            const std::optional<QList<MatchRange>> m = filterMatch(m_filter, o.text);
            self = m.has_value();
            if (m)
                n->matches = *m;
        }
        visible = self || anyChild;
        n->hidden = !visible;
        return n;
    };
    std::vector<TreeList::NodePtr> roots;
    for (const OutlineNode& o : buildOutline(headings)) {
        bool visible = true;
        roots.push_back(make(o, visible));
    }
    if (roots.empty()) {
        auto msg = std::make_unique<TreeList::Node>();
        msg->kind = TreeList::Kind::Message;
        msg->centered = true;
        msg->title = tr("No headings found.");
        roots.push_back(std::move(msg));
    }
    if (!sameFile)
        m_tree->setAllCollapsedFlag(false);
    m_tree->setRoots(std::move(roots), sameFile);
    m_active = -1;
    updateActiveHeading();
    updateCollapseButton();
}

MarkdownView* OutlineView::correspondingView() const
{
    const QString path = filePath();
    if (path.isEmpty())
        return nullptr;
    auto matches = [&](WorkspaceLeaf* l) -> MarkdownView* {
        auto* mv = l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
        return mv && mv->file() && mv->file()->path() == path ? mv : nullptr;
    };
    if (MarkdownView* mv = matches(linkedLeaf()))
        return mv;
    Workspace* ws = workspace();
    return ws ? matches(ws->activeRootLeaf()) : nullptr;
}

MarkdownView* OutlineView::owner() const
{
    Workspace* ws = workspace();
    if (ws)
        if (auto* mv = qobject_cast<MarkdownView*>(ws->activeLeaf() ? ws->activeLeaf()->view() : nullptr))
            if (mv->file() && mv->file()->path() == filePath())
                return mv;
    return correspondingView();
}

void OutlineView::watchOwner()
{
    MarkdownView* mv = owner();
    if (mv == m_watched)
        return;
    for (const auto& c : std::as_const(m_ownerConnections))
        disconnect(c);
    m_ownerConnections.clear();
    m_watched = mv;
    if (!mv)
        return;
    EditorView* e = mv->editor();
    m_ownerConnections << connect(e, &EditorView::cursorMoved, this, [this] { m_selection.start(); });
    m_ownerConnections << connect(e, &EditorView::scrolled, this, [this] { m_selection.start(); });
    m_ownerConnections << connect(e, &EditorView::modeChanged, this, [this] { m_selection.start(); });
}

void OutlineView::updateActiveHeading()
{
    m_selection.stop();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    const md::CachedMetadata* meta = mc ? mc->fileCache(filePath()) : nullptr;
    MarkdownView* mv = owner();
    int active = -1;
    if (meta && mv && mv->document()) {
        EditorView* e = mv->editor();
        if (e->mode() == EditorView::Mode::Reading) {
            // Obsidian's preview: the last heading whose end the scroll position has passed
            const int scroll = e->ephemeralState().value(QStringLiteral("scroll")).toInt();
            for (int i = 0; i < meta->headings.size(); ++i)
                if (scroll >= meta->headings[i].position.end.line)
                    active = i;
        } else {
            active = headingIndexAtLine(meta->headings, mv->document()->text().lineAt(e->cursorPosition()));
        }
    }
    TreeList::Node* activeNode = nullptr;
    for (TreeList::Node* n : m_tree->allNodes()) {
        const bool on = n->kind == TreeList::Kind::Item && n->data.toInt() == active && active >= 0;
        if (on)
            activeNode = n;
        n->active = on;
    }
    m_active = active;
    m_tree->viewport()->update();
    if (m_followCursor && activeNode)
        m_tree->scrollToNode(activeNode, 20);
}

void OutlineView::navigateTo(int index, bool newLeaf)
{
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    const md::CachedMetadata* meta = mc ? mc->fileCache(filePath()) : nullptr;
    Workspace* ws = workspace();
    if (!meta || !ws || index < 0 || index >= meta->headings.size())
        return;
    const QJsonObject eState{{QStringLiteral("line"), meta->headings[index].position.start.line}};
    if (MarkdownView* mv = newLeaf ? nullptr : correspondingView()) {
        ws->setActiveLeaf(mv->leaf(), true);
        mv->setEphemeralState(eState);
        return;
    }
    if (TFile* f = file())
        ws->openFile(f, newLeaf ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No, eState);
}

bool OutlineView::moveSection(int from, int target)
{
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    const md::CachedMetadata* meta = mc ? mc->fileCache(filePath()) : nullptr;
    if (!meta)
        return false;
    const std::optional<QString> content = readNoteText(s, filePath());
    if (!content)
        return false;
    const std::optional<TextEdit> edit = moveSectionEdit(*content, meta->headings, from, target);
    if (!edit)
        return false;
    return replaceNoteText(s, filePath(), edit->from, edit->to, content->mid(edit->from, edit->to - edit->from), edit->text, QStringLiteral("move.section"));
}

} // namespace oc
