#include "editor/live_preview_decorator.h"

#include "markdown/doc_source.h"
#include "render/services/code_highlighter.h"
#include "render/widgets/inline_widgets.h"
#include "editor/embeds/embed_registry.h"
#include "editor/footnotes/footnotes.h"
#include "editor/html/html_block.h"
#include "editor/html/html_inline.h"
#include "editor/widgets/block_widgets.h"

#include <QFontMetricsF>
#include <QRegularExpression>
#include <QUrl>

namespace oc {

namespace {
using md::InlineType;
using md::LineRole;
using md::MarkKind;
using md::NodeType;

StyleId headingStyle(int level) { return StyleId(int(StyleId::Heading1) + std::clamp(level, 1, 6) - 1); }

bool isCodeRole(LineRole r)
{
    return r == LineRole::FenceOpen || r == LineRole::FenceBody || r == LineRole::FenceClose || r == LineRole::IndentedCode;
}

bool isImageExt(const QString& target)
{
    static const QStringList exts = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
                                     QStringLiteral("bmp"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
    const qsizetype dot = target.lastIndexOf(QLatin1Char('.'));
    return dot >= 0 && exts.contains(target.mid(dot + 1).toLower());
}

// PDF / audio / video / note embeds (registered providers, see editor/embeds): hosted widgets.
std::shared_ptr<InlineWidget> fileEmbed(const std::function<QString(const QString&)>& resolve, const QString& target, const QString& subpath,
                                        int width, int height, qreal maxWidth)
{
    return makeLinkEmbedWidget(resolve, target, subpath, width, height, maxWidth);
}

bool hasScheme(const QString& dest)
{
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
    return re.match(dest).hasMatch();
}

// Clip a doc range to a line and convert to columns.
bool toColumns(qsizetype a, qsizetype b, qsizetype lineStart, qsizetype lineEnd, int* ca, int* cb)
{
    if (b < lineStart || a > lineEnd)
        return false;
    *ca = int(std::max(a, lineStart) - lineStart);
    *cb = int(std::min(b, lineEnd) - lineStart);
    return *cb >= *ca;
}
} // namespace

bool LivePreviewDecorator::touches(qsizetype from, qsizetype to) const
{
    if (m_reading)
        return false;
    for (const SelRange& r : m_sel.ranges())
        if (r.from() <= to && r.to() >= from)
            return true;
    return false;
}

bool LivePreviewDecorator::lineActive(int line) const
{
    if (m_reading || !m_doc)
        return false;
    const Text& t = m_doc->text();
    return touches(t.lineStart(line), t.lineEnd(line));
}

StyleId LivePreviewDecorator::quickLineStyle(int line)
{
    if (!m_doc)
        return StyleId::Normal;
    const md::SyntaxTree& tree = m_doc->syntax();
    switch (tree.role(line)) {
    case LineRole::AtxHeading:
    case LineRole::SetextText: {
        const int leaf = tree.leafAt(line);
        return leaf >= 0 ? headingStyle(tree.heading(leaf).level) : StyleId::Normal;
    }
    case LineRole::Paragraph:
        return footnotes::isDefinitionLine(tree, m_doc->text(), line) ? StyleId::FootnoteLine : StyleId::Normal;
    case LineRole::FenceOpen:
    case LineRole::FenceBody:
    case LineRole::FenceClose:
    case LineRole::IndentedCode:
        return StyleId::CodeBlock;
    case LineRole::MathOpen:
    case LineRole::MathBody:
    case LineRole::MathClose:
    case LineRole::MathOneLine:
        return StyleId::MathBlock;
    case LineRole::FrontmatterOpen:
    case LineRole::FrontmatterBody:
    case LineRole::FrontmatterClose:
        return StyleId::Frontmatter;
    case LineRole::CommentOpen:
    case LineRole::CommentBody:
    case LineRole::CommentClose:
        return StyleId::CommentBlock;
    case LineRole::Html:
        return StyleId::HtmlBlock;
    default:
        return StyleId::Normal;
    }
}

qreal LivePreviewDecorator::quickVerticalPadding(int line)
{
    if (!m_doc || !m_styles)
        return 0;
    const LineRole r = m_doc->syntax().role(line);
    if (r == LineRole::AtxHeading || r == LineRole::SetextText)
        return m_styles->pSpacing();
    return 0;
}

QList<QPair<int, int>> LivePreviewDecorator::affectedLines(const Selection& a, const Selection& b) const
{
    QList<QPair<int, int>> out;
    if (!m_doc)
        return out;
    const Text& t = m_doc->text();
    const md::SyntaxTree& tree = m_doc->syntax();
    auto addRange = [&](qsizetype from, qsizetype to) {
        int la = t.lineAt(from), lb = t.lineAt(to);
        // extend to whole leaves (inline nodes can span lines; fences/blocks reveal as a unit)
        const int leafA = tree.leafAt(la), leafB = tree.leafAt(lb);
        if (leafA >= 0)
            la = std::min(la, tree.node(leafA).fromLine);
        if (leafB >= 0)
            lb = std::max(lb, tree.node(leafB).toLine - 1);
        out.push_back({la, lb + 1});
    };
    for (const SelRange& r : a.ranges())
        addRange(r.from(), r.to());
    for (const SelRange& r : b.ranges())
        addRange(r.from(), r.to());
    return out;
}

bool LivePreviewDecorator::codeBlockEmbed(int line, int leaf, md::LineRole role, LineDecoration& d)
{
    // Fenced code blocks of a registered language (EmbedRegistry::registerCodeBlockLanguage) render
    // as the provider's embed on the opening line, the other lines hidden, unless being edited.
    if ((role != LineRole::FenceOpen && role != LineRole::FenceBody && role != LineRole::FenceClose) || leaf < 0
        || EmbedRegistry::instance().isEmptyOfCodeBlocks())
        return false;
    const md::SyntaxTree& tree = m_doc->syntax();
    if (tree.node(leaf).type != NodeType::FencedCode)
        return false;
    const md::FenceInfo fi = tree.fence(leaf);
    if (fi.language.isEmpty() || !EmbedRegistry::instance().isCodeBlockLanguageRegistered(fi.language))
        return false;
    const Text& text = m_doc->text();
    const md::BlockNode& n = tree.node(leaf);
    if (touches(text.lineStart(n.fromLine), text.lineEnd(n.toLine - 1)))
        return false;
    if (line != n.fromLine) {
        d.hidden = true;
        return true;
    }
    QString source;
    for (int l = n.fromLine + 1; l < n.toLine - (fi.closed ? 1 : 0); ++l)
        source += text.lineView(l).mid(std::min(tree.contentColumn(l), text.lineLength(l))) + u'\n';
    std::shared_ptr<InlineWidget> w = makeCodeBlockEmbedWidget(fi.language, source, m_contentWidth);
    if (!w)
        return false;
    d.lineStyle = StyleId::Normal;
    d.replaces.push_back({0, int(text.lineLength(line)), Replace::Kind::Widget, {}, StyleId::Normal, std::move(w)});
    return true;
}

LineDecoration LivePreviewDecorator::decorate(int line)
{
    LineDecoration d;
    if (!m_doc || !m_styles)
        return d;
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    const QStringView lv = text.lineView(line);
    const int len = int(lv.size());
    const LineRole role = tree.role(line);
    const bool active = lineActive(line);
    const int leaf = tree.leafAt(line);
    d.lineStyle = quickLineStyle(line);
    // Reading view: footnote definitions leave the flow (the footnotes section shows them).
    if (hidesFootnoteDefinitions() && footnotes::inDefinition(tree, line)) {
        d.hidden = true;
        return d;
    }
    if (codeBlockEmbed(line, leaf, role, d))
        return d; // a ```base fence (EmbedRegistry code block languages) shown as its embed

    // ---- container markers -----------------------------------------------------------------------
    bool inList = false;
    bool footnoteLine = false;
    int listItemNode = -1;
    for (const md::ContainerMarker& m : tree.containerMarkers(line)) {
        switch (m.kind) {
        case md::ContainerMarker::Kind::Quote: {
            // Obsidian keeps '>' in place (transparent on inactive lines) and draws a bar at its x.
            d.marks.push_back({m.from, m.to, active ? StyleId::Formatting : StyleId::Invisible});
            d.quoteColumns.push_back(m.from);
            break;
        }
        case md::ContainerMarker::Kind::ListMarker: {
            inList = true;
            listItemNode = m.node;
            break;
        }
        case md::ContainerMarker::Kind::ListIndent:
            inList = true;
            break;
        case md::ContainerMarker::Kind::FootnoteLabel: {
            // "[^label]:" (.cm-hmd-footnote); Live Preview hides "[^" and "]:" while the selection is away.
            footnoteLine = true;
            const int close = int(lv.indexOf(u']', m.from + 2));
            if (m_reading) {
                d.replaces.push_back({m.from, m.to, Replace::Kind::Hide, {}, StyleId::Normal, nullptr}); // content only
            } else if (close > m.from + 2 && close + 2 <= m.to) {
                const qsizetype ls = text.lineStart(line);
                d.marks.push_back({m.from, close + 2, StyleId::FootnoteMarker});
                if (touches(ls + m.from, ls + close + 2)) {
                    d.marks.push_back({m.from, m.from + 2, StyleId::Formatting});
                    d.marks.push_back({close, close + 2, StyleId::Formatting});
                } else {
                    d.replaces.push_back({m.from, m.from + 2, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
                    d.replaces.push_back({close, close + 2, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
                }
            }
            break;
        }
        case md::ContainerMarker::Kind::FootnoteIndent:
            footnoteLine = true;
            break;
        }
    }
    if (footnoteLine && d.lineStyle == StyleId::Normal)
        d.lineStyle = StyleId::FootnoteLine; // .HyperMD-footnote

    if (inList) {
        d.tabStop = m_styles->listIndent();
        d.hangingColumn = tree.contentColumn(line);
        d.paddingTop = m_styles->listSpacing();
        d.paddingBottom = m_styles->listSpacing();
    }

    // List item marker (bullet / checkbox) on the item's first line.
    if (listItemNode >= 0 && tree.node(listItemNode).type == NodeType::ListItem && tree.node(listItemNode).fromLine == line) {
        const md::ListItemInfo li = tree.listItem(listItemNode);
        const bool task = tree.node(listItemNode).flags & md::FlagTask;
        const qsizetype ls = text.lineStart(line);
        if (task && li.taskFrom >= 0) {
            const int boxTo = li.taskFrom + 3; // "[x]"
            if (!touches(ls + li.markerFrom, ls + boxTo)) {
                Replace hideBullet;
                hideBullet.from = li.markerFrom;
                hideBullet.to = li.taskFrom;
                hideBullet.kind = Replace::Kind::Hide;
                d.replaces.push_back(hideBullet);
                Replace box;
                box.from = li.taskFrom;
                box.to = boxTo;
                box.kind = Replace::Kind::Widget;
                const qreal size = m_styles->textSize();
                box.widget = std::make_shared<CheckboxWidget>(li.taskChar, size, m_styles->textSize() * 0.5);
                d.replaces.push_back(box);
                if (li.taskChar != u' ')
                    d.marks.push_back({boxTo, len, StyleId::TaskDone});
            } else {
                d.marks.push_back({li.markerFrom, li.markerTo, StyleId::ListMarker});
                d.marks.push_back({li.taskFrom, boxTo, StyleId::Formatting});
            }
        } else if (li.bullet == u'-' || li.bullet == u'*' || li.bullet == u'+') {
            if (li.markerTo > li.markerFrom) {
                Replace r;
                r.from = li.markerFrom;
                r.to = li.markerTo;
                r.kind = Replace::Kind::Widget;
                const qreal w = QFontMetricsF(m_styles->line(d.lineStyle).font).horizontalAdvance(lv.mid(li.markerFrom, li.markerTo - li.markerFrom).toString());
                r.widget = std::make_shared<BulletWidget>(m_styles->line(StyleId::Normal).color.isValid() ? m_styles->faint() : m_styles->faint(), w);
                d.replaces.push_back(r);
            }
        } else {
            d.marks.push_back({li.markerFrom, li.markerTo, StyleId::ListMarker});
        }
    }

    // ---- block roles -------------------------------------------------------------------------------
    switch (role) {
    case LineRole::AtxHeading: {
        if (leaf >= 0) {
            const md::HeadingInfo h = tree.heading(leaf);
            if (!active) {
                d.replaces.push_back({h.markerFrom, h.markerTo, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
                if (h.closeFrom >= 0)
                    d.replaces.push_back({h.closeFrom, len, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
            } else {
                d.marks.push_back({h.markerFrom, h.markerTo, StyleId::Formatting});
                if (h.closeFrom >= 0)
                    d.marks.push_back({h.closeFrom, len, StyleId::Formatting});
            }
        }
        d.paddingTop = m_styles->pSpacing();
        break;
    }
    case LineRole::SetextText:
        d.paddingTop = m_styles->pSpacing();
        break;
    case LineRole::SetextUnderline:
        if (!active)
            d.replaces.push_back({0, len, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
        else
            d.marks.push_back({0, len, StyleId::Formatting});
        break;
    case LineRole::ThematicBreak:
        if (!active) {
            d.replaces.push_back({0, len, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
            d.hrRule = true;
        } else {
            d.marks.push_back({0, len, StyleId::Formatting});
        }
        break;
    case LineRole::FenceOpen:
    case LineRole::FenceClose: {
        const bool blockActive = leaf >= 0 && touches(text.lineStart(tree.node(leaf).fromLine), text.lineEnd(tree.node(leaf).toLine - 1));
        const int col = tree.contentColumn(line);
        if (!blockActive)
            d.replaces.push_back({col, len, Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
        else
            d.marks.push_back({col, len, StyleId::Formatting});
        break;
    }
    case LineRole::FenceBody:
        codeHighlight(line, leaf, d);
        break;
    case LineRole::MathOpen:
    case LineRole::MathBody:
    case LineRole::MathClose:
    case LineRole::MathOneLine:
        d.marks.push_back({tree.contentColumn(line), len, StyleId::Math});
        break;
    case LineRole::FrontmatterOpen:
    case LineRole::FrontmatterClose:
        d.marks.push_back({0, len, StyleId::Formatting});
        break;
    case LineRole::CommentOpen:
    case LineRole::CommentBody:
    case LineRole::CommentClose:
        if (m_reading)
            d.hidden = true;
        break;
    case LineRole::Blank:
        if (m_reading && d.quoteColumns.isEmpty()) {
            // Reading view: paragraph spacing instead of empty lines; runs of blank lines collapse.
            if (previousVisibleLineBlank(line))
                d.hidden = true;
            else
                d.fixedHeight = m_styles->pSpacing();
        }
        break;
    default:
        break;
    }

    if (isCodeRole(role)) {
        const bool prevCode = line > 0 && isCodeRole(tree.role(line - 1)) && tree.leafAt(line - 1) == leaf;
        const bool nextCode = line + 1 < tree.lineCount() && isCodeRole(tree.role(line + 1)) && tree.leafAt(line + 1) == leaf;
        d.code = prevCode ? (nextCode ? LineDecoration::CodeEdge::Middle : LineDecoration::CodeEdge::Bottom)
                          : (nextCode ? LineDecoration::CodeEdge::Top : LineDecoration::CodeEdge::Single);
        d.hangingColumn = -1;
    }

    // ---- inline ------------------------------------------------------------------------------------
    if (leaf >= 0) {
        const NodeType lt = tree.node(leaf).type;
        if (lt == NodeType::Paragraph || lt == NodeType::AtxHeading || lt == NodeType::SetextHeading)
            inlineDecorations(line, leaf, d);
        else if (lt == NodeType::HtmlBlock && tree.parent(leaf) > 0) {
            // An HTML block inside a list / quote (top-level ones are block widgets): its elements render inline
            // and reveal one by one, like a paragraph's.
            d.lineStyle = StyleId::Normal;
            const md::LeafContent content = html::blockContent(tree, leaf, text);
            inlineDecorationsFor(content, tree.inlineTree(content), text.lineStart(line), text.lineEnd(line), text.lineStart(line), d, d.lineStyle, false);
        }
    }

    std::sort(d.replaces.begin(), d.replaces.end(), [](const Replace& a, const Replace& b) { return a.from < b.from || (a.from == b.from && a.to < b.to); });
    // drop overlapping replaces (keep the first)
    QList<Replace> clean;
    int lastTo = -1;
    for (const Replace& r : d.replaces) {
        if (r.from < lastTo)
            continue;
        clean.push_back(r);
        lastTo = std::max(lastTo, r.to);
    }
    d.replaces = clean;
    if (m_indentGuides && !d.hidden) {
        if (m_reading)
            readingGuides(line, d);
        else // --indentation-guide-editing-indent: 0.85em, 0 in code blocks (.cm-hmd-codeblock)
            indent::decorateLine(lv, line, m_activeIndent, isCodeRole(role) ? 0 : 0.85, *m_styles, d);
    }
    if (extra)
        extra(line, d);
    return d;
}

LineDecoration LivePreviewDecorator::decorateRun(int fromLine, int toLine)
{
    // Strict line breaks (Reading view): the lines of a paragraph as one line. Each soft break (the previous line's
    // trailing whitespace, the line break, the next line's container markers and leading whitespace, all stripped
    // by CommonMark) shows as one space; line-level properties (list / quote layout, guides) are the first line's.
    LineDecoration d = decorate(fromLine);
    if (!m_doc)
        return d;
    const Text& text = m_doc->text();
    const md::SyntaxTree& tree = m_doc->syntax();
    const qsizetype base = text.lineStart(fromLine);
    for (int l = fromLine + 1; l < toLine; ++l) {
        LineDecoration n = decorate(l);
        const int shift = int(text.lineStart(l) - base);
        const QStringView prev = text.lineView(l - 1);
        int trail = int(prev.size());
        while (trail > 0 && (prev[trail - 1] == u' ' || prev[trail - 1] == u'\t'))
            --trail;
        int sepFrom = int(text.lineStart(l - 1) - base) + trail;
        for (const Replace& r : std::as_const(d.replaces))
            sepFrom = std::max(sepFrom, r.to);
        sepFrom = std::min(sepFrom, shift - 1); // at least the line break
        const QStringView cur = text.lineView(l);
        int lead = std::min<int>(tree.contentColumn(l), int(cur.size()));
        while (lead < cur.size() && (cur[lead] == u' ' || cur[lead] == u'\t'))
            ++lead;
        int sepTo = shift + lead;
        for (const Replace& r : std::as_const(n.replaces))
            sepTo = std::min(sepTo, shift + r.from);
        sepTo = std::max(sepTo, shift);
        d.replaces.push_back({sepFrom, sepTo, Replace::Kind::Text, QStringLiteral(" "), StyleId::Normal, nullptr});
        const quint16 customBase = quint16(d.customStyles.size());
        for (Replace r : std::as_const(n.replaces)) {
            r.from += shift;
            r.to += shift;
            d.replaces.push_back(std::move(r));
        }
        for (Mark m : std::as_const(n.marks)) {
            m.from += shift;
            m.to += shift;
            if (m.custom > 0)
                m.custom += customBase;
            d.marks.push_back(m);
        }
        d.customStyles += n.customStyles;
        d.paddingBottom = n.paddingBottom;
    }
    return d;
}

void LivePreviewDecorator::readingGuides(int line, LineDecoration& d) const
{
    // .markdown-rendered.show-indentation-guide li > ul::before (and li > ol::before): a line from the top of a
    // nested list to its bottom, inset -0.85em from the list's start, i.e. under the parent item's bullet. Each
    // line of the nested list paints its piece over the line's full height, so the pieces connect.
    const md::SyntaxTree& tree = m_doc->syntax();
    const QList<int> ancestors = tree.ancestorsAt(line);
    for (int id : ancestors) {
        const md::BlockNode& n = tree.node(id);
        if (n.type != NodeType::BulletList && n.type != NodeType::OrderedList)
            continue;
        const int item = tree.parent(id);
        if (item < 0 || tree.node(item).type != NodeType::ListItem)
            continue;
        if (tree.role(line) == LineRole::Blank) {
            // blank lines after the nested list's last item aren't part of it
            int next = line + 1;
            while (next < n.toLine && tree.role(next) == LineRole::Blank)
                ++next;
            if (next >= n.toLine)
                continue;
        }
        d.guidePositions.push_back(markerCenter(item) - m_styles->guideWidth() / 2);
    }
}

qreal LivePreviewDecorator::markerCenter(int item) const
{
    // The x of the item's bullet / number / checkbox centre on its first line (as decorate() lays it out).
    const md::SyntaxTree& tree = m_doc->syntax();
    const md::ListItemInfo li = tree.listItem(item);
    const QStringView lv = m_doc->text().lineView(tree.node(item).fromLine);
    const QFontMetricsF fm(m_styles->line(StyleId::Normal).font);
    const qreal tab = m_styles->listIndent(); // list lines' tab stops
    qreal x = 0;
    for (int c = 0; c < li.markerFrom && c < lv.size(); ++c)
        x = lv[c] == u'\t' ? (std::floor(x / tab + 1e-6) + 1) * tab : x + fm.horizontalAdvance(lv[c]);
    if ((tree.node(item).flags & md::FlagTask) && li.taskFrom >= 0)
        return x + m_styles->textSize() / 2; // the checkbox replaces the bullet
    const int markerTo = std::clamp<int>(li.markerTo, li.markerFrom, int(lv.size()));
    return x + fm.horizontalAdvance(lv.mid(li.markerFrom, markerTo - li.markerFrom).toString()) / 2;
}

void LivePreviewDecorator::paragraphRuns(int fromLine, int toLine, QList<BlockRange>& out) const
{
    // Strict line breaks: runs of paragraph lines joined by soft breaks, starting in [fromLine, toLine). Hard breaks
    // (two trailing spaces, an unescaped trailing backslash) end a run; a code span or HTML tag across the break
    // isn't told apart (rare).
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    auto isHardBreak = [&](int l) {
        const QStringView v = text.lineView(l);
        if (v.size() >= 2 && v.endsWith(u"  "))
            return true;
        int slashes = 0;
        for (qsizetype i = v.size() - 1; i >= 0 && v[i] == u'\\'; --i)
            ++slashes;
        return slashes % 2 == 1;
    };
    auto visit = [&](int id) {
        const md::BlockNode& n = tree.node(id);
        if (n.type != NodeType::Paragraph || n.toLine - n.fromLine < 2 || n.toLine <= fromLine)
            return;
        for (int p = tree.parent(id); p > 0; p = tree.parent(p)) {
            const NodeType t = tree.node(p).type;
            if (t == NodeType::Callout && tree.parent(p) == 0)
                return; // rendered by the callout widget (a nested rendering with its own runs)
            if (t == NodeType::FootnoteDef && hidesFootnoteDefinitions())
                return; // not in the flow
        }
        int start = n.fromLine;
        for (int l = n.fromLine; l < n.toLine; ++l) {
            if (l + 1 < n.toLine && !isHardBreak(l))
                continue;
            if (l > start && start >= fromLine && start < toLine)
                out.push_back(BlockRange{start, l + 1, nullptr, true});
            start = l + 1;
        }
    };
    const int first = tree.leafAt(fromLine);
    if (first >= 0 && tree.node(first).fromLine < fromLine)
        visit(first); // a paragraph running into the window (a run after a hard break may start in it)
    for (int id = tree.firstNodeAtOrAfter(fromLine); id < tree.nodeCount() && tree.node(id).fromLine < toLine; ++id)
        visit(id);
}

void LivePreviewDecorator::inlineDecorations(int line, int leaf, LineDecoration& d)
{
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    const md::LeafContent content = tree.leafContent(leaf, text);
    const md::InlineTreePtr it = tree.inlineTree(content);
    const qsizetype ls = text.lineStart(line), le = text.lineEnd(line);
    inlineDecorationsFor(content, it, ls, le, ls, d, d.lineStyle, false);
}

void LivePreviewDecorator::inlineDecorationsFor(const md::LeafContent& content, const md::InlineTreePtr& it, qsizetype ls, qsizetype le,
                                                qsizetype colBase, LineDecoration& d, StyleId lineStyle, bool forceInactive) const
{
    if (!it || it->size() == 0)
        return;
    const Text& text = m_doc->text();
    auto docRange = [&](const md::InlineNode& n) { return std::pair<qsizetype, qsizetype>(content.toDoc(n.from, text), content.toDoc(n.to, text)); };
    auto addMark = [&](qsizetype a, qsizetype b, StyleId s) {
        int ca, cb;
        if (toColumns(a, b, ls, le, &ca, &cb) && cb > ca)
            d.marks.push_back({ca + int(ls - colBase), cb + int(ls - colBase), s});
    };
    auto addHide = [&](qsizetype a, qsizetype b) {
        int ca, cb;
        if (toColumns(a, b, ls, le, &ca, &cb) && cb > ca)
            d.replaces.push_back({ca + int(ls - colBase), cb + int(ls - colBase), Replace::Kind::Hide, {}, StyleId::Normal, nullptr});
    };
    auto addWidget = [&](qsizetype a, qsizetype b, std::shared_ptr<InlineWidget> w) {
        if (a < ls || b > le)
            return false; // widgets must stay within the window
        d.replaces.push_back({int(a - colBase), int(b - colBase), Replace::Kind::Widget, {}, StyleId::Normal, std::move(w)});
        return true;
    };
    auto addSubstitute = [&](qsizetype a, qsizetype b, const QString& str, StyleId st) {
        if (a < ls || b > le)
            return;
        d.replaces.push_back({int(a - colBase), int(b - colBase), Replace::Kind::Text, str, st, nullptr});
    };
    auto forChildren = [&](int id, const std::function<void(int)>& fn) {
        for (int c = it->node(id).firstChild; c >= 0; c = it->node(c).next)
            fn(c);
    };
    auto contentRange = [&](int id) {
        auto [a, b] = docRange(it->node(id));
        forChildren(id, [&](int c) {
            const md::InlineNode& ch = it->node(c);
            if (ch.type != InlineType::Mark)
                return;
            auto [ca, cb] = docRange(ch);
            if (ca == a)
                a = cb;
            else if (cb == b)
                b = ca;
        });
        return std::pair<qsizetype, qsizetype>(a, b);
    };
    const qreal textPx = m_styles->line(lineStyle).font.pixelSize();
    const QColor textColor = m_styles->line(lineStyle).color;
    const bool reading = m_reading || forceInactive;
    QList<int> htmlTags; // rendered together below (tags pair up across the leaf)
    // Nodes outside the window are skipped in content offsets, before mapping them to the document (every line
    // of a long paragraph walks all of the paragraph's nodes).
    const auto [winFrom, winTo] = html::contentWindow(content, text, ls, le);
    for (int id = 0; id < it->size(); ++id) {
        const md::InlineNode& n = it->node(id);
        if (n.type == InlineType::Mark || n.type == InlineType::Text)
            continue;
        if (n.type == InlineType::HtmlTag) {
            htmlTags.push_back(id);
            continue;
        }
        if (n.to < winFrom || n.from > winTo)
            continue;
        const auto [a, b] = docRange(n);
        if (b < ls || a > le)
            continue;
        const bool act = !forceInactive && touches(a, b);
        auto marksOf = [&](int nodeId, bool hide) {
            forChildren(nodeId, [&](int c) {
                const md::InlineNode& ch = it->node(c);
                if (ch.type != InlineType::Mark)
                    return;
                const auto [ca, cb] = docRange(ch);
                if (hide)
                    addHide(ca, cb);
                else
                    addMark(ca, cb, StyleId::Formatting);
            });
        };
        switch (n.type) {
        case InlineType::Strong:
        case InlineType::Emphasis:
        case InlineType::Strikethrough:
        case InlineType::Highlight: {
            const StyleId s = n.type == InlineType::Strong ? StyleId::Strong
                              : n.type == InlineType::Emphasis ? StyleId::Emphasis
                              : n.type == InlineType::Strikethrough ? StyleId::Strikethrough
                                                                    : StyleId::Highlight;
            const auto [ca, cb] = contentRange(id);
            addMark(ca, cb, s);
            marksOf(id, !act);
            break;
        }
        case InlineType::CodeSpan: {
            if (act) {
                addMark(a, b, StyleId::InlineCode);
            } else {
                const auto [ca, cb] = contentRange(id);
                addMark(ca, cb, StyleId::InlineCode);
                marksOf(id, true);
            }
            break;
        }
        case InlineType::InlineMath:
        case InlineType::DisplayMath: {
            const auto [ca, cb] = contentRange(id);
            if (!act) {
                const QString tex = text.slice(ca, cb);
                if (!addWidget(a, b, std::make_shared<MathWidget>(tex, n.type == InlineType::DisplayMath, textPx, textColor)))
                    addMark(a, b, StyleId::Math);
            } else {
                addMark(ca, cb, StyleId::Math);
                marksOf(id, false);
            }
            break;
        }
        case InlineType::Comment:
            if (reading)
                addHide(a, b);
            else
                addMark(a, b, StyleId::Comment);
            break;
        case InlineType::WikiLink:
        case InlineType::Embed: {
            const md::WikiLinkInfo& wl = it->wikiLink(id);
            if (n.type == InlineType::Embed && !act && resolveAttachment && isImageExt(wl.target)) {
                const QString path = resolveAttachment(wl.target);
                if (!path.isEmpty()) {
                    auto w = std::make_shared<ImageWidget>(path, wl.width, wl.height, m_contentWidth);
                    if (w->valid() && addWidget(a, b, w))
                        break;
                }
            }
            if (n.type == InlineType::Embed && !act) {
                if (auto w = fileEmbed(resolveAttachment, wl.target, wl.subpath, wl.width, wl.height, m_contentWidth); w && addWidget(a, b, w))
                    break;
            }
            const bool resolved = wl.target.isEmpty() || !isResolved || isResolved(wl.target);
            const StyleId linkStyle = resolved ? StyleId::InternalLink : StyleId::InternalLinkUnresolved;
            if (act) {
                forChildren(id, [&](int c) {
                    const md::InlineNode& ch = it->node(c);
                    const auto [ca, cb] = docRange(ch);
                    if (ch.mark == MarkKind::WikiTarget)
                        addMark(ca, cb, linkStyle);
                    else if (ch.type == InlineType::Mark)
                        addMark(ca, cb, StyleId::Formatting);
                });
                if (!wl.alias.isEmpty()) {
                    // alias text = after the pipe
                    qsizetype pipeEnd = -1, closeStart = b;
                    forChildren(id, [&](int c) {
                        const md::InlineNode& ch = it->node(c);
                        if (ch.mark == MarkKind::WikiPipe)
                            pipeEnd = docRange(ch).second;
                        if (ch.mark == MarkKind::WikiClose)
                            closeStart = docRange(ch).first;
                    });
                    if (pipeEnd >= 0)
                        addMark(pipeEnd, closeStart, linkStyle);
                }
            } else {
                qsizetype targetA = -1, targetB = -1, pipeEnd = -1, closeStart = b;
                forChildren(id, [&](int c) {
                    const md::InlineNode& ch = it->node(c);
                    const auto [ca, cb] = docRange(ch);
                    switch (ch.mark) {
                    case MarkKind::WikiOpen:
                        addHide(ca, cb);
                        break;
                    case MarkKind::WikiClose:
                        addHide(ca, cb);
                        closeStart = ca;
                        break;
                    case MarkKind::WikiPipe:
                        pipeEnd = cb;
                        if (!wl.alias.isEmpty())
                            addHide(ca, cb);
                        break;
                    case MarkKind::WikiTarget:
                        targetA = ca;
                        targetB = cb;
                        break;
                    default:
                        break;
                    }
                });
                if (!wl.alias.isEmpty() && pipeEnd >= 0) {
                    if (targetA >= 0)
                        addHide(targetA, targetB);
                    addMark(pipeEnd, closeStart, linkStyle);
                } else if (targetA >= 0) {
                    if (!wl.subpath.isEmpty())
                        addSubstitute(targetA, targetB, wl.displayText(), linkStyle);
                    else
                        addMark(targetA, targetB, linkStyle);
                }
            }
            break;
        }
        case InlineType::Link:
        case InlineType::Image: {
            const md::LinkInfo& li = it->link(id);
            if (n.type == InlineType::Image && !act && !hasScheme(li.destination)) {
                const QString dest = QUrl::fromPercentEncoding(li.destination.toUtf8());
                const qsizetype hash = dest.indexOf(QLatin1Char('#'));
                if (auto w = fileEmbed(resolveAttachment, dest.left(hash), hash < 0 ? QString() : dest.mid(hash), li.width, li.height, m_contentWidth);
                    w && addWidget(a, b, w))
                    break;
            }
            if (n.type == InlineType::Image && !act && !hasScheme(li.destination) && resolveAttachment) {
                const QString path = resolveAttachment(QUrl::fromPercentEncoding(li.destination.toUtf8()));
                if (!path.isEmpty()) {
                    auto w = std::make_shared<ImageWidget>(path, li.width, li.height, m_contentWidth);
                    if (w->valid() && addWidget(a, b, w))
                        break;
                }
            }
            const StyleId linkStyle = hasScheme(li.destination) ? StyleId::ExternalLink : StyleId::InternalLink;
            qsizetype textA = a, textB = b;
            forChildren(id, [&](int c) {
                const md::InlineNode& ch = it->node(c);
                const auto [ca, cb] = docRange(ch);
                if (ch.mark == MarkKind::LinkOpen) {
                    textA = cb;
                    if (!act)
                        addHide(ca, cb);
                    else
                        addMark(ca, cb, StyleId::Formatting);
                } else if (ch.mark == MarkKind::LinkClose) {
                    textB = ca;
                    if (!act) {
                        addHide(ca, cb);
                    } else {
                        addMark(ca, cb, StyleId::Formatting);
                        // the destination is a LinkUrl mark nested in LinkClose
                        forChildren(c, [&](int u) {
                            if (it->node(u).mark == MarkKind::LinkUrl) {
                                const auto [ua, ub] = docRange(it->node(u));
                                addMark(ua, ub, StyleId::Url);
                            }
                        });
                    }
                } else if (ch.mark == MarkKind::LinkUrl && act) {
                    addMark(ca, cb, StyleId::Url);
                }
            });
            addMark(textA, textB, linkStyle);
            break;
        }
        case InlineType::Autolink: {
            marksOf(id, !act);
            const auto [ca, cb] = contentRange(id);
            addMark(ca, cb, StyleId::Url);
            break;
        }
        case InlineType::BareUrl:
            addMark(a, b, StyleId::Url);
            break;
        case InlineType::Tag:
            addMark(a, b, StyleId::Tag);
            break;
        case InlineType::FootnoteRef:
        case InlineType::InlineFootnote: {
            if (m_reading) {
                // Reading view: "[1]" (or "[1-2]" for a repeat); a reference without a definition is its
                // lowercased label as plain text (app.js's remark transformer).
                QString label;
                StyleId st = StyleId::FootnoteLink;
                const std::shared_ptr<const footnotes::Index> idx = footnoteIndex();
                const auto ref = idx && !m_sharedFootnotes ? idx->refAt(a) : std::nullopt;
                if (ref) {
                    if (ref->note >= 0)
                        label = QLatin1Char('[') + footnotes::Index::refLabel(ref->note, ref->occurrence) + QLatin1Char(']');
                } else if (idx) {
                    // nested rendering: by label / inline text
                    const int note = n.type == InlineType::FootnoteRef ? idx->noteForId(it->footnote(id).label)
                                                                       : idx->noteForInlineText(text.slice(contentRange(id).first, contentRange(id).second));
                    if (note >= 0)
                        label = QLatin1Char('[') + footnotes::Index::refLabel(note, 0) + QLatin1Char(']');
                }
                if (label.isEmpty() && n.type == InlineType::FootnoteRef) {
                    label = it->footnote(id).label.toLower();
                    st = StyleId::Normal;
                }
                if (label.isEmpty()) {
                    addHide(a, b);
                } else if (b >= ls && a <= le) {
                    // one substitution on the line where it starts (an inline footnote may span lines)
                    if (a >= ls)
                        d.replaces.push_back({int(a - colBase), int(std::min(b, le) - colBase), Replace::Kind::Text, label, st, nullptr});
                    else
                        addHide(a, b);
                }
            } else {
                // .cm-footref: smaller and raised, "[^" / "^[" and "]" faint; a reference's label muted
                addMark(a, b, n.type == InlineType::FootnoteRef ? StyleId::FootnoteRef : StyleId::FootnoteInline);
                marksOf(id, false);
            }
            break;
        }
        case InlineType::BlockId:
            if (reading)
                addHide(a, b);
            else
                addMark(a, b, StyleId::BlockId);
            break;
        case InlineType::Escape:
            marksOf(id, !act);
            break;
        case InlineType::HardBreak:
            // "\" at the end of a line: Reading view renders the break without it (Live Preview keeps the source)
            if (reading && it->content().at(n.from) == u'\\')
                addHide(a, a + 1);
            break;
        default:
            break;
        }
    }
    if (!htmlTags.isEmpty()) {
        html::InlineContext hc;
        hc.tree = it;
        hc.content = &content;
        hc.text = &text;
        hc.lineFrom = ls;
        hc.lineTo = le;
        hc.colBase = colBase;
        hc.reading = m_reading;
        hc.forceInactive = forceInactive;
        hc.touches = [this](qsizetype a, qsizetype b) { return touches(a, b); };
        hc.styles = m_styles;
        hc.lineStyle = lineStyle;
        hc.contentWidth = m_contentWidth;
        hc.resolveAttachment = resolveAttachment;
        html::decorateInline(hc, htmlTags, d);
    }
}

void LivePreviewDecorator::codeHighlight(int line, int leaf, LineDecoration& d)
{
    const md::SyntaxTree& tree = m_doc->syntax();
    if (leaf < 0 || tree.node(leaf).type != NodeType::FencedCode)
        return;
    const md::FenceInfo fi = tree.fence(leaf);
    if (fi.language.isEmpty())
        return;
    const int openLine = tree.node(leaf).fromLine;
    if (m_fenceCache.size() > 128)
        m_fenceCache.clear();
    FenceCache& c = m_fenceCache[openLine];
    if (c.version != m_doc->version()) {
        c = FenceCache{};
        c.version = m_doc->version();
        c.def = CodeHighlighter::instance().definitionFor(fi.language);
    }
    if (!c.def.isValid())
        return;
    const int idx = line - openLine - 1;
    if (idx < 0)
        return;
    const Text& text = m_doc->text();
    while (int(c.states.size()) <= idx) {
        const int l = openLine + 1 + int(c.states.size());
        const int col = std::min(tree.contentColumn(l), text.lineLength(l));
        const KSyntaxHighlighting::State prev = c.states.empty() ? KSyntaxHighlighting::State() : c.states.back();
        QList<Mark> marks;
        c.states.push_back(CodeHighlighter::instance().highlightLine(c.def, text.lineView(l).mid(col), prev, marks, col));
        c.marks.push_back(std::move(marks));
    }
    d.marks += c.marks[size_t(idx)];
}

void LivePreviewDecorator::blockRanges(int fromLine, int toLine, QList<BlockRange>& out)
{
    if (!m_doc || !m_styles)
        return;
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    const StyleSheetPtr styles(std::shared_ptr<const StyleSheet>(), m_styles); // non-owning alias
    if (m_blockCache.size() > 256)
        m_blockCache.clear();
    // Top-level blocks only (children of the document); nested ones render inside their parents.
    for (int id = tree.firstChild(0); id >= 0; id = tree.nextSibling(id)) {
        const md::BlockNode& n = tree.node(id);
        if (n.toLine <= fromLine)
            continue;
        if (n.fromLine >= toLine)
            break;
        if (n.fromLine < fromLine)
            continue; // starts before the window: owned by an earlier query
        if (n.type == NodeType::HtmlBlock) {
            if (!touches(text.lineStart(n.fromLine), text.lineEnd(n.toLine - 1)))
                if (auto r = html::blockRangeFor(tree, text, id, *m_styles, m_reading, m_depth, resolveAttachment, m_blockCache))
                    out.push_back(*r);
            continue;
        }
        const bool isTable = n.type == NodeType::Table;
        const bool isCallout = n.type == NodeType::Callout;
        const bool isMath = n.type == NodeType::MathBlock;
        if (!isTable && !isCallout && !isMath)
            continue;
        const qsizetype a = text.lineStart(n.fromLine), b = text.lineEnd(n.toLine - 1);
        if (touches(a, b))
            continue; // being edited: show source
        const QString src = text.slice(a, b);
        const quint64 key = qHashMulti(uint(n.type), src, m_depth, m_reading, m_styles->generation(), a, m_indentGuides, m_strictLineBreaks);
        std::shared_ptr<BlockWidget> w = m_blockCache.value(key);
        if (!w) {
            if (isTable) {
                w = std::make_shared<TableWidget>(m_doc, id, this, styles);
            } else if (isCallout) {
                w = std::make_shared<CalloutWidget>(m_doc, id, this, styles, m_depth);
            } else {
                // $$ ... $$: strip the delimiters
                QString tex = src.trimmed();
                if (tex.startsWith(QLatin1String("$$")))
                    tex.remove(0, 2);
                if (tex.endsWith(QLatin1String("$$")))
                    tex.chop(2);
                w = std::make_shared<MathBlockWidget>(tex.trimmed(), a, styles);
            }
            m_blockCache.insert(key, w);
        }
        out.push_back(BlockRange{n.fromLine, n.toLine, w});
    }
    if (m_reading && m_strictLineBreaks) {
        const qsizetype blocks = out.size();
        paragraphRuns(fromLine, toLine, out);
        if (out.size() > blocks) // merge with the block widgets (they never overlap: runs skip rendered blocks)
            std::stable_sort(out.begin(), out.end(), [](const BlockRange& a, const BlockRange& b) { return a.fromLine < b.fromLine; });
    }
}

std::optional<LivePreviewDecorator::LinkHit> LivePreviewDecorator::linkAt(qsizetype pos) const
{
    if (!m_doc)
        return std::nullopt;
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    const int line = text.lineAt(pos);
    const int leaf = tree.leafAt(line);
    if (leaf < 0)
        return std::nullopt;
    const md::LeafContent content = tree.node(leaf).type == NodeType::HtmlBlock ? html::blockContent(tree, leaf, text) : tree.leafContent(leaf, text);
    const md::InlineTreePtr it = tree.inlineTree(content);
    if (!it)
        return std::nullopt;
    const int off = content.fromDoc(pos, text);
    if (off < 0)
        return std::nullopt;
    for (int id = 0; id < it->size(); ++id) {
        const md::InlineNode& n = it->node(id);
        if (off < n.from || off > n.to)
            continue;
        LinkHit h;
        h.from = content.toDoc(n.from, text);
        h.to = content.toDoc(n.to, text);
        switch (n.type) {
        case InlineType::WikiLink:
        case InlineType::Embed: {
            const md::WikiLinkInfo& wl = it->wikiLink(id);
            h.kind = n.type == InlineType::Embed ? LinkHit::Kind::Embed : LinkHit::Kind::WikiLink;
            h.target = wl.target + wl.subpath;
            return h;
        }
        case InlineType::Link:
            h.kind = LinkHit::Kind::Link;
            h.target = it->link(id).destination;
            return h;
        case InlineType::Autolink:
        case InlineType::BareUrl:
            h.kind = LinkHit::Kind::Url;
            h.target = it->content().mid(n.from, n.to - n.from);
            if (h.target.startsWith(u'<'))
                h.target = h.target.mid(1, h.target.size() - 2);
            return h;
        case InlineType::Tag:
            h.kind = LinkHit::Kind::Tag;
            h.target = QLatin1Char('#') + it->tag(id).tag;
            return h;
        case InlineType::FootnoteRef:
        case InlineType::InlineFootnote: {
            // Hover previews "#[^id]" (named ones); a click scrolls to the footnote in Reading view.
            const bool named = n.type == InlineType::FootnoteRef;
            h.kind = LinkHit::Kind::WikiLink;
            h.target = named ? QStringLiteral("#[^") + it->footnote(id).label.toLower() + QLatin1Char(']') : QString();
            h.anchor = QStringLiteral("fn");
            if (m_reading) {
                const std::shared_ptr<const footnotes::Index> idx = footnoteIndex();
                int note = -1;
                if (idx && !m_sharedFootnotes) {
                    const auto ref = idx->refAt(h.from);
                    note = ref ? ref->note : -1;
                } else if (idx) { // nested rendering: by label / inline text
                    int open = -1, close = -1;
                    for (int c = n.firstChild; c >= 0; c = it->node(c).next)
                        if (it->node(c).type == InlineType::Mark) {
                            if (open < 0)
                                open = c;
                            close = c;
                        }
                    note = named ? idx->noteForId(it->footnote(id).label)
                                 : open >= 0 && close > open ? idx->noteForInlineText(it->content().mid(it->node(open).to, it->node(close).from - it->node(open).to))
                                                             : -1;
                }
                if (note < 0)
                    break; // plain text in Reading view
                h.anchor = QStringLiteral("fn-") + QString::number(note + 1);
            } else if (!named) {
                break; // Obsidian's editor doesn't make inline footnotes clickable
            }
            return h;
        }
        default:
            break;
        }
    }
    if (const auto a = html::anchorAt(it, content, text, off))
        return LinkHit{a->external ? LinkHit::Kind::Url : LinkHit::Kind::Link, a->href, a->from, a->to};
    return std::nullopt;
}

std::shared_ptr<const footnotes::Index> LivePreviewDecorator::footnoteIndex() const
{
    if (m_sharedFootnotes)
        return m_sharedFootnotes;
    if (!m_doc)
        return nullptr;
    const md::SyntaxTree& tree = m_doc->syntax();
    const quint64 key = qHashMulti(quintptr(m_doc), m_doc->version(), tree.validUpTo(), tree.lineCount());
    if (!m_footnotes || key != m_footnotesKey) {
        m_footnotes = footnotes::Index::build(tree, m_doc->text());
        m_footnotesKey = key;
        ++m_footnotesSerial;
    }
    return m_footnotes;
}

std::optional<qsizetype> LivePreviewDecorator::footnoteRefPosition(const QString& anchor) const
{
    const std::shared_ptr<const footnotes::Index> idx = m_reading ? footnoteIndex() : nullptr;
    return idx ? idx->refPosition(anchor) : std::nullopt;
}

bool LivePreviewDecorator::previousVisibleLineBlank(int line) const
{
    const md::SyntaxTree& tree = m_doc->syntax();
    // hidden footnote definitions take no room: look past them (a bounded walk)
    int prev = line - 1;
    for (int steps = 0; prev >= 0 && steps < 2000 && hidesFootnoteDefinitions() && tree.role(prev) != LineRole::Blank
                        && footnotes::inDefinition(tree, prev);
         ++steps)
        --prev;
    return prev >= 0 && tree.role(prev) == LineRole::Blank;
}

std::shared_ptr<BlockWidget> LivePreviewDecorator::trailer()
{
    if (!m_reading || m_depth > 0 || m_footnoteMode != FootnoteMode::Document || !m_doc || !m_styles || m_sharedFootnotes) {
        m_footnoteSection.reset();
        return nullptr;
    }
    const std::shared_ptr<const footnotes::Index> idx = footnoteIndex();
    if (!idx || idx->isEmpty()) {
        m_footnoteSection.reset();
        return nullptr;
    }
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    // The <hr>'s 2rem top margin collapses with the last block's --p-spacing: when the note ends with a blank
    // line (shown as a paragraph gap) that gap already counts.
    int last = text.lineCount() - 1;
    while (last >= 0 && footnotes::inDefinition(tree, last))
        --last;
    const qreal gap = last >= 0 && tree.role(last) == LineRole::Blank ? m_styles->pSpacing() : 0;
    const qreal topGap = std::max<qreal>(0, 32 - gap);
    const quint64 key = qHashMulti(m_footnotesSerial, m_styles->generation(), topGap, m_indentGuides, m_strictLineBreaks);
    if (!m_footnoteSection || key != m_footnoteSectionKey) {
        const StyleSheetPtr styles(std::shared_ptr<const StyleSheet>(), m_styles); // non-owning alias
        m_footnoteSection = std::make_shared<footnotes::SectionWidget>(idx, tree, text, styles, this, topGap);
        m_footnoteSectionKey = key;
    }
    return m_footnoteSection;
}

} // namespace oc
