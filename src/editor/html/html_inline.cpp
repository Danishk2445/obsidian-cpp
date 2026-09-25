#include "editor/html/html_inline.h"

#include "render/widgets/inline_widgets.h"
#include "text/text.h"

#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace oc::html {

namespace {

// Formatting of the elements that inline rendering knows (Chromium UA sheet + app.css).
StyleId elementStyle(const Tag& t)
{
    const QString& n = t.name;
    if (n == u"b" || n == u"strong")
        return StyleId::Strong;
    if (n == u"i" || n == u"em" || n == u"cite" || n == u"dfn" || n == u"var")
        return StyleId::Emphasis;
    if (n == u"u" || n == u"ins")
        return StyleId::Underline;
    if (n == u"s" || n == u"strike" || n == u"del")
        return StyleId::Strikethrough;
    if (n == u"mark")
        return StyleId::Highlight;
    if (n == u"code")
        return StyleId::InlineCode;
    if (n == u"kbd")
        return StyleId::Kbd;
    if (n == u"tt" || n == u"samp")
        return StyleId::Monospace;
    if (n == u"sub")
        return StyleId::Subscript;
    if (n == u"sup")
        return StyleId::Superscript;
    if (n == u"small")
        return StyleId::SmallText;
    if (n == u"big")
        return StyleId::LargeText;
    if (n == u"a" && t.hasAttr(u"href"))
        return hasUrlScheme(t.attr(u"href").trimmed()) ? StyleId::ExternalLink : StyleId::InternalLink;
    return StyleId::Normal;
}

// "200" / "200px" -> 200; percentages and garbage -> -1.
int pixelAttr(const QString& v)
{
    QString s = v.trimmed();
    if (s.endsWith(QLatin1String("px")))
        s.chop(2);
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok && d > 0 ? int(std::lround(d)) : -1;
}

bool isBlank(QStringView s)
{
    for (QChar c : s)
        if (!c.isSpace())
            return false;
    return true;
}

} // namespace

bool hasUrlScheme(const QString& url)
{
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
    return re.match(url).hasMatch();
}

QList<int> htmlTagNodes(const md::InlineTree& tree)
{
    QList<int> out;
    for (int id = 0; id < tree.size(); ++id)
        if (tree.node(id).type == md::InlineType::HtmlTag)
            out.push_back(id);
    return out;
}

md::LeafContent blockContent(const md::SyntaxTree& tree, int leaf, const Text& text)
{
    md::LeafContent c;
    const md::BlockNode& n = tree.node(leaf);
    for (int line = n.fromLine; line < n.toLine && line < text.lineCount(); ++line) {
        if (!c.segments.isEmpty())
            c.text += QLatin1Char('\n');
        const QStringView s = text.lineView(line);
        const int from = std::clamp(int(tree.contentColumn(line)), 0, int(s.size()));
        c.segments.push_back(md::LeafContent::Segment{line, from, int(c.text.size()), int(s.size()) - from});
        c.text += s.mid(from);
    }
    return c;
}

InlineStructure pairTags(const md::InlineTree& tree, const QList<int>& htmlNodesIn)
{
    InlineStructure st;
    QList<int> stack; // indices into st.elements
    const QString& content = tree.content();
    QList<int> htmlNodes = htmlNodesIn; // (preorder is document order for these leaves; make sure)
    std::stable_sort(htmlNodes.begin(), htmlNodes.end(), [&](int a, int b) { return tree.node(a).from < tree.node(b).from; });
    for (int id : std::as_const(htmlNodes)) {
        const md::InlineNode& n = tree.node(id);
        Tag tag;
        if (!parseTag(QStringView(content).mid(n.from, n.to - n.from), &tag) || tag.comment) {
            st.comments.push_back(id);
            continue;
        }
        if (tag.closing) {
            qsizetype k = stack.size() - 1;
            while (k >= 0 && st.elements[stack[k]].tag.name != tag.name)
                --k;
            if (k < 0) {
                st.strayClosing.push_back(id);
                continue;
            }
            // elements opened inside the closed one end with it
            for (qsizetype j = stack.size() - 1; j > k; --j)
                st.elements[stack[j]].contentTo = n.from;
            InlineElement& e = st.elements[stack[k]];
            e.close = id;
            e.contentTo = n.from;
            stack.resize(k);
            continue;
        }
        InlineElement e;
        e.open = id;
        e.parent = stack.isEmpty() ? -1 : stack.last();
        e.contentFrom = e.contentTo = n.to;
        const bool isVoid = tag.selfClosing || isVoidElement(tag.name);
        e.tag = std::move(tag);
        st.elements.push_back(std::move(e));
        if (!isVoid)
            stack.push_back(int(st.elements.size() - 1));
    }
    for (int k : std::as_const(stack)) {
        st.elements[k].unclosed = true;
        st.elements[k].contentTo = int(content.size());
    }
    return st;
}

void decorateInline(const InlineContext& c, const QList<int>& htmlNodes, LineDecoration& d)
{
    if (htmlNodes.isEmpty() || !c.tree || !c.content || !c.text || !c.styles)
        return;
    const md::InlineTree& it = *c.tree;
    const Text& text = *c.text;
    const qsizetype ls = c.lineFrom, le = c.lineTo;
    // Browser semantics (Reading view, PDF, rendered table cells): stray / unclosed tags are consumed by the
    // HTML parser instead of being shown as source.
    const bool browser = c.reading || c.forceInactive;

    auto toDoc = [&](int off) { return c.content->toDoc(off, text); };
    auto nodeRange = [&](int id) { return std::pair<qsizetype, qsizetype>(toDoc(it.node(id).from), toDoc(it.node(id).to)); };
    auto clip = [&](qsizetype a, qsizetype b, int* ca, int* cb) {
        if (b < ls || a > le)
            return false;
        *ca = int(std::max(a, ls) - c.colBase);
        *cb = int(std::min(b, le) - c.colBase);
        return *cb > *ca;
    };
    auto mark = [&](qsizetype a, qsizetype b, StyleId s, quint16 custom = 0) {
        int ca, cb;
        if (clip(a, b, &ca, &cb))
            d.marks.push_back(Mark{ca, cb, s, custom});
    };
    auto hide = [&](qsizetype a, qsizetype b) {
        int ca, cb;
        if (clip(a, b, &ca, &cb))
            d.replaces.push_back({ca, cb, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
    };
    auto substitute = [&](qsizetype a, qsizetype b, const QString& s, StyleId style) {
        if (a < ls || b > le)
            return false;
        d.replaces.push_back({int(a - c.colBase), int(b - c.colBase), Replace::Kind::Text, s, style, nullptr});
        return true;
    };
    auto widget = [&](qsizetype a, qsizetype b, std::shared_ptr<InlineWidget> w) {
        if (a < ls || b > le)
            return false;
        d.replaces.push_back({int(a - c.colBase), int(b - c.colBase), Replace::Kind::Widget, {}, StyleId::Normal, std::move(w)});
        return true;
    };
    auto source = [&](int id) {
        const auto [a, b] = nodeRange(id);
        mark(a, b, StyleId::Html);
    };

    const InlineStructure st = pairTags(it, htmlNodes);

    for (int id : st.comments) {
        const auto [a, b] = nodeRange(id);
        if (browser)
            hide(a, b);
        else
            mark(a, b, StyleId::Comment);
    }
    for (int id : st.strayClosing) {
        if (browser)
            hide(nodeRange(id).first, nodeRange(id).second);
        else
            source(id);
    }

    // Live preview reveals a whole nest (the outermost element, widened to its lines when it is alone on
    // them) while the selection touches it, like Obsidian's HTML widget range.
    QList<qint8> rootActive(st.elements.size(), -1);
    auto elementEnd = [&](const InlineElement& e) {
        return e.close >= 0 ? nodeRange(e.close).second : std::max(nodeRange(e.open).second, toDoc(e.contentTo));
    };
    auto rootOf = [&](int k) {
        while (st.elements[k].parent >= 0)
            k = st.elements[k].parent;
        return k;
    };
    auto active = [&](int k) {
        if (browser || !c.touches)
            return false;
        const int r = rootOf(k);
        if (rootActive[r] < 0) {
            qsizetype a = nodeRange(st.elements[r].open).first, b = elementEnd(st.elements[r]);
            const int la = text.lineAt(a), lb = text.lineAt(b);
            const qsizetype lsA = text.lineStart(la), leB = text.lineEnd(lb);
            if (isBlank(text.lineView(la).left(a - lsA)) && isBlank(text.lineView(lb).mid(b - text.lineStart(lb))))
                a = lsA, b = leB;
            rootActive[r] = c.touches(a, b) ? 1 : 0;
        }
        return rootActive[r] == 1;
    };

    const qreal basePx = c.styles->line(c.lineStyle).font.pixelSize();
    for (int k = 0; k < st.elements.size(); ++k) {
        const InlineElement& e = st.elements[k];
        const Tag& t = e.tag;
        const auto [oa, ob] = nodeRange(e.open);
        const qsizetype end = elementEnd(e);
        if (end < ls || oa > le)
            continue;
        qsizetype ca = -1, cb = -1;
        if (e.close >= 0)
            std::tie(ca, cb) = nodeRange(e.close);

        if (isExcludedElement(t.name)) {
            // Obsidian renders none of these; its reading view sanitizes script/style away with their content.
            if (!browser) {
                source(e.open);
                if (e.close >= 0)
                    source(e.close);
            } else if (t.name == u"script" || t.name == u"style") {
                hide(oa, end);
            } else {
                hide(oa, ob);
                if (e.close >= 0)
                    hide(ca, cb);
            }
            continue;
        }
        // Live preview: an element that is never closed keeps HyperMD in HTML mode, so it and everything
        // inside it stays source.
        if ((!browser && st.elements[rootOf(k)].unclosed) || active(k)) {
            source(e.open);
            if (e.close >= 0)
                source(e.close);
            continue;
        }

        if (t.name == u"br") {
            // A line break (U+2028 makes QTextLayout break the line). A trailing one adds no line in a browser:
            // live preview's next document line is another block, Reading view ends the paragraph.
            bool trailing;
            if (!browser) {
                const int line = text.lineAt(ob);
                trailing = isBlank(text.lineView(line).mid(ob - text.lineStart(line)));
            } else {
                trailing = isBlank(QStringView(it.content()).mid(e.contentFrom));
            }
            if (trailing || !substitute(oa, ob, QString(QChar(QChar::LineSeparator)), StyleId::Normal))
                hide(oa, ob);
            continue;
        }
        if (t.name == u"img") {
            const QString src = t.attr(u"src").trimmed();
            QString path;
            if (!src.isEmpty() && c.resolveAttachment) {
                if (src.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
                    path = QUrl(src).toLocalFile();
                else if (!hasUrlScheme(src) && !src.startsWith(QLatin1String("//")))
                    path = c.resolveAttachment(QUrl::fromPercentEncoding(src.toUtf8()));
            }
            if (!path.isEmpty()) {
                auto w = std::make_shared<ImageWidget>(path, pixelAttr(t.attr(u"width")), pixelAttr(t.attr(u"height")), c.contentWidth);
                if (w->valid() && widget(oa, ob, w))
                    continue;
            }
            // Remote / missing images: the alt text (images aren't fetched from the network).
            const QString alt = t.attr(u"alt").simplified();
            if (alt.isEmpty() || !substitute(oa, ob, alt, StyleId::Formatting))
                hide(oa, ob);
            continue;
        }

        hide(oa, ob);
        if (e.close >= 0)
            hide(ca, cb);
        const qsizetype a = toDoc(e.contentFrom), b = toDoc(e.contentTo);
        if (b <= a)
            continue;
        if (const StyleId s = elementStyle(t); s != StyleId::Normal)
            mark(a, b, s);
        InlineStyle css = parseInlineStyle(t.attr(u"style"));
        if (t.name == u"font") {
            if (const QColor fc = parseCssColor(t.attr(u"color")); fc.isValid() && !css.color.isValid())
                css.color = fc;
            if (t.hasAttr(u"size") && css.sizePx <= 0 && css.sizeScale <= 0)
                css.sizePx = fontSizeAttrPx(t.attr(u"size"), basePx);
        }
        if (css.isEmpty())
            continue;
        StyleSpec spec;
        QTextCharFormat& f = spec.format;
        if (css.color.isValid())
            f.setForeground(css.color);
        if (css.background.isValid())
            f.setBackground(css.background);
        if (css.weight > 0)
            f.setFontWeight(css.weight);
        if (css.italic >= 0)
            f.setFontItalic(css.italic == 1);
        if (css.underline >= 0)
            f.setFontUnderline(css.underline == 1);
        if (css.strike >= 0)
            f.setFontStrikeOut(css.strike == 1);
        const qreal px = css.sizePx > 0 ? css.sizePx : css.sizeScale > 0 ? basePx * css.sizeScale : 0;
        if (px > 0)
            f.setProperty(QTextFormat::FontPixelSize, std::max(1, int(std::lround(px))));
        mark(a, b, StyleId::Normal, d.addCustomStyle(spec));
    }
}

std::optional<AnchorHit> anchorAt(const md::InlineTree& tree, const md::LeafContent& content, const Text& text, int off)
{
    const QList<int> nodes = htmlTagNodes(tree);
    if (nodes.isEmpty())
        return std::nullopt;
    const InlineStructure st = pairTags(tree, nodes);
    const InlineElement* best = nullptr;
    for (const InlineElement& e : st.elements) {
        if (e.tag.name != u"a" || !e.tag.hasAttr(u"href") || e.close < 0)
            continue;
        if (off >= e.contentFrom && off <= e.contentTo)
            best = &e; // later = inner
    }
    if (!best)
        return std::nullopt;
    AnchorHit h;
    h.href = best->tag.attr(u"href").trimmed();
    h.external = hasUrlScheme(h.href);
    h.from = content.toDoc(tree.node(best->open).from, text);
    h.to = content.toDoc(tree.node(best->close).to, text);
    return h;
}

} // namespace oc::html
