#include "core/metadata/outline.h"

#include "markdown/metadata.h"

#include <QRegularExpression>

#include <algorithm>

namespace oc {

QList<OutlineNode> buildOutline(const QList<md::HeadingCache>& headings)
{
    QList<OutlineNode> roots;
    // Stack of (level, path of indices into the tree) for the open headings.
    struct Open {
        int level;
        QList<int> path;
    };
    QList<Open> stack;
    auto nodeAt = [&](const QList<int>& path) -> OutlineNode& {
        OutlineNode* n = &roots[path.first()];
        for (qsizetype i = 1; i < path.size(); ++i)
            n = &n->children[path[i]];
        return *n;
    };
    for (int i = 0; i < headings.size(); ++i) {
        const md::HeadingCache& h = headings[i];
        OutlineNode n;
        n.index = i;
        n.level = h.level;
        n.line = h.position.start.line;
        n.heading = h.heading;
        n.text = headingDisplayText(h.heading);
        while (!stack.isEmpty() && stack.last().level >= h.level)
            stack.removeLast();
        QList<int> path;
        if (stack.isEmpty()) {
            roots.push_back(n);
            path = {int(roots.size()) - 1};
        } else {
            OutlineNode& parent = nodeAt(stack.last().path);
            parent.children.push_back(n);
            path = stack.last().path;
            path.push_back(int(parent.children.size()) - 1);
        }
        stack.push_back({h.level, path});
    }
    return roots;
}

int headingIndexAtLine(const QList<md::HeadingCache>& headings, int line)
{
    int found = -1;
    for (int i = 0; i < headings.size(); ++i) {
        if (headings[i].position.start.line > line)
            break;
        found = i;
    }
    return found;
}

QString headingDisplayText(const QString& heading)
{
    QString s = heading;
    static const QRegularExpression comment(QStringLiteral("%%.*?%%"));
    static const QRegularExpression html(QStringLiteral("<[^>\\n]+>"));
    static const QRegularExpression wiki(QStringLiteral("!?\\[\\[([^\\]|#]*)(#[^\\]|]*)?(?:\\|([^\\]]*))?\\]\\]"));
    static const QRegularExpression mdLink(QStringLiteral("!?\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression code(QStringLiteral("`([^`]*)`"));
    static const QRegularExpression strong(QStringLiteral("(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1"));
    static const QRegularExpression em(QStringLiteral("(?<![\\w*])([*_])(?=\\S)(.+?)(?<=\\S)\\1(?![\\w*])"));
    static const QRegularExpression strike(QStringLiteral("~~(?=\\S)(.+?)(?<=\\S)~~"));
    static const QRegularExpression mark(QStringLiteral("==(?=\\S)(.+?)(?<=\\S)=="));
    static const QRegularExpression escape(QStringLiteral("\\\\([!-/:-@\\[-`{-~])"));
    s.remove(comment);
    s.remove(html);
    // Links show their display text: the alias, else the target ("Note > Heading" for a subpath).
    QString out;
    qsizetype last = 0;
    auto it = wiki.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        QString text = m.captured(3);
        if (text.isEmpty()) {
            text = m.captured(1).trimmed();
            const QString sub = m.captured(2).mid(1).split(u'#', Qt::SkipEmptyParts).join(QStringLiteral(" > "));
            if (!sub.isEmpty())
                text = text.isEmpty() ? sub : text + QStringLiteral(" > ") + sub;
        }
        out += text;
        last = m.capturedEnd();
    }
    out += s.mid(last);
    s = out;
    s.replace(mdLink, QStringLiteral("\\1"));
    s.replace(code, QStringLiteral("\\1"));
    for (int pass = 0; pass < 2; ++pass) { // nested emphasis ("***x***")
        s.replace(strong, QStringLiteral("\\2"));
        s.replace(em, QStringLiteral("\\2"));
    }
    s.replace(strike, QStringLiteral("\\1"));
    s.replace(mark, QStringLiteral("\\1"));
    s.replace(escape, QStringLiteral("\\1"));
    s = s.trimmed();
    return s.isEmpty() ? heading : s;
}

namespace {
// Obsidian's fee(): text that ends with a blank line ("\n\n").
QString withBlankLine(const QString& s)
{
    const bool nl = s.endsWith(u'\n');
    const bool nl2 = s.size() >= 2 && s.at(s.size() - 2) == u'\n';
    return nl ? (nl2 ? s : s + QLatin1Char('\n')) : s + QStringLiteral("\n\n");
}
} // namespace

std::optional<TextEdit> moveSectionEdit(const QString& content, const QList<md::HeadingCache>& headings, int from, int target)
{
    const int n = int(headings.size());
    if (from < 0 || from >= n || target < 0 || target > n || from == target)
        return std::nullopt;
    const md::HeadingCache& dragged = headings[from];
    int l = from + 1;
    while (l < n && headings[l].level > dragged.level)
        ++l;
    const int size = int(content.size());
    const int c = int(std::clamp<qsizetype>(dragged.position.start.offset, 0, size));
    int u = l < n ? int(std::clamp<qsizetype>(headings[l].position.start.offset, 0, size)) : -1;
    int h = target < n ? int(std::clamp<qsizetype>(headings[target].position.start.offset, 0, size)) : -1;
    const int p = std::max({c, u, h}) + 1;
    const int f = u == -1 ? p : u;
    const int d = h == -1 ? p : h;
    if (d >= c && d <= f)
        return std::nullopt; // onto itself or into its own section
    if (u == -1)
        u = size;
    if (h == -1)
        h = size;
    const QString moved = withBlankLine(content.mid(c, u - c));
    if (h < c) {
        // (Obsidian would also put a blank line above a section moved to the very start; not done.)
        const QString before = content.left(h);
        const QString extra = before.isEmpty() ? QString() : withBlankLine(before).mid(before.size());
        return TextEdit{h, u, extra + moved + content.mid(h, c - h)};
    }
    return TextEdit{c, h, withBlankLine(content.mid(u, h - u)) + moved};
}

} // namespace oc
