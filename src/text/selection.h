#pragma once

#include "text/changeset.h"

#include <QList>

#include <cmath>
#include <limits>

namespace oc {

struct SelRange {
    qsizetype anchor = 0;
    qsizetype head = 0;
    float goalX = std::numeric_limits<float>::quiet_NaN(); // preferred x for vertical motion (content px)

    SelRange() = default;
    SelRange(qsizetype a, qsizetype h, float goal = std::numeric_limits<float>::quiet_NaN())
        : anchor(a), head(h), goalX(goal) {}
    static SelRange cursor(qsizetype pos) { return SelRange(pos, pos); }

    qsizetype from() const { return std::min(anchor, head); }
    qsizetype to() const { return std::max(anchor, head); }
    bool empty() const { return anchor == head; }
    bool hasGoal() const { return !std::isnan(goalX); }

    SelRange map(const ChangeDesc& change, int assoc = -1) const;
    SelRange extend(qsizetype newHead) const { return SelRange(anchor, newHead); }
    bool operator==(const SelRange& o) const { return anchor == o.anchor && head == o.head; }
};

// Normalized multi-range selection: ranges sorted by position, overlapping ranges merged, `main`
// indexes the primary range.
class Selection {
public:
    Selection() : m_ranges{SelRange()} {}
    static Selection single(qsizetype anchor, qsizetype head);
    static Selection cursor(qsizetype pos) { return single(pos, pos); }
    static Selection create(QList<SelRange> ranges, int main = 0);

    const QList<SelRange>& ranges() const { return m_ranges; }
    int mainIndex() const { return m_main; }
    const SelRange& main() const { return m_ranges[m_main]; }
    int size() const { return int(m_ranges.size()); }
    bool allEmpty() const;

    Selection map(const ChangeDesc& change, int assoc = -1) const;
    Selection withMain(const SelRange& r) const;
    Selection addRange(const SelRange& r, bool makeMain = true) const;
    Selection asSingle() const { return Selection::create({main()}, 0); }
    Selection clamped(qsizetype docLength) const;

    bool operator==(const Selection& o) const { return m_main == o.m_main && m_ranges == o.m_ranges; }

private:
    QList<SelRange> m_ranges;
    int m_main = 0;
};

} // namespace oc
