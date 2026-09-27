#pragma once

// What Bases reads from the vault (Obsidian's app.vault + app.metadataCache as the formula values
// use them): the files, their stats, their CachedMetadata, link resolution and backlinks.
// VaultDataSource reads a live session (GUI thread); MemoryDataSource holds notes in memory
// (tests, previews).

#include "markdown/metadata.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace oc {
class VaultSession;
}

namespace oc::bases {

struct FileStat {
    QString path;      // "Folder/Note.md"
    QString name;      // "Note.md"
    QString basename;  // "Note"
    QString extension; // "md" (case preserved)
    QString folder;    // "Folder" ("/" for the root, like Obsidian's parent.path)
    qint64 size = 0;
    qint64 ctime = 0; // ms since epoch
    qint64 mtime = 0;
};

class DataSource {
public:
    virtual ~DataSource() = default;
    virtual QStringList files() const = 0; // every file of the vault (not folders)
    virtual std::optional<FileStat> stat(const QString& path) const = 0;
    virtual const md::CachedMetadata* metadata(const QString& path) const = 0; // markdown files only
    // getFirstLinkpathDest(linkpath, sourcePath): the file path, "" when unresolved. `linkpath` has
    // no subpath.
    virtual QString resolveLink(const QString& linkpath, const QString& sourcePath) const = 0;
    virtual QStringList backlinks(const QString& path) const = 0; // sources with a resolved link to `path`
    // fileToLinktext(): the shortest link text for `path` from `sourcePath` ("Note", "Folder/Note").
    virtual QString linkText(const QString& path, const QString& sourcePath) const = 0;
    // Excluded files (Settings → Files and links → Excluded files).
    virtual bool isIgnored(const QString& path) const { Q_UNUSED(path); return false; }

    static FileStat makeStat(const QString& path, qint64 size, qint64 ctime, qint64 mtime);
};

// The live vault of a session (Vault + MetadataCache). `ignored` = the Excluded files filter.
class VaultDataSource : public DataSource {
public:
    explicit VaultDataSource(VaultSession* session, std::function<bool(const QString&)> ignored = {});
    QStringList files() const override;
    std::optional<FileStat> stat(const QString& path) const override;
    const md::CachedMetadata* metadata(const QString& path) const override;
    QString resolveLink(const QString& linkpath, const QString& sourcePath) const override;
    QStringList backlinks(const QString& path) const override;
    QString linkText(const QString& path, const QString& sourcePath) const override;
    bool isIgnored(const QString& path) const override;
    VaultSession* session() const { return m_session; }

private:
    VaultSession* m_session;
    std::function<bool(const QString&)> m_ignored;
};

// Notes in memory: markdown text is parsed into CachedMetadata; links resolve by path / basename.
class MemoryDataSource : public DataSource {
public:
    void addFile(const QString& path, const QString& content = {}, qint64 ctime = 0, qint64 mtime = 0);
    void removeFile(const QString& path);
    QStringList files() const override { return m_order; }
    std::optional<FileStat> stat(const QString& path) const override;
    const md::CachedMetadata* metadata(const QString& path) const override;
    QString resolveLink(const QString& linkpath, const QString& sourcePath) const override;
    QStringList backlinks(const QString& path) const override;
    QString linkText(const QString& path, const QString& sourcePath) const override;

private:
    struct File {
        FileStat stat;
        std::optional<md::CachedMetadata> meta;
    };
    QHash<QString, File> m_files;
    QStringList m_order;
};

} // namespace oc::bases
