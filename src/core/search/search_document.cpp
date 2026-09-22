#include "core/search/search_document.h"

#include "markdown/metadata.h"

#include <algorithm>

namespace oc::search {

qsizetype SearchDocument::nameOffset() const { return path.lastIndexOf(u'/') + 1; }

QStringView SearchDocument::name() const { return QStringView(path).mid(nameOffset()); }

QStringView SearchDocument::basename() const
{
    const QStringView n = name();
    const qsizetype dot = n.lastIndexOf(u'.');
    return dot > 0 ? n.left(dot) : n; // a leading dot is part of the name (".hidden")
}

QStringView SearchDocument::pathNoExtension() const
{
    const QStringView n = name();
    const qsizetype dot = n.lastIndexOf(u'.');
    return dot > 0 ? QStringView(path).left(nameOffset() + dot) : QStringView(path);
}

SearchDocument SearchDocument::fromMetadata(const QString& path, const QString& content, const md::CachedMetadata& meta)
{
    SearchDocument d;
    d.path = path;
    d.content = content;
    const int size = int(content.size());
    const auto clampOffset = [size](qsizetype o) { return int(std::clamp<qsizetype>(o, 0, size)); };
    const auto rangeOf = [&](const md::Range& r) { return TextRange{clampOffset(r.start.offset), clampOffset(r.end.offset)}; };

    // Frontmatter tags come first in allTags(), followed by the inline tags (which carry positions).
    const QStringList all = meta.allTags();
    for (qsizetype i = 0; i < all.size() - meta.tags.size(); ++i)
        d.tags.push_back({all[i], -1, -1, -1});
    for (const md::TagCache& t : meta.tags)
        d.tags.push_back({t.tag, t.position.start.line, clampOffset(t.position.start.offset), clampOffset(t.position.end.offset)});

    // Sections: the text before the first heading, then each heading up to the next one (of any level).
    QList<int> starts;
    for (const md::HeadingCache& h : meta.headings)
        starts.push_back(clampOffset(h.position.start.offset));
    std::sort(starts.begin(), starts.end());
    int prev = 0;
    for (int s : starts) {
        if (s > prev)
            d.sections.push_back({prev, s});
        prev = s;
    }
    if (size > prev || d.sections.isEmpty())
        d.sections.push_back({prev, size});

    for (const md::SectionCache& s : meta.sections) {
        if (s.type == QLatin1StringView("yaml"))
            continue;
        const TextRange r = rangeOf(s.position);
        if (s.type == QLatin1StringView("list")) {
            bool any = false;
            for (const md::ListItemCache& li : meta.listItems) {
                const TextRange item = rangeOf(li.position);
                if (item.from >= r.from && item.to <= r.to) {
                    d.blocks.push_back(item);
                    any = true;
                }
            }
            if (any)
                continue;
        }
        d.blocks.push_back(r);
    }

    for (const md::ListItemCache& li : meta.listItems)
        if (!li.task.isNull())
            d.tasks.push_back({rangeOf(li.position), li.task});

    if (meta.frontmatter)
        for (const md::FrontmatterValue& v : *meta.frontmatter)
            d.properties.push_back({v.key, v.value});
    return d;
}

} // namespace oc::search
