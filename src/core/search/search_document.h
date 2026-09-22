#pragma once

#include <QList>
#include <QString>
#include <QStringView>
#include <QVariant>

namespace oc::md {
struct CachedMetadata;
}

namespace oc::search {

// A [from, to) range of UTF-16 offsets into SearchDocument::content.
struct TextRange {
    int from = 0;
    int to = 0;
    bool operator==(const TextRange&) const = default;
};

struct SearchTag {
    QString tag;   // "#work/sub" (the leading '#' is optional)
    int line = -1; // 0-based content line; -1 when the tag is not written in the body (frontmatter list)
    int from = -1; // content offsets of the tag text including '#', or -1 when unknown
    int to = -1;
};

struct SearchTask {
    TextRange range; // the task item (its first line through its last continuation line)
    QChar status;    // the character between the brackets: ' ' = todo, anything else = done (Obsidian's rule)
};

struct SearchProperty {
    QString key;
    // Frontmatter value with md::FrontmatterValue's conventions: an invalid/null QVariant for "no value", or
    // bool, double, QString, QVariantList (a list property), QVariantMap.
    QVariant value;
};

// What global search needs to know about one vault file. Build it with fromMetadata() for notes that have a
// metadata cache entry, or fill it directly (tests, canvases: their extracted text as content).
struct SearchDocument {
    QString path;           // vault path, e.g. "Folder/Note.md"
    bool hasContent = true; // notes and canvases; other files (attachments) only match file:/path: terms
    QString content;        // searched text, '\n' line breaks (offsets below refer to it)
    QList<SearchTag> tags;
    // Heading-delimited sections ("text between two headings", each including its heading line). Empty: the
    // whole content is one section.
    QList<TextRange> sections;
    // Blocks for block: (paragraphs, list items, code blocks, tables...). Empty: paragraphs separated by blank lines.
    QList<TextRange> blocks;
    QList<SearchTask> tasks;
    QList<SearchProperty> properties;

    QStringView name() const;         // "Note.md"
    QStringView basename() const;     // "Note"
    QStringView pathNoExtension() const; // "Folder/Note": what a plain term matches besides the content
    qsizetype nameOffset() const;     // index of name() in path

    // Notes: content = the note text ('\n'-joined, as md::CachedMetadata offsets expect), tags = inline tags with
    // positions plus frontmatter tags (line -1), sections from the headings, blocks from the metadata sections
    // (list sections split into their items, frontmatter excluded), tasks from the list items, properties from
    // the frontmatter.
    static SearchDocument fromMetadata(const QString& path, const QString& content, const md::CachedMetadata& meta);
};

} // namespace oc::search
