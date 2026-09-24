#pragma once

// The vault side of the graph views: building the vault-wide GraphData from the Vault and the
// MetadataCache the way Obsidian's graph engine does, and evaluating graph search / colour-group
// queries with the vault search (oc::search).

#include "core/search/search_query.h"
#include "graph/graph_data.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace oc {

class MetadataCache;
class Vault;
class VaultSession;

// Obsidian's attachment test for the graph: any extension other than md, canvas and base.
bool isGraphAttachment(const QString& path);

// The complete vault graph (finalized, weights = link counts), like Obsidian's graph data engine
// with every filter off:
//  - one node per vault file: notes, canvases and bases are File nodes, every other file an
//    Attachment node (shown only with "Attachments" on, orphans included); timestamps are
//    min(ctime, mtime) for the timelapse;
//  - resolvedLinks → links (to notes and attachments), unresolvedLinks → Unresolved nodes (id =
//    link path) and links;
//  - tags (frontmatter + inline) → Tag nodes ("#tag", in the vault's most frequent spelling)
//    linked from each file using them.
// Links from one file are added resolved, unresolved, then tags, each sorted (deterministic).
struct VaultGraphFile {
    QString path;
    qint64 timestamp = 0; // min(ctime, mtime), ms
};
graph::GraphData buildVaultGraph(const QList<VaultGraphFile>& files, const QHash<QString, QHash<QString, int>>& resolvedLinks,
                                 const QHash<QString, QHash<QString, int>>& unresolvedLinks,
                                 const QHash<QString, QStringList>& tags); // note -> tags with '#', in order
// The same from a vault and its metadata cache. `vault` may be null: the cache's notes are used.
graph::GraphData buildVaultGraph(const Vault* vault, const MetadataCache& metadata);
// buildVaultGraph() for the session's vault and metadata cache (empty without a metadata cache).
graph::GraphData graphDataFromSession(VaultSession& session);

// Graph search/colour-group queries run with the vault search, like Obsidian's graph (which runs
// the global search matcher over every file). compile() returns a predicate over the latest
// results of the query and starts an asynchronous vault search the first time a query is seen;
// updated() fires when results change (search progress, notes edited), after which the owner
// recompiles. Notes are matched with their content and metadata, attachments on their path
// (file:/path: terms), tags as a tiny document holding just the tag.
class GraphVaultSearch : public QObject {
    Q_OBJECT
public:
    explicit GraphVaultSearch(VaultSession* session, QObject* parent = nullptr);
    ~GraphVaultSearch() override;

    graph::NodePredicate compile(const QString& query);
    bool isSearching() const;
    // Blocks (processing events) until no search is running. Tests.
    bool waitForIdle(int timeoutMs = 10000);

signals:
    void updated();
    void searchingChanged(bool searching);

private:
    struct Entry;
    void start(const std::shared_ptr<Entry>& entry);
    void fileChanged(const QString& path);
    void fileDeleted(const QString& path);
    void scheduleUpdate();
    void updateSearching();

    VaultSession* m_session;
    QHash<QString, std::shared_ptr<Entry>> m_entries;
    quint64 m_clock = 0;
    bool m_searching = false;
    QTimer m_notify;
};

} // namespace oc
