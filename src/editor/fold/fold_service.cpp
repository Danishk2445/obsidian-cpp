#include "editor/fold/fold_service.h"

namespace oc {

namespace {
constexpr int kNoHeading = 100;

bool isFrontmatterDash(QStringView s) { return s == u"---"; }

std::optional<FoldRange> headingFold(const Text& text, const md::SyntaxTree& tree, int line)
{
    const int n = text.lineCount();
    if (line >= n - 1)
        return std::nullopt;
    const int level = foldHeadingLevel(text, tree, line);
    if (level == kNoHeading)
        return std::nullopt;
    int last = line;
    for (int l = line + 1; l < n; ++l) {
        if (foldHeadingLevel(text, tree, l) <= level)
            break;
        last = l;
    }
    if (last == line)
        return std::nullopt;
    return FoldRange{line, last};
}

std::optional<FoldRange> indentFold(const Text& text, int line, int tabSize)
{
    const int base = indentColumns(text.lineView(line), tabSize);
    if (base < 0)
        return std::nullopt;
    int last = -1;
    for (int l = line + 1; l < text.lineCount(); ++l) {
        const int ind = indentColumns(text.lineView(l), tabSize);
        if (ind < 0)
            continue; // blank lines don't end the region
        if (ind <= base)
            break;
        last = l;
    }
    if (last < 0)
        return std::nullopt;
    return FoldRange{line, last};
}

std::optional<FoldRange> frontmatterFold(const Text& text, int line)
{
    if (!isFrontmatterDash(text.lineView(line)))
        return std::nullopt;
    for (int l = line - 1; l >= 0; --l)
        if (!text.lineView(l).trimmed().isEmpty())
            return std::nullopt;
    for (int l = line + 1; l < text.lineCount(); ++l)
        if (isFrontmatterDash(text.lineView(l)))
            return FoldRange{line, l};
    return std::nullopt;
}
} // namespace

int indentColumns(QStringView s, int tabSize)
{
    int col = 0;
    for (QChar c : s) {
        if (c == u' ')
            ++col;
        else if (c == u'\t')
            col += tabSize - col % tabSize;
        else
            return col;
    }
    return -1; // blank
}

int foldHeadingLevel(const Text& text, const md::SyntaxTree& tree, int line)
{
    if (line < 0 || line >= text.lineCount() || line >= tree.lineCount())
        return kNoHeading;
    const md::LineRole role = tree.role(line);
    if (role == md::LineRole::AtxHeading) {
        // Like Obsidian, only lines that start with the '#'s (not headings inside quotes/lists).
        const QStringView v = text.lineView(line);
        qsizetype i = 0;
        while (i < v.size() && v[i].isSpace())
            ++i;
        if (i >= v.size() || v[i] != u'#')
            return kNoHeading;
        const int leaf = tree.leafAt(line);
        return leaf >= 0 ? tree.heading(leaf).level : kNoHeading;
    }
    if (role == md::LineRole::SetextText) {
        // Only the text line right above the underline starts the fold (like Obsidian's check).
        if (line + 1 < tree.lineCount() && tree.role(line + 1) == md::LineRole::SetextUnderline) {
            const int leaf = tree.leafAt(line);
            return leaf >= 0 ? tree.heading(leaf).level : kNoHeading;
        }
    }
    return kNoHeading;
}

std::optional<FoldRange> foldableAt(const Text& text, const md::SyntaxTree& tree, int line, const FoldOptions& o)
{
    if (line < 0 || line >= text.lineCount())
        return std::nullopt;
    if (o.headings)
        if (auto r = headingFold(text, tree, line))
            return r;
    if (o.indent)
        if (auto r = indentFold(text, line, o.tabSize))
            return r;
    return frontmatterFold(text, line);
}

bool isFoldable(const Text& text, const md::SyntaxTree& tree, int line, const FoldOptions& o)
{
    const int n = text.lineCount();
    if (line < 0 || line >= n)
        return false;
    if (o.headings) {
        const int level = foldHeadingLevel(text, tree, line);
        if (level != 100)
            return line < n - 1 && foldHeadingLevel(text, tree, line + 1) > level;
    }
    if (o.indent) {
        const int base = indentColumns(text.lineView(line), o.tabSize);
        if (base >= 0) {
            // The next non-blank line decides (bounded scan over blank lines).
            for (int l = line + 1, budget = 0; l < n && budget < 1000; ++l, ++budget) {
                const int ind = indentColumns(text.lineView(l), o.tabSize);
                if (ind < 0)
                    continue;
                if (ind > base)
                    return true;
                break;
            }
        }
    }
    if (isFrontmatterDash(text.lineView(line)) && line == 0)
        return frontmatterFold(text, line).has_value();
    return false;
}

} // namespace oc
