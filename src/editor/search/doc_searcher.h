#pragma once

#include "text/text.h"

#include <QList>
#include <QRegularExpression>

#include <functional>
#include <optional>
#include <utility>

namespace oc {

// What to look for in a document. Obsidian's in-file search is plain text and case-insensitive; the
// regex/case/whole-word options serve vim-style search.
struct SearchQuery {
    QString text;
    bool caseSensitive = false;
    bool regex = false;
    bool wholeWord = false;

    bool isEmpty() const { return text.isEmpty(); }
    bool operator==(const SearchQuery&) const = default;
};

struct SearchMatch {
    qsizetype from = 0;
    qsizetype to = 0;
    bool operator==(const SearchMatch&) const = default;
};

// Finds matches of a query in a Text. Matches never span lines (the query comes from a single-line
// input), are non-overlapping left to right within a line, and are never empty.
class DocSearcher {
public:
    explicit DocSearcher(SearchQuery query);

    const SearchQuery& query() const { return m_q; }
    bool isValid() const; // non-empty and (for regex) compiles

    // First match in `line` starting at or after column `from`: {start, end} columns.
    std::optional<std::pair<int, int>> matchInLine(QStringView line, int from) const;

    // First match with from >= pos; wraps around to the start of the document.
    std::optional<SearchMatch> next(const Text& text, qsizetype pos, bool wrap = true) const;
    // Last match ending at or before pos; wraps around to the last match of the document.
    std::optional<SearchMatch> previous(const Text& text, qsizetype pos, bool wrap = true) const;
    // Matches in document order, at most `limit` (-1 = all).
    QList<SearchMatch> all(const Text& text, int limit = -1) const;
    // Matches in lines [fromLine, toLine) as columns, for decorations.
    void forEachInLine(QStringView line, const std::function<void(int from, int to)>& fn) const;
    // {index, count}: index = matches starting at or before `current` (1-based position of the current
    // match, 0 when there is none); both stop counting at `cap` like Obsidian's counter.
    std::pair<int, int> indexAndCount(const Text& text, const std::optional<SearchMatch>& current, int cap = 9999) const;

private:
    bool wordBoundaryOk(QStringView line, int from, int to) const;

    SearchQuery m_q;
    QRegularExpression m_re;
};

} // namespace oc
