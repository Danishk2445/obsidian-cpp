#include "editor/live_preview_decorator.h"

#include "markdown/doc_source.h"
#include "render/services/code_highlighter.h"
#include "render/widgets/inline_widgets.h"
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

    // ---- container markers -----------------------------------------------------------------------
    bool inList = false;
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
        case md::ContainerMarker::Kind::FootnoteLabel:
            d.marks.push_back({m.from, m.to, StyleId::FootnoteRef});
            break;
        case md::ContainerMarker::Kind::FootnoteIndent:
            break;
        }
    }

    if (inList) {
        d.tabStop = m_styles->listIndent();
        d.hangingColumn = tree.contentColumn(line);
        d.paddingTop = m_styles->listSpacing();
        d.paddingBottom = m_styles->listSpacing();
        // indentation guides at each leading tab stop
        int col = 0;
        while (col < len && (lv[col] == u'\t' || lv[col] == u' ')) {
            if (lv[col] == u'\t')
                d.guideColumns.push_back(col);
            ++col;
        }
        d.guideOffset = m_styles->textSize() * 0.85 - m_styles->listIndent() * 0; // --indentation-guide-editing-indent
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
            if (line > 0 && tree.role(line - 1) == LineRole::Blank)
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
    if (extra)
        extra(line, d);
    return d;
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
    for (int id = 0; id < it->size(); ++id) {
        const md::InlineNode& n = it->node(id);
        if (n.type == InlineType::Mark || n.type == InlineType::Text)
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
                    if (!act)
                        addHide(ca, cb);
                    else
                        addMark(ca, cb, StyleId::Formatting);
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
        case InlineType::InlineFootnote:
            addMark(a, b, StyleId::FootnoteRef);
            break;
        case InlineType::BlockId:
            if (reading)
                addHide(a, b);
            else
                addMark(a, b, StyleId::BlockId);
            break;
        case InlineType::HtmlTag:
            addMark(a, b, StyleId::Html);
            break;
        case InlineType::Escape:
            marksOf(id, !act);
            break;
        default:
            break;
        }
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
        const bool isTable = n.type == NodeType::Table;
        const bool isCallout = n.type == NodeType::Callout;
        const bool isMath = n.type == NodeType::MathBlock;
        if (!isTable && !isCallout && !isMath)
            continue;
        const qsizetype a = text.lineStart(n.fromLine), b = text.lineEnd(n.toLine - 1);
        if (touches(a, b))
            continue; // being edited: show source
        const QString src = text.slice(a, b);
        const quint64 key = qHashMulti(uint(n.type), src, m_depth, m_reading, m_styles->generation(), a);
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
    const md::LeafContent content = tree.leafContent(leaf, text);
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
        default:
            break;
        }
    }
    return std::nullopt;
}

} // namespace oc
