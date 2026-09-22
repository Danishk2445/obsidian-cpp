#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringMatcher>

#include <memory>
#include <optional>

namespace oc::search {

// Obsidian's global search query language (core "Search" plugin, https://help.obsidian.md/Plugins/Search):
//   - words are ANDed; `OR` (uppercase, between terms) binds looser than AND: `a b OR c` = (a AND b) OR c;
//   - `-x` negates a term, a group, an operator or a property; `( )` groups;
//   - `"exact phrase"` with \" and \\ escapes; `/regex/` in JavaScript syntax (translated to PCRE2; ^ and $
//     match at line boundaries; the closing slash must end the term, so `/usr/bin` stays a plain word);
//   - operators taking one argument (a word, phrase, regex, group or negated term; an empty argument matches
//     everything in the operator's scope, e.g. `task-todo:` finds files with open tasks):
//       file: path: content: match-case: ignore-case: tag: line: block: section: task: task-todo: task-done:
//   - property terms `[key]`, `[key:value]`, `[key:>5]`, `[key:<5]` (also quoted: `[key:"<5"]`), `[key:null]`
//     (alias `EMPTY`: the property exists without a value), `[key:true]`, `[key:false]`; key and value accept
//     sub-queries (`[status:Draft OR Published]`), inside brackets operators and nested brackets are literal.
//   - `[[` starts a plain word (searching for a wikilink), not a property.
//
// Matching semantics (see search_match.h): a plain term matches the file's path without extension (folders and
// file name) or its content; text terms and phrases are substring matches, case-insensitive unless the view's
// "Match case" option or match-case: applies; tags compare hierarchically (tag:#work matches #work and
// #work/sub, not #homework or #myjob/work) and are case-insensitive unless match-case: is explicit.
// The parser is lenient like Obsidian's: it never fails; unbalanced brackets/quotes are closed implicitly and
// reported through error().

enum class NodeKind : quint8 {
    And,      // children: all must match
    Or,       // children: any must match
    Not,      // children[0]
    Text,     // a word                     (text, matcher)
    Phrase,   // "exact text"               (text, matcher)
    Regex,    // /source/                   (text = JS source, regex)
    Empty,    // an empty operator argument: matches everything in its scope
    Scope,    // file:/path:/content:/tag:  (scope, children[0])
    Region,   // line:/block:/section:/task:/task-todo:/task-done:  (region, children[0])
    Case,     // match-case:/ignore-case:   (caseSensitive, children[0]); leaves below carry the resolved case
    Property, // [key] / [key:value]        (children[0] = key query, children[1] = value query or null)
    Compare,  // property value > or <      (compareOp, text = operand, number when numeric)
    IsNull,   // property value is null ("null" / "EMPTY")
    IsTrue,   // property value is true
    IsFalse,  // property value is false
};

// What a leaf is matched against (fixed at parse time from its enclosing operators; the innermost wins).
enum class Scope : quint8 {
    Default,       // path without extension, or content
    FileName,      // file:  the file name with extension
    Path,          // path:  the vault path
    Content,       // content: (and everything under line:/block:/section:/task*:)
    Tag,           // tag:   the document's tags
    PropertyKey,   // [key...]
    PropertyValue, // [...:value]
};

enum class Region : quint8 { Line, Block, Section, Task, TaskTodo, TaskDone };

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
    NodeKind kind = NodeKind::And;
    QList<NodePtr> children;
    QString text;                 // Text/Phrase: the term (unescaped); Regex: JS source; Compare: operand
    Scope scope = Scope::Default; // leaves: effective scope; Scope nodes: the scope they introduce
    Region region = Region::Line; // Region nodes
    bool caseSensitive = false;   // leaves: resolved sensitivity; Case nodes: the requested one
    bool caseExplicit = false;    // leaves: set by an enclosing match-case:/ignore-case:
    char compareOp = 0;           // Compare: '<' or '>'
    std::optional<double> number; // Compare: numeric operand
    QString tagName;              // Text/Phrase in Tag scope: the tag without '#', case-folded unless case-sensitive
    QStringMatcher matcher;       // Text/Phrase: compiled with the resolved case sensitivity
    QRegularExpression regex;     // Regex: compiled (an invalid pattern never matches)

    bool isLeaf() const
    {
        return kind == NodeKind::Text || kind == NodeKind::Phrase || kind == NodeKind::Regex || kind == NodeKind::Empty
            || kind == NodeKind::Compare || kind == NodeKind::IsNull || kind == NodeKind::IsTrue || kind == NodeKind::IsFalse;
    }
};

struct SearchOptions {
    bool matchCase = false; // the search view's "Match case" toggle (the default for all terms)
};

// An immutable, parsed query; cheap to copy and safe to share between threads.
class SearchQuery {
public:
    SearchQuery() = default;

    bool isEmpty() const { return !m_root; } // no terms: matches nothing
    const NodePtr& root() const { return m_root; }
    const QString& source() const { return m_source; }
    const SearchOptions& options() const { return m_options; }
    // First problem found while parsing (unbalanced parenthesis/bracket/quote, invalid regex). The query is
    // still usable: groups are closed implicitly and invalid regexes never match.
    const QString& error() const { return m_error; }
    // True when a non-negated file:/path: term exists, i.e. files without searchable content (attachments) can
    // match at all. Obsidian only searches notes and canvases otherwise.
    bool canMatchNonNotes() const { return m_canMatchNonNotes; }
    // Compact S-expression of the tree for tests/debugging, e.g. (and "a" (not (file "b"))).
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

// The search view's "Explain search terms" text (Obsidian's English UI strings), one item per line, nested items
// indented by four spaces. A single-line operand is appended to its label. For `meeting -file:draft`:
//   Match all of:
//       Matches text: "meeting"
//       Excluding: Match file name: Matches text: "draft"
// Empty for an empty query.
QString explainSearchQuery(const SearchQuery& query);

// JavaScript regular expression source (non-unicode mode, as Obsidian compiles it) -> PCRE2 pattern with the same
// meaning: \uXXXX, \xHH, \v, [^] and [] are translated, JS identity escapes of letters that are special in PCRE
// (\A \Z \Q \E \K \R \X \h \e \a ...) become the plain letter, and '[' inside a class is escaped.
QString jsRegexToPcre(QStringView js);

} // namespace oc::search
