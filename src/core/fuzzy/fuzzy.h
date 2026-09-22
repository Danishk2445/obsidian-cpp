#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <optional>
#include <vector>

namespace oc {

// A [from, to) range of UTF-16 indices, for highlighting.
struct MatchRange {
    int from = 0;
    int to = 0;
    bool operator==(const MatchRange&) const = default;
};

// Result of FuzzyQuery::match(). Higher scores are better; scores are only comparable between results of
// the same query. `matches` are the matched UTF-16 indices in the target, ascending.
struct FuzzyResult {
    double score = 0;
    QList<int> matches;
};

// Obsidian's prepareFuzzySearch() equivalent, used by the quick switcher, the command palette and
// suggesters. Case-insensitive (simple Unicode case folding); whitespace in the query is ignored, every
// other query character must appear in the target in order.
//
// Scoring is fzf's "v2" algorithm (a Smith-Waterman style DP over the characters between the first possible
// and the last possible match): each match scores 16, gaps cost 3 to open and 1 per extra character,
// consecutive matches inherit the bonus of their run's first character, and characters get bonuses for
// starting a word: after whitespace or at the start (10), after '/' (9), after other separators such as
// '-', '_', '.' (8), camelCase humps and digits after letters (7). The first query character's bonus
// counts double. The best-scoring alignment is backtracked for the highlight indices. Shorter targets are
// slightly preferred (-0.01 per character, capped at 200 characters).
//
// Matching 10k paths costs well under 5 ms: a folded in-order scan rejects most targets before the DP,
// which only runs over the minimal window that can contain the match. Thread-safe (const methods only).
class FuzzyQuery {
public:
    FuzzyQuery() = default;
    explicit FuzzyQuery(const QString& query);

    const QString& query() const { return m_query; }
    bool isEmpty() const { return m_chars.empty(); } // an empty query matches everything with score 0

    std::optional<FuzzyResult> match(QStringView text) const;
    // For vault paths ("Folder/Sub/Name.md", or without the .md extension as the quick switcher shows them):
    // characters matched inside the file name (after the last '/') earn a +2 bonus each, so the same query
    // prefers hits in the basename over hits in folder names.
    std::optional<FuzzyResult> matchPath(QStringView path) const;

private:
    std::optional<FuzzyResult> matchImpl(QStringView text, qsizetype basenameStart) const;

    QString m_query;
    std::vector<char16_t> m_chars; // case-folded, whitespace removed
};

// Merges ascending match indices into ranges (adjacent indices form one range).
QList<MatchRange> matchIndicesToRanges(const QList<int>& indices);

struct SimpleSearchResult {
    double score = 0;
    QList<MatchRange> matches; // sorted, non-overlapping
};

// Obsidian's prepareSimpleSearch() equivalent: every whitespace-separated word of the query must occur in
// the target as a case-insensitive substring. `matches` holds the first occurrence of each word (merged).
// The score favours occurrences at word starts and near the beginning of the target (higher is better).
class SimpleSearch {
public:
    SimpleSearch() = default;
    explicit SimpleSearch(const QString& query);

    bool isEmpty() const { return m_words.isEmpty(); } // an empty query matches everything with score 0
    const QStringList& words() const { return m_words; }

    std::optional<SimpleSearchResult> match(QStringView text) const;

private:
    QStringList m_words;
};

} // namespace oc
