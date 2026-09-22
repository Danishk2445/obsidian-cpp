// TEMPORARY STUB (lead engineer's tree) — replaced by the real inline parser (inline_parser.cpp) at
// merge time. Provides parseInline() returning a single Text node, plus the LeafContent / WikiLinkInfo
// helpers declared in inline_tree.h.
#include "markdown/inline_tree.h"

namespace oc::md {

InlineTreePtr parseInline(const QString& content, const QList<LinkRefDefinition>&)
{
    auto t = std::make_shared<InlineTree>();
    t->mutableContent() = content;
    return t;
}

qsizetype LeafContent::toDoc(int off, const Text& doc) const
{
    for (const Segment& s : segments)
        if (off >= s.contentFrom && off <= s.contentFrom + s.length)
            return doc.lineStart(s.line) + s.col + (off - s.contentFrom);
    return segments.isEmpty() ? 0 : doc.lineStart(segments.last().line) + segments.last().col + segments.last().length;
}

int LeafContent::fromDoc(qsizetype pos, const Text& doc) const
{
    for (const Segment& s : segments) {
        const qsizetype start = doc.lineStart(s.line) + s.col;
        if (pos >= start && pos <= start + s.length)
            return s.contentFrom + int(pos - start);
    }
    return -1;
}

int LeafContent::lineOf(int off) const
{
    for (const Segment& s : segments)
        if (off <= s.contentFrom + s.length)
            return s.line;
    return segments.isEmpty() ? 0 : segments.last().line;
}

QString WikiLinkInfo::displayText() const
{
    if (!alias.isEmpty())
        return alias;
    if (subpath.isEmpty())
        return target;
    QString sub = subpath;
    sub.remove(0, 1);
    return target.isEmpty() ? sub : target + QStringLiteral(" > ") + sub.replace(QLatin1Char('#'), QStringLiteral(" > "));
}

} // namespace oc::md
