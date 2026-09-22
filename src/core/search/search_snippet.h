#pragma once

#include "core/search/search_match.h"

namespace oc::search {

struct SnippetOptions {
    int contextChars = 60; // characters kept before the first and after the last match, within the line
    int contextLines = 0;  // whole extra lines before and after (the search view's "Show more context")
    int maxSnippets = 0;   // 0 = no limit (the view shows the rest as "... and N more matches")
};

struct Snippet {
    int line = 0;                // 0-based line of the first character of `text`
    int from = 0;                // content range of `text`
    int to = 0;
    QString text;                // content[from, to); several lines with contextLines or multi-line matches
    bool truncatedStart = false; // `from` is inside a line: render a leading ellipsis
    bool truncatedEnd = false;   // `to` is inside a line: render a trailing ellipsis
    QList<MatchRange> matches;   // highlights relative to `text`
};

// Context snippets for a search result: the line of each match, cut to `contextChars` characters around the
// match (at word boundaries when possible, never inside a surrogate pair), or extended by `contextLines`
// whole lines. Matches whose windows overlap share one snippet. `matches` must be sorted by offset (as in
// SearchResult::contentMatches); their `line` fields are used for Snippet::line.
QList<Snippet> buildSnippets(QStringView content, const QList<ContentMatch>& matches, const SnippetOptions& options = {});

} // namespace oc::search
