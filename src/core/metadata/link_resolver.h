#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace oc {

// Resolves link paths to vault files the way Obsidian's metadataCache.getFirstLinkpathDest() does,
// and generates link text for new links (Files & links > "New link format").
// Paths are vault-relative with '/' separators. Lookups are case-insensitive.
class LinkResolver {
public:
    enum class LinkFormat { Shortest, Relative, Absolute };

    void setFiles(const QStringList& paths);
    void addFile(const QString& path);
    void removeFile(const QString& path);
    bool contains(const QString& path) const { return m_byPath.contains(path.toLower()); }
    int fileCount() const { return int(m_byPath.size()); }

    // "Note#Heading" -> ("Note", "#Heading"); "#^id" -> ("", "#^id").
    static QPair<QString, QString> splitSubpath(const QString& link);

    // Resolve a link path (no subpath) from `sourcePath`. Returns the file path or "" (unresolved).
    // markdownLink: the link came from [text](dest) (tried relative to the source first).
    QString resolve(const QString& linkpath, const QString& sourcePath, bool markdownLink = false) const;
    // All candidates for a link path, best first (for disambiguation UIs and tests).
    QStringList candidates(const QString& linkpath, const QString& sourcePath) const;

    // Link text to use when linking from `sourcePath` to `targetPath` (Obsidian's fileToLinktext).
    // omitMdExtension: wikilinks drop ".md" from notes; markdown links keep it.
    QString linkText(const QString& targetPath, const QString& sourcePath, LinkFormat format, bool omitMdExtension = true) const;
    // Is `name` (basename for notes, file name otherwise) unique in the vault?
    bool isUniqueName(const QString& targetPath) const;

    static QString folderOf(const QString& path);
    static QString normalize(const QString& path); // resolve "." / ".." segments, strip leading "/"

private:
    static QString nameKey(const QString& path); // lowercase basename for .md, lowercase file name otherwise
    QHash<QString, QString> m_byPath;           // lowercase path -> path
    QHash<QString, QStringList> m_byName;       // nameKey -> paths
};

} // namespace oc
