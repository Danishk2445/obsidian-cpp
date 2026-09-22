// Obsidian-flavoured inline Markdown parser (md::parseInline), LeafContent mapping and
// WikiLinkInfo::displayText().
//
// One left-to-right pass in the style of cmark's inline parser:
//  - "atomic" tokens are recognised where they start (earliest start wins; at the same start the order
//    of the switch below is the precedence of inline_tree.h): escapes, code spans, %%comments%%,
//    [[wikilinks]] / ![[embeds]], $$display$$ / $inline$ math, <autolinks> / raw HTML, GFM bare URLs,
//    #tags, [^footnote] refs, the trailing ^block-id, entities, line breaks;
//  - '*' '_' runs and exactly-two '~~' '==' runs go on the CommonMark delimiter stack, '[' '![' '^['
//    on the bracket stack; ']' resolves inline / reference links and images (and inline footnotes)
//    immediately, emphasis is resolved with the CommonMark process_emphasis algorithm;
//  - GFM extended email autolinks are found afterwards in the remaining plain text (as cmark-gfm does),
//    outside links.
// Every node is a content range; the preorder tree is obtained by sorting ranges (outer first). No Text
// nodes are emitted: any content not covered by a node is plain text of its parent.
#include "markdown/inline_rules.h"
#include "markdown/inline_tree.h"

#include <QHash>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace oc::md {

namespace {

using namespace rules;

struct RawNode {
    InlineType type = InlineType::Text;
    MarkKind mark = MarkKind::None;
    qint32 from = 0;
    qint32 to = 0;
    qint32 data = -1;
    qint32 seq = 0;
    quint16 flags = 0;
    bool dead = false;
};

struct Delim {
    char16_t ch = 0;
    qint32 pos = 0;
    qint32 len = 0;    // original run length
    // Unused chars are [left, right): closing consumes from the left, opening from the right (a run
    // that both closes and opens is consumed from both ends).
    qint32 left = 0;
    qint32 right = 0;
    qint32 count() const { return right - left; }
    bool canOpen = false;
    bool canClose = false;
    qint32 prev = -1;  // doubly linked stack
    qint32 next = -1;
};

enum class BracketKind : quint8 { Link, Image, Footnote };

struct Bracket {
    BracketKind kind = BracketKind::Link;
    qint32 pos = 0;          // '[' / '!' / '^'
    qint32 textFrom = 0;     // first char of the link text
    qint32 delimBottom = -1; // delimiter stack top when pushed
    bool active = true;
    bool bracketAfter = false;
};

// ASCII characters that may start something; everything else is plain text.
constexpr std::array<bool, 128> kSpecial = [] {
    std::array<bool, 128> t{};
    for (char c : {'\\', '`', '%', '[', '!', ']', '$', '<', '#', '*', '_', '~', '=', '^', '&', '\n', 'w', 'h', 'H', 'f', 'F'})
        t[size_t(c)] = true;
    return t;
}();

class Parser {
public:
    Parser(const QString& content, const QList<LinkRefDefinition>& refs, InlineTree& tree)
        : m_str(content)
        , s(content.utf16())
        , n(qint32(content.size()))
        , m_refs(refs)
        , m_tree(tree)
    {
    }

    void run();

private:
    char16_t at(qint32 i) const { return i >= 0 && i < n ? char16_t(s[i]) : char16_t(0); }

    qint32 addNode(InlineType t, qint32 from, qint32 to, qint32 data = -1)
    {
        RawNode r;
        r.type = t;
        r.from = from;
        r.to = to;
        r.data = data;
        r.seq = qint32(m_nodes.size());
        m_nodes.push_back(r);
        return r.seq;
    }
    void addMark(MarkKind k, qint32 from, qint32 to)
    {
        if (to <= from)
            return;
        const qint32 id = addNode(InlineType::Mark, from, to);
        m_nodes[size_t(id)].mark = k;
    }

    void computeBlockId();
    qint32 onBackslash(qint32 i);
    qint32 onBacktick(qint32 i);
    qint32 onPercent(qint32 i);
    qint32 onOpenBracket(qint32 i);
    qint32 onBang(qint32 i);
    qint32 onCloseBracket(qint32 i);
    qint32 onDollar(qint32 i);
    qint32 onLessThan(qint32 i);
    qint32 onHash(qint32 i);
    qint32 onEmphasisRun(qint32 i);
    qint32 onDoubleRun(qint32 i);
    qint32 onCaret(qint32 i);
    qint32 onAmpersand(qint32 i);
    qint32 onNewline(qint32 i);
    qint32 onLetter(qint32 i);

    bool tryWikiLink(qint32 i, bool embed);
    void emitWikiLink(const WikiParts& p, bool embed);
    qint32 findBacktickCloser(qint32 len, qint32 from);
    void pushDelim(char16_t ch, qint32 pos, qint32 len);
    void removeDelim(qint32 d);
    void processEmphasis(qint32 bottom);
    void pushBracket(BracketKind kind, qint32 pos, qint32 textFrom);
    void popBracket();
    qint32 skipSpaces(qint32 i) const
    {
        while (i < n && (s[i] == u' ' || s[i] == u'\t' || s[i] == u'\n' || s[i] == u'\v' || s[i] == u'\f' || s[i] == u'\r'))
            ++i;
        return i;
    }
    const LinkRefDefinition* lookupRef(QStringView raw, QString* key);
    void detectEmails();
    void finish();

    const QString& m_str;
    const ushort* s;
    const qint32 n;
    const QList<LinkRefDefinition>& m_refs;
    InlineTree& m_tree;

    std::vector<RawNode> m_nodes;
    std::vector<Delim> m_delims;
    qint32 m_delimTop = -1;
    std::vector<Bracket> m_brackets;
    qint32 m_linkBrackets = 0; // Link/Image brackets on the stack (GFM: no bare URLs inside brackets)
    qint32 m_lastEnd = 0;      // end of the last node (lower bound for trailing whitespace)
    qint32 m_blockIdFrom = -1;
    qint32 m_blockIdTo = -1;

    struct TickList {
        std::vector<qint32> pos;
        size_t cursor = 0;
    };
    bool m_ticksBuilt = false;
    std::unordered_map<qint32, TickList> m_ticks;

    // "next position >= from matching a predicate" with a cursor; `from` must not decrease between calls.
    struct NextFinder {
        qint32 pos = -2; // -2: nothing searched yet, -1: none left
        template <class Pred>
        qint32 find(qint32 from, qint32 n, Pred pred)
        {
            if (pos == -1 || pos >= from)
                return pos;
            for (qint32 k = std::max(from, pos + 1); k < n; ++k) {
                if (pred(k)) {
                    pos = k;
                    return k;
                }
            }
            pos = -1;
            return -1;
        }
    };
    NextFinder m_nextWikiClose;   // "]]"
    NextFinder m_nextWikiOpen;    // "[["
    NextFinder m_nextNewline;     // '\n'
    qint32 m_noDisplayCloseFrom = -1; // no "$$" closer from this offset on
    HtmlScanMemo m_htmlMemo;

    bool m_refIndexBuilt = false;
    QHash<QString, qint32> m_refIndex;
};

void Parser::run()
{
    m_tree.mutableContent() = m_str;
    if (n == 0)
        return;
    m_nodes.reserve(size_t(std::min<qint32>(n / 4 + 8, 1 << 16)));
    computeBlockId();
    qint32 i = 0;
    while (i < n) {
        const char16_t c = s[i];
        if (c >= 128 || !kSpecial[c]) {
            ++i;
            continue;
        }
        switch (c) {
        case u'\\':
            i = onBackslash(i);
            break;
        case u'`':
            i = onBacktick(i);
            break;
        case u'%':
            i = onPercent(i);
            break;
        case u'[':
            i = onOpenBracket(i);
            break;
        case u'!':
            i = onBang(i);
            break;
        case u']':
            i = onCloseBracket(i);
            break;
        case u'$':
            i = onDollar(i);
            break;
        case u'<':
            i = onLessThan(i);
            break;
        case u'#':
            i = onHash(i);
            break;
        case u'*':
        case u'_':
            i = onEmphasisRun(i);
            break;
        case u'~':
        case u'=':
            i = onDoubleRun(i);
            break;
        case u'^':
            i = onCaret(i);
            break;
        case u'&':
            i = onAmpersand(i);
            break;
        case u'\n':
            i = onNewline(i);
            break;
        default:
            i = onLetter(i);
            break;
        }
    }
    processEmphasis(-1);
    if (std::find(s, s + n, u'@') != s + n)
        detectEmails();
    finish();
}

// " ^block-id" at the very end of the content (trailing whitespace allowed); the caret must start the
// content or follow whitespace.
void Parser::computeBlockId()
{
    qint32 e = n;
    while (e > 0 && isJsSpace(s[e - 1]))
        --e;
    qint32 b = e;
    while (b > 0 && (isAsciiAlnum(s[b - 1]) || s[b - 1] == u'-'))
        --b;
    if (b == e || b == 0 || s[b - 1] != u'^')
        return;
    const qint32 caret = b - 1;
    if (caret > 0 && !isJsSpace(s[caret - 1]))
        return;
    m_blockIdFrom = caret;
    m_blockIdTo = e;
}

qint32 Parser::onBackslash(qint32 i)
{
    const char16_t c = at(i + 1);
    if (c == u'\n') {
        qint32 k = i + 2;
        while (k < n && (s[k] == u' ' || s[k] == u'\t'))
            ++k;
        addNode(InlineType::HardBreak, i, k);
        m_lastEnd = k;
        return k;
    }
    if (c && isAsciiPunct(c)) {
        addNode(InlineType::Escape, i, i + 2);
        addMark(MarkKind::EscapeChar, i, i + 1);
        m_lastEnd = i + 2;
        return i + 2;
    }
    return i + 1;
}

qint32 Parser::findBacktickCloser(qint32 len, qint32 from)
{
    if (!m_ticksBuilt) {
        m_ticksBuilt = true;
        for (qint32 k = 0; k < n;) {
            if (s[k] != u'`') {
                ++k;
                continue;
            }
            const qint32 start = k;
            while (k < n && s[k] == u'`')
                ++k;
            m_ticks[k - start].pos.push_back(start);
        }
    }
    const auto it = m_ticks.find(len);
    if (it == m_ticks.end())
        return -1;
    TickList& tl = it->second;
    while (tl.cursor < tl.pos.size() && tl.pos[tl.cursor] < from)
        ++tl.cursor;
    return tl.cursor < tl.pos.size() ? tl.pos[tl.cursor] : -1;
}

qint32 Parser::onBacktick(qint32 i)
{
    qint32 k = i;
    while (k < n && s[k] == u'`')
        ++k;
    const qint32 len = k - i;
    const qint32 close = findBacktickCloser(len, k);
    if (close < 0)
        return k; // literal backticks
    addNode(InlineType::CodeSpan, i, close + len);
    addMark(MarkKind::CodeDelim, i, k);
    addMark(MarkKind::CodeDelim, close, close + len);
    m_lastEnd = close + len;
    return close + len;
}

qint32 Parser::onPercent(qint32 i)
{
    if (at(i + 1) != u'%')
        return i + 1;
    const qsizetype close = m_str.indexOf(QLatin1StringView("%%"), i + 2);
    if (close < 0)
        return i + 2;
    const qint32 end = qint32(close) + 2;
    addNode(InlineType::Comment, i, end);
    addMark(MarkKind::CommentDelim, i, i + 2);
    addMark(MarkKind::CommentDelim, qint32(close), end);
    m_lastEnd = end;
    return end;
}

// Fast pre-check with monotonic cursors (so runs of unclosed "[[" stay linear), then the exact rule.
bool Parser::tryWikiLink(qint32 i, bool embed)
{
    const qint32 innerFrom = i + (embed ? 3 : 2);
    if (innerFrom >= n || s[innerFrom] == u'\n')
        return false;
    const qint32 close = m_nextWikiClose.find(innerFrom + 1, n - 1, [this](qint32 k) { return s[k] == u']' && s[k + 1] == u']'; });
    if (close < 0)
        return false;
    const qint32 nl = m_nextNewline.find(innerFrom, n, [this](qint32 k) { return s[k] == u'\n'; });
    if (nl >= 0 && nl < close)
        return false;
    const qint32 open = m_nextWikiOpen.find(innerFrom, n - 1, [this](qint32 k) { return s[k] == u'[' && s[k + 1] == u'['; });
    if (open >= 0 && open + 1 < close)
        return false;
    WikiParts p;
    if (scanWikiLink(m_str, i, &p) < 0)
        return false;
    emitWikiLink(p, embed);
    return true;
}

void Parser::emitWikiLink(const WikiParts& p, bool embed)
{
    const qint32 id = qint32(m_tree.mutableWikiLinks().size());
    m_tree.mutableWikiLinks().push_back(wikiLinkInfo(m_str, p, embed));
    addNode(embed ? InlineType::Embed : InlineType::WikiLink, qint32(p.open), qint32(p.end), id);
    addMark(MarkKind::WikiOpen, qint32(p.open), qint32(p.innerFrom));
    addMark(MarkKind::WikiTarget, qint32(p.innerFrom), qint32(p.pipeFrom >= 0 ? p.pipeFrom : p.innerTo));
    if (p.pipeFrom >= 0)
        addMark(MarkKind::WikiPipe, qint32(p.pipeFrom), qint32(p.pipeTo));
    addMark(MarkKind::WikiClose, qint32(p.innerTo), qint32(p.end));
    m_lastEnd = qint32(p.end);
}

qint32 Parser::onOpenBracket(qint32 i)
{
    const char16_t next = at(i + 1);
    if (next == u'[') {
        if (tryWikiLink(i, false))
            return m_lastEnd;
    } else if (next == u'^') {
        // [^label]: no whitespace in the label; "[^x](url)" is left to the link rules.
        qint32 k = i + 2;
        while (k < n && s[k] != u']' && s[k] != u' ' && s[k] != u'\t' && s[k] != u'\n')
            ++k;
        if (k < n && s[k] == u']' && k > i + 2 && at(k + 1) != u'(') {
            const qint32 id = qint32(m_tree.mutableFootnotes().size());
            m_tree.mutableFootnotes().push_back(FootnoteInfo{m_str.mid(i + 2, k - i - 2)});
            addNode(InlineType::FootnoteRef, i, k + 1, id);
            addMark(MarkKind::FootnoteDelim, i, i + 2);
            addMark(MarkKind::FootnoteDelim, k, k + 1);
            m_lastEnd = k + 1;
            return k + 1;
        }
    }
    pushBracket(BracketKind::Link, i, i + 1);
    return i + 1;
}

qint32 Parser::onBang(qint32 i)
{
    if (at(i + 1) != u'[')
        return i + 1;
    if (at(i + 2) == u'[' && tryWikiLink(i, true))
        return m_lastEnd;
    pushBracket(BracketKind::Image, i, i + 2);
    return i + 2;
}

qint32 Parser::onCaret(qint32 i)
{
    if (i == m_blockIdFrom) {
        addNode(InlineType::BlockId, i, m_blockIdTo);
        addMark(MarkKind::BlockIdCaret, i, i + 1);
        m_lastEnd = m_blockIdTo;
        return m_blockIdTo;
    }
    if (at(i + 1) == u'[') {
        pushBracket(BracketKind::Footnote, i, i + 2);
        return i + 2;
    }
    return i + 1;
}

void Parser::pushBracket(BracketKind kind, qint32 pos, qint32 textFrom)
{
    if (!m_brackets.empty())
        m_brackets.back().bracketAfter = true;
    Bracket b;
    b.kind = kind;
    b.pos = pos;
    b.textFrom = textFrom;
    b.delimBottom = m_delimTop;
    m_brackets.push_back(b);
    if (kind != BracketKind::Footnote)
        ++m_linkBrackets;
}

void Parser::popBracket()
{
    if (m_brackets.back().kind != BracketKind::Footnote)
        --m_linkBrackets;
    m_brackets.pop_back();
}

const LinkRefDefinition* Parser::lookupRef(QStringView raw, QString* key)
{
    if (m_refs.isEmpty() || raw.size() > 999)
        return nullptr;
    *key = normalizeLinkLabel(raw);
    if (key->isEmpty())
        return nullptr;
    if (!m_refIndexBuilt) {
        m_refIndexBuilt = true;
        m_refIndex.reserve(m_refs.size());
        for (qint32 k = 0; k < qint32(m_refs.size()); ++k) {
            // re-normalize: robust to labels folded differently (idempotent for normalized ones)
            const QString norm = normalizeLinkLabel(m_refs[k].label);
            if (!norm.isEmpty() && !m_refIndex.contains(norm))
                m_refIndex.insert(norm, k);
        }
    }
    const auto it = m_refIndex.constFind(*key);
    return it == m_refIndex.constEnd() ? nullptr : &m_refs[*it];
}

qint32 Parser::onCloseBracket(qint32 i)
{
    if (m_brackets.empty())
        return i + 1;
    const Bracket b = m_brackets.back();
    if (!b.active) {
        popBracket();
        return i + 1;
    }
    if (b.kind == BracketKind::Footnote) {
        const qint32 id = qint32(m_tree.mutableFootnotes().size());
        m_tree.mutableFootnotes().push_back(FootnoteInfo{});
        addNode(InlineType::InlineFootnote, b.pos, i + 1, id);
        addMark(MarkKind::FootnoteDelim, b.pos, b.textFrom);
        addMark(MarkKind::FootnoteDelim, i, i + 1);
        processEmphasis(b.delimBottom);
        popBracket();
        m_lastEnd = i + 1;
        return i + 1;
    }

    const bool image = b.kind == BracketKind::Image;
    const qint32 after = i + 1;
    LinkInfo info;
    qint32 closeEnd = -1, urlFrom = -1, urlTo = -1;

    // Inline link: "(" dest [title] ")".
    if (at(after) == u'(') {
        const qint32 p = skipSpaces(after + 1);
        qsizetype df = 0, dt = 0;
        bool pointy = false;
        const qsizetype de = scanLinkDestination(m_str, p, &df, &dt, &pointy);
        if (de >= 0) {
            const qint32 endUrl = qint32(de);
            const qint32 startTitle = skipSpaces(endUrl);
            qint32 endTitle = startTitle;
            qsizetype tf = -1, tt = -1;
            if (startTitle != endUrl) {
                const qsizetype te = scanLinkTitle(m_str, startTitle, &tf, &tt);
                if (te >= 0)
                    endTitle = qint32(te);
                else
                    tf = tt = -1;
            }
            const qint32 endAll = skipSpaces(endTitle);
            if (at(endAll) == u')') {
                closeEnd = endAll + 1;
                info.destination = unescapeString(QStringView(m_str).mid(df, dt - df));
                if (tf >= 0)
                    info.title = unescapeString(QStringView(m_str).mid(tf, tt - tf));
                if (pointy) {
                    urlFrom = qint32(df) - 1;
                    urlTo = qint32(dt) + 1;
                } else if (dt > df) {
                    urlFrom = qint32(df);
                    urlTo = qint32(dt);
                }
            }
        }
    }

    // Reference link: [text][label], [label][] or [label].
    if (closeEnd < 0) {
        qsizetype lf = 0, lt = 0;
        const qsizetype le = scanLinkLabel(m_str, after, &lf, &lt);
        QStringView raw;
        qint32 refEnd = after;
        bool haveLabel = false;
        if (le >= 0) {
            raw = QStringView(m_str).mid(lf, lt - lf).trimmed();
            haveLabel = true;
            refEnd = qint32(le);
        }
        if ((le < 0 || raw.isEmpty()) && !b.bracketAfter) {
            raw = QStringView(m_str).mid(b.textFrom, i - b.textFrom);
            haveLabel = true;
        }
        QString key;
        if (haveLabel && !raw.isEmpty()) {
            if (const LinkRefDefinition* def = lookupRef(raw, &key)) {
                closeEnd = refEnd;
                info.destination = def->destination;
                info.title = def->title;
                info.reference = true;
                info.label = key;
            }
        }
    }

    if (closeEnd < 0) {
        popBracket();
        return i + 1;
    }
    if (image) {
        int w = -1, h = -1;
        if (parseSizeSuffix(QStringView(m_str).mid(b.textFrom, i - b.textFrom), &w, &h)) {
            info.width = w;
            info.height = h;
        }
    }
    const qint32 id = qint32(m_tree.mutableLinks().size());
    m_tree.mutableLinks().push_back(std::move(info));
    addNode(image ? InlineType::Image : InlineType::Link, b.pos, closeEnd, id);
    addMark(MarkKind::LinkOpen, b.pos, b.textFrom);
    addMark(MarkKind::LinkClose, i, closeEnd);
    if (urlFrom >= 0)
        addMark(MarkKind::LinkUrl, urlFrom, urlTo);
    processEmphasis(b.delimBottom);
    popBracket();
    if (!image) {
        // Links may not contain other links: deactivate earlier '[' openers. Everything below an
        // already inactive opener is inactive too (amortized linear, as in cmark).
        for (auto it = m_brackets.rbegin(); it != m_brackets.rend(); ++it) {
            if (it->kind != BracketKind::Link)
                continue;
            if (!it->active)
                break;
            it->active = false;
        }
    }
    m_lastEnd = closeEnd;
    return closeEnd;
}

qint32 Parser::onDollar(qint32 i)
{
    if (at(i + 1) == u'$') {
        const qsizetype end = (m_noDisplayCloseFrom >= 0 && i >= m_noDisplayCloseFrom) ? -1 : scanDisplayMath(m_str, i);
        if (end < 0) {
            // no "$$" closer after this one (exact: a later scan start is never a skipped escape target)
            if (m_noDisplayCloseFrom < 0)
                m_noDisplayCloseFrom = i;
            return i + 1; // the second '$' may still open inline math
        }
        addNode(InlineType::DisplayMath, i, qint32(end));
        addMark(MarkKind::MathDelim, i, i + 2);
        addMark(MarkKind::MathDelim, qint32(end) - 2, qint32(end));
        m_lastEnd = qint32(end);
        return qint32(end);
    }
    const qsizetype end = scanInlineMath(m_str, i);
    if (end < 0)
        return i + 1;
    addNode(InlineType::InlineMath, i, qint32(end));
    addMark(MarkKind::MathDelim, i, i + 1);
    addMark(MarkKind::MathDelim, qint32(end) - 1, qint32(end));
    m_lastEnd = qint32(end);
    return qint32(end);
}

qint32 Parser::onLessThan(qint32 i)
{
    bool email = false;
    qsizetype end = scanAutolink(m_str, i, &email);
    if (end > 0) {
        const qint32 id = addNode(InlineType::Autolink, i, qint32(end));
        if (email)
            m_nodes[size_t(id)].flags = FlagUrlEmail;
        addMark(MarkKind::AutolinkDelim, i, i + 1);
        addMark(MarkKind::AutolinkDelim, qint32(end) - 1, qint32(end));
        m_lastEnd = qint32(end);
        return qint32(end);
    }
    HtmlTagInfo info;
    end = scanHtmlTag(m_str, i, &info, &m_htmlMemo);
    if (end > 0) {
        const qint32 id = qint32(m_tree.mutableHtml().size());
        m_tree.mutableHtml().push_back(std::move(info));
        addNode(InlineType::HtmlTag, i, qint32(end), id);
        m_lastEnd = qint32(end);
        return qint32(end);
    }
    return i + 1;
}

qint32 Parser::onHash(qint32 i)
{
    if (i > 0 && !isJsSpace(s[i - 1]))
        return i + 1;
    const qsizetype end = scanTag(m_str, i);
    if (end < 0)
        return i + 1;
    const qint32 id = qint32(m_tree.mutableTags().size());
    m_tree.mutableTags().push_back(TagInfo{m_str.mid(i + 1, end - i - 1)});
    addNode(InlineType::Tag, i, qint32(end), id);
    addMark(MarkKind::TagHash, i, i + 1);
    m_lastEnd = qint32(end);
    return qint32(end);
}

qint32 Parser::onAmpersand(qint32 i)
{
    const qsizetype end = scanEntity(m_str, i);
    if (end < 0)
        return i + 1;
    addNode(InlineType::Entity, i, qint32(end));
    m_lastEnd = qint32(end);
    return qint32(end);
}

qint32 Parser::onNewline(qint32 i)
{
    qint32 ws = i;
    while (ws > m_lastEnd && (s[ws - 1] == u' ' || s[ws - 1] == u'\t'))
        --ws;
    const bool hard = i - 2 >= m_lastEnd && s[i - 1] == u' ' && s[i - 2] == u' ';
    qint32 k = i + 1;
    while (k < n && (s[k] == u' ' || s[k] == u'\t'))
        ++k;
    addNode(hard ? InlineType::HardBreak : InlineType::SoftBreak, ws, k);
    m_lastEnd = k;
    return k;
}

qint32 Parser::onLetter(qint32 i)
{
    if (m_linkBrackets > 0)
        return i + 1; // GFM: no extended autolinks inside link text
    const char16_t c = s[i];
    qsizetype end = -1;
    if (c == u'w') {
        const char16_t p = at(i - 1);
        if (i == 0 || p == u' ' || p == u'\t' || p == u'\n' || p == u'\v' || p == u'\f' || p == u'\r' || p == u'*' || p == u'_' || p == u'~'
            || p == u'(')
            end = scanWwwAutolink(m_str, i);
    } else if (i == 0 || !isAsciiAlpha(s[i - 1])) {
        end = scanSchemeAutolink(m_str, i);
    }
    if (end < 0)
        return i + 1;
    const qint32 id = addNode(InlineType::BareUrl, i, qint32(end));
    if (c == u'w')
        m_nodes[size_t(id)].flags = FlagUrlWww;
    m_lastEnd = qint32(end);
    return qint32(end);
}

// ---- delimiter runs ------------------------------------------------------------------------------

void Parser::pushDelim(char16_t ch, qint32 pos, qint32 len)
{
    const char32_t before = codePointBefore(m_str, pos);
    const char32_t after = codePointAt(m_str, pos + len);
    const bool wsBefore = isUnicodeWhitespace(before), wsAfter = isUnicodeWhitespace(after);
    const bool punctBefore = isUnicodePunctuation(before), punctAfter = isUnicodePunctuation(after);
    const bool left = !wsAfter && (!punctAfter || wsBefore || punctBefore);
    const bool right = !wsBefore && (!punctBefore || wsAfter || punctAfter);
    bool canOpen = left, canClose = right;
    if (ch == u'_') {
        canOpen = left && (!right || punctBefore);
        canClose = right && (!left || punctAfter);
    }
    if (!canOpen && !canClose)
        return;
    Delim d;
    d.ch = ch;
    d.pos = pos;
    d.len = len;
    d.left = pos;
    d.right = pos + len;
    d.canOpen = canOpen;
    d.canClose = canClose;
    d.prev = m_delimTop;
    const qint32 id = qint32(m_delims.size());
    if (m_delimTop >= 0)
        m_delims[size_t(m_delimTop)].next = id;
    m_delims.push_back(d);
    m_delimTop = id;
}

qint32 Parser::onEmphasisRun(qint32 i)
{
    const char16_t ch = s[i];
    qint32 k = i;
    while (k < n && s[k] == ch)
        ++k;
    pushDelim(ch, i, k - i);
    return k;
}

qint32 Parser::onDoubleRun(qint32 i)
{
    const char16_t ch = s[i];
    qint32 k = i;
    while (k < n && s[k] == ch)
        ++k;
    if (k - i == 2) // Obsidian: exactly "~~" / "=="
        pushDelim(ch, i, 2);
    return k;
}

void Parser::removeDelim(qint32 d)
{
    Delim& x = m_delims[size_t(d)];
    if (x.prev >= 0)
        m_delims[size_t(x.prev)].next = x.next;
    if (x.next >= 0)
        m_delims[size_t(x.next)].prev = x.prev;
    else
        m_delimTop = x.prev;
    x.prev = x.next = -1;
}

// CommonMark "process emphasis" over the delimiters above `bottom` (-1 = whole stack), extended with
// '~~' (strikethrough) and '==' (highlight) which only pair with a run of the same character.
void Parser::processEmphasis(qint32 bottom)
{
    auto charIndex = [](char16_t c) { return c == u'*' ? 0 : c == u'_' ? 1 : c == u'~' ? 2 : 3; };
    qint32 openersBottom[4][2][3];
    for (auto& a : openersBottom)
        for (auto& b : a)
            for (qint32& c : b)
                c = bottom;

    qint32 closer = m_delimTop;
    while (closer >= 0 && m_delims[size_t(closer)].prev != bottom)
        closer = m_delims[size_t(closer)].prev;

    while (closer >= 0) {
        Delim& c = m_delims[size_t(closer)];
        if (!c.canClose) {
            closer = c.next;
            continue;
        }
        const int ci = charIndex(c.ch);
        qint32& obottom = openersBottom[ci][c.canOpen ? 1 : 0][c.len % 3];
        qint32 opener = c.prev;
        bool found = false;
        while (opener >= 0 && opener != bottom && opener != obottom) {
            const Delim& o = m_delims[size_t(opener)];
            if (o.ch == c.ch && o.canOpen) {
                const bool emphasisChar = c.ch == u'*' || c.ch == u'_';
                const bool oddMatch = emphasisChar && (c.canOpen || o.canClose) && (o.len + c.len) % 3 == 0 && !(o.len % 3 == 0 && c.len % 3 == 0);
                if (!oddMatch) {
                    found = true;
                    break;
                }
            }
            opener = o.prev;
        }
        if (!found) {
            obottom = c.prev;
            const qint32 next = c.next;
            if (!c.canOpen)
                removeDelim(closer);
            closer = next;
            continue;
        }
        Delim& o = m_delims[size_t(opener)];
        const bool emphasisChar = c.ch == u'*' || c.ch == u'_';
        const qint32 use = emphasisChar ? ((c.count() >= 2 && o.count() >= 2) ? 2 : 1) : 2;
        const qint32 oFrom = o.right - use;
        const qint32 cFrom = c.left;
        InlineType t = InlineType::Emphasis;
        if (use == 2)
            t = emphasisChar ? InlineType::Strong : c.ch == u'~' ? InlineType::Strikethrough : InlineType::Highlight;
        addNode(t, oFrom, cFrom + use);
        addMark(MarkKind::EmphasisDelim, oFrom, oFrom + use);
        addMark(MarkKind::EmphasisDelim, cFrom, cFrom + use);
        o.right -= use;
        c.left += use;
        // delimiters between opener and closer become text
        for (qint32 d = c.prev; d != opener;) {
            const qint32 prev = m_delims[size_t(d)].prev;
            removeDelim(d);
            d = prev;
        }
        if (o.count() == 0)
            removeDelim(opener);
        if (c.count() == 0) {
            const qint32 next = c.next;
            removeDelim(closer);
            closer = next;
        }
    }
    while (m_delimTop >= 0 && m_delimTop != bottom)
        removeDelim(m_delimTop);
}

// ---- GFM extended email autolinks (cmark-gfm postprocess: plain text outside links) -----------------

void Parser::detectEmails()
{
    std::vector<std::pair<qint32, qint32>> cover;   // ranges that are not plain text
    std::vector<std::pair<qint32, qint32>> noLinks; // link / image ranges
    for (const RawNode& r : m_nodes) {
        if (r.dead)
            continue;
        switch (r.type) {
        case InlineType::Emphasis:
        case InlineType::Strong:
        case InlineType::Strikethrough:
        case InlineType::Highlight:
        case InlineType::InlineFootnote:
            break; // containers: their content is text
        case InlineType::Link:
        case InlineType::Image:
            noLinks.emplace_back(r.from, r.to);
            break;
        default:
            cover.emplace_back(r.from, r.to);
            break;
        }
    }
    std::sort(cover.begin(), cover.end());
    // links nest or are disjoint: merge them into disjoint outermost ranges
    std::sort(noLinks.begin(), noLinks.end());
    std::vector<std::pair<qint32, qint32>> linkRanges;
    for (const auto& r : noLinks) {
        if (!linkRanges.empty() && r.first < linkRanges.back().second)
            linkRanges.back().second = std::max(linkRanges.back().second, r.second);
        else
            linkRanges.push_back(r);
    }
    size_t lp = 0;
    const QStringView all(m_str);
    auto insideLink = [&](qint32 a, qint32 b) { // runs are visited in increasing order
        while (lp < linkRanges.size() && linkRanges[lp].second <= a)
            ++lp;
        return lp < linkRanges.size() && linkRanges[lp].first <= a && b <= linkRanges[lp].second;
    };
    auto scanRun = [&](qint32 a, qint32 b) {
        if (b <= a || insideLink(a, b))
            return;
        const QStringView run = all.left(b);
        qint32 i = a;
        while (i < b) {
            const qsizetype atPos = run.indexOf(u'@', i);
            if (atPos < 0)
                break;
            qint32 start = qint32(atPos);
            while (start > i && isEmailLocalChar(s[start - 1]))
                --start;
            if (start == atPos) {
                i = qint32(atPos) + 1;
                continue;
            }
            // "mailto:" / "xmpp:" right before the local part, not glued to a word
            bool scheme = false;
            for (const QLatin1StringView proto : {QLatin1StringView("mailto:"), QLatin1StringView("xmpp:")}) {
                const qint32 ps = start - qint32(proto.size());
                if (ps >= i && run.mid(ps, proto.size()) == proto && (ps == i || !isAsciiAlnum(s[ps - 1]))) {
                    start = ps;
                    scheme = true;
                    break;
                }
            }
            const qsizetype end = scanEmailAutolink(run, start);
            if (end > 0) {
                const qint32 id = addNode(InlineType::BareUrl, start, qint32(end));
                m_nodes[size_t(id)].flags = scheme ? 0 : FlagUrlEmail;
                i = qint32(end);
            } else {
                i = qint32(atPos) + 1;
            }
        }
    };
    qint32 pos = 0;
    for (const auto& [f, t] : cover) {
        if (f > pos)
            scanRun(pos, f);
        pos = std::max(pos, t);
    }
    scanRun(pos, n);
}

// ---- output -------------------------------------------------------------------------------------

void Parser::finish()
{
    std::vector<qint32> order;
    order.reserve(m_nodes.size());
    for (const RawNode& r : m_nodes)
        if (!r.dead)
            order.push_back(r.seq);
    std::sort(order.begin(), order.end(), [&](qint32 a, qint32 b) {
        const RawNode& x = m_nodes[size_t(a)];
        const RawNode& y = m_nodes[size_t(b)];
        if (x.from != y.from)
            return x.from < y.from;
        if (x.to != y.to)
            return x.to > y.to;
        return x.seq < y.seq;
    });
    QList<InlineNode>& out = m_tree.mutableNodes();
    out.resize(qsizetype(order.size()));
    std::vector<qint32> stack;
    std::vector<qint32> lastChild(order.size(), -1);
    qint32 lastRoot = -1;
    for (qint32 k = 0; k < qint32(order.size()); ++k) {
        const RawNode& r = m_nodes[size_t(order[size_t(k)])];
        while (!stack.empty()) {
            const InlineNode& top = out[stack.back()];
            if (top.from <= r.from && r.to <= top.to)
                break;
            stack.pop_back();
        }
        InlineNode& node = out[k];
        node.type = r.type;
        node.mark = r.mark;
        node.from = r.from;
        node.to = r.to;
        node.data = r.data;
        node.flags = r.flags;
        node.parent = stack.empty() ? -1 : stack.back();
        if (node.parent >= 0) {
            qint32& lc = lastChild[size_t(node.parent)];
            if (lc >= 0)
                out[lc].next = k;
            else
                out[node.parent].firstChild = k;
            lc = k;
        } else {
            if (lastRoot >= 0)
                out[lastRoot].next = k;
            lastRoot = k;
        }
        stack.push_back(k);
    }
}

} // namespace

InlineTreePtr parseInline(const QString& content, const QList<LinkRefDefinition>& refdefs)
{
    auto tree = std::make_shared<InlineTree>();
    Parser(content, refdefs, *tree).run();
    return tree;
}

// ---- LeafContent ----------------------------------------------------------------------------------

namespace {
// Segment containing a content offset; the '\n' joining two segments belongs to the first one.
const LeafContent::Segment& segmentFor(const QList<LeafContent::Segment>& segs, int off)
{
    const auto it = std::upper_bound(segs.begin(), segs.end(), off, [](int v, const LeafContent::Segment& sg) { return v < sg.contentFrom; });
    return it == segs.begin() ? segs.front() : *(it - 1);
}
} // namespace

qsizetype LeafContent::toDoc(int contentOffset, const Text& doc) const
{
    if (segments.isEmpty())
        return 0;
    const Segment& sg = segmentFor(segments, contentOffset);
    return doc.lineStart(sg.line) + sg.col + std::clamp(contentOffset - sg.contentFrom, 0, sg.length);
}

int LeafContent::fromDoc(qsizetype docOffset, const Text& doc) const
{
    if (segments.isEmpty() || docOffset < 0 || docOffset > doc.length())
        return -1;
    const int line = doc.lineAt(docOffset);
    auto it = std::lower_bound(segments.begin(), segments.end(), line, [](const Segment& sg, int l) { return sg.line < l; });
    for (; it != segments.end() && it->line == line; ++it) {
        const qsizetype start = doc.lineStart(it->line) + it->col;
        if (docOffset >= start && docOffset <= start + it->length)
            return it->contentFrom + int(docOffset - start);
    }
    return -1;
}

int LeafContent::lineOf(int contentOffset) const
{
    if (segments.isEmpty())
        return 0;
    return segmentFor(segments, contentOffset).line;
}

// ---- WikiLinkInfo --------------------------------------------------------------------------------

QString WikiLinkInfo::displayText() const
{
    if (!alias.isEmpty())
        return alias;
    if (subpath.isEmpty() && !target.contains(u'#'))
        return target;
    // Obsidian: split "target#sub#path" on '#', drop empty parts, join with " > ".
    const QString href = target + subpath;
    return href.split(u'#', Qt::SkipEmptyParts).join(QStringLiteral(" > ")).trimmed();
}

} // namespace oc::md
