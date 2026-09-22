#include "core/metadata/subpath.h"

#include "core/metadata/link_suggest.h"
#include "markdown/metadata.h"

#include <QSet>

namespace oc {

std::optional<SubpathRange> resolveSubpath(const md::CachedMetadata& meta, const QString& subpath)
{
    QStringList parts;
    for (const QString& p : subpath.split(u'#'))
        if (!p.isEmpty())
            parts << p;
    if (parts.isEmpty())
        return std::nullopt;
    if (parts.size() == 1) {
        const QString& p = parts.first();
        if (p.startsWith(u'^')) {
            const QString id = p.mid(1).toLower();
            const auto it = meta.blocks.constFind(id);
            if (it == meta.blocks.cend())
                return std::nullopt;
            SubpathRange r{SubpathRange::Type::Block, it->position.start.line, it->position.end.line, 0};
            // a list item brings its nested items along
            for (qsizetype i = 0; i < meta.listItems.size(); ++i) {
                const md::ListItemCache& li = meta.listItems[i];
                if (li.id.toLower() != id)
                    continue;
                QSet<int> family{li.position.start.line};
                int end = li.position.end.line;
                for (qsizetype j = i + 1; j < meta.listItems.size(); ++j) {
                    const md::ListItemCache& c = meta.listItems[j];
                    if (!family.contains(c.parent))
                        break;
                    family.insert(c.position.start.line);
                    end = std::max(end, c.position.end.line);
                }
                r.startLine = li.position.start.line;
                r.endLine = end;
                break;
            }
            return r;
        }
        if (p.startsWith(u"[^") && p.endsWith(u']')) {
            const QString id = p.mid(2, p.size() - 3);
            for (const md::FootnoteCache& f : meta.footnotes)
                if (f.id == id)
                    return SubpathRange{SubpathRange::Type::Footnote, f.position.start.line, f.position.end.line, 0};
            return std::nullopt;
        }
    }
    qsizetype seg = 0;
    int level = 0;
    const md::HeadingCache* found = nullptr;
    for (const md::HeadingCache& h : meta.headings) {
        if (found) {
            if (h.level <= found->level)
                return SubpathRange{SubpathRange::Type::Heading, found->position.start.line, h.position.start.line - 1, found->level};
            continue;
        }
        if (h.level > level && headingSubpathText(h.heading).toLower() == headingSubpathText(parts[seg]).toLower()) {
            ++seg;
            level = h.level;
            if (seg == parts.size())
                found = &h;
        }
    }
    if (found)
        return SubpathRange{SubpathRange::Type::Heading, found->position.start.line, -1, found->level};
    return std::nullopt;
}

} // namespace oc
