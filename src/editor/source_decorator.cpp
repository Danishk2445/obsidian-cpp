#include "editor/source_decorator.h"

#include "editor/footnotes/footnotes.h"
#include "markdown/doc_source.h"
#include "render/services/code_highlighter.h"
#include "render/widgets/inline_widgets.h"

#include <cmath>

namespace oc {

namespace {
StyleId headingStyle(int level) { return StyleId(int(StyleId::Heading1) + std::clamp(level, 1, 6) - 1); }

bool isCodeRole(md::LineRole r)
{
    using R = md::LineRole;
    return r == R::FenceOpen || r == R::FenceBody || r == R::FenceClose || r == R::IndentedCode;
}
} // namespace

StyleId SourceDecorator::quickLineStyle(int line)
{
    if (!m_doc)
        return StyleId::Normal;
    const md::SyntaxTree& tree = m_doc->syntax();
    using R = md::LineRole;
    switch (tree.role(line)) {
    case R::AtxHeading:
    case R::SetextText: {
        const int leaf = tree.leafAt(line);
        return leaf >= 0 ? headingStyle(tree.heading(leaf).level) : StyleId::Normal;
    }
    case R::FenceOpen:
    case R::FenceBody:
    case R::FenceClose:
    case R::IndentedCode:
        return StyleId::CodeBlock;
    case R::MathOpen:
    case R::MathBody:
    case R::MathClose:
    case R::MathOneLine:
        return StyleId::MathBlock;
    case R::FrontmatterOpen:
    case R::FrontmatterBody:
    case R::FrontmatterClose:
        return StyleId::Frontmatter;
    case R::CommentOpen:
    case R::CommentBody:
    case R::CommentClose:
        return StyleId::CommentBlock;
    case R::ThematicBreak:
        return StyleId::HrLine;
    case R::Html:
        return StyleId::HtmlBlock;
    case R::Paragraph:
        return footnotes::isDefinitionLine(tree, m_doc->text(), line) ? StyleId::FootnoteLine : StyleId::Normal;
    default:
        return StyleId::Normal;
    }
}

qreal SourceDecorator::quickVerticalPadding(int line)
{
    if (!m_doc || !m_styles)
        return 0;
    const md::LineRole r = m_doc->syntax().role(line);
    if (r == md::LineRole::AtxHeading || r == md::LineRole::SetextText)
        return m_styles->pSpacing();
    return 0;
}

LineDecoration SourceDecorator::decorate(int line)
{
    LineDecoration d;
    if (!m_doc)
        return d;
    const md::SyntaxTree& tree = m_doc->syntax();
    const Text& text = m_doc->text();
    const int len = text.lineLength(line);
    using R = md::LineRole;
    const R role = tree.role(line);
    d.lineStyle = quickLineStyle(line);

    // Container markers: quote '>' faint, list bullets in the list marker colour.
    bool inList = false;
    bool markerLine = false;
    for (const md::ContainerMarker& m : tree.containerMarkers(line)) {
        switch (m.kind) {
        case md::ContainerMarker::Kind::Quote:
            d.marks.push_back({m.from, m.to, StyleId::Formatting});
            break;
        case md::ContainerMarker::Kind::ListMarker: {
            inList = markerLine = true;
            // .cm-formatting-list-ul / -ol: the marker and the whitespace after it (tabular-nums: Inter's hyphen
            // and the spaces are tabular too)
            int end = m.from;
            const QStringView v = text.lineView(line);
            while (end < v.size() && !v[end].isSpace())
                ++end;
            while (end < v.size() && (v[end] == u' ' || v[end] == u'\t'))
                ++end;
            d.marks.push_back({m.from, end, StyleId::ListMarker});
            const md::BlockNode& item = tree.node(m.node);
            if (m_styles && item.type == md::NodeType::ListItem && item.fromLine == line && (item.flags & md::FlagTask)) {
                const md::ListItemInfo li = tree.listItem(m.node);
                if (li.taskFrom >= 0 && li.taskFrom + 3 <= len) {
                    // .cm-s-obsidian span.cm-formatting-task { font-family: var(--font-monospace); color: var(--text-muted) }
                    StyleSpec box;
                    box.format.setFontFamilies(m_styles->spec(StyleId::Monospace).format.fontFamilies().toStringList());
                    box.format.setProperty(QTextFormat::FontPixelSize, int(std::lround(m_styles->line(d.lineStyle).font.pixelSize())));
                    box.format.setForeground(m_styles->muted());
                    d.marks.push_back({li.taskFrom, li.taskFrom + 3, StyleId::Normal, d.addCustomStyle(box)});
                    // .markdown-source-view.mod-cm6 .HyperMD-task-line[data-task="x"], [data-task="X"]
                    if (li.taskChar == u'x' || li.taskChar == u'X')
                        d.marks.push_back({li.taskFrom + 3, len, StyleId::TaskDone});
                }
            }
            break;
        }
        case md::ContainerMarker::Kind::ListIndent:
            inList = true;
            break;
        case md::ContainerMarker::Kind::FootnoteLabel: {
            // "[^label]:" (.cm-hmd-footnote), its "[^" and "]:" faint; the line is a .HyperMD-footnote line
            const int close = int(text.lineView(line).indexOf(u']', m.from + 2));
            if (close > m.from + 2 && close + 2 <= m.to) {
                d.marks.push_back({m.from, close + 2, StyleId::FootnoteMarker});
                d.marks.push_back({m.from, m.from + 2, StyleId::Formatting});
                d.marks.push_back({close, close + 2, StyleId::Formatting});
            }
            if (d.lineStyle == StyleId::Normal)
                d.lineStyle = StyleId::FootnoteLine;
            break;
        }
        case md::ContainerMarker::Kind::FootnoteIndent:
            if (d.lineStyle == StyleId::Normal)
                d.lineStyle = StyleId::FootnoteLine;
            break;
        }
    }
    if (inList && m_styles) {
        // .cm-line.HyperMD-list-line { tab-size: var(--list-indent) }, --list-spacing above (not on a continuation
        // line, .HyperMD-list-line-nobullet) and below; the marker has no padding (--list-indent-source: 0)
        d.tabStop = m_styles->listIndent();
        d.paddingTop = markerLine ? m_styles->listSpacing() : 0;
        d.paddingBottom = m_styles->listSpacing();
    }

    switch (role) {
    case R::AtxHeading: {
        const int leaf = tree.leafAt(line);
        if (leaf >= 0) {
            const md::HeadingInfo h = tree.heading(leaf);
            d.marks.push_back({h.markerFrom, h.markerTo, StyleId::Formatting});
            if (h.closeFrom >= 0)
                d.marks.push_back({h.closeFrom, len, StyleId::Formatting});
        }
        if (m_styles)
            d.paddingTop = m_styles->pSpacing();
        break;
    }
    case R::SetextText:
        if (m_styles)
            d.paddingTop = m_styles->pSpacing();
        break;
    case R::SetextUnderline:
        d.marks.push_back({0, len, StyleId::Formatting});
        break;
    case R::FenceOpen:
    case R::FenceClose: {
        d.marks.push_back({tree.contentColumn(line), len, StyleId::Formatting});
        break;
    }
    case R::FrontmatterOpen:
    case R::FrontmatterClose:
        d.marks.push_back({0, len, StyleId::Formatting});
        break;
    case R::MathOpen:
    case R::MathClose:
    case R::MathBody:
    case R::MathOneLine:
        d.marks.push_back({tree.contentColumn(line), len, StyleId::Math});
        break;
    case R::ThematicBreak:
        d.marks.push_back({0, len, StyleId::Formatting});
        break;
    default:
        break;
    }

    if (role == R::FenceBody)
        highlightCode(line, d);

    if (isCodeRole(role)) {
        const bool prevCode = line > 0 && isCodeRole(tree.role(line - 1)) && tree.leafAt(line - 1) == tree.leafAt(line);
        const bool nextCode = line + 1 < tree.lineCount() && isCodeRole(tree.role(line + 1)) && tree.leafAt(line + 1) == tree.leafAt(line);
        d.code = prevCode ? (nextCode ? LineDecoration::CodeEdge::Middle : LineDecoration::CodeEdge::Bottom)
                          : (nextCode ? LineDecoration::CodeEdge::Top : LineDecoration::CodeEdge::Single);
    } else if (const int h = indent::hangingPrefix(text.lineView(line)); h > 0) {
        d.hangingColumn = h; // wrapped lines hang under the quote / list prefix
    }
    if (m_guides && m_styles) {
        indent::decorateLine(text.lineView(line), line, m_activeIndent, 0.25, *m_styles, d); // --indentation-guide-source-indent
        // .HyperMD-list-line-nobullet > .cm-hmd-list-indent > .cm-indent-spacing:last-child { padding-inline-start:
        // calc(var(--list-indent-editing) + var(--list-marker-space)) } with --list-marker-space: 0 in Source mode
        if (inList && !markerLine && !isCodeRole(role)) {
            if (const int col = indent::lastSpacingUnit(text.lineView(line)); col >= 0) {
                const qreal em = m_styles->line(d.lineStyle).font.pixelSize();
                const qreal pad = m_styles->listIndentEditing() / m_styles->textSize() * em;
                d.replaces.push_back({col, col, Replace::Kind::Widget, {}, StyleId::Normal, std::make_shared<SpacerWidget>(pad)});
            }
        }
    }
    if (extra)
        extra(line, d);
    return d;
}

void SourceDecorator::highlightCode(int line, LineDecoration& d)
{
    const md::SyntaxTree& tree = m_doc->syntax();
    const int leaf = tree.leafAt(line);
    if (leaf < 0 || tree.node(leaf).type != md::NodeType::FencedCode)
        return;
    const md::FenceInfo fi = tree.fence(leaf);
    if (fi.language.isEmpty())
        return; // most fences in real notes have no language: skip highlighting entirely
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

} // namespace oc
