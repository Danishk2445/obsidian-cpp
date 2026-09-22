#include "markdown/inline_html.h"

#include "markdown/inline_rules.h"

namespace oc::md::test {

namespace {

// cmark houdini HREF_SAFE table (ASCII).
constexpr bool hrefSafe(unsigned char c)
{
    if (c >= 0x80 || c <= 0x20)
        return false;
    switch (c) {
    case '"':
    case '&':
    case '\'':
    case '<':
    case '>':
    case '[':
    case '\\':
    case ']':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
    case 0x7F:
        return false;
    default:
        return true;
    }
}

void escapeHtml(QString& out, QStringView s)
{
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'&':
            out += QLatin1StringView("&amp;");
            break;
        case u'<':
            out += QLatin1StringView("&lt;");
            break;
        case u'>':
            out += QLatin1StringView("&gt;");
            break;
        case u'"':
            out += QLatin1StringView("&quot;");
            break;
        default:
            out += c;
        }
    }
}

void escapeHref(QString& out, QStringView s)
{
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray utf8 = s.toUtf8();
    for (const char ch : utf8) {
        const auto b = static_cast<unsigned char>(ch);
        if (hrefSafe(b)) {
            out += QLatin1Char(char(b));
        } else if (b == '&') {
            out += QLatin1StringView("&amp;");
        } else if (b == '\'') {
            out += QLatin1StringView("&#x27;");
        } else {
            out += QLatin1Char('%');
            out += QLatin1Char(hex[b >> 4]);
            out += QLatin1Char(hex[b & 0xF]);
        }
    }
}

QString decodeEntities(QStringView s)
{
    QString out;
    for (qsizetype i = 0; i < s.size();) {
        const qsizetype end = s[i] == u'&' ? rules::scanEntity(s, i) : -1;
        if (end > 0) {
            out += rules::decodeEntity(s.mid(i, end - i));
            i = end;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// Range between the first and last Mark child (the construct's inner text).
std::pair<int, int> innerRange(const InlineTree& t, int id)
{
    const InlineNode& n = t.node(id);
    int a = n.from, b = n.to;
    int firstMark = -1, lastMark = -1;
    for (int c = n.firstChild; c >= 0; c = t.node(c).next) {
        if (t.node(c).type != InlineType::Mark)
            continue;
        if (firstMark < 0)
            firstMark = c;
        lastMark = c;
    }
    if (firstMark >= 0 && t.node(firstMark).from == n.from)
        a = t.node(firstMark).to;
    if (lastMark >= 0 && lastMark != firstMark && t.node(lastMark).to == n.to)
        b = t.node(lastMark).from;
    return {a, b};
}

QString codeText(const InlineTree& t, int id)
{
    const auto [a, b] = innerRange(t, id);
    QString code = t.content().mid(a, b - a);
    code.replace(QLatin1Char('\n'), QLatin1Char(' '));
    bool allSpaces = true;
    for (const QChar c : code)
        if (c != u' ')
            allSpaces = false;
    if (code.size() >= 2 && !allSpaces && code.startsWith(u' ') && code.endsWith(u' '))
        code = code.mid(1, code.size() - 2);
    return code;
}

class Renderer {
public:
    explicit Renderer(const InlineTree& t)
        : t(t)
        , c(t.content())
    {
    }

    void children(int parent, int from, int to, bool plain)
    {
        int pos = from;
        const int first = parent < 0 ? t.firstRoot() : t.node(parent).firstChild;
        for (int id = first; id >= 0; id = t.node(id).next) {
            const InlineNode& n = t.node(id);
            if (n.from > pos)
                text(pos, n.from, plain);
            if (n.type != InlineType::Mark)
                node(id, plain);
            pos = std::max(pos, n.to);
        }
        if (to > pos)
            text(pos, to, plain);
    }

    void text(int a, int b, bool)
    {
        QStringView v = QStringView(c).mid(a, b - a);
        if (b == c.size()) {
            while (!v.isEmpty() && (v.back() == u' ' || v.back() == u'\t'))
                v.chop(1);
        }
        escapeHtml(out, v);
    }

    void wrap(const char* tag, int id, bool plain)
    {
        const InlineNode& n = t.node(id);
        const bool tags = !plain
                          && !(gfmCompat && n.type == InlineType::Strong && n.parent >= 0 && t.node(n.parent).type == InlineType::Strong);
        if (tags)
            out += QLatin1Char('<') + QLatin1StringView(tag) + QLatin1Char('>');
        children(id, n.from, n.to, plain);
        if (tags)
            out += QLatin1StringView("</") + QLatin1StringView(tag) + QLatin1Char('>');
    }

    void node(int id, bool plain)
    {
        const InlineNode& n = t.node(id);
        const QStringView raw = QStringView(c).mid(n.from, n.to - n.from);
        switch (n.type) {
        case InlineType::Emphasis:
            wrap("em", id, plain);
            break;
        case InlineType::Strong:
            wrap("strong", id, plain);
            break;
        case InlineType::Strikethrough:
            wrap("del", id, plain);
            break;
        case InlineType::Highlight:
            wrap("mark", id, plain);
            break;
        case InlineType::CodeSpan:
            if (!plain)
                out += QLatin1StringView("<code>");
            escapeHtml(out, codeText(t, id));
            if (!plain)
                out += QLatin1StringView("</code>");
            break;
        case InlineType::InlineMath:
        case InlineType::DisplayMath: {
            const auto [a, b] = innerRange(t, id);
            if (!plain)
                out += n.type == InlineType::InlineMath ? QLatin1StringView("<span class=\"math math-inline\">")
                                                        : QLatin1StringView("<span class=\"math math-block\">");
            escapeHtml(out, QStringView(c).mid(a, b - a));
            if (!plain)
                out += QLatin1StringView("</span>");
            break;
        }
        case InlineType::Comment:
        case InlineType::BlockId:
            break;
        case InlineType::WikiLink:
        case InlineType::Embed: {
            const WikiLinkInfo& wl = t.wikiLink(id);
            if (plain) {
                escapeHtml(out, wl.displayText());
                break;
            }
            if (n.type == InlineType::WikiLink) {
                out += QLatin1StringView("<a class=\"internal-link\" data-href=\"");
                escapeHtml(out, QString(wl.target + wl.subpath));
                out += QLatin1StringView("\">");
                escapeHtml(out, wl.displayText());
                out += QLatin1StringView("</a>");
            } else {
                out += QLatin1StringView("<span class=\"internal-embed\" src=\"");
                escapeHtml(out, QString(wl.target + wl.subpath));
                out += QLatin1StringView("\" alt=\"");
                escapeHtml(out, wl.alias);
                out += QLatin1Char('"');
                if (wl.width > 0)
                    out += QStringLiteral(" width=\"%1\"").arg(wl.width);
                if (wl.height > 0)
                    out += QStringLiteral(" height=\"%1\"").arg(wl.height);
                out += QLatin1StringView("></span>");
            }
            break;
        }
        case InlineType::Link: {
            if (plain) {
                children(id, n.from, n.to, true);
                break;
            }
            const LinkInfo& li = t.link(id);
            out += QLatin1StringView("<a href=\"");
            escapeHref(out, li.destination);
            out += QLatin1Char('"');
            if (!li.title.isEmpty()) {
                out += QLatin1StringView(" title=\"");
                escapeHtml(out, li.title);
                out += QLatin1Char('"');
            }
            out += QLatin1Char('>');
            children(id, n.from, n.to, false);
            out += QLatin1StringView("</a>");
            break;
        }
        case InlineType::Image: {
            if (plain) {
                children(id, n.from, n.to, true);
                break;
            }
            const LinkInfo& li = t.link(id);
            out += QLatin1StringView("<img src=\"");
            escapeHref(out, li.destination);
            out += QLatin1StringView("\" alt=\"");
            children(id, n.from, n.to, true);
            out += QLatin1Char('"');
            if (!li.title.isEmpty()) {
                out += QLatin1StringView(" title=\"");
                escapeHtml(out, li.title);
                out += QLatin1Char('"');
            }
            out += QLatin1StringView(" />");
            break;
        }
        case InlineType::Autolink: {
            // entity references are decoded (cmark), backslash escapes are not (spec)
            const QString url = decodeEntities(raw.mid(1, raw.size() - 2));
            if (plain) {
                escapeHtml(out, url);
                break;
            }
            out += QLatin1StringView("<a href=\"");
            if (n.flags & rules::FlagUrlEmail)
                out += QLatin1StringView("mailto:");
            escapeHref(out, url);
            out += QLatin1StringView("\">");
            escapeHtml(out, url);
            out += QLatin1StringView("</a>");
            break;
        }
        case InlineType::BareUrl: {
            if (plain) {
                escapeHtml(out, raw);
                break;
            }
            // GFM: www links get "http://", bare emails "mailto:"; scheme URLs and mailto:/xmpp: stay
            QString href = raw.toString();
            if (n.flags & rules::FlagUrlWww)
                href.prepend(QLatin1StringView("http://"));
            else if (n.flags & rules::FlagUrlEmail)
                href.prepend(QLatin1StringView("mailto:"));
            out += QLatin1StringView("<a href=\"");
            escapeHref(out, href);
            out += QLatin1StringView("\">");
            escapeHtml(out, raw);
            out += QLatin1StringView("</a>");
            break;
        }
        case InlineType::Tag:
            if (!plain) {
                out += QLatin1StringView("<a class=\"tag\" href=\"#");
                escapeHtml(out, t.tag(id).tag);
                out += QLatin1StringView("\">");
            }
            escapeHtml(out, raw);
            if (!plain)
                out += QLatin1StringView("</a>");
            break;
        case InlineType::FootnoteRef:
            if (!plain) {
                out += QLatin1StringView("<sup class=\"footnote-ref\" data-footnote=\"");
                escapeHtml(out, t.footnote(id).label);
                out += QLatin1StringView("\">");
            }
            escapeHtml(out, raw);
            if (!plain)
                out += QLatin1StringView("</sup>");
            break;
        case InlineType::InlineFootnote:
            wrap("sup", id, plain);
            break;
        case InlineType::HtmlTag:
            if (plain)
                escapeHtml(out, raw);
            else
                out += raw;
            break;
        case InlineType::Escape:
            escapeHtml(out, raw.mid(1));
            break;
        case InlineType::Entity:
            escapeHtml(out, rules::decodeEntity(raw));
            break;
        case InlineType::HardBreak:
            out += plain ? QLatin1StringView(" ") : QLatin1StringView("<br />\n");
            break;
        case InlineType::SoftBreak:
            out += plain ? QLatin1StringView(" ") : QLatin1StringView("\n");
            break;
        case InlineType::Text:
        case InlineType::Mark:
            escapeHtml(out, raw);
            break;
        }
    }

    const InlineTree& t;
    const QString& c;
    QString out;
    bool gfmCompat = false;
};

QString quote(QStringView s)
{
    QString out = QStringLiteral("\"");
    for (const QChar ch : s) {
        switch (ch.unicode()) {
        case u'"':
            out += QLatin1StringView("\\\"");
            break;
        case u'\\':
            out += QLatin1StringView("\\\\");
            break;
        case u'\n':
            out += QLatin1StringView("\\n");
            break;
        case u'\t':
            out += QLatin1StringView("\\t");
            break;
        default:
            out += ch;
        }
    }
    out += QLatin1Char('"');
    return out;
}

QString info(const InlineTree& t, int id)
{
    const InlineNode& n = t.node(id);
    QStringList parts;
    switch (n.type) {
    case InlineType::WikiLink:
    case InlineType::Embed: {
        const WikiLinkInfo& w = t.wikiLink(id);
        parts << QStringLiteral("target=") + quote(w.target);
        if (!w.subpath.isEmpty())
            parts << QStringLiteral("subpath=") + quote(w.subpath);
        if (!w.alias.isEmpty())
            parts << QStringLiteral("alias=") + quote(w.alias);
        if (w.width >= 0)
            parts << QStringLiteral("w=%1").arg(w.width);
        if (w.height >= 0)
            parts << QStringLiteral("h=%1").arg(w.height);
        break;
    }
    case InlineType::Link:
    case InlineType::Image: {
        const LinkInfo& l = t.link(id);
        parts << QStringLiteral("dest=") + quote(l.destination);
        if (!l.title.isEmpty())
            parts << QStringLiteral("title=") + quote(l.title);
        if (l.reference)
            parts << QStringLiteral("ref=") + quote(l.label);
        if (l.width >= 0)
            parts << QStringLiteral("w=%1").arg(l.width);
        if (l.height >= 0)
            parts << QStringLiteral("h=%1").arg(l.height);
        break;
    }
    case InlineType::Tag:
        parts << QStringLiteral("tag=") + quote(t.tag(id).tag);
        break;
    case InlineType::FootnoteRef:
        parts << QStringLiteral("label=") + quote(t.footnote(id).label);
        break;
    case InlineType::HtmlTag: {
        const HtmlTagInfo& h = t.htmlTag(id);
        if (!h.name.isEmpty())
            parts << QStringLiteral("name=") + quote(h.name);
        if (h.closing)
            parts << QStringLiteral("closing");
        if (h.selfClosing)
            parts << QStringLiteral("selfClosing");
        if (h.comment)
            parts << QStringLiteral("comment");
        break;
    }
    case InlineType::BlockId:
        parts << QStringLiteral("id=") + quote(t.blockId(id));
        break;
    case InlineType::BareUrl:
    case InlineType::Autolink:
        if (n.flags & rules::FlagUrlWww)
            parts << QStringLiteral("www");
        if (n.flags & rules::FlagUrlEmail)
            parts << QStringLiteral("email");
        break;
    default:
        break;
    }
    return parts.isEmpty() ? QString() : QLatin1Char('{') + parts.join(QLatin1Char(' ')) + QLatin1Char('}');
}

void dumpRange(const InlineTree& t, int parent, int from, int to, QStringList& items);

QString dumpNode(const InlineTree& t, int id)
{
    const InlineNode& n = t.node(id);
    QStringList items;
    dumpRange(t, id, n.from, n.to, items);
    QString out = QLatin1Char('(') + (n.type == InlineType::Mark ? QLatin1StringView(markName(n.mark)) + QString() : QLatin1StringView(typeName(n.type)) + info(t, id));
    for (const QString& it : items)
        out += QLatin1Char(' ') + it;
    return out + QLatin1Char(')');
}

void dumpRange(const InlineTree& t, int parent, int from, int to, QStringList& items)
{
    int pos = from;
    const int first = parent < 0 ? t.firstRoot() : t.node(parent).firstChild;
    for (int id = first; id >= 0; id = t.node(id).next) {
        const InlineNode& n = t.node(id);
        if (n.from > pos)
            items << quote(QStringView(t.content()).mid(pos, n.from - pos));
        items << dumpNode(t, id);
        pos = std::max(pos, n.to);
    }
    if (to > pos)
        items << quote(QStringView(t.content()).mid(pos, to - pos));
}

} // namespace

const char* typeName(InlineType t)
{
    switch (t) {
    case InlineType::Text: return "Text";
    case InlineType::Emphasis: return "Emphasis";
    case InlineType::Strong: return "Strong";
    case InlineType::Strikethrough: return "Strikethrough";
    case InlineType::Highlight: return "Highlight";
    case InlineType::CodeSpan: return "CodeSpan";
    case InlineType::InlineMath: return "InlineMath";
    case InlineType::DisplayMath: return "DisplayMath";
    case InlineType::Comment: return "Comment";
    case InlineType::WikiLink: return "WikiLink";
    case InlineType::Embed: return "Embed";
    case InlineType::Link: return "Link";
    case InlineType::Image: return "Image";
    case InlineType::Autolink: return "Autolink";
    case InlineType::BareUrl: return "BareUrl";
    case InlineType::Tag: return "Tag";
    case InlineType::FootnoteRef: return "FootnoteRef";
    case InlineType::InlineFootnote: return "InlineFootnote";
    case InlineType::BlockId: return "BlockId";
    case InlineType::HtmlTag: return "HtmlTag";
    case InlineType::Escape: return "Escape";
    case InlineType::Entity: return "Entity";
    case InlineType::HardBreak: return "HardBreak";
    case InlineType::SoftBreak: return "SoftBreak";
    case InlineType::Mark: return "Mark";
    }
    return "?";
}

const char* markName(MarkKind k)
{
    switch (k) {
    case MarkKind::None: return "None";
    case MarkKind::EmphasisDelim: return "EmphasisDelim";
    case MarkKind::CodeDelim: return "CodeDelim";
    case MarkKind::MathDelim: return "MathDelim";
    case MarkKind::CommentDelim: return "CommentDelim";
    case MarkKind::WikiOpen: return "WikiOpen";
    case MarkKind::WikiClose: return "WikiClose";
    case MarkKind::WikiTarget: return "WikiTarget";
    case MarkKind::WikiPipe: return "WikiPipe";
    case MarkKind::LinkOpen: return "LinkOpen";
    case MarkKind::LinkClose: return "LinkClose";
    case MarkKind::LinkUrl: return "LinkUrl";
    case MarkKind::EscapeChar: return "EscapeChar";
    case MarkKind::FootnoteDelim: return "FootnoteDelim";
    case MarkKind::TagHash: return "TagHash";
    case MarkKind::AutolinkDelim: return "AutolinkDelim";
    case MarkKind::BlockIdCaret: return "BlockIdCaret";
    }
    return "?";
}

QString renderHtml(const InlineTree& tree, bool cmarkGfmCompat)
{
    Renderer r(tree);
    r.gfmCompat = cmarkGfmCompat;
    r.children(-1, 0, int(tree.content().size()), false);
    return r.out;
}

QString renderParagraph(const InlineTree& tree, bool cmarkGfmCompat)
{
    return QStringLiteral("<p>") + renderHtml(tree, cmarkGfmCompat) + QStringLiteral("</p>\n");
}

QString dumpSexpr(const InlineTree& tree)
{
    QStringList items;
    dumpRange(tree, -1, 0, int(tree.content().size()), items);
    return items.join(QLatin1Char(' '));
}

QString dumpSexpr(const QString& content, const QList<LinkRefDefinition>& refs) { return dumpSexpr(*parseInline(content, refs)); }

} // namespace oc::md::test
