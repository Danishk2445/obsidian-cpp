#include "markdown/inline_rules.h"

#include <QChar>

#include <algorithm>
#include <cstring>

namespace oc::md::rules {

namespace {

struct EntityRow {
    const char* name;
    char32_t a;
    char32_t b;
};
const EntityRow kEntities[] = {
#include "markdown/html_entities_table.inc"
};

struct FoldRow {
    char32_t cp;
    char32_t a, b, c;
};
const FoldRow kFullFolds[] = {
#include "markdown/case_fold_table.inc"
};

bool isHexDigit(char16_t c) { return isAsciiDigit(c) || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F'); }
int hexValue(char16_t c) { return isAsciiDigit(c) ? c - u'0' : (c >= u'a' && c <= u'f') ? c - u'a' + 10 : c - u'A' + 10; }

// cmark_isspace: ASCII whitespace.
bool isCmarkSpace(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\v' || c == u'\f' || c == u'\r'; }
bool isAsciiControl(char16_t c) { return c < 0x20 || c == 0x7F; }

// cmark-gfm's Unicode punctuation (P categories only; used by the autolink extension).
bool isCmarkPunct(char32_t c)
{
    if (c < 0x80)
        return isAsciiPunct(char16_t(c));
    const auto cat = QChar::category(c);
    return cat >= QChar::Punctuation_Connector && cat <= QChar::Punctuation_Other;
}

bool isCmarkUnicodeSpace(char32_t c)
{
    return c == 9 || c == 10 || c == 12 || c == 13 || c == 32 || c == 160 || c == 5760 || (c >= 8192 && c <= 8202) || c == 8239 || c == 8287
           || c == 12288;
}

bool isValidHostChar(QStringView s, qsizetype i)
{
    const char32_t c = codePointAt(s, i);
    return !isCmarkUnicodeSpace(c) && !isCmarkPunct(c);
}

bool startsWithAt(QStringView s, qsizetype i, QLatin1StringView what, Qt::CaseSensitivity cs = Qt::CaseSensitive)
{
    return i >= 0 && s.size() - i >= what.size() && s.mid(i, what.size()).compare(what, cs) == 0;
}

// cmark-gfm check_domain(): returns the length of the domain part scanned from `from` (0 = invalid).
qsizetype checkDomain(QStringView s, qsizetype from, qsizetype size, bool allowShort)
{
    if (size < 1)
        return 0;
    qsizetype np = 0, uscore1 = 0, uscore2 = 0;
    qsizetype i = 1;
    for (; i < size - 1; ++i) {
        if (s[from + i] == u'\\' && i < size - 2)
            ++i;
        const char16_t c = s[from + i].unicode();
        if (c == u'_') {
            ++uscore2;
        } else if (c == u'.') {
            uscore1 = uscore2;
            uscore2 = 0;
            ++np;
        } else if (!isValidHostChar(s, from + i) && c != u'-') {
            break;
        }
    }
    if ((uscore1 > 0 || uscore2 > 0) && np <= 10)
        return 0;
    if (allowShort)
        return i;
    return np ? i : 0;
}

// cmark-gfm autolink_delim(): trailing punctuation / unbalanced ')' / entity-like "&xx;" trimming.
qsizetype autolinkDelim(QStringView s, qsizetype from, qsizetype linkEnd)
{
    for (qsizetype i = 0; i < linkEnd; ++i) {
        if (s[from + i] == u'<') {
            linkEnd = i;
            break;
        }
    }
    // Parentheses are counted once (cmark-gfm recounts per trailing ')', which is quadratic); trimming
    // never removes '(' and removes ')' only in the branch that updates the count.
    qsizetype opening = 0, closing = 0;
    for (qsizetype i = 0; i < linkEnd; ++i) {
        if (s[from + i] == u'(')
            ++opening;
        else if (s[from + i] == u')')
            ++closing;
    }
    while (linkEnd > 0) {
        const char16_t cclose = s[from + linkEnd - 1].unicode();
        if (cclose == u'?' || cclose == u'!' || cclose == u'.' || cclose == u',' || cclose == u':' || cclose == u'*' || cclose == u'_'
            || cclose == u'~' || cclose == u'\'' || cclose == u'"') {
            --linkEnd;
        } else if (cclose == u';') {
            if (linkEnd < 2) {
                --linkEnd;
                continue;
            }
            qsizetype newEnd = linkEnd - 2;
            while (newEnd > 0 && isAsciiAlpha(s[from + newEnd].unicode()))
                --newEnd;
            if (newEnd < linkEnd - 2 && s[from + newEnd] == u'&')
                linkEnd = newEnd;
            else
                --linkEnd;
        } else if (cclose == u')') {
            if (closing <= opening)
                break;
            --linkEnd;
            --closing;
        } else {
            break;
        }
    }
    return linkEnd;
}

// Whitespace allowed inside HTML tags (spaces, tabs, line endings).
qsizetype skipHtmlSpace(QStringView s, qsizetype i)
{
    while (i < s.size() && isCmarkSpace(s[i].unicode()))
        ++i;
    return i;
}

QString jsTrimmed(QStringView v)
{
    qsizetype a = 0, b = v.size();
    while (a < b && isJsSpace(v[a].unicode()))
        ++a;
    while (b > a && isJsSpace(v[b - 1].unicode()))
        --b;
    return v.mid(a, b - a).toString();
}

void appendCodePoint(QString& out, char32_t cp)
{
    if (cp < 0x10000) {
        out += QChar(char16_t(cp));
    } else {
        const char32_t v = cp - 0x10000;
        out += QChar(char16_t(0xD800 + (v >> 10)));
        out += QChar(char16_t(0xDC00 + (v & 0x3FF)));
    }
}

} // namespace

// ---- character classes ------------------------------------------------------------------------

bool isJsSpace(char16_t c)
{
    switch (c) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

bool isUnicodeWhitespace(char32_t c)
{
    if (c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20)
        return true;
    if (c < 0x80)
        return false;
    return QChar::category(c) == QChar::Separator_Space;
}

bool isUnicodePunctuation(char32_t c)
{
    if (c < 0x80)
        return isAsciiPunct(char16_t(c));
    const auto cat = QChar::category(c);
    return cat >= QChar::Punctuation_Connector && cat <= QChar::Symbol_Other;
}

char32_t codePointBefore(QStringView s, qsizetype i)
{
    if (i <= 0 || i > s.size())
        return U'\n';
    const char16_t c = s[i - 1].unicode();
    if (QChar::isLowSurrogate(c) && i >= 2 && QChar::isHighSurrogate(s[i - 2].unicode()))
        return QChar::surrogateToUcs4(s[i - 2].unicode(), c);
    return c;
}

char32_t codePointAt(QStringView s, qsizetype i)
{
    if (i < 0 || i >= s.size())
        return U'\n';
    const char16_t c = s[i].unicode();
    if (QChar::isHighSurrogate(c) && i + 1 < s.size() && QChar::isLowSurrogate(s[i + 1].unicode()))
        return QChar::surrogateToUcs4(c, s[i + 1].unicode());
    return c;
}

// ---- Obsidian constructs ----------------------------------------------------------------------

bool isTagChar(char16_t c)
{
    if (isJsSpace(c))
        return false;
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x2E00 && c <= 0x2E7F))
        return false;
    if (isAsciiPunct(c))
        return c == u'-' || c == u'_' || c == u'/';
    return true;
}

qsizetype scanTag(QStringView s, qsizetype i)
{
    qsizetype j = i + 1;
    bool nonDigit = false;
    while (j < s.size() && isTagChar(s[j].unicode())) {
        if (!isAsciiDigit(s[j].unicode()))
            nonDigit = true;
        ++j;
    }
    if (j == i + 1 || !nonDigit)
        return -1;
    return j;
}

qsizetype scanInlineMath(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    if (i + 1 >= n)
        return -1;
    const char16_t first = s[i + 1].unicode();
    if (isJsSpace(first) || first == u'$')
        return -1;
    qsizetype q = i + 2;
    for (; q < n; ++q) {
        const char16_t c = s[q].unicode();
        if (c == u'\n')
            return -1;
        if (c == u'$' && s[q - 1] != u'\\')
            break;
    }
    if (q >= n)
        return -1;
    if (isJsSpace(s[q - 1].unicode()))
        return -1;
    if (q + 1 < n && isAsciiDigit(s[q + 1].unicode()))
        return -1;
    return q + 1;
}

qsizetype scanDisplayMath(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    qsizetype j = i + 2;
    while (j + 1 < n) {
        const char16_t c = s[j].unicode();
        if (c == u'\\') {
            j += 2;
            continue;
        }
        if (c == u'$' && s[j + 1] == u'$')
            return j + 2;
        ++j;
    }
    return -1;
}

qsizetype scanWikiLink(QStringView s, qsizetype i, WikiParts* parts)
{
    const qsizetype n = s.size();
    qsizetype j = i;
    if (j < n && s[j] == u'!')
        ++j;
    if (j + 1 >= n || s[j] != u'[' || s[j + 1] != u'[')
        return -1;
    const qsizetype innerFrom = j + 2;
    if (innerFrom >= n || s[innerFrom] == u'\n')
        return -1;
    qsizetype k = innerFrom + 1; // the inner text has at least one character
    for (;; ++k) {
        if (k + 1 >= n)
            return -1;
        const char16_t c = s[k].unicode();
        if (c == u'\n')
            return -1;
        if (c == u']' && s[k + 1] == u']')
            break;
    }
    const qsizetype innerTo = k;
    for (qsizetype m = innerFrom; m + 1 < innerTo; ++m)
        if (s[m] == u'[' && s[m + 1] == u'[')
            return -1;
    WikiParts p;
    p.open = i;
    p.innerFrom = innerFrom;
    p.innerTo = innerTo;
    p.end = innerTo + 2;
    qsizetype trimmedFrom = innerFrom;
    while (trimmedFrom < innerTo && isJsSpace(s[trimmedFrom].unicode()))
        ++trimmedFrom;
    for (qsizetype m = innerFrom; m < innerTo; ++m) {
        if (s[m] == u'|') {
            if (m > trimmedFrom) { // Obsidian: a '|' at the start of the (trimmed) text is not a separator
                p.pipeTo = m + 1;
                p.pipeFrom = (s[m - 1] == u'\\') ? m - 1 : m;
            }
            break;
        }
    }
    if (parts)
        *parts = p;
    return p.end;
}

WikiLinkInfo wikiLinkInfo(QStringView s, const WikiParts& parts, bool embed)
{
    WikiLinkInfo info;
    QStringView targetPart;
    if (parts.pipeTo >= 0) {
        const qsizetype bar = parts.pipeTo - 1;
        targetPart = s.mid(parts.innerFrom, bar - parts.innerFrom);
        info.alias = jsTrimmed(s.mid(bar + 1, parts.innerTo - bar - 1));
    } else {
        targetPart = s.mid(parts.innerFrom, parts.innerTo - parts.innerFrom);
    }
    QString href = jsTrimmed(targetPart);
    if (href.endsWith(u'\\'))
        href.chop(1);
    href.replace(QChar(0xA0), QLatin1Char(' '));
    href = jsTrimmed(href).normalized(QString::NormalizationForm_C);
    const qsizetype hash = href.indexOf(u'#');
    if (hash >= 0) {
        info.target = jsTrimmed(QStringView(href).left(hash));
        info.subpath = href.mid(hash);
    } else {
        info.target = href;
    }
    if (embed) {
        int w = -1, h = -1;
        if (parseSizeSuffix(info.alias, &w, &h)) {
            info.width = w;
            info.height = h;
        }
    }
    return info;
}

bool parseImageSize(QStringView s, int* width, int* height)
{
    const qsizetype n = s.size();
    qsizetype i = 0;
    auto skipSpace = [&] {
        while (i < n && isJsSpace(s[i].unicode()))
            ++i;
    };
    auto number = [&](int* out) {
        const qsizetype start = i;
        qint64 v = 0;
        while (i < n && isAsciiDigit(s[i].unicode())) {
            v = std::min<qint64>(v * 10 + (s[i].unicode() - u'0'), 1'000'000'000);
            ++i;
        }
        *out = int(v);
        return i > start;
    };
    int w = 0, h = 0;
    bool hasH = false;
    skipSpace();
    if (!number(&w))
        return false;
    skipSpace();
    if (i < n && s[i] == u'x') {
        ++i;
        skipSpace();
        if (!number(&h))
            return false;
        hasH = true;
        skipSpace();
    }
    if (i != n)
        return false;
    // Obsidian treats 0 as "not set".
    *width = w > 0 ? w : -1;
    *height = hasH && h > 0 ? h : -1;
    return true;
}

bool parseSizeSuffix(QStringView alt, int* width, int* height)
{
    if (alt.isEmpty())
        return false;
    const qsizetype bar = alt.lastIndexOf(u'|');
    return parseImageSize(bar >= 0 ? alt.mid(bar + 1) : alt, width, height);
}

// ---- GFM extended autolinks -------------------------------------------------------------------

qsizetype scanWwwAutolink(QStringView s, qsizetype i)
{
    const qsizetype size = s.size() - i;
    if (size < 4 || !startsWithAt(s, i, QLatin1StringView("www.")))
        return -1;
    qsizetype linkEnd = checkDomain(s, i, size, false);
    if (linkEnd == 0)
        return -1;
    while (linkEnd < size && !isCmarkSpace(s[i + linkEnd].unicode()) && s[i + linkEnd] != u'<')
        ++linkEnd;
    linkEnd = autolinkDelim(s, i, linkEnd);
    return linkEnd > 0 ? i + linkEnd : -1;
}

qsizetype scanSchemeAutolink(QStringView s, qsizetype i)
{
    static const QLatin1StringView schemes[] = {QLatin1StringView("http://"), QLatin1StringView("https://"), QLatin1StringView("ftp://")};
    const qsizetype size = s.size() - i;
    qsizetype len = 0;
    for (const QLatin1StringView& sc : schemes) {
        if (size > sc.size() && startsWithAt(s, i, sc, Qt::CaseInsensitive) && isValidHostChar(s, i + sc.size())) {
            len = sc.size();
            break;
        }
    }
    if (len == 0)
        return -1;
    const qsizetype domainLen = checkDomain(s, i + len, size - len, true);
    if (domainLen == 0)
        return -1;
    qsizetype linkEnd = len + domainLen;
    while (linkEnd < size && !isCmarkSpace(s[i + linkEnd].unicode()) && s[i + linkEnd] != u'<')
        ++linkEnd;
    linkEnd = autolinkDelim(s, i, linkEnd);
    return linkEnd > len ? i + linkEnd : -1;
}

qsizetype scanEmailAutolink(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    qsizetype j = i;
    bool xmpp = false;
    if (startsWithAt(s, i, QLatin1StringView("mailto:"))) {
        j += 7;
    } else if (startsWithAt(s, i, QLatin1StringView("xmpp:"))) {
        j += 5;
        xmpp = true;
    }
    const qsizetype local = j;
    while (j < n && isEmailLocalChar(s[j].unicode()))
        ++j;
    if (j == local || j >= n || s[j] != u'@')
        return -1;
    const qsizetype at = j;
    qsizetype k = 1, np = 0;
    while (at + k < n) {
        const char16_t c = s[at + k].unicode();
        if (isAsciiAlnum(c)) {
            ++k;
        } else if (c == u'@') {
            return -1; // a later '@' starts a new candidate (cmark-gfm rewinds from it)
        } else if (c == u'.' && at + k + 1 < n && isAsciiAlnum(s[at + k + 1].unicode())) {
            ++np;
            ++k;
        } else if (c == u'/' && xmpp) {
            ++k;
        } else if (c == u'-' || c == u'_') {
            ++k;
        } else {
            break;
        }
    }
    if (k < 2 || np == 0)
        return -1;
    const char16_t last = s[at + k - 1].unicode();
    if (!isAsciiAlpha(last) && last != u'.')
        return -1;
    k = autolinkDelim(s, at, k);
    if (k == 0)
        return -1;
    return at + k;
}

// ---- CommonMark ---------------------------------------------------------------------------------

qsizetype scanAutolink(QStringView s, qsizetype i, bool* email)
{
    const qsizetype n = s.size();
    if (i + 1 >= n || s[i] != u'<')
        return -1;
    // URI autolink: scheme (2-32 chars) ':' then no controls, spaces, '<' or '>'.
    if (isAsciiAlpha(s[i + 1].unicode())) {
        qsizetype k = i + 2;
        while (k < n && k - (i + 1) < 32) {
            const char16_t c = s[k].unicode();
            if (!(isAsciiAlnum(c) || c == u'+' || c == u'.' || c == u'-'))
                break;
            ++k;
        }
        if (k - (i + 1) >= 2 && k < n && s[k] == u':') {
            qsizetype m = k + 1;
            while (m < n) {
                const char16_t c = s[m].unicode();
                if (c == u'>' || c == u'<' || c <= 0x20 || c == 0x7F)
                    break;
                ++m;
            }
            if (m < n && s[m] == u'>') {
                if (email)
                    *email = false;
                return m + 1;
            }
        }
    }
    // Email autolink (HTML5 "valid e-mail address").
    auto isLocal = [](char16_t c) {
        if (isAsciiAlnum(c))
            return true;
        switch (c) {
        case u'.':
        case u'!':
        case u'#':
        case u'$':
        case u'%':
        case u'&':
        case u'\'':
        case u'*':
        case u'+':
        case u'/':
        case u'=':
        case u'?':
        case u'^':
        case u'_':
        case u'`':
        case u'{':
        case u'|':
        case u'}':
        case u'~':
        case u'-':
            return true;
        default:
            return false;
        }
    };
    qsizetype k = i + 1;
    while (k < n && isLocal(s[k].unicode()))
        ++k;
    if (k == i + 1 || k >= n || s[k] != u'@')
        return -1;
    qsizetype m = k + 1;
    for (;;) {
        if (m >= n || !isAsciiAlnum(s[m].unicode()))
            return -1;
        const qsizetype labelStart = m++;
        while (m < n && m - labelStart < 63 && (isAsciiAlnum(s[m].unicode()) || s[m] == u'-'))
            ++m;
        if (s[m - 1] == u'-')
            return -1;
        if (m < n && s[m] == u'.') {
            ++m;
            continue;
        }
        break;
    }
    if (m < n && s[m] == u'>') {
        if (email)
            *email = true;
        return m + 1;
    }
    return -1;
}

qsizetype scanHtmlTag(QStringView s, qsizetype i, HtmlTagInfo* info, HtmlScanMemo* memo)
{
    const qsizetype n = s.size();
    if (i + 1 >= n || s[i] != u'<')
        return -1;
    const char16_t c = s[i + 1].unicode();
    // indexOf with a "known absent from" memo
    auto find = [&](auto needle, qsizetype from, qsizetype HtmlScanMemo::*field) -> qsizetype {
        if (memo && memo->*field >= 0 && from >= memo->*field)
            return -1;
        const qsizetype r = s.indexOf(needle, from);
        if (r < 0 && memo)
            memo->*field = memo->*field >= 0 ? std::min(memo->*field, from) : from;
        return r;
    };
    auto tagNameEnd = [&](qsizetype from) {
        qsizetype j = from + 1;
        while (j < n && (isAsciiAlnum(s[j].unicode()) || s[j] == u'-'))
            ++j;
        return j;
    };
    if (isAsciiAlpha(c)) {
        const qsizetype nameEnd = tagNameEnd(i + 1);
        qsizetype j = nameEnd;
        for (;;) {
            const qsizetype ws = skipHtmlSpace(s, j);
            if (ws == j || ws >= n)
                break;
            const char16_t a = s[ws].unicode();
            if (!(isAsciiAlpha(a) || a == u'_' || a == u':'))
                break;
            qsizetype k = ws + 1;
            while (k < n) {
                const char16_t b = s[k].unicode();
                if (!(isAsciiAlnum(b) || b == u'_' || b == u'.' || b == u':' || b == u'-'))
                    break;
                ++k;
            }
            // optional value specification
            const qsizetype eq = skipHtmlSpace(s, k);
            if (eq < n && s[eq] == u'=') {
                qsizetype v = skipHtmlSpace(s, eq + 1);
                if (v >= n)
                    return -1;
                const char16_t q = s[v].unicode();
                if (q == u'"' || q == u'\'') {
                    const qsizetype close = find(QChar(q), v + 1, q == u'"' ? &HtmlScanMemo::noDquote : &HtmlScanMemo::noSquote);
                    if (close < 0)
                        return -1;
                    k = close + 1;
                } else {
                    const qsizetype start = v;
                    while (v < n) {
                        const char16_t b = s[v].unicode();
                        if (b <= 0x20 || b == u'"' || b == u'\'' || b == u'=' || b == u'<' || b == u'>' || b == u'`')
                            break;
                        ++v;
                    }
                    if (v == start)
                        return -1;
                    k = v;
                }
            }
            j = k;
        }
        j = skipHtmlSpace(s, j);
        bool selfClosing = false;
        if (j < n && s[j] == u'/') {
            selfClosing = true;
            ++j;
        }
        if (j >= n || s[j] != u'>')
            return -1;
        if (info) {
            *info = HtmlTagInfo{};
            info->name = s.mid(i + 1, nameEnd - i - 1).toString().toLower();
            info->selfClosing = selfClosing;
        }
        return j + 1;
    }
    if (c == u'/') {
        if (i + 2 >= n || !isAsciiAlpha(s[i + 2].unicode()))
            return -1;
        const qsizetype nameEnd = tagNameEnd(i + 2);
        const qsizetype j = skipHtmlSpace(s, nameEnd);
        if (j >= n || s[j] != u'>')
            return -1;
        if (info) {
            *info = HtmlTagInfo{};
            info->name = s.mid(i + 2, nameEnd - i - 2).toString().toLower();
            info->closing = true;
        }
        return j + 1;
    }
    auto finish = [&](qsizetype end, bool comment) {
        if (info) {
            *info = HtmlTagInfo{};
            info->comment = comment;
        }
        return end;
    };
    if (c == u'!') {
        if (startsWithAt(s, i, QLatin1StringView("<!--"))) {
            if (startsWithAt(s, i + 4, QLatin1StringView(">")))
                return finish(i + 5, true);
            if (startsWithAt(s, i + 4, QLatin1StringView("->")))
                return finish(i + 6, true);
            const qsizetype close = find(QLatin1StringView("-->"), i + 4, &HtmlScanMemo::noCommentEnd);
            return close < 0 ? -1 : finish(close + 3, true);
        }
        if (startsWithAt(s, i, QLatin1StringView("<![CDATA["))) {
            const qsizetype close = find(QLatin1StringView("]]>"), i + 9, &HtmlScanMemo::noCdataEnd);
            return close < 0 ? -1 : finish(close + 3, false);
        }
        if (i + 2 < n && isAsciiAlpha(s[i + 2].unicode())) {
            const qsizetype close = find(QChar(u'>'), i + 3, &HtmlScanMemo::noGreater);
            return close < 0 ? -1 : finish(close + 1, false);
        }
        return -1;
    }
    if (c == u'?') {
        const qsizetype close = find(QLatin1StringView("?>"), i + 2, &HtmlScanMemo::noPiEnd);
        return close < 0 ? -1 : finish(close + 2, false);
    }
    return -1;
}

bool lookupEntity(QStringView name, char32_t* cp1, char32_t* cp2)
{
    if (name.isEmpty() || name.size() > 40)
        return false;
    char buf[48];
    for (qsizetype k = 0; k < name.size(); ++k) {
        const char16_t c = name[k].unicode();
        if (c >= 0x80)
            return false;
        buf[k] = char(c);
    }
    buf[name.size()] = 0;
    const auto* end = std::end(kEntities);
    const auto* it = std::lower_bound(std::begin(kEntities), end, buf, [](const EntityRow& r, const char* key) { return std::strcmp(r.name, key) < 0; });
    if (it == end || std::strcmp(it->name, buf) != 0)
        return false;
    if (cp1)
        *cp1 = it->a;
    if (cp2)
        *cp2 = it->b;
    return true;
}

qsizetype scanEntity(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    if (i + 2 >= n || s[i] != u'&')
        return -1;
    qsizetype j = i + 1;
    if (s[j] == u'#') {
        ++j;
        qsizetype k = j;
        if (j < n && (s[j] == u'x' || s[j] == u'X')) {
            k = ++j;
            while (k < n && isHexDigit(s[k].unicode()) && k - j < 6)
                ++k;
        } else {
            while (k < n && isAsciiDigit(s[k].unicode()) && k - j < 7)
                ++k;
        }
        if (k == j || k >= n || s[k] != u';')
            return -1;
        return k + 1;
    }
    if (!isAsciiAlpha(s[j].unicode()))
        return -1;
    qsizetype k = j;
    while (k < n && isAsciiAlnum(s[k].unicode()) && k - j <= 32)
        ++k;
    if (k >= n || s[k] != u';' || !lookupEntity(s.mid(j, k - j), nullptr, nullptr))
        return -1;
    return k + 1;
}

QString decodeEntity(QStringView ref)
{
    QString out;
    if (ref.size() < 3 || ref.front() != u'&' || ref.back() != u';')
        return ref.toString();
    const QStringView body = ref.mid(1, ref.size() - 2);
    if (body.front() == u'#') {
        char32_t cp = 0;
        const bool hex = body.size() > 1 && (body[1] == u'x' || body[1] == u'X');
        for (qsizetype k = hex ? 2 : 1; k < body.size(); ++k) {
            const char16_t c = body[k].unicode();
            cp = hex ? cp * 16 + char32_t(hexValue(c)) : cp * 10 + char32_t(c - u'0');
            if (cp > 0x10FFFF)
                cp = 0x110000; // saturate: invalid
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        appendCodePoint(out, cp);
        return out;
    }
    char32_t a = 0, b = 0;
    if (!lookupEntity(body, &a, &b))
        return ref.toString();
    appendCodePoint(out, a);
    if (b)
        appendCodePoint(out, b);
    return out;
}

QString unescapeString(QStringView s)
{
    if (!s.contains(u'\\') && !s.contains(u'&'))
        return s.toString();
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size();) {
        const char16_t c = s[i].unicode();
        if (c == u'\\' && i + 1 < s.size() && isAsciiPunct(s[i + 1].unicode())) {
            out += s[i + 1];
            i += 2;
        } else if (c == u'&') {
            const qsizetype end = scanEntity(s, i);
            if (end > 0) {
                out += decodeEntity(s.mid(i, end - i));
                i = end;
            } else {
                out += s[i++];
            }
        } else {
            out += s[i++];
        }
    }
    return out;
}

qsizetype scanLinkDestination(QStringView s, qsizetype i, qsizetype* from, qsizetype* to, bool* pointy)
{
    const qsizetype n = s.size();
    if (i < n && s[i] == u'<') {
        qsizetype j = i + 1;
        while (j < n) {
            const char16_t c = s[j].unicode();
            if (c == u'>') {
                *from = i + 1;
                *to = j;
                *pointy = true;
                return j + 1;
            }
            if (c == u'\\') {
                j += 2;
                continue;
            }
            if (c == u'\n' || c == u'<')
                return -1;
            ++j;
        }
        return -1;
    }
    qsizetype j = i;
    int depth = 0;
    while (j < n) {
        const char16_t c = s[j].unicode();
        if (c == u'\\' && j + 1 < n && isAsciiPunct(s[j + 1].unicode())) {
            j += 2;
        } else if (c == u'(') {
            if (++depth > 32)
                return -1;
            ++j;
        } else if (c == u')') {
            if (depth == 0)
                break;
            --depth;
            ++j;
        } else if (isCmarkSpace(c) || isAsciiControl(c)) {
            if (j == i)
                return -1;
            break;
        } else {
            ++j;
        }
    }
    if (j >= n || depth != 0)
        return -1;
    *from = i;
    *to = j;
    *pointy = false;
    return j;
}

qsizetype scanLinkTitle(QStringView s, qsizetype i, qsizetype* from, qsizetype* to)
{
    const qsizetype n = s.size();
    if (i >= n)
        return -1;
    const char16_t open = s[i].unicode();
    const char16_t close = open == u'"' ? u'"' : open == u'\'' ? u'\'' : open == u'(' ? u')' : 0;
    if (!close)
        return -1;
    for (qsizetype j = i + 1; j < n; ++j) {
        const char16_t c = s[j].unicode();
        if (c == u'\\' && j + 1 < n && isAsciiPunct(s[j + 1].unicode())) {
            ++j;
            continue;
        }
        if (c == close) {
            *from = i + 1;
            *to = j;
            return j + 1;
        }
        if (close == u')' && c == u'(')
            return -1;
    }
    return -1;
}

qsizetype scanLinkLabel(QStringView s, qsizetype i, qsizetype* from, qsizetype* to)
{
    const qsizetype n = s.size();
    if (i >= n || s[i] != u'[')
        return -1;
    qsizetype j = i + 1, length = 0;
    while (j < n && s[j] != u'[' && s[j] != u']') {
        if (s[j] == u'\\') {
            ++j;
            ++length;
            if (j < n && isAsciiPunct(s[j].unicode())) {
                ++j;
                ++length;
            }
        } else {
            ++j;
            ++length;
        }
        if (length > 999)
            return -1;
    }
    if (j < n && s[j] == u']') {
        *from = i + 1;
        *to = j;
        return j + 1;
    }
    return -1;
}

QString normalizeLinkLabel(QStringView label)
{
    QString out;
    out.reserve(label.size());
    bool pendingSpace = false;
    for (qsizetype i = 0; i < label.size();) {
        const char32_t cp = codePointAt(label, i);
        i += cp >= 0x10000 ? 2 : 1;
        if (cp == u' ' || cp == u'\t' || cp == u'\n' || cp == u'\r') {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            out += QLatin1Char(' ');
            pendingSpace = false;
        }
        const auto* end = std::end(kFullFolds);
        const auto* it = std::lower_bound(std::begin(kFullFolds), end, cp, [](const FoldRow& r, char32_t key) { return r.cp < key; });
        if (it != end && it->cp == cp) {
            appendCodePoint(out, it->a);
            if (it->b)
                appendCodePoint(out, it->b);
            if (it->c)
                appendCodePoint(out, it->c);
        } else {
            appendCodePoint(out, QChar::toCaseFolded(cp));
        }
    }
    return out;
}

} // namespace oc::md::rules
