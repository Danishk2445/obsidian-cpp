#pragma once

#include "core/metadata/link_resolver.h"
#include "markdown/metadata.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace oc {

// Obsidian's app.metadataCache: per-file CachedMetadata for markdown files, link resolution, and the
// vault-wide link graph (resolvedLinks / unresolvedLinks: source -> {target -> count}) with a
// backlink index, tag and alias indexes. Parsing happens on a thread pool; resolution on the GUI thread.
class MetadataCache : public QObject {
    Q_OBJECT
public:
    using ContentProvider = std::function<std::optional<QByteArray>(const QString& path)>;

    explicit MetadataCache(QObject* parent = nullptr);
    ~MetadataCache() override;

    void setContentProvider(ContentProvider p) { m_provider = std::move(p); }

    // Initial full index. `files` = every vault file (for link resolution); markdown ones get parsed.
    void indexAll(const QStringList& files);
    bool isReady() const { return m_ready; }

    // Vault events (paths vault-relative).
    void fileCreated(const QString& path);
    void fileModified(const QString& path);
    void fileDeleted(const QString& path);
    void fileRenamed(const QString& oldPath, const QString& newPath);
    // Live text of an open, saved document (avoids re-reading the file).
    void updateFromText(const QString& path, const Text& text);

    const md::CachedMetadata* fileCache(const QString& path) const;
    QStringList markdownFiles() const { return m_cache.keys(); }
    const LinkResolver& resolver() const { return m_resolver; }
    QString firstLinkpathDest(const QString& link, const QString& sourcePath) const; // accepts "x#sub"
    bool isResolved(const QString& link, const QString& sourcePath) const { return !firstLinkpathDest(link, sourcePath).isEmpty(); }

    const QHash<QString, QHash<QString, int>>& resolvedLinks() const { return m_resolved; }
    const QHash<QString, QHash<QString, int>>& unresolvedLinks() const { return m_unresolved; }
    QStringList backlinksOf(const QString& path) const; // sources with a resolved link/embed to path
    int backlinkCount(const QString& path) const;

    QHash<QString, int> tagCounts() const;   // "#tag" -> occurrences across the vault (incl. frontmatter)
    QStringList filesWithTag(const QString& tag) const; // tag with '#', includes nested "#a/b" for "#a"
    QHash<QString, QStringList> aliases() const;        // file -> aliases

signals:
    void changed(const QString& path);  // metadata of `path` updated
    void deleted(const QString& path);
    void resolved();                    // link graph updated (after batches / events)

private:
    struct Result {
        QString path;
        md::CachedMetadata meta;
    };
    void parseAsync(const QStringList& paths);
    void applyResult(const QString& path, md::CachedMetadata meta);
    void resolveSource(const QString& source);
    void unresolveSource(const QString& source);
    static md::CachedMetadata parseBytes(const QByteArray& bytes);

    ContentProvider m_provider;
    LinkResolver m_resolver;
    QHash<QString, md::CachedMetadata> m_cache;
    QHash<QString, QHash<QString, int>> m_resolved;
    QHash<QString, QHash<QString, int>> m_unresolved;
    QHash<QString, QSet<QString>> m_backlinks;          // target -> sources
    QHash<QString, QSet<QString>> m_unresolvedByName;    // lowercase link basename -> sources
    int m_pending = 0;
    bool m_ready = false;
    quint64 m_generation = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace oc
