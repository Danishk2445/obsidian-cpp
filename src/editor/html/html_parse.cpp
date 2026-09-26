#include "editor/html/html_parse.h"

#include "markdown/inline_rules.h"
#include "render/theme/css_value.h"

#include <QFont>
#include <QHash>
#include <QSet>

#include <cmath>

namespace oc::html {

namespace {
using md::rules::isAsciiAlnum;
using md::rules::isAsciiAlpha;

bool isHtmlSpace(QChar c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f'; }

bool inSet(const QSet<QString>& set, QStringView name)
{
    // QSet<QString>::contains needs a QString; names are short.
    return set.contains(name.toString());
}
} // namespace

bool Tag::hasAttr(QStringView name) const
{
    for (const Attribute& a : attrs)
        if (a.name == name)
            return true;
    return false;
}

QString Tag::attr(QStringView name) const
{
    for (const Attribute& a : attrs)
        if (a.name == name)
            return a.value;
    return {};
}

QString decodeEntities(QStringView s)
{
    if (!s.contains(u'&'))
        return s.toString();
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size();) {
        if (s[i] == u'&') {
            const qsizetype end = md::rules::scanEntity(s, i);
            if (end > i) {
                out += md::rules::decodeEntity(s.mid(i, end - i));
                i = end;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

bool parseTag(QStringView s, Tag* out)
{
    if (s.size() < 2 || s[0] != u'<')
        return false;
    Tag t;
    if (s.startsWith(u"<!") || s.startsWith(u"<?")) {
        t.comment = true;
        *out = std::move(t);
        return true;
    }
    qsizetype i = 1;
    if (s[i] == u'/') {
        t.closing = true;
        ++i;
    }
    if (i >= s.size() || !isAsciiAlpha(s[i].unicode()))
        return false;
    const qsizetype nameFrom = i;
    while (i < s.size() && (isAsciiAlnum(s[i].unicode()) || s[i] == u'-'))
        ++i;
    t.name = s.mid(nameFrom, i - nameFrom).toString().toLower();
    const qsizetype n = s.size();
    for (;;) {
        while (i < n && isHtmlSpace(s[i]))
            ++i;
        if (i >= n)
            return false;
        if (s[i] == u'>')
            break;
        if (s[i] == u'/') {
            ++i;
            if (i < n && s[i] == u'>') {
                t.selfClosing = true;
                break;
            }
            continue;
        }
        // attribute name
        const qsizetype an = i;
        while (i < n && !isHtmlSpace(s[i]) && s[i] != u'=' && s[i] != u'>' && s[i] != u'/')
            ++i;
        Attribute a;
        a.name = s.mid(an, i - an).toString().toLower();
        qsizetype j = i;
        while (j < n && isHtmlSpace(s[j]))
            ++j;
        if (j < n && s[j] == u'=') {
            ++j;
            while (j < n && isHtmlSpace(s[j]))
                ++j;
            if (j < n && (s[j] == u'"' || s[j] == u'\'')) {
                const QChar q = s[j];
                const qsizetype close = s.indexOf(q, j + 1);
                if (close < 0)
                    return false;
                a.value = decodeEntities(s.mid(j + 1, close - j - 1));
                i = close + 1;
            } else {
                const qsizetype v = j;
                while (j < n && !isHtmlSpace(s[j]) && s[j] != u'>')
                    ++j;
                a.value = decodeEntities(s.mid(v, j - v));
                i = j;
            }
        }
        if (!t.closing && !a.name.isEmpty())
            t.attrs.push_back(std::move(a));
    }
    *out = std::move(t);
    return true;
}

bool isVoidElement(QStringView name)
{
    static const QSet<QString> set = {QStringLiteral("area"),  QStringLiteral("base"),   QStringLiteral("br"),    QStringLiteral("col"),
                                      QStringLiteral("embed"), QStringLiteral("hr"),     QStringLiteral("img"),   QStringLiteral("input"),
                                      QStringLiteral("link"),  QStringLiteral("meta"),   QStringLiteral("param"), QStringLiteral("source"),
                                      QStringLiteral("track"), QStringLiteral("wbr"),    QStringLiteral("keygen"), QStringLiteral("command")};
    return inSet(set, name);
}

bool isExcludedElement(QStringView name)
{
    static const QSet<QString> set = {QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("link"),   QStringLiteral("meta"),
                                      QStringLiteral("object"), QStringLiteral("embed"), QStringLiteral("webview")};
    return inSet(set, name);
}

bool isInlineElement(QStringView name)
{
    // app.js: an explicit inline list, an explicit block list, otherwise getComputedStyle(el).display.
    static const QSet<QString> inlineTags = {
        QStringLiteral("a"),      QStringLiteral("abbr"),     QStringLiteral("acronym"), QStringLiteral("b"),      QStringLiteral("bdi"),
        QStringLiteral("bdo"),    QStringLiteral("big"),      QStringLiteral("br"),      QStringLiteral("button"), QStringLiteral("canvas"),
        QStringLiteral("cite"),   QStringLiteral("code"),     QStringLiteral("data"),    QStringLiteral("del"),    QStringLiteral("dfn"),
        QStringLiteral("em"),     QStringLiteral("embed"),    QStringLiteral("i"),       QStringLiteral("iframe"), QStringLiteral("img"),
        QStringLiteral("input"),  QStringLiteral("ins"),      QStringLiteral("kbd"),     QStringLiteral("label"),  QStringLiteral("map"),
        QStringLiteral("mark"),   QStringLiteral("meter"),    QStringLiteral("noscript"), QStringLiteral("object"), QStringLiteral("output"),
        QStringLiteral("picture"), QStringLiteral("progress"), QStringLiteral("q"),      QStringLiteral("ruby"),   QStringLiteral("s"),
        QStringLiteral("samp"),   QStringLiteral("select"),   QStringLiteral("small"),   QStringLiteral("span"),   QStringLiteral("strong"),
        QStringLiteral("sub"),    QStringLiteral("sup"),      QStringLiteral("svg"),     QStringLiteral("textarea"), QStringLiteral("time"),
        QStringLiteral("u"),      QStringLiteral("tt"),       QStringLiteral("var"),     QStringLiteral("video"),  QStringLiteral("wbr"),
        QStringLiteral("font"),   QStringLiteral("strike"),   QStringLiteral("nobr")};
    static const QSet<QString> blockTags = {
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),   QStringLiteral("blockquote"), QStringLiteral("details"),
        QStringLiteral("dialog"),  QStringLiteral("dd"),      QStringLiteral("div"),     QStringLiteral("dl"),         QStringLiteral("dt"),
        QStringLiteral("fieldset"), QStringLiteral("figcaption"), QStringLiteral("figure"), QStringLiteral("footer"),   QStringLiteral("form"),
        QStringLiteral("h1"),      QStringLiteral("h2"),      QStringLiteral("h3"),      QStringLiteral("h4"),         QStringLiteral("h5"),
        QStringLiteral("h6"),      QStringLiteral("header"),  QStringLiteral("hgroup"),  QStringLiteral("hr"),         QStringLiteral("li"),
        QStringLiteral("main"),    QStringLiteral("nav"),     QStringLiteral("ol"),      QStringLiteral("p"),          QStringLiteral("pre"),
        QStringLiteral("ul"),
        // not in Obsidian's list, but not `display: inline*` in Chromium's UA sheet
        QStringLiteral("table"),   QStringLiteral("caption"), QStringLiteral("thead"),   QStringLiteral("tbody"),      QStringLiteral("tfoot"),
        QStringLiteral("tr"),      QStringLiteral("td"),      QStringLiteral("th"),      QStringLiteral("col"),        QStringLiteral("colgroup"),
        QStringLiteral("center"),  QStringLiteral("section"), QStringLiteral("summary"), QStringLiteral("menu"),       QStringLiteral("dir"),
        QStringLiteral("legend"),  QStringLiteral("optgroup"), QStringLiteral("option"), QStringLiteral("search"),     QStringLiteral("body"),
        QStringLiteral("html"),    QStringLiteral("head"),    QStringLiteral("title"),   QStringLiteral("template"),   QStringLiteral("style"),
        QStringLiteral("script"),  QStringLiteral("link"),    QStringLiteral("meta"),    QStringLiteral("base"),       QStringLiteral("audio"),
        QStringLiteral("listing"), QStringLiteral("plaintext"), QStringLiteral("xmp"),   QStringLiteral("frameset"),   QStringLiteral("frame"),
        QStringLiteral("noframes"), QStringLiteral("param"),  QStringLiteral("source"),  QStringLiteral("track"),      QStringLiteral("area"),
        QStringLiteral("datalist")};
    if (name.isEmpty())
        return false;
    for (QChar c : name)
        if (!isAsciiAlpha(c.unicode()))
            return false; // Obsidian: /^[a-z]+$/i (h1..h6 and custom elements with '-' count as block)
    if (inSet(inlineTags, name))
        return true;
    return !inSet(blockTags, name);
}

QColor parseCssColor(QStringView value)
{
    const QString v = value.trimmed().toString().toLower();
    if (v.isEmpty())
        return {};
    if (v.startsWith(u'#'))
        return CssEvaluator::parseHexColor(QStringView(v).mid(1));
    if (v == u"transparent")
        return QColor(0, 0, 0, 0);
    if (v.contains(u'(')) {
        if (v.contains(QLatin1String("var(")))
            return {};
        static const CssEvaluator eval([](const QString&, bool* found) {
            *found = false;
            return QString();
        });
        return eval.evaluate(v).toColor();
    }
    QColor c = QColor::fromString(v); // SVG colour keywords = CSS named colours
    return c.isValid() ? c : QColor();
}

qreal fontSizeAttrPx(QStringView value, qreal basePx)
{
    // HTML's legacy <font size>: 1..7 -> x-small, small, medium, large, x-large, xx-large, xxx-large.
    static const qreal sizes[] = {10, 13, 16, 18, 24, 32, 48};
    const QString v = value.trimmed().toString();
    bool ok = false;
    int n = v.toInt(&ok);
    if (!ok)
        return 0;
    if (v.startsWith(u'+') || v.startsWith(u'-'))
        n += 3;
    n = std::clamp(n, 1, 7);
    return sizes[n - 1] * basePx / 16.0;
}

InlineStyle parseInlineStyle(QStringView css)
{
    InlineStyle st;
    for (QStringView decl : css.tokenize(u';')) {
        const qsizetype colon = decl.indexOf(u':');
        if (colon < 0)
            continue;
        const QString prop = decl.left(colon).trimmed().toString().toLower();
        QString value = decl.mid(colon + 1).trimmed().toString().toLower();
        if (value.endsWith(QLatin1String("!important")))
            value = value.chopped(10).trimmed();
        if (prop == u"color") {
            st.color = parseCssColor(value);
        } else if (prop == u"background-color" || prop == u"background") {
            // `background` shorthand: take the first token that is a colour
            QColor c = parseCssColor(value);
            if (!c.isValid() && prop == u"background")
                for (QStringView part : QStringView(value).tokenize(u' ', Qt::SkipEmptyParts))
                    if ((c = parseCssColor(part)).isValid())
                        break;
            st.background = c;
        } else if (prop == u"font-weight") {
            bool ok = false;
            const int w = value.toInt(&ok);
            if (ok)
                st.weight = std::clamp(w, 1, 1000);
            else if (value == u"bold" || value == u"bolder")
                st.weight = QFont::Bold;
            else if (value == u"normal" || value == u"lighter")
                st.weight = QFont::Normal;
        } else if (prop == u"font-style") {
            st.italic = value == u"italic" || value == u"oblique" ? 1 : value == u"normal" ? 0 : -1;
        } else if (prop == u"text-decoration" || prop == u"text-decoration-line") {
            if (value.contains(QLatin1String("underline")))
                st.underline = 1;
            if (value.contains(QLatin1String("line-through")))
                st.strike = 1;
            if (value == u"none") {
                st.underline = 0;
                st.strike = 0;
            }
        } else if (prop == u"font-size") {
            static const QHash<QString, qreal> keywords = {
                {QStringLiteral("xx-small"), 9}, {QStringLiteral("x-small"), 10}, {QStringLiteral("small"), 13},  {QStringLiteral("medium"), 16},
                {QStringLiteral("large"), 18},   {QStringLiteral("x-large"), 24}, {QStringLiteral("xx-large"), 32}, {QStringLiteral("xxx-large"), 48}};
            bool ok = false;
            if (value == u"smaller") {
                st.sizeScale = 1 / 1.2;
            } else if (value == u"larger") {
                st.sizeScale = 1.2;
            } else if (keywords.contains(value)) {
                st.sizeScale = keywords.value(value) / 16.0;
            } else if (value.endsWith(QLatin1String("px"))) {
                const qreal px = value.chopped(2).trimmed().toDouble(&ok);
                if (ok && px > 0)
                    st.sizePx = px;
            } else if (value.endsWith(QLatin1String("rem")) || value.endsWith(QLatin1String("em"))) {
                const qreal em = value.chopped(value.endsWith(QLatin1String("rem")) ? 3 : 2).trimmed().toDouble(&ok);
                if (ok && em > 0)
                    st.sizeScale = em;
            } else if (value.endsWith(u'%')) {
                const qreal pct = value.chopped(1).trimmed().toDouble(&ok);
                if (ok && pct > 0)
                    st.sizeScale = pct / 100;
            } else if (value.endsWith(QLatin1String("pt"))) {
                const qreal pt = value.chopped(2).trimmed().toDouble(&ok);
                if (ok && pt > 0)
                    st.sizePx = pt * 96 / 72;
            }
        }
    }
    return st;
}

QList<Token> tokenize(QStringView s)
{
    QList<Token> out;
    md::rules::HtmlScanMemo memo;
    qsizetype text = 0;
    auto flushText = [&](qsizetype to) {
        if (to > text)
            out.push_back(Token{Token::Kind::Text, text, to, {}});
    };
    for (qsizetype i = s.indexOf(u'<'); i >= 0 && i < s.size(); i = s.indexOf(u'<', i)) {
        const qsizetype end = md::rules::scanHtmlTag(s, i, nullptr, &memo);
        Tag tag;
        if (end > i && parseTag(s.mid(i, end - i), &tag)) {
            flushText(i);
            out.push_back(Token{Token::Kind::Tag, i, end, std::move(tag)});
            text = i = end;
        } else {
            ++i;
        }
    }
    flushText(s.size());
    return out;
}

} // namespace oc::html
