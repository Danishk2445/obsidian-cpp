#include "core/search/search_query.h"

#include <QStringList>

namespace oc::search {
namespace {

struct Ctx {
    Scope scope = Scope::Default;
    bool caseSensitive = false;
    bool caseExplicit = false;
    bool inProperty = false;  // inside [...]: ']' ends terms; operators and nested properties are literal
    bool stopAtColon = false; // property key: ':' separates key and value
    int depth = 0;            // open '(' groups (within the current bracket level)
};

enum class Op { File, Path, Content, Tag, MatchCase, IgnoreCase, Line, Block, Section, Task, TaskTodo, TaskDone };

struct OpName {
    QLatin1StringView name;
    Op op;
};

const OpName kOperators[] = {
    {QLatin1StringView("file"), Op::File},
    {QLatin1StringView("path"), Op::Path},
    {QLatin1StringView("content"), Op::Content},
    {QLatin1StringView("tag"), Op::Tag},
    {QLatin1StringView("match-case"), Op::MatchCase},
    {QLatin1StringView("ignore-case"), Op::IgnoreCase},
    {QLatin1StringView("line"), Op::Line},
    {QLatin1StringView("block"), Op::Block},
    {QLatin1StringView("section"), Op::Section},
    {QLatin1StringView("task"), Op::Task},
    {QLatin1StringView("task-todo"), Op::TaskTodo},
    {QLatin1StringView("task-done"), Op::TaskDone},
};

bool isHex(QChar c)
{
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

bool isAsciiLetter(QChar c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }

class Parser {
public:
    Parser(QStringView s, const SearchOptions& options)
        : m_s(s)
        , m_options(options)
    {
    }

    NodePtr parseAll()
    {
        Ctx ctx;
        ctx.caseSensitive = m_options.matchCase;
        return parseOr(ctx); // at the top level nothing closes a term list: a stray ')' or ']' is just text
    }

    QString error;

private:
    bool atEnd() const { return m_pos >= m_s.size(); }
    QChar peek(qsizetype off = 0) const { return m_pos + off < m_s.size() ? m_s[m_pos + off] : QChar(); }
    void skipSpace()
    {
        while (!atEnd() && m_s[m_pos].isSpace())
            ++m_pos;
    }
    void setError(const QString& e)
    {
        if (error.isEmpty())
            error = e;
    }

    static bool isCloser(QChar c, const Ctx& ctx)
    {
        return (c == u')' && ctx.depth > 0) || (c == u']' && ctx.inProperty) || (c == u':' && ctx.stopAtColon);
    }
    bool atCloser(const Ctx& ctx) const { return !atEnd() && isCloser(m_s[m_pos], ctx); }

    // "OR" as a standalone word (it may touch a closing bracket or an opening parenthesis).
    bool atOrKeyword(const Ctx& ctx) const
    {
        if (!m_s.mid(m_pos).startsWith(QLatin1StringView("OR")))
            return false;
        const QChar n = peek(2);
        return n.isNull() || n.isSpace() || n == u'(' || isCloser(n, ctx);
    }

    static NodePtr combine(NodeKind kind, QList<NodePtr> items)
    {
        if (items.isEmpty())
            return nullptr;
        if (items.size() == 1)
            return items.first();
        auto n = std::make_shared<Node>();
        n->kind = kind;
        n->children = std::move(items);
        return n;
    }

    NodePtr parseOr(const Ctx& ctx)
    {
        QList<NodePtr> alternatives;
        while (true) {
            if (NodePtr a = parseAnd(ctx))
                alternatives.push_back(a);
            skipSpace();
            if (!atOrKeyword(ctx))
                break;
            m_pos += 2;
        }
        return combine(NodeKind::Or, std::move(alternatives));
    }

    NodePtr parseAnd(const Ctx& ctx)
    {
        QList<NodePtr> terms;
        while (true) {
            skipSpace();
            if (atEnd() || atCloser(ctx))
                break;
            if (atOrKeyword(ctx)) {
                if (!terms.isEmpty())
                    break;
                m_pos += 2; // a leading or repeated OR is ignored
                continue;
            }
            const qsizetype before = m_pos;
            if (NodePtr t = parseUnary(ctx))
                terms.push_back(t);
            if (m_pos == before)
                ++m_pos; // never loop without progress
        }
        return combine(NodeKind::And, std::move(terms));
    }

    NodePtr parseUnary(const Ctx& ctx)
    {
        if (peek() == u'-') {
            const QChar n = peek(1);
            if (!n.isNull() && !n.isSpace() && !isCloser(n, ctx)) {
                ++m_pos;
                NodePtr child = parseUnary(ctx);
                if (!child)
                    return nullptr;
                auto neg = std::make_shared<Node>();
                neg->kind = NodeKind::Not;
                neg->children = {child};
                return neg;
            }
        }
        return parsePrimary(ctx);
    }

    NodePtr parsePrimary(const Ctx& ctx)
    {
        const QChar c = peek();
        if (c == u'(')
            return parseGroup(ctx);
        if (c == u'"')
            return parsePhrase(ctx);
        if (c == u'/') {
            if (NodePtr r = tryParseRegex(ctx))
                return r;
        }
        if (!ctx.inProperty) {
            if (c == u'[' && peek(1) != u'[')
                return parseProperty(ctx);
            const QStringView rest = m_s.mid(m_pos);
            for (const OpName& op : kOperators) {
                if (rest.size() > op.name.size() && rest.startsWith(op.name) && rest[op.name.size()] == u':') {
                    m_pos += op.name.size() + 1;
                    return parseOperator(op.op, ctx);
                }
            }
        }
        return parseWord(ctx);
    }

    NodePtr parseGroup(const Ctx& ctx)
    {
        ++m_pos; // '('
        Ctx inner = ctx;
        inner.depth = ctx.depth + 1;
        NodePtr n = parseOr(inner);
        skipSpace();
        if (peek() == u')')
            ++m_pos;
        else
            setError(QStringLiteral("Missing closing parenthesis"));
        return n;
    }

    NodePtr parsePhrase(const Ctx& ctx)
    {
        ++m_pos; // '"'
        QString text;
        bool closed = false;
        while (!atEnd()) {
            const QChar c = m_s[m_pos];
            if (c == u'\\' && m_pos + 1 < m_s.size() && (m_s[m_pos + 1] == u'"' || m_s[m_pos + 1] == u'\\')) {
                text += m_s[m_pos + 1];
                m_pos += 2;
                continue;
            }
            if (c == u'"') {
                ++m_pos;
                closed = true;
                break;
            }
            text += c;
            ++m_pos;
        }
        if (!closed)
            setError(QStringLiteral("Missing closing quote"));
        if (ctx.scope == Scope::PropertyValue && (text.startsWith(u'>') || text.startsWith(u'<')))
            return makeCompare(text, ctx);
        return makeLeaf(NodeKind::Phrase, text, ctx);
    }

    // `/source/` where the closing slash ends the term; nullptr when there is none (the text is a plain word).
    NodePtr tryParseRegex(const Ctx& ctx)
    {
        qsizetype q = m_pos + 1;
        bool inClass = false;
        bool found = false;
        while (q < m_s.size()) {
            const QChar c = m_s[q];
            if (c == u'\\') {
                q += 2;
                continue;
            }
            if (inClass) {
                if (c == u']')
                    inClass = false;
            } else if (c == u'[') {
                inClass = true;
            } else if (c == u'/') {
                const QChar after = q + 1 < m_s.size() ? m_s[q + 1] : QChar();
                if (after.isNull() || after.isSpace() || isCloser(after, ctx)) {
                    found = true;
                    break;
                }
            }
            ++q;
        }
        if (!found || q == m_pos + 1)
            return nullptr; // no closing slash (e.g. "/usr/bin") or "//": plain text
        const QString source = m_s.mid(m_pos + 1, q - m_pos - 1).toString();
        m_pos = q + 1;
        auto n = std::make_shared<Node>();
        n->kind = NodeKind::Regex;
        n->text = source;
        n->scope = ctx.scope;
        n->caseSensitive = resolvedCase(ctx);
        n->caseExplicit = ctx.caseExplicit;
        QRegularExpression::PatternOptions opts = QRegularExpression::MultilineOption;
        if (!n->caseSensitive)
            opts |= QRegularExpression::CaseInsensitiveOption;
        n->regex = QRegularExpression(jsRegexToPcre(source), opts);
        if (n->regex.isValid())
            n->regex.optimize(); // compile (and JIT) now, before the query is shared with worker threads
        else
            setError(QStringLiteral("Invalid regular expression /%1/: %2").arg(source, n->regex.errorString()));
        return n;
    }

    NodePtr parseProperty(const Ctx& ctx)
    {
        ++m_pos; // '['
        Ctx keyCtx = ctx;
        keyCtx.scope = Scope::PropertyKey;
        keyCtx.inProperty = true;
        keyCtx.stopAtColon = true;
        keyCtx.depth = 0;
        NodePtr key = parseOr(keyCtx);
        if (!key)
            key = makeLeaf(NodeKind::Empty, QString(), keyCtx); // "[:value]": any property
        skipSpace();
        NodePtr value;
        if (peek() == u':') {
            ++m_pos;
            Ctx valueCtx = keyCtx;
            valueCtx.scope = Scope::PropertyValue;
            valueCtx.stopAtColon = false;
            value = parseOr(valueCtx);
            if (!value)
                value = makeLeaf(NodeKind::Empty, QString(), valueCtx); // "[key:]": any value
        }
        skipSpace();
        if (peek() == u']')
            ++m_pos;
        else
            setError(QStringLiteral("Missing closing bracket"));
        auto n = std::make_shared<Node>();
        n->kind = NodeKind::Property;
        n->children = {key, value};
        return n;
    }

    NodePtr parseOperator(Op op, const Ctx& ctx)
    {
        Ctx inner = ctx;
        auto wrap = std::make_shared<Node>();
        switch (op) {
        case Op::File:
        case Op::Path:
        case Op::Content:
        case Op::Tag:
            wrap->kind = NodeKind::Scope;
            wrap->scope = op == Op::File ? Scope::FileName : op == Op::Path ? Scope::Path : op == Op::Content ? Scope::Content : Scope::Tag;
            inner.scope = wrap->scope;
            break;
        case Op::MatchCase:
        case Op::IgnoreCase:
            wrap->kind = NodeKind::Case;
            wrap->caseSensitive = op == Op::MatchCase;
            inner.caseSensitive = wrap->caseSensitive;
            inner.caseExplicit = true;
            break;
        case Op::Line:
        case Op::Block:
        case Op::Section:
        case Op::Task:
        case Op::TaskTodo:
        case Op::TaskDone:
            wrap->kind = NodeKind::Region;
            wrap->region = op == Op::Line ? Region::Line
                : op == Op::Block         ? Region::Block
                : op == Op::Section       ? Region::Section
                : op == Op::Task          ? Region::Task
                : op == Op::TaskTodo      ? Region::TaskTodo
                                          : Region::TaskDone;
            inner.scope = Scope::Content;
            break;
        }
        NodePtr arg;
        const QChar c = peek();
        if (!c.isNull() && !c.isSpace() && !isCloser(c, ctx))
            arg = parseUnary(inner);
        if (!arg)
            arg = makeLeaf(NodeKind::Empty, QString(), inner);
        wrap->children = {arg};
        return wrap;
    }

    NodePtr parseWord(const Ctx& ctx)
    {
        const qsizetype start = m_pos;
        int localParens = 0;
        while (!atEnd()) {
            const QChar c = m_s[m_pos];
            if (c.isSpace())
                break;
            if (c == u')') {
                if (localParens > 0)
                    --localParens;
                else if (ctx.depth > 0)
                    break;
            } else if (c == u'(') {
                if (m_pos > start)
                    ++localParens; // "f(x)" keeps its parentheses
            } else if ((c == u']' && ctx.inProperty) || (c == u':' && ctx.stopAtColon)) {
                break;
            }
            ++m_pos;
        }
        if (m_pos == start)
            return nullptr;
        const QString word = m_s.mid(start, m_pos - start).toString();
        if (ctx.scope == Scope::PropertyValue) {
            if (word.compare(QLatin1StringView("null"), Qt::CaseInsensitive) == 0
                || word.compare(QLatin1StringView("empty"), Qt::CaseInsensitive) == 0)
                return makeLeaf(NodeKind::IsNull, word, ctx);
            if (word.compare(QLatin1StringView("true"), Qt::CaseInsensitive) == 0)
                return makeLeaf(NodeKind::IsTrue, word, ctx);
            if (word.compare(QLatin1StringView("false"), Qt::CaseInsensitive) == 0)
                return makeLeaf(NodeKind::IsFalse, word, ctx);
            if (word.startsWith(u'>') || word.startsWith(u'<'))
                return makeCompare(word, ctx);
        }
        return makeLeaf(NodeKind::Text, word, ctx);
    }

    // Tags are case-insensitive in Obsidian; only an explicit match-case: makes them sensitive.
    static bool resolvedCase(const Ctx& ctx) { return ctx.scope == Scope::Tag ? ctx.caseExplicit && ctx.caseSensitive : ctx.caseSensitive; }

    static NodePtr makeCompare(const QString& expr, const Ctx& ctx)
    {
        auto n = std::make_shared<Node>();
        n->kind = NodeKind::Compare;
        n->scope = ctx.scope;
        n->compareOp = char(expr[0].unicode());
        n->text = expr.mid(1).trimmed();
        n->caseSensitive = ctx.caseSensitive;
        n->caseExplicit = ctx.caseExplicit;
        bool ok = false;
        const double v = n->text.toDouble(&ok);
        if (ok)
            n->number = v;
        return n;
    }

    static NodePtr makeLeaf(NodeKind kind, const QString& text, const Ctx& ctx)
    {
        auto n = std::make_shared<Node>();
        n->kind = kind;
        n->text = text;
        n->scope = ctx.scope;
        n->caseSensitive = resolvedCase(ctx);
        n->caseExplicit = ctx.caseExplicit;
        if (kind == NodeKind::Text || kind == NodeKind::Phrase || kind == NodeKind::IsTrue || kind == NodeKind::IsFalse) {
            n->matcher = QStringMatcher(text, n->caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
            if (ctx.scope == Scope::Tag) {
                const QString t = text.startsWith(u'#') ? text.mid(1) : text;
                n->tagName = n->caseSensitive ? t : t.toCaseFolded();
            }
        }
        return n;
    }

    QStringView m_s;
    qsizetype m_pos = 0;
    SearchOptions m_options;
};

bool hasPositiveFileTerm(const NodePtr& n, bool negated)
{
    if (!n)
        return false;
    if (n->kind == NodeKind::Not)
        return hasPositiveFileTerm(n->children.value(0), !negated);
    if (n->kind == NodeKind::Property)
        return false;
    if (n->isLeaf())
        return !negated && (n->scope == Scope::FileName || n->scope == Scope::Path);
    for (const NodePtr& c : n->children)
        if (hasPositiveFileTerm(c, negated))
            return true;
    return false;
}

QString quoted(const QString& s)
{
    QString e = s;
    e.replace(u'\\', QLatin1StringView("\\\\"));
    e.replace(u'"', QLatin1StringView("\\\""));
    return u'"' + e + u'"';
}

QLatin1StringView scopeName(Scope s)
{
    switch (s) {
    case Scope::FileName:
        return QLatin1StringView("file");
    case Scope::Path:
        return QLatin1StringView("path");
    case Scope::Content:
        return QLatin1StringView("content");
    case Scope::Tag:
        return QLatin1StringView("tag");
    case Scope::Default:
    case Scope::PropertyKey:
    case Scope::PropertyValue:
        break;
    }
    return QLatin1StringView("scope");
}

QLatin1StringView regionName(Region r)
{
    switch (r) {
    case Region::Line:
        return QLatin1StringView("line");
    case Region::Block:
        return QLatin1StringView("block");
    case Region::Section:
        return QLatin1StringView("section");
    case Region::Task:
        return QLatin1StringView("task");
    case Region::TaskTodo:
        return QLatin1StringView("task-todo");
    case Region::TaskDone:
        return QLatin1StringView("task-done");
    }
    return QLatin1StringView("region");
}

QString dump(const NodePtr& n)
{
    if (!n)
        return QStringLiteral("*");
    const auto children = [&] {
        QStringList parts;
        for (const NodePtr& c : n->children)
            parts << dump(c);
        return parts.join(u' ');
    };
    switch (n->kind) {
    case NodeKind::And:
        return QStringLiteral("(and ") + children() + u')';
    case NodeKind::Or:
        return QStringLiteral("(or ") + children() + u')';
    case NodeKind::Not:
        return QStringLiteral("(not ") + children() + u')';
    case NodeKind::Text:
        return quoted(n->text);
    case NodeKind::Phrase:
        return QStringLiteral("(phrase ") + quoted(n->text) + u')';
    case NodeKind::Regex:
        return u'/' + n->text + u'/';
    case NodeKind::Empty:
        return QStringLiteral("(empty)");
    case NodeKind::Scope:
        return u'(' + scopeName(n->scope) + u' ' + children() + u')';
    case NodeKind::Region:
        return u'(' + regionName(n->region) + u' ' + children() + u')';
    case NodeKind::Case:
        return (n->caseSensitive ? QStringLiteral("(match-case ") : QStringLiteral("(ignore-case ")) + children() + u')';
    case NodeKind::Property: {
        QString s = QStringLiteral("(prop ") + dump(n->children.value(0));
        if (n->children.value(1))
            s += u' ' + dump(n->children.value(1));
        return s + u')';
    }
    case NodeKind::Compare:
        return u'(' + QChar(QLatin1Char(n->compareOp)) + u' ' + n->text + u')';
    case NodeKind::IsNull:
        return QStringLiteral("null");
    case NodeKind::IsTrue:
        return QStringLiteral("true");
    case NodeKind::IsFalse:
        return QStringLiteral("false");
    }
    return QString();
}

// ---- explain ---------------------------------------------------------------------------------------------

QString caseNote(const Node& n)
{
    if (n.caseSensitive)
        return QStringLiteral(" (Case sensitive)");
    if (n.caseExplicit)
        return QStringLiteral(" (Case insensitive)");
    return QString();
}

QStringList explainLines(const NodePtr& n);

QStringList indented(const QStringList& lines)
{
    QStringList out;
    out.reserve(lines.size());
    for (const QString& l : lines)
        out << QStringLiteral("    ") + l;
    return out;
}

// "Label: child" when the child is a single line, otherwise "Label:" followed by the indented child.
QStringList labelled(const QString& label, const QStringList& child)
{
    if (child.size() == 1)
        return {label + child.first()};
    QStringList out{label.trimmed()};
    out << indented(child);
    return out;
}

QStringList explainLines(const NodePtr& n)
{
    if (!n)
        return {};
    switch (n->kind) {
    case NodeKind::And:
    case NodeKind::Or: {
        QStringList out{n->kind == NodeKind::And ? QStringLiteral("Match all of:") : QStringLiteral("Match any of:")};
        for (const NodePtr& c : n->children)
            out << indented(explainLines(c));
        return out;
    }
    case NodeKind::Not:
        return labelled(QStringLiteral("Excluding: "), explainLines(n->children.value(0)));
    case NodeKind::Text:
        return {QStringLiteral("Matches text: ") + quoted(n->text) + caseNote(*n)};
    case NodeKind::Phrase:
        return {QStringLiteral("Contains exact text: ") + quoted(n->text) + caseNote(*n)};
    case NodeKind::Regex:
        return {QStringLiteral("Matches regex: /") + n->text + u'/' + caseNote(*n)};
    case NodeKind::Empty:
        return {QStringLiteral("Matches text: \"\"")};
    case NodeKind::Scope: {
        const QString label = n->scope == Scope::FileName ? QStringLiteral("Match file name: ")
            : n->scope == Scope::Path                     ? QStringLiteral("Match file path: ")
            : n->scope == Scope::Content                  ? QStringLiteral("Match file content: ")
                                                          : QStringLiteral("Match tag: ");
        return labelled(label, explainLines(n->children.value(0)));
    }
    case NodeKind::Region: {
        static const char* const labels[] = {"Match line: ", "Match block: ", "Match section: ", "Match task: ",
                                             "Match task (todo): ", "Match task (done): "};
        return labelled(QString::fromLatin1(labels[int(n->region)]), explainLines(n->children.value(0)));
    }
    case NodeKind::Case:
        return explainLines(n->children.value(0)); // the leaves carry "(Case sensitive)" / "(Case insensitive)"
    case NodeKind::Property: {
        // Obsidian's label has no trailing space: the key query and the value query are listed below it.
        QStringList out{QStringLiteral("Contains property:")};
        out << indented(explainLines(n->children.value(0)));
        out << indented(explainLines(n->children.value(1)));
        return out;
    }
    case NodeKind::Compare:
        return {(n->compareOp == '>' ? QStringLiteral("Greater than: ") : QStringLiteral("Less than: ")) + n->text};
    case NodeKind::IsNull:
        return {QStringLiteral("Is empty")};
    case NodeKind::IsTrue:
        return {QStringLiteral("Is true")};
    case NodeKind::IsFalse:
        return {QStringLiteral("Is false")};
    }
    return {};
}

} // namespace

QString jsRegexToPcre(QStringView js)
{
    QString out;
    out.reserve(js.size() + 8);
    bool inClass = false;
    qsizetype i = 0;
    while (i < js.size()) {
        const QChar c = js[i];
        if (c == u'\\') {
            if (i + 1 >= js.size()) { // trailing backslash: a syntax error in both flavours
                out += c;
                ++i;
                continue;
            }
            const QChar d = js[i + 1];
            const char16_t du = d.unicode();
            if (du == u'u') { // \uXXXX; without the u flag "\u{41}" is 'u' repeated 41 times, a lone "\u" is 'u'
                const auto hex4 = [&](qsizetype at) -> int { // value of "\uXXXX" at `at`, or -1
                    if (at + 5 >= js.size() || js[at] != u'\\' || js[at + 1] != u'u')
                        return -1;
                    for (qsizetype k = at + 2; k < at + 6; ++k)
                        if (!isHex(js[k]))
                            return -1;
                    return js.mid(at + 2, 4).toInt(nullptr, 16);
                };
                const int unit = hex4(i);
                if (unit >= 0) {
                    const int low = QChar::isHighSurrogate(char32_t(unit)) ? hex4(i + 6) : -1;
                    if (low >= 0 && QChar::isLowSurrogate(char32_t(low))) { // a UTF-16 pair: one code point for PCRE
                        out += QStringLiteral("\\x{%1}").arg(uint(QChar::surrogateToUcs4(char16_t(unit), char16_t(low))), 0, 16);
                        i += 12;
                    } else {
                        out += QStringLiteral("\\x{") + js.mid(i + 2, 4) + u'}';
                        i += 6;
                    }
                } else {
                    out += u'u';
                    i += 2;
                }
                continue;
            }
            if (du == u'x') { // \xHH, otherwise 'x'
                if (i + 3 < js.size() && isHex(js[i + 2]) && isHex(js[i + 3])) {
                    out += js.mid(i, 4);
                    i += 4;
                } else {
                    out += u'x';
                    i += 2;
                }
                continue;
            }
            if (du == u'c') { // \cX control character; "\c" before a non-letter is a literal backslash + 'c'
                if (i + 2 < js.size() && isAsciiLetter(js[i + 2])) {
                    out += js.mid(i, 3);
                    i += 3;
                } else {
                    out += QStringLiteral("\\\\c");
                    i += 2;
                }
                continue;
            }
            if (isAsciiLetter(d)) {
                // Escapes JS and PCRE share; \k<name> is a named backreference, \p{..}/\P{..} Unicode properties
                // (only valid with the u flag in JS, kept for usefulness). Any other letter is a JS identity
                // escape, while PCRE gives many of them a meaning (\A \Z \Q \E \K \R \X \h \e \a \g \N ...).
                const bool shared = QStringView(u"dDwWsSfnrt").contains(d) || (!inClass && (du == u'b' || du == u'B'))
                    || (inClass && du == u'b');
                const bool namedRef = du == u'k' && i + 2 < js.size() && js[i + 2] == u'<';
                const bool property = (du == u'p' || du == u'P') && i + 2 < js.size() && js[i + 2] == u'{';
                if (du == u'v') {
                    out += QStringLiteral("\\x{0B}"); // PCRE's \v is a class of vertical whitespace
                } else if (shared || namedRef || property) {
                    out += c;
                    out += d;
                } else {
                    out += d;
                }
                i += 2;
                continue;
            }
            out += c; // digits (backreferences, \0), punctuation and non-ASCII: same meaning in PCRE
            out += d;
            i += 2;
            continue;
        }
        if (inClass) {
            if (c == u']')
                inClass = false;
            else if (c == u'[') { // literal in JS classes; PCRE would read "[:" as a POSIX class
                out += QStringLiteral("\\[");
                ++i;
                continue;
            }
            out += c;
            ++i;
            continue;
        }
        if (c == u'[') {
            if (js.mid(i).startsWith(u"[^]")) {
                out += QStringLiteral("[\\s\\S]"); // JS: any character
                i += 3;
                continue;
            }
            if (js.mid(i).startsWith(u"[]")) {
                out += QStringLiteral("(?!)"); // JS: matches nothing
                i += 2;
                continue;
            }
            inClass = true;
            out += c;
            ++i;
            if (i < js.size() && js[i] == u'^') { // keep "[^" together so a following ']' is not misread
                out += js[i];
                ++i;
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

SearchQuery parseSearchQuery(const QString& query, const SearchOptions& options, QString* error)
{
    Parser parser(query, options);
    SearchQuery q;
    q.m_source = query;
    q.m_options = options;
    q.m_root = parser.parseAll();
    q.m_error = parser.error;
    q.m_canMatchNonNotes = hasPositiveFileTerm(q.m_root, false);
    if (error)
        *error = parser.error;
    return q;
}

QString SearchQuery::toDebugString() const { return m_root ? dump(m_root) : QStringLiteral("(nothing)"); }

QString explainSearchQuery(const SearchQuery& query)
{
    if (query.isEmpty())
        return QString();
    return explainLines(query.root()).join(u'\n');
}

} // namespace oc::search
