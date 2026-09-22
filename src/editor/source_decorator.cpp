#include "editor/source_decorator.h"

#include "markdown/doc_source.h"
#include "render/services/code_highlighter.h"

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
    for (const md::ContainerMarker& m : tree.containerMarkers(line)) {
        switch (m.kind) {
        case md::ContainerMarker::Kind::Quote:
            d.marks.push_back({m.from, m.to, StyleId::Formatting});
            break;
        case md::ContainerMarker::Kind::ListMarker: {
            inList = true;
            // colour only the marker glyphs, not the following spaces
            int end = m.to;
            QStringView v = text.lineView(line);
            while (end > m.from && end - 1 < v.size() && v[end - 1].isSpace())
                --end;
            d.marks.push_back({m.from, end, StyleId::ListMarker});
            break;
        }
        case md::ContainerMarker::Kind::ListIndent:
            inList = true;
            break;
        case md::ContainerMarker::Kind::FootnoteLabel:
            d.marks.push_back({m.from, m.to, StyleId::FootnoteRef});
            break;
        case md::ContainerMarker::Kind::FootnoteIndent:
            break;
        }
    }
    if (inList && m_styles) {
        d.tabStop = m_styles->listIndent();
        d.hangingColumn = tree.contentColumn(line);
        d.paddingTop = m_styles->listSpacing();
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
        d.hangingColumn = -1;
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
