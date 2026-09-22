#pragma once

#include <cmath>
#include <vector>

namespace oc::graph {

// Structure-compatible port of d3-quadtree (addAll/cover/add): same integer-aligned doubling
// extent, same split rule (x >= midpoint goes right), coincident points chained with the newest
// first. Force passes traverse it in d3's order (pre-order, children 0..3), so Barnes-Hut and
// collision visit exactly the quads d3-force would. Accumulated values live in the force.
class Quadtree {
public:
    struct Node {
        int child[4] = {-1, -1, -1, -1}; // internal node children (NW, NE, SW, SE)
        int data = -1;                   // leaf: point index (head of the coincident chain)
        int next = -1;                   // leaf: next leaf with identical coordinates
        bool isLeaf() const { return data >= 0; }
    };

    // Rebuilds the tree from n points (NaN coordinates are skipped, like d3).
    void build(const double* xs, const double* ys, int n);

    int root() const { return m_root; }
    double x0() const { return m_x0; }
    double y0() const { return m_y0; }
    double x1() const { return m_x1; }
    double y1() const { return m_y1; }
    int size() const { return int(m_nodes.size()); }
    const Node& node(int i) const { return m_nodes[size_t(i)]; }

private:
    int newLeaf(int data);
    int newInternal();
    void cover(double x, double y);
    void add(double x, double y, int d, const double* xs, const double* ys);

    std::vector<Node> m_nodes;
    int m_root = -1;
    double m_x0 = NAN, m_y0 = NAN, m_x1 = NAN, m_y1 = NAN;
};

} // namespace oc::graph
