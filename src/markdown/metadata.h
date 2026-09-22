#pragma once

// Obsidian's CachedMetadata (shapes follow the MIT-licensed obsidian.d.ts), extracted from the parse
// tree. Positions: line/col are 0-based (col in UTF-16 units), offset is the document offset.

#include "markdown/syntax_tree.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>

#include <optional>

namespace oc::md {

struct Pos {
    int line = 0;
    int col = 0;
    qsizetype offset = 0;
};
struct Range {
    Pos start;
    Pos end;
};

struct LinkCache {
    QString link;        // target + subpath, e.g. "Note#Heading" (markdown links: decoded destination)
    QString original;    // raw source text, e.g. "[[Note#Heading|alias]]"
    QString displayText; // alias / link text / derived display
    Range position;      // whole link
    bool embed = false;
    bool markdown = false; // [text](dest) style
};

struct TagCache {
    QString tag; // with '#'
    Range position;
};

struct HeadingCache {
    QString heading;
    int level = 1;
    Range position;
};

struct SectionCache {
    QString type; // "heading","paragraph","list","code","blockquote","callout","table","thematicBreak","html","math","comment","yaml","footnoteDefinition","definition"
    Range position;
    QString id;   // block id if any
};

struct ListItemCache {
    Range position;
    int parent = 0;     // line of the parent item, or -(first line of the list) for top-level items
    QChar task;         // null if not a task
    QString id;         // block id
};

struct BlockCache {
    QString id;
    Range position;
};

struct FootnoteCache {
    QString id;
    Range position;
};

struct FrontmatterValue {
    QString key;
    QVariant value; // QString / double / bool / QVariantList / QVariantMap / null QVariant
};

struct CachedMetadata {
    QList<LinkCache> links;
    QList<LinkCache> embeds;
    QList<TagCache> tags;
    QList<HeadingCache> headings;
    QList<SectionCache> sections;
    QList<ListItemCache> listItems;
    QHash<QString, BlockCache> blocks; // lowercase id -> block
    QList<FootnoteCache> footnotes;
    QList<FootnoteCache> footnoteRefs;
    std::optional<QList<FrontmatterValue>> frontmatter; // ordered
    std::optional<Range> frontmatterPosition;
    QList<LinkCache> frontmatterLinks; // links written in property values ("[[x]]")
    QString frontmatterError;          // YAML parse error, if any

    // Helpers mirroring Obsidian's API.
    QStringList allTags() const;       // frontmatter "tags"/"tag" + inline tags, with '#', in order
    QStringList aliases() const;       // frontmatter "aliases"/"alias"
    QVariant property(const QString& key) const; // case-insensitive key lookup
};

// Parse YAML frontmatter text (between the --- lines). Returns ordered key/values; error set on failure.
QList<FrontmatterValue> parseFrontmatter(const QString& yaml, QString* error);

// Build metadata for a parsed document. The tree must be complete (validUpTo() == lineCount()).
CachedMetadata extractMetadata(const SyntaxTree& tree, const Text& text);

} // namespace oc::md
