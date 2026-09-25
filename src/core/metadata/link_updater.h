#pragma once

#include "core/metadata/link_resolver.h"
#include "markdown/metadata.h"
#include "text/text.h"

#include <QHash>
#include <QList>

#include <functional>

namespace oc {

class MetadataCache;

// Obsidian's "Automatically update internal links". Before a rename/move, snapshot() records every
// link, embed and property link together with what it resolves to; after the rename, edits() rewrites
// the links whose resolution changed so they point at the file's new location (in the configured
// link format), keeping subpaths, aliases and markdown-link titles like Obsidian does.
class LinkUpdater {
public:
    struct Reference {
        QString sourcePath;        // at snapshot time
        md::LinkCache link;
        QStringList resolvedPaths; // best first, at snapshot time (never empty)
    };
    struct Edit {
        qsizetype from = 0; // span of the link in the source text
        qsizetype to = 0;
        QString expected;   // what the span holds now (edits whose span changed are skipped)
        QString text;       // the rewritten link
    };
    // Where a pre-rename path lives now (identity for paths the rename didn't touch).
    using PathMap = std::function<QString(const QString& oldPath)>;
    static PathMap renameMap(const QString& oldPath, const QString& newPath); // file or folder

    using MetadataLookup = std::function<const md::CachedMetadata*(const QString& path)>;
    static QList<Reference> snapshot(const MetadataCache& cache);
    static QList<Reference> snapshot(const LinkResolver& resolver, QStringList markdownFiles, const MetadataLookup& metadata);
    // Edits per current source path, given the resolver state after the rename.
    static QHash<QString, QList<Edit>> edits(const QList<Reference>& refs, const LinkResolver& after, const PathMap& moved,
                                             LinkResolver::LinkFormat format);
    // Where a snapshotted reference should point now: a file path, or "" to leave it alone. Used when
    // content moves between notes (note composer), where resolution alone can't tell what moved.
    using Retarget = std::function<QString(const Reference& ref)>;
    static QHash<QString, QList<Edit>> retarget(const QList<Reference>& refs, const LinkResolver& after, const Retarget& target,
                                                LinkResolver::LinkFormat format);
    // The link's source text with its target replaced by `newLink` (Obsidian's rules for aliases,
    // display text, escaped pipes in tables, angle brackets and URL encoding).
    static QString rewrite(const md::LinkCache& link, const QString& newLink);
    // Apply edits (any order) to `text`, skipping those whose span no longer holds `expected`.
    // Returns the number applied.
    static int apply(Text& text, QList<Edit> edits);
    // Percent-encode what would break a markdown link destination (spaces, backslashes, controls).
    static QString encodeMarkdownDestination(const QString& dest);
    // All files a link path could mean from `sourcePath`, best first (getLinkpathDest).
    static QStringList resolveAll(const LinkResolver& resolver, const QString& linkpath, const QString& sourcePath, bool markdownLink);
};

} // namespace oc
