#pragma once

// Obsidian's backlink / outgoing-link logic, minus the widgets: which ranges of a note mention another
// note (linked: links, embeds and property links that resolve to it; unlinked: its name or an alias
// as plain text), and how search-style results cut those ranges into snippets ("Show more context",
// the hover buttons that extend a snippet). Offsets are md::CachedMetadata offsets, i.e. into the
// note's '\n'-joined text (Text::toString()).

#include "core/util/match_range.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>

namespace oc::md {
struct CachedMetadata;
}

namespace oc {

// A letter or digit for Obsidian's whole-word mention rule (its FA + digit ranges). CJK ideographs,
// kana and hangul are not word characters there, so a name written in them matches anywhere.
bool isMentionWordChar(QChar c);

// Obsidian's mention pattern (VA): `name` case-insensitively, with no word character right before it
// when it starts with one, and none right after it when it ends with one. Occurrences in [from, to)
// (to < 0: the end), left to right, non-overlapping.
QList<MatchRange> findMentions(QStringView content, const QString& name, int from = 0, int to = -1);
bool isMentionAt(QStringView content, int pos, int length);

// Linked mentions (backlinks): the links, embeds and property links of `meta` whose link path
// resolves to the target (`resolvesToTarget(linkpath)`, the path without "#subpath"), sorted.
QList<MatchRange> linkedMentionRanges(const md::CachedMetadata& meta, const std::function<bool(const QString& linkpath)>& resolvesToTarget);

// Unlinked mentions of a note in `content` (another note, with metadata `meta`, may be null): every
// occurrence of any of `names` (the note's basename and aliases) that is neither inside a link / embed
// nor inside the frontmatter. Sorted by start; occurrences of different names may overlap.
QList<MatchRange> unlinkedMentionRanges(QStringView content, const QStringList& names, const md::CachedMetadata* meta);

// Outgoing-links "Unlinked mentions": other notes' names and aliases mentioned in `content` (the
// active note) outside links / embeds and after the frontmatter. `candidates` maps a lowercase name to
// the notes it names (a basename or alias; several notes may share it).
struct OutgoingMention {
    MatchRange range;
    QStringList targets; // vault paths
};
struct MentionCandidate {
    QString text;        // lowercase name
    QStringList targets; // vault paths, in insertion order
};
// Builds the candidate list like Obsidian: every markdown file except `self` contributes its basename
// and aliases (lowercased, merged), sorted by natural order of the text.
QList<MentionCandidate> mentionCandidates(const QList<QPair<QString, QStringList>>& filesWithAliases, const QString& self);
QList<OutgoingMention> outgoingUnlinkedMentions(QStringView content, const md::CachedMetadata* meta, const QList<MentionCandidate>& candidates);

// One snippet of a search-style result (.search-result-file-match): content[from, to) with the
// matches (absolute offsets) it shows, and whether "..." goes before / after it.
struct MatchSnippet {
    int from = 0;
    int to = 0;
    bool ellipsisBefore = false;
    bool ellipsisAfter = false;
    QList<MatchRange> matches;
};

// Obsidian's line context of a match: up to `maxChars` characters before its start and after its end
// within their lines; the ellipsis flags tell whether the cut hit the limit.
MatchSnippet lineSnippet(QStringView content, MatchRange match, int maxChars = 100);
// "Show more context": the match's list item (with its nested items), else its section, else its
// list block / paragraph (at most 1000 characters around it).
MatchSnippet extraContextSnippet(QStringView content, MatchRange match, const md::CachedMetadata* meta);
// Obsidian's renderContentMatches(): sorted matches cut into snippets. Unless `separate`, a snippet
// takes in the following matches that start inside it (one that runs past its end extends it and
// closes it).
QList<MatchSnippet> buildMatchSnippets(QStringView content, QList<MatchRange> matches, bool extraContext, const md::CachedMetadata* meta,
                                       bool separate = false);
// The hover buttons of a snippet: the new start / end after one step of more context (the enclosing
// list item or section, else the previous / next line).
int moreContextBefore(QStringView content, const md::CachedMetadata* meta, int start);
int moreContextAfter(QStringView content, const md::CachedMetadata* meta, int end);

// The "Link" button of an unlinked mention: `link` (FileManager::generateMarkdownLink with the
// mention as display text) replaces content[range]; inside a table the pipe is escaped like Obsidian.
QString mentionLinkText(const QString& link, bool inTable);
bool isInTable(const md::CachedMetadata* meta, MatchRange range);

} // namespace oc
