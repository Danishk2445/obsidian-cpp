#include "text/selection.h"

#include <algorithm>

namespace oc {

SelRange SelRange::map(const ChangeDesc& change, int assoc) const
{
    if (change.empty())
        return *this;
    qsizetype from, to;
    if (empty()) {
        from = to = change.mapPos(this->from(), assoc);
    } else {
        from = change.mapPos(this->from(), 1);
        to = change.mapPos(this->to(), -1);
        if (to < from)
            to = from;
    }
    if (anchor <= head)
        return SelRange(from, to);
    return SelRange(to, from);
}

Selection Selection::single(qsizetype anchor, qsizetype head)
{
    Selection s;
    s.m_ranges = {SelRange(anchor, head)};
    s.m_main = 0;
    return s;
}

Selection Selection::create(QList<SelRange> ranges, int main)
{
    if (ranges.isEmpty())
        ranges = {SelRange()};
    main = std::clamp(main, 0, int(ranges.size()) - 1);
    const SelRange mainRange = ranges[main];
    // sort by from, then merge overlaps (touching empty cursors merge too)
    QList<std::pair<SelRange, bool>> items;
    for (int i = 0; i < ranges.size(); ++i)
        items.push_back({ranges[i], i == main});
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first.from() < b.first.from(); });
    QList<SelRange> out;
    int newMain = 0;
    for (auto& [r, isMain] : items) {
        if (!out.isEmpty()) {
            SelRange& prev = out.last();
            const bool overlap = r.empty() || prev.empty() ? r.from() <= prev.to() : r.from() < prev.to();
            if (overlap) {
                const qsizetype from = std::min(prev.from(), r.from()), to = std::max(prev.to(), r.to());
                const bool forward = (isMain ? r : prev).anchor <= (isMain ? r : prev).head;
                prev = forward ? SelRange(from, to) : SelRange(to, from);
                if (isMain)
                    newMain = int(out.size()) - 1;
                continue;
            }
        }
        out.push_back(r);
        if (isMain)
            newMain = int(out.size()) - 1;
    }
    Selection s;
    s.m_ranges = out;
    s.m_main = newMain;
    // keep goal column of the main range when it survived unchanged
    if (s.m_ranges[newMain] == mainRange)
        s.m_ranges[newMain].goalX = mainRange.goalX;
    return s;
}

bool Selection::allEmpty() const
{
    return std::all_of(m_ranges.begin(), m_ranges.end(), [](const SelRange& r) { return r.empty(); });
}

Selection Selection::map(const ChangeDesc& change, int assoc) const
{
    if (change.empty())
        return *this;
    QList<SelRange> out;
    out.reserve(m_ranges.size());
    for (const SelRange& r : m_ranges)
        out.push_back(r.map(change, assoc));
    return create(out, m_main);
}

Selection Selection::withMain(const SelRange& r) const
{
    QList<SelRange> out = m_ranges;
    out[m_main] = r;
    return create(out, m_main);
}

Selection Selection::addRange(const SelRange& r, bool makeMain) const
{
    QList<SelRange> out = m_ranges;
    out.push_back(r);
    return create(out, makeMain ? int(out.size()) - 1 : m_main);
}

Selection Selection::clamped(qsizetype len) const
{
    QList<SelRange> out;
    for (const SelRange& r : m_ranges)
        out.push_back(SelRange(std::clamp<qsizetype>(r.anchor, 0, len), std::clamp<qsizetype>(r.head, 0, len), r.goalX));
    return create(out, m_main);
}

} // namespace oc
