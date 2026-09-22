#pragma once

#include "core/search/search_document.h"
#include "core/search/search_query.h"
#include "core/util/match_range.h"

#include <atomic>
#include <optional>

namespace oc::search {

struct ContentMatch {
    int from = 0; // content offsets [from, to)
    int to = 0;
    int line = 0; // 0-based line of `from`
    bool operator==(const ContentMatch&) const = default;
};

struct PropertyMatch {
    QString key;
    QString value;                 // the matched value (list element) as text; empty for [key] terms and nulls
    QList<MatchRange> keyRanges;   // highlights in `key`
    QList<MatchRange> valueRanges; // highlights in `value`
    bool operator==(const PropertyMatch&) const = default;
};

struct SearchResult {
    qsizetype index = -1; // SearchEngine: index of the document in its SearchSource; -1 from matchDocument()
    QString path;
    QList<MatchRange> fileMatches;        // ranges in `path` hit by plain, file: and path: terms (sorted, merged)
    QList<ContentMatch> contentMatches;   // sorted by offset, overlapping hits merged
    QList<PropertyMatch> propertyMatches; // in document order of the properties
};

// Evaluates `query` against one document (Obsidian's semantics):
//  - a plain term matches the path without extension (folders and file name) or the content; file: the file name
//    with extension; path: the vault path; content: the content only;
//  - words and phrases are substring matches; regexes use JavaScript syntax (translated, see jsRegexToPcre);
//  - tag: compares the document's tags hierarchically (tag:#a matches #a and #a/b); inside line:/block:/... only
//    tags written in that region count;
//  - line:/block:/section:/task:/task-todo:/task-done: need one region in which the whole sub-query matches;
//    `-line:x` therefore means "no line matches x";
//  - [key] / [key:value]: key and value sub-queries are matched as text against each property (substring, like
//    Obsidian: [topic] also finds "parent_topic"); list values match when any element matches; numbers print like
//    JavaScript (5, 2.5), booleans as true/false; null/EMPTY only matches a property without a value (not "" or
//    []); > and < compare numerically when both sides are numbers, otherwise as strings (ISO dates work);
//  - documents without content (hasContent == false) only match when the query has a positive file:/path: term.
// Negated terms never produce highlights. Returns std::nullopt when the document does not match, or when
// `cancel` becomes true during the evaluation (checked every ~1k lines and between large scans).
std::optional<SearchResult> matchDocument(const SearchQuery& query, const SearchDocument& doc,
                                          const std::atomic<bool>* cancel = nullptr);

// Yes/no evaluation without collecting highlights (stops at the first hit of each term).
bool documentMatches(const SearchQuery& query, const SearchDocument& doc, const std::atomic<bool>* cancel = nullptr);

// How a (scalar) property value is matched as text: JS-like number formatting, "true"/"false", maps as JSON,
// nested lists as comma-separated elements; an invalid QVariant (null) gives an empty string.
QString propertyValueText(const QVariant& value);

} // namespace oc::search
