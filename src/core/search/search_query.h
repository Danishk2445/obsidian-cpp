#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringMatcher>

#include <memory>
#include <optional>

namespace oc::search {

// Obsidian's global search query language (core "Search" plugin):
//   words are ANDed; `OR` (uppercase, between terms) has lower precedence than AND; `-x` negates; `( )`
//   groups; `"exact phrase"` (with \" and \\ escapes); `/regex/` (JavaScript syntax, translated to PCRE2;
//   ^ and $ match at line boundaries); operators taking one argument (a word, phrase, regex or group; a
//   leading '-' negates inside the operator):
//     file: path: content: match-case: ignore-case: tag: line: block: section: task: task-todo: task-done:
//   and property terms [prop], [prop:value], [prop:>5], [prop:<5], [prop:null] (alias EMPTY: no value),
//   [prop:true] / [prop:false]; both property and value accept sub-queries ([status:Draft OR Published]).
//
// A plain term matches the file's base name or its content (for notes; other files are only reachable
// through file:/path:), case-insensitively unless the global "Match case" option or match-case: applies.

enum class NodeKind : quint8 {
    And,      // children: all must match
    Or,       // children: any must match
    Not,      // children[0]
    Text,     // a word                     (text, matcher)
    Phrase,   // "exact text"               (text, matcher)
    Regex,    // /source/                   (text = source, regex)
    Empty,    // an empty operator argument: matches everything in its scope
    Scope,    // file:/path:/content:/tag:  (scope, children[0])
    Region,   // line:/block:/section:/task:/task-todo:/task-done:  (region, children[0])
    Case,     // match-case:/ignore-case:   (caseSensitive, children[0]); leaves below carry the resolved case
    Property, // [key] / [key:value]        (children[0] = key query or null for any key, children[1] = value or null)
    Compare,  // property value > or <      (compareOp, text = operand, number when numeric)
    IsNull,   // property value is null ("null"/"EMPTY")
    IsTrue,   // property value is true
    IsFalse,  // property value is false
};

// What a leaf is matched against (fixed at parse time from its enclosing operators).
enum class Scope : quint8 {
    Default,       // file base name or content
    FileName,      // file:  the file name with extension
    Path,          // path:  the vault path
    Content,       // content: (and everything under line:/block:/section:/task*:)
    Tag,           // tag:   the document's tags (#a matches #a and #a/b)
    PropertyKey,   // [key...]
    PropertyValue, // [...:value]
};

enum class Region : quint8 { Line, Block, Section, Task, TaskTodo, TaskDone };

struct Node {
    NodeKind kind = NodeKind::And;
    QList<std::shared_ptr<const Node>> children;
    QString text;               // Text/Phrase: the term (unescaped); Regex: source; Compare: operand
    Scope scope = Scope::Default;   // leaves: effective scope; Scope nodes: the scope they introduce
    Region region = Region::Line;   // Region nodes
    bool caseSensitive = false;     // leaves: resolved sensitivity; Case nodes: the requested one
    bool caseExplicit = false;      // leaves: set by an enclosing match-case:/ignore-case:
    char compareOp = 0;             // Compare: '<' or '>'
    std::optional<double> number;   // Compare: numeric operand
    QString tagName;                // Text/Phrase in Tag scope: tag without '#', folded unless case-sensitive
    QStringMatcher matcher;         // Text/Phrase: compiled with the resolved case sensitivity
    QRegularExpression regex;       // Regex: compiled (invalid patterns never match)

    bool isLeaf() const
    {
        return kind == NodeKind::Text || kind == NodeKind::Phrase || kind == NodeKind::Regex || kind == NodeKind::Empty
            || kind == NodeKind::Compare || kind == NodeKind::IsNull || kind == NodeKind::IsTrue || kind == NodeKind::IsFalse;
    }
};

using NodePtr = std::shared_ptr<const Node>;

struct SearchOptions {
    bool matchCase = false; // the search view's "Match case" toggle (default for all terms)
};

// An immutable, parsed query; cheap to copy and safe to share between threads.
class SearchQuery {
public:
    SearchQuery() = default;

    bool isEmpty() const { return !m_root; } // no terms: matches nothing
    const NodePtr& root() const { return m_root; }
    const QString& source() const { return m_source; }
    const SearchOptions& options() const { return m_options; }
    // First problem found while parsing (unbalanced parenthesis/bracket/quote, invalid regex); the query is
    // still usable (lenient parse), invalid regexes simply never match.
    const QString& error() const { return m_error; }
    // True when a positive file:/path: term exists, i.e. non-note files (attachments) can match at all.
    bool canMatchNonNotes() const { return m_canMatchNonNotes; }
    // Compact S-expression of the tree, e.g. (and (text "a") (not (file (text "b")))); for tests/debugging.
    QString toDebugString() const;

private:
    friend SearchQuery parseSearchQuery(const QString&, const SearchOptions&, QString*);
    NodePtr m_root;
    QString m_source;
    QString m_error;
    SearchOptions m_options;
    bool m_canMatchNonNotes = false;
};

SearchQuery parseSearchQuery(const QString& query, const SearchOptions& options, QString* error = nullptr);
inline SearchQuery parseSearchQuery(const QString& query, QString* error = nullptr)
{
    return parseSearchQuery(query, SearchOptions{}, error);
}

// Human readable breakdown like the search view's "Explain search terms", one item per line, children
// indented by four spaces, e.g. for `meeting -file:draft`:
//   Match all of:
//       Matches text: "meeting"
//       Excluding: Match file name: Matches text: "draft"
QString explainSearchQuery(const SearchQuery& query);

} // namespace oc::search
