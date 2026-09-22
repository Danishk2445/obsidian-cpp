#pragma once

#include "core/util/match_range.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringView>

#include <functional>
#include <optional>

namespace oc {

class MetadataCache;
class Vault;

// Obsidian's editor suggests (the "[[", "#" and "[^" autocomplete popups), minus the widgets: when they
// trigger, what they suggest, and what choosing a suggestion inserts. All columns are UTF-16 offsets
// within one line of text.

struct SuggestTrigger {
    int start = 0; // first column of the replaced range
    int end = 0;   // the cursor column
    QString query;
};

// "[[query" before the cursor with no "]" after the "[[" (the query may contain "#", "^", "|").
// No trigger after "[[image.png|" (the part after the pipe is an image size there).
std::optional<SuggestTrigger> linkSuggestTrigger(QStringView line, int cursor);
// "#query" preceded by the line start or whitespace, the query free of punctuation/whitespace, and the
// cursor not directly before another "#". start is the column of the "#".
std::optional<SuggestTrigger> tagSuggestTrigger(QStringView line, int cursor);
// "[^query" (not "[[^") where the "]" that follows, if any, isn't followed by ":" (a footnote definition).
std::optional<SuggestTrigger> footnoteSuggestTrigger(QStringView line, int cursor);

struct LinkSuggestion {
    enum class Type { None, File, Alias, Linktext, Heading, Block };
    Type type = Type::None;
    QString file;    // vault path of the target file (File/Alias/Heading/Block; empty = unresolved)
    QString path;    // link path as it'll be written: file path without ".md", an unresolved link text,
                     // or the typed target for heading/block suggestions (empty = global/current file)
    QString alias;   // Alias: the alias (or the typed display text)
    QString subpath; // Heading: "#Heading#Sub"
    QString heading; // Heading: the heading text
    int level = 0;   // Heading: 1..6 (0 for the "create" placeholder)
    QString display; // Block: text of the block ("## Heading" for headings)
    QString blockId; // Block: existing block id (empty: one gets generated when chosen)
    int blockFrom = 0, blockTo = 0; // Block: [from, to) offsets of the block in `content`
    QString blockType;              // Block: "paragraph", "listItem", "heading", "code", ...
    int blockParagraphEnd = -1;     // Block (list items with sub-lists): end of the item's own text
    QString content;                // Block: the target file's text at suggestion time
    double score = 0;
    QList<MatchRange> matches;   // highlight ranges in the title (path/alias/heading/display)
    QList<MatchRange> idMatches; // Block: highlight ranges in the block id
    bool downranked = false;
};

// What choosing a link suggestion changes on the cursor's line (Obsidian's link replacement): the
// columns [start, end) are replaced with `replacement`, then the selection is [selectionStart, selectionEnd).
struct LinkReplacement {
    QString replacement;
    int start = 0;
    int end = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
    QString newBlockId; // non-empty when a block id has to be appended to the target block
};

// The "[[" link suggest (Obsidian's file/heading/block/display-text modes).
//   "" / "Note"            files, aliases and unresolved link texts (fuzzy; empty query: by mtime)
//   "Note#Head#Sub"        headings of Note (nested path), "#Head" = headings of the current file
//   "Note#^text" / "Note^" blocks of Note matching the words
//   "##text" / "^^text"    headings / blocks across the vault
//   "Note|text"            aliases of Note matching the text (or the typed text as display text)
class LinkSuggestProvider {
public:
    enum class Mode { File, Heading, Block, Display };
    // Reads a markdown file's current text (open document or disk); used for block suggestions.
    using ContentReader = std::function<std::optional<QString>(const QString& path)>;
    // Whether a non-markdown file can be linked (Obsidian: a registered view or showUnsupportedFiles).
    using SupportedPredicate = std::function<bool(const QString& extension)>;

    LinkSuggestProvider(const Vault* vault, const MetadataCache* cache);
    void setContentReader(ContentReader reader) { m_reader = std::move(reader); }
    void setSupportedPredicate(SupportedPredicate p) { m_supported = std::move(p); }

    // Suggestions for `query` (already trimmed by the caller the way Obsidian does), best first.
    // Never empty: a single None suggestion ("No match found") when nothing matches.
    QList<LinkSuggestion> suggestions(const QString& query, const QString& sourcePath);
    Mode mode() const { return m_mode; }
    bool isGlobal() const { return m_global; }
    // The popup's instruction row: ("Type #", "to link heading") ... in file mode, ("↵", "to accept") otherwise.
    QList<QPair<QString, QString>> instructions() const;
    void reset() { m_fileSuggestions.reset(); } // the popup closed: rebuild the file list next time

    // Obsidian's getLinkSuggestions(): every linkable file (markdown without ".md"), one entry per
    // alias, then unresolved link texts (≤ 500 chars) not matching a file.
    struct Entry {
        QString file; // empty for unresolved link texts
        QString path;
        QString alias;
        qint64 mtime = 0;
    };
    QList<Entry> linkSuggestionEntries() const;

private:
    QList<LinkSuggestion> fileSuggestions(const QString& query);
    QList<LinkSuggestion> headingSuggestions(const QString& target, const QString& subpath, const QString& sourcePath);
    QList<LinkSuggestion> globalHeadingSuggestions(const QString& query);
    QList<LinkSuggestion> blockSuggestions(const QString& target, const QString& query, const QString& sourcePath);
    QList<LinkSuggestion> globalBlockSuggestions(const QString& query, const QString& sourcePath);
    QList<LinkSuggestion> displaySuggestions(const QString& target, const QString& subpath, const QString& display,
                                             const QString& sourcePath);
    void appendBlocks(QList<LinkSuggestion>& out, const QString& file, const QString& path, const QStringList& words) const;

    const Vault* m_vault;
    const MetadataCache* m_cache;
    ContentReader m_reader;
    SupportedPredicate m_supported;
    std::optional<QList<Entry>> m_fileSuggestions;
    Mode m_mode = Mode::File;
    bool m_global = false;
};

// Link text for a target file as seen from sourcePath (fileToLinktext with the vault's link format).
using LinktextFn = std::function<QString(const QString& targetPath, const QString& sourcePath, bool omitMdExtension)>;

// Obsidian's link replacement for a chosen suggestion.
//   line: the whole line; linkStart: column of the "[[" (trigger start - 2); cursor: trigger end.
//   key: how it was chosen: "" (click/Enter), "Enter", "Tab", "#", "^" or "|".
//   useMarkdownLinks: write [text](path) instead of [[path]] (not for "#"/"^").
LinkReplacement linkSuggestReplacement(const LinkSuggestion& s, const QString& query, QStringView line, int linkStart, int cursor,
                                       const QString& sourcePath, const QString& key, bool useMarkdownLinks, const LinktextFn& linktext);

// Obsidian's block-id insertion for a chosen block without an id: where in the target file's content
// " ^id" (paragraphs, list items) or "\n\n^id" (other blocks) goes, and how many newlines that adds.
struct BlockIdInsertion {
    int at = 0;
    QString text;
    int newlines = 0;
};
BlockIdInsertion blockIdInsertion(const LinkSuggestion& block, const QString& id);
QString generateBlockId(); // 6 random hex digits

// Heading text as used in "#subpath" links (Obsidian strips ":#|^\\, %%, [[ ]]" and collapses spaces).
QString headingSubpathText(const QString& heading);

// Obsidian's getTags(): tag (with '#', the most frequent spelling of each case-insensitive tag) ->
// occurrences, counting every parent of a nested tag too ("#a/b" also counts for "#a").
QHash<QString, int> vaultTags(const MetadataCache& cache);

struct TagSuggestion {
    QString tag; // without '#'
    double score = 0;
    QList<MatchRange> matches;
};
QList<TagSuggestion> tagSuggestions(const MetadataCache& cache, const QString& query);

} // namespace oc
