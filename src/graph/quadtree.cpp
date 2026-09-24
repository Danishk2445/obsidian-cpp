#include "graph/quadtree.h"

#include <limits>

namespace oc::graph {

int Quadtree::newLeaf(int data)
{
    Node n;
    n.data = data;
    m_nodes.push_back(n);
    return int(m_nodes.size()) - 1;
}

int Quadtree::newInternal()
{
    m_nodes.emplace_back();
    return int(m_nodes.size()) - 1;
}

void Quadtree::build(const double* xs, const double* ys, int n)
{
    m_nodes.clear();
    m_nodes.reserve(size_t(n) * 2 + 8);
    m_root = -1;
    m_x0 = m_y0 = m_x1 = m_y1 = NAN;

    double ex0 = std::numeric_limits<double>::infinity(), ey0 = ex0;
    double ex1 = -ex0, ey1 = -ex0;
    for (int i = 0; i < n; ++i) {
        const double x = xs[i], y = ys[i];
        if (std::isnan(x) || std::isnan(y))
            continue;
        if (x < ex0)
            ex0 = x;
        if (x > ex1)
            ex1 = x;
        if (y < ey0)
            ey0 = y;
        if (y > ey1)
            ey1 = y;
    }
    if (ex0 > ex1 || ey0 > ey1)
        return;
    cover(ex0, ey0);
    cover(ex1, ey1);
    for (int i = 0; i < n; ++i)
        add(xs[i], ys[i], i, xs, ys);
}

void Quadtree::cover(double x, double y)
{
    if (std::isnan(x) || std::isnan(y))
        return;
    double x0 = m_x0, y0 = m_y0, x1 = m_x1, y1 = m_y1;
    if (std::isnan(x0)) {
        // Integer extents so that later doublings keep quadrant boundaries exact.
        x0 = std::floor(x);
        x1 = x0 + 1;
        y0 = std::floor(y);
        y1 = y0 + 1;
    } else {
        double z = x1 - x0;
        if (z == 0 || std::isnan(z))
            z = 1;
        int node = m_root;
        const bool wrap = m_root >= 0 && !m_nodes[size_t(m_root)].isLeaf();
        while (x0 > x || x >= x1 || y0 > y || y >= y1) {
            const int i = (int(y < y0) << 1) | int(x < x0);
            if (wrap) {
                const int parent = newInternal();
                m_nodes[size_t(parent)].child[i] = node;
                node = parent;
            }
            z *= 2;
            switch (i) {
            case 0:
                x1 = x0 + z;
                y1 = y0 + z;
                break;
            case 1:
                x0 = x1 - z;
                y1 = y0 + z;
                break;
            case 2:
                x1 = x0 + z;
                y0 = y1 - z;
                break;
            case 3:
                x0 = x1 - z;
                y0 = y1 - z;
                break;
            }
        }
        if (wrap)
            m_root = node;
    }
    m_x0 = x0;
    m_y0 = y0;
    m_x1 = x1;
    m_y1 = y1;
}

void Quadtree::add(double x, double y, int d, const double* xs, const double* ys)
{
    if (std::isnan(x) || std::isnan(y))
        return;
    const int leaf = newLeaf(d);
    if (m_root < 0) {
        m_root = leaf;
        return;
    }
    double x0 = m_x0, y0 = m_y0, x1 = m_x1, y1 = m_y1;
    int node = m_root;
    int parent = -1;
    int i = 0;
    double xm = 0, ym = 0;
    // Find the existing leaf for the new point, or add it.
    while (!m_nodes[size_t(node)].isLeaf()) {
        const bool right = x >= (xm = (x0 + x1) / 2);
        if (right)
            x0 = xm;
        else
            x1 = xm;
        const bool bottom = y >= (ym = (y0 + y1) / 2);
        if (bottom)
            y0 = ym;
        else
            y1 = ym;
        parent = node;
        i = (int(bottom) << 1) | int(right);
        node = m_nodes[size_t(parent)].child[i];
        if (node < 0) {
            m_nodes[size_t(parent)].child[i] = leaf;
            return;
        }
    }
    // Exactly coincident with the existing point: chain, newest first.
    const double xp = xs[m_nodes[size_t(node)].data];
    const double yp = ys[m_nodes[size_t(node)].data];
    if (x == xp && y == yp) {
        m_nodes[size_t(leaf)].next = node;
        if (parent >= 0)
            m_nodes[size_t(parent)].child[i] = leaf;
        else
            m_root = leaf;
        return;
    }
    // Otherwise split until the old and new points are separated.
    int j = 0;
    do {
        const int split = newInternal();
        if (parent >= 0)
            m_nodes[size_t(parent)].child[i] = split;
        else
            m_root = split;
        parent = split;
        const bool right = x >= (xm = (x0 + x1) / 2);
        if (right)
            x0 = xm;
        else
            x1 = xm;
        const bool bottom = y >= (ym = (y0 + y1) / 2);
        if (bottom)
            y0 = ym;
        else
            y1 = ym;
        i = (int(bottom) << 1) | int(right);
        j = (int(yp >= ym) << 1) | int(xp >= xm);
    } while (i == j);
    m_nodes[size_t(parent)].child[j] = node;
    m_nodes[size_t(parent)].child[i] = leaf;
}

} // namespace oc::graph
