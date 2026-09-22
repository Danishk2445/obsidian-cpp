#include "text/diff.h"

#include <QHash>

#include <algorithm>

namespace oc {

std::vector<std::pair<int, int>> diffSequences(const std::vector<int>& A, const std::vector<int>& B, int maxD)
{
    // Trim common prefix/suffix first (cheap and handles the common case).
    int pre = 0;
    const int n0 = int(A.size()), m0 = int(B.size());
    while (pre < n0 && pre < m0 && A[pre] == B[pre])
        ++pre;
    int suf = 0;
    while (suf < n0 - pre && suf < m0 - pre && A[n0 - 1 - suf] == B[m0 - 1 - suf])
        ++suf;
    const int n = n0 - pre - suf, m = m0 - pre - suf;
    std::vector<std::pair<int, int>> out;
    out.reserve(size_t(std::min(n0, m0)));
    for (int i = 0; i < pre; ++i)
        out.push_back({i, i});

    if (n > 0 && m > 0) {
        auto a = [&](int i) { return A[pre + i]; };
        auto b = [&](int j) { return B[pre + j]; };
        const int max = n + m;
        const int dCap = std::min(max, maxD);
        const int off = max;
        std::vector<int> v(size_t(2 * max + 2), 0);
        std::vector<std::vector<int>> trace;
        bool found = false;
        int foundD = 0;
        for (int d = 0; d <= dCap && !found; ++d) {
            trace.push_back(v);
            for (int k = -d; k <= d; k += 2) {
                int x;
                if (k == -d || (k != d && v[size_t(off + k - 1)] < v[size_t(off + k + 1)]))
                    x = v[size_t(off + k + 1)];
                else
                    x = v[size_t(off + k - 1)] + 1;
                int y = x - k;
                while (x < n && y < m && a(x) == b(y)) {
                    ++x;
                    ++y;
                }
                v[size_t(off + k)] = x;
                if (x >= n && y >= m) {
                    found = true;
                    foundD = d;
                    break;
                }
            }
        }
        if (found) {
            // Backtrack
            std::vector<std::pair<int, int>> mid;
            int x = n, y = m;
            for (int d = foundD; d > 0; --d) {
                const std::vector<int>& vp = trace[size_t(d)];
                const int k = x - y;
                int prevK;
                if (k == -d || (k != d && vp[size_t(off + k - 1)] < vp[size_t(off + k + 1)]))
                    prevK = k + 1;
                else
                    prevK = k - 1;
                const int prevX = vp[size_t(off + prevK)];
                const int prevY = prevX - prevK;
                while (x > prevX && y > prevY) {
                    --x;
                    --y;
                    mid.push_back({x, y});
                }
                x = prevX;
                y = prevY;
            }
            while (x > 0 && y > 0) {
                --x;
                --y;
                mid.push_back({x, y});
            }
            std::reverse(mid.begin(), mid.end());
            for (auto [i, j] : mid)
                out.push_back({pre + i, pre + j});
        }
        // else: too different — no matches in the middle (replaced wholesale)
    }
    for (int s = suf; s > 0; --s)
        out.push_back({n0 - s, m0 - s});
    return out;
}

namespace {

void charRefine(const QString& oldPart, const QString& newPart, qsizetype baseA, QList<ChangeSpec>& specs)
{
    if (oldPart.size() > 4000 || newPart.size() > 4000 || (qint64(oldPart.size()) * newPart.size() > 4'000'000)) {
        specs.push_back({baseA, baseA + oldPart.size(), newPart});
        return;
    }
    std::vector<int> a(size_t(oldPart.size())), b(size_t(newPart.size()));
    for (qsizetype i = 0; i < oldPart.size(); ++i)
        a[size_t(i)] = oldPart[i].unicode();
    for (qsizetype i = 0; i < newPart.size(); ++i)
        b[size_t(i)] = newPart[i].unicode();
    const auto matches = diffSequences(a, b, 800);
    int ai = 0, bi = 0;
    auto flushSpan = [&](int aEnd, int bEnd) {
        if (aEnd > ai || bEnd > bi)
            specs.push_back({baseA + ai, baseA + aEnd, newPart.mid(bi, bEnd - bi)});
    };
    for (auto [i, j] : matches) {
        flushSpan(i, j);
        ai = i + 1;
        bi = j + 1;
    }
    flushSpan(int(oldPart.size()), int(newPart.size()));
}

} // namespace

ChangeSet diffTexts(const Text& a, const Text& b)
{
    QHash<QString, int> ids;
    auto idOf = [&](const QString& s) {
        auto it = ids.constFind(s);
        if (it != ids.cend())
            return *it;
        const int id = int(ids.size());
        ids.insert(s, id);
        return id;
    };
    std::vector<int> la(size_t(a.lineCount())), lb(size_t(b.lineCount()));
    for (int i = 0; i < a.lineCount(); ++i)
        la[size_t(i)] = idOf(a.line(i));
    for (int i = 0; i < b.lineCount(); ++i)
        lb[size_t(i)] = idOf(b.line(i));
    const auto matches = diffSequences(la, lb);

    QList<ChangeSpec> specs;
    int ai = 0, bi = 0;
    auto hunk = [&](int aEnd, int bEnd) {
        if (aEnd == ai && bEnd == bi)
            return;
        // Lines [ai, aEnd) in a replaced by [bi, bEnd) in b. Express as a char range including the
        // separators so pure line insertions/deletions stay line-shaped.
        qsizetype fromA, toA;
        QString ins;
        if (aEnd > ai) {
            fromA = a.lineStart(ai);
            toA = a.lineEnd(aEnd - 1);
        } else {
            fromA = toA = ai < a.lineCount() ? a.lineStart(ai) : a.length();
        }
        if (bEnd > bi)
            ins = b.slice(b.lineStart(bi), b.lineEnd(bEnd - 1));
        if (aEnd > ai && bEnd > bi) {
            charRefine(a.slice(fromA, toA), ins, fromA, specs);
        } else if (aEnd > ai) { // deletion of whole lines: also remove one separator
            if (aEnd < a.lineCount())
                toA += 1;
            else if (ai > 0)
                fromA -= 1;
            specs.push_back({fromA, toA, QString()});
        } else { // insertion of whole lines
            if (ai < a.lineCount())
                specs.push_back({fromA, fromA, ins + QLatin1Char('\n')});
            else
                specs.push_back({a.length(), a.length(), QLatin1Char('\n') + ins});
        }
    };
    for (auto [i, j] : matches) {
        hunk(i, j);
        ai = i + 1;
        bi = j + 1;
    }
    hunk(a.lineCount(), b.lineCount());
    return ChangeSet::of(specs, a.length());
}

std::optional<ChangeSet> merge3(const Text& base, const Text& ours, const Text& theirs)
{
    const ChangeSet mine = diffTexts(base, ours);
    const ChangeSet other = diffTexts(base, theirs);
    // Overlap check: any changed range of `other` touching a changed range of `mine`.
    bool conflict = false;
    other.iterChangedRanges([&](qsizetype fromA, qsizetype toA, qsizetype, qsizetype) {
        if (mine.touchesRange(fromA, toA))
            conflict = true;
    });
    if (conflict)
        return std::nullopt;
    return other.map(mine.desc());
}

} // namespace oc
