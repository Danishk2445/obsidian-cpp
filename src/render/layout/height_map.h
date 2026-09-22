#pragma once

#include <algorithm>
#include <vector>

namespace oc {

// Fenwick tree over unit heights: prefix sums / point updates / y -> index in O(log n).
class HeightMap {
public:
    void assign(const std::vector<double>& heights)
    {
        m_h = heights;
        const size_t n = heights.size();
        m_tree.assign(n + 1, 0.0);
        for (size_t i = 0; i < n; ++i) {
            m_tree[i + 1] += heights[i];
            const size_t p = (i + 1) + ((i + 1) & (~(i + 1) + 1));
            if (p <= n)
                m_tree[p] += m_tree[i + 1];
        }
        m_total = 0;
        for (double v : heights)
            m_total += v;
    }
    int size() const { return int(m_h.size()); }
    double height(int i) const { return m_h[size_t(i)]; }
    double total() const { return m_total; }

    void set(int i, double h)
    {
        const double delta = h - m_h[size_t(i)];
        if (delta == 0)
            return;
        m_h[size_t(i)] = h;
        m_total += delta;
        for (size_t k = size_t(i) + 1; k < m_tree.size(); k += k & (~k + 1))
            m_tree[k] += delta;
    }

    // Sum of heights of units [0, i).
    double top(int i) const
    {
        double s = 0;
        for (size_t k = size_t(std::clamp(i, 0, size())); k > 0; k -= k & (~k + 1))
            s += m_tree[k];
        return s;
    }

    // Index of the unit containing y (clamped to [0, size-1]); for y beyond the end returns size-1.
    int indexAt(double y) const
    {
        if (m_h.empty())
            return 0;
        if (y <= 0)
            return 0;
        size_t pos = 0;
        size_t step = 1;
        while (step * 2 <= m_h.size())
            step *= 2;
        double rem = y;
        for (; step > 0; step >>= 1) {
            const size_t next = pos + step;
            if (next <= m_h.size() && m_tree[next] <= rem) {
                pos = next;
                rem -= m_tree[next];
            }
        }
        return std::min(int(pos), size() - 1);
    }

private:
    std::vector<double> m_h;
    std::vector<double> m_tree;
    double m_total = 0;
};

} // namespace oc
