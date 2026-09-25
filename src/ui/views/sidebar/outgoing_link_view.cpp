#include "ui/views/sidebar/outgoing_link_view.h"

#include "core/files/file_manager.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/mentions.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/subpath.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>

namespace oc {

namespace {
enum NodeType { kHeader = 1, kLink, kMatch };

QString withoutMd(const QString& path)
{
    return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) ? path.chopped(3) : path;
}

QString folderOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}
} // namespace

OutgoingLinkView::OutgoingLinkView(WorkspaceLeaf* leaf, QWidget* parent)
    : LinkedFileView(leaf, parent)
    , m_tree(new TreeList(this))
{
    setObjectName(QStringLiteral("ocOutgoingLinkView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_tree);
    // .outgoing-link-pane: padding 12 12 32
    m_tree->setContentMargins(12, 12, 12, 32);

    TreeList::Handlers& h = m_tree->handlers();
    h.activate = [this](TreeList::Node* n, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool) {
        if (n->type == kHeader) {
            if (n->id == u"links")
                setLinksCollapsed(!m_linksCollapsed);
            else
                setUnlinkedCollapsed(!m_unlinkedCollapsed);
            emit stateChanged();
            return;
        }
        const bool newLeaf = button == Qt::MiddleButton || (mods & Qt::ControlModifier);
        if (n->type == kLink) {
            MarkdownView::openLinkText(workspace(), n->data.toString(), 0, filePath(), newLeaf);
        } else if (n->type == kMatch && m_content) {
            if (TFile* f = file()) {
                QJsonArray matches;
                for (const MatchRange& m : n->matches)
                    matches.append(QJsonArray{m.from, m.to});
                const QJsonObject eState{{QStringLiteral("match"), QJsonObject{{QStringLiteral("content"), *m_content}, {QStringLiteral("matches"), matches}}}};
                if (Workspace* ws = workspace())
                    ws->openFile(f, newLeaf ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No, eState);
            }
        }
    };
    h.contextMenu = [this](TreeList::Node* n, const QPoint& g) { showContextMenu(n, g); };
    h.hover = [this](TreeList::Node* n, const QRect& r) {
        if (n->type == kLink)
            hoverLink(this, QStringLiteral("outgoing-link"), n->data.toString(), filePath(), r, m_tree->viewport());
    };
    h.unhover = [this] { unhoverLink(this, m_tree->viewport()); };
    h.chip = [this](TreeList::Node* n, int chip) {
        if (n->type == kMatch)
            linkMention(n->data.toInt(), chip);
    };
    h.activateView = [this] {
        if (workspace() && this->leaf())
            workspace()->setActiveLeaf(this->leaf(), false);
    };

    m_update.setSingleShot(true);
    m_update.setInterval(50); // Obsidian's requestUpdate debounce
    connect(&m_update, &QTimer::timeout, this, &OutgoingLinkView::recompute);
    if (VaultSession* s = session()) {
        if (MetadataCache* mc = s->metadata()) {
            m_connections << connect(mc, &MetadataCache::changed, this, [this](const QString& path) {
                if (path == filePath())
                    m_update.start();
            });
            m_connections << connect(mc, &MetadataCache::resolved, this, [this] { m_update.start(); });
            m_connections << connect(mc, &MetadataCache::deleted, this, [this] { m_update.start(); });
        }
    }
    rebuild();
}

QString OutgoingLinkView::baseTitle() const { return tr("Outgoing links"); }
QString OutgoingLinkView::boundTitle() const { return tr("Outgoing links from %1"); }

QJsonObject OutgoingLinkView::state() const
{
    QJsonObject o = LinkedFileView::state();
    o.insert(QStringLiteral("linksCollapsed"), m_linksCollapsed);
    o.insert(QStringLiteral("unlinkedCollapsed"), m_unlinkedCollapsed);
    return o;
}

void OutgoingLinkView::setState(const QJsonObject& st)
{
    if (st.value(QStringLiteral("linksCollapsed")).isBool())
        m_linksCollapsed = st.value(QStringLiteral("linksCollapsed")).toBool();
    if (st.value(QStringLiteral("unlinkedCollapsed")).isBool())
        m_unlinkedCollapsed = st.value(QStringLiteral("unlinkedCollapsed")).toBool();
    LinkedFileView::setState(st);
    recompute();
}

void OutgoingLinkView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

void OutgoingLinkView::fileChanged() { recompute(); }

void OutgoingLinkView::leafActiveChanged(bool active) { m_tree->setLeafActive(active); }

void OutgoingLinkView::setLinksCollapsed(bool collapsed)
{
    if (collapsed == m_linksCollapsed)
        return;
    m_linksCollapsed = collapsed;
    recompute();
}

void OutgoingLinkView::setUnlinkedCollapsed(bool collapsed)
{
    if (collapsed == m_unlinkedCollapsed)
        return;
    m_unlinkedCollapsed = collapsed;
    recompute();
}

int OutgoingLinkView::unlinkedCount() const { return int(m_mentions.size()); }

void OutgoingLinkView::recompute()
{
    m_update.stop();
    m_links.clear();
    m_mentions.clear();
    m_content.reset();
    VaultSession* s = session();
    MetadataCache* mc = s ? s->metadata() : nullptr;
    Vault* vault = s ? s->vault() : nullptr;
    const QString source = filePath();
    const md::CachedMetadata* meta = mc && source.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) ? mc->fileCache(source) : nullptr;
    if (meta && !m_linksCollapsed) {
        // Obsidian: every link, embed and property link by position, one row per destination.
        struct Entry {
            QString link;
            qsizetype pos;
        };
        QList<Entry> entries;
        for (const QList<md::LinkCache>* list : {&meta->frontmatterLinks, &meta->links, &meta->embeds})
            for (const md::LinkCache& l : *list)
                entries.push_back({l.link, list == &meta->frontmatterLinks ? 0 : l.position.start.offset});
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.pos < b.pos; });
        QSet<QString> seen;
        for (const Entry& e : std::as_const(entries)) {
            auto [path, subpath] = LinkResolver::splitSubpath(e.link);
            const QString dest = mc->firstLinkpathDest(path, source);
            if (dest.isEmpty())
                path = withoutMd(path);
            const QString key = (dest.isEmpty() ? path : dest) + subpath;
            if (seen.contains(key))
                continue;
            seen.insert(key);
            LinkRow row;
            row.linktext = e.link;
            row.file = dest;
            row.title = path;
            if (dest.isEmpty()) {
                row.icon = QStringLiteral("file-plus");
                row.unresolved = true;
            } else if (!subpath.isEmpty()) {
                const md::CachedMetadata* target = mc->fileCache(dest);
                const std::optional<SubpathRange> r = target ? resolveSubpath(*target, subpath) : std::nullopt;
                row.subtext = path;
                if (r && r->type == SubpathRange::Type::Heading) {
                    row.icon = QStringLiteral("heading");
                    for (const md::HeadingCache& hc : target->headings)
                        if (hc.position.start.line == r->startLine)
                            row.title = hc.heading;
                } else if (r && r->type == SubpathRange::Type::Block) {
                    row.icon = QStringLiteral("layout-list");
                    row.title = subpath.mid(subpath.indexOf(u'^') + 1);
                } else {
                    row.icon = QStringLiteral("file-question");
                    row.title = subpath;
                }
            } else {
                row.icon = QStringLiteral("link");
                row.subtext = folderOf(dest);
            }
            m_links << row;
        }
    }
    if (meta && !m_unlinkedCollapsed && vault) {
        if (std::optional<QString> text = readNoteText(s, source)) {
            m_content = std::make_shared<const QString>(*text);
            QList<QPair<QString, QStringList>> files;
            for (TFile* f : vault->markdownFiles()) {
                const md::CachedMetadata* m = mc->fileCache(f->path());
                files.push_back({f->path(), m ? m->aliases() : QStringList()});
            }
            for (const OutgoingMention& om : outgoingUnlinkedMentions(*m_content, meta, mentionCandidates(files, source)))
                m_mentions.push_back({om.range, om.targets});
        }
    }
    rebuild();
}

void OutgoingLinkView::rebuild()
{
    std::vector<TreeList::NodePtr> roots;
    const QString source = filePath();
    if (!source.isEmpty()) {
        auto links = std::make_unique<TreeList::Node>();
        links->kind = TreeList::Kind::Header;
        links->type = kHeader;
        links->id = QStringLiteral("links");
        links->title = tr("Links");
        links->collapsed = m_linksCollapsed;
        links->tooltip = m_linksCollapsed ? tr("Click to expand") : tr("Click to collapse");
        links->selectable = links->draggable = false;
        if (!m_linksCollapsed) {
            links->flair = QString::number(m_links.size());
            links->spaceAfter = 4;
            for (const LinkRow& row : std::as_const(m_links)) {
                TreeList::Node* n = links->add(TreeList::Kind::Item, QStringLiteral("link:%1").arg(row.file.isEmpty() ? row.linktext : row.file + row.linktext));
                n->type = kLink;
                n->data = row.linktext;
                n->icon = row.icon;
                n->title = row.title;
                n->subtext = row.subtext;
                n->faint = row.unresolved;
                n->tooltip = row.unresolved ? tr("Not created yet") : QString();
                n->selectable = n->draggable = false;
            }
        }
        roots.push_back(std::move(links));

        auto unlinked = std::make_unique<TreeList::Node>();
        unlinked->kind = TreeList::Kind::Header;
        unlinked->type = kHeader;
        unlinked->id = QStringLiteral("unlinked");
        unlinked->title = tr("Unlinked mentions");
        unlinked->collapsed = m_unlinkedCollapsed;
        unlinked->tooltip = m_unlinkedCollapsed ? tr("Click to expand") : tr("Click to collapse");
        unlinked->selectable = unlinked->draggable = false;
        if (!m_linksCollapsed)
            unlinked->spaceBefore = 16;
        if (!m_unlinkedCollapsed) {
            unlinked->flair = QString::number(m_mentions.size());
            unlinked->spaceAfter = 4;
            VaultSession* s = session();
            Vault* vault = s ? s->vault() : nullptr;
            for (int i = 0; i < m_mentions.size() && m_content; ++i) {
                const Mention& mention = m_mentions[i];
                const MatchSnippet sn = lineSnippet(*m_content, mention.range);
                TreeList::Node* m = unlinked->add(TreeList::Kind::Match);
                m->type = kMatch;
                m->data = i;
                m->content = m_content;
                m->from = sn.from;
                m->to = sn.to;
                m->ellipsisBefore = sn.ellipsisBefore;
                m->ellipsisAfter = sn.ellipsisAfter;
                m->matches = {mention.range};
                m->selectable = m->draggable = false;
                for (const QString& target : mention.targets) {
                    TFile* f = vault ? vault->file(target) : nullptr;
                    m->chips << (f ? f->basename() : withoutMd(target.mid(target.lastIndexOf(u'/') + 1)));
                    const QString tip = tr("Link this file");
                    m->chipTooltips << (target.contains(u'/') ? withoutMd(target) + QStringLiteral("\n\n") + tip : tip);
                }
            }
            // (no empty-state text: Obsidian's result for the note is there, just without matches)
        }
        roots.push_back(std::move(unlinked));
    }
    m_tree->setContentMargins(12, 12, 12, m_unlinkedCollapsed ? 32 : 48);
    m_tree->setRoots(std::move(roots));
}

bool OutgoingLinkView::linkMention(int matchIndex, int chip)
{
    VaultSession* s = session();
    if (!s || !m_content || matchIndex < 0 || matchIndex >= m_mentions.size())
        return false;
    const Mention mention = m_mentions[matchIndex];
    if (chip < 0 || chip >= mention.targets.size())
        return false;
    const QString source = filePath();
    const QString text = m_content->mid(mention.range.from, mention.range.to - mention.range.from);
    FileManager* fm = s->fileManager();
    QString link = fm ? fm->generateMarkdownLink(mention.targets[chip], source, QString(), text) : QStringLiteral("[[%1]]").arg(text);
    const md::CachedMetadata* meta = s->metadata() ? s->metadata()->fileCache(source) : nullptr;
    link = mentionLinkText(link, isInTable(meta, mention.range));
    if (!replaceNoteText(s, source, mention.range.from, mention.range.to, text, link))
        return false;
    m_mentions.removeAt(matchIndex);
    rebuild();
    return true;
}

void OutgoingLinkView::showContextMenu(TreeList::Node* n, const QPoint& globalPos)
{
    if (!n || n->type != kLink)
        return;
    QMenu menu(this);
    buildSectionedMenu(&menu, linkMenuEntries(workspace(), n->data.toString(), filePath(), this));
    if (menu.isEmpty())
        return;
    m_tree->setMenuNode(n);
    QPointer<OutgoingLinkView> self(this);
    menu.exec(globalPos);
    if (self)
        m_tree->setMenuNode(nullptr);
}

} // namespace oc
