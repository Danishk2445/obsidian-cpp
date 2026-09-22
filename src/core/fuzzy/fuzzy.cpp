#include "core/fuzzy/fuzzy.h"

#include <QChar>

#include <algorithm>
#include <array>
#include <limits>

namespace oc {
namespace {

// fzf's character classes; order matters (class > NonWord means "word-like").
enum CharClass : quint8 { White, NonWord, Delimiter, Lower, Upper, Letter, Number };

constexpr int kScoreMatch = 16;
constexpr int kScoreGapStart = -3;
constexpr int kScoreGapExtension = -1;
constexpr int kBonusBoundary = kScoreMatch / 2;                          // 8: after '-', '_', '.', ...
constexpr int kBonusNonWord = kScoreMatch / 2;                           // 8: matching a separator itself
constexpr int kBonusCamel123 = kBonusBoundary + kScoreGapExtension;      // 7: camelCase hump, digit after letter
constexpr int kBonusConsecutive = -(kScoreGapStart + kScoreGapExtension); // 4
constexpr int kBonusFirstCharMultiplier = 2;
constexpr int kBonusBoundaryWhite = kBonusBoundary + 2;     // 10: after whitespace / at the start
constexpr int kBonusBoundaryDelimiter = kBonusBoundary + 1; // 9: after '/'
constexpr int kBonusBasename = 2;                           // matchPath(): per character matched in the file name
constexpr double kLengthPenalty = 0.01;
constexpr qsizetype kLengthPenaltyCap = 200;
constexpr qsizetype kMaxDpCells = qsizetype(1) << 22;

struct AsciiTables {
    std::array<char16_t, 128> fold{};
    std::array<CharClass, 128> cls{};
};

constexpr AsciiTables makeAsciiTables()
{
    AsciiTables t;
    for (int c = 0; c < 128; ++c) {
        t.fold[c] = char16_t(c >= 'A' && c <= 'Z' ? c + 32 : c);
        CharClass k = NonWord;
        if (c >= 'a' && c <= 'z')
            k = Lower;
        else if (c >= 'A' && c <= 'Z')
            k = Upper;
        else if (c >= '0' && c <= '9')
            k = Number;
        else if (c == ' ' || (c >= '\t' && c <= '\r'))
            k = White;
        else if (c == '/' || c == ',' || c == ':' || c == ';' || c == '|')
            k = Delimiter;
        t.cls[c] = k;
    }
    return t;
}

constexpr AsciiTables kAscii = makeAsciiTables();

inline char16_t foldChar(char16_t c)
{
    return c < 128 ? kAscii.fold[c] : char16_t(QChar::toCaseFolded(char32_t(c)));
}

CharClass classOfNonAscii(char16_t c)
{
    const QChar ch(c);
    if (ch.isSpace())
        return White;
    if (ch.isLower())
        return Lower;
    if (ch.isUpper())
        return Upper;
    if (ch.isLetter())
        return Letter;
    if (ch.isDigit())
        return Number;
    return NonWord;
}

inline CharClass classOf(char16_t c) { return c < 128 ? kAscii.cls[c] : classOfNonAscii(c); }

int bonusFor(CharClass prev, CharClass cls)
{
    if (cls > NonWord) {
        if (prev == White)
            return kBonusBoundaryWhite;
        if (prev == Delimiter)
            return kBonusBoundaryDelimiter;
        if (prev == NonWord)
            return kBonusBoundary;
    }
    if ((prev == Lower && cls == Upper) || (prev != Number && cls == Number))
        return kBonusCamel123;
    if (cls == NonWord || cls == Delimiter)
        return kBonusNonWord;
    if (cls == White)
        return kBonusBoundaryWhite;
    return 0;
}

// Per-thread scratch buffers so matching thousands of targets does not allocate.
struct Scratch {
    std::vector<qsizetype> first;
    std::vector<char16_t> text;
    std::vector<int> bonus, extra, H, C;
};

Scratch& scratch()
{
    thread_local Scratch s;
    return s;
}

double finalScore(int raw, qsizetype length)
{
    return raw - kLengthPenalty * double(std::min(length, kLengthPenaltyCap));
}

// Score of a given alignment with the same rules as the DP (used when the DP is skipped or fails).
int scoreAlignment(QStringView text, const QList<int>& pos, qsizetype basenameStart)
{
    int score = 0;
    int firstBonus = 0;
    qsizetype prevPos = -1;
    for (qsizetype k = 0; k < pos.size(); ++k) {
        const qsizetype i = pos[k];
        const CharClass prev = i > 0 ? classOf(text[i - 1].unicode()) : White;
        int b = bonusFor(prev, classOf(text[i].unicode()));
        if (k == 0) {
            score += kScoreMatch + b * kBonusFirstCharMultiplier;
            firstBonus = b;
        } else if (i == prevPos + 1) {
            if (b >= kBonusBoundary && b > firstBonus)
                firstBonus = b; // a stronger boundary starts a new run
            else
                b = std::max(b, std::max(kBonusConsecutive, firstBonus));
            score += kScoreMatch + b;
        } else {
            const qsizetype gap = i - prevPos - 1;
            score += kScoreGapStart + int(gap - 1) * kScoreGapExtension;
            score += kScoreMatch + b;
            firstBonus = b;
        }
        if (i >= basenameStart)
            score += kBonusBasename;
        prevPos = i;
    }
    return std::max(score, 0);
}

} // namespace

FuzzyQuery::FuzzyQuery(const QString& query)
    : m_query(query)
{
    m_chars.reserve(size_t(query.size()));
    for (QChar c : query)
        if (!c.isSpace())
            m_chars.push_back(foldChar(c.unicode()));
}

std::optional<FuzzyResult> FuzzyQuery::match(QStringView text) const
{
    return matchImpl(text, std::numeric_limits<qsizetype>::max());
}

std::optional<FuzzyResult> FuzzyQuery::matchPath(QStringView path) const
{
    return matchImpl(path, path.lastIndexOf(u'/') + 1);
}

std::optional<FuzzyResult> FuzzyQuery::matchImpl(QStringView text, qsizetype basenameStart) const
{
    const qsizetype m = qsizetype(m_chars.size());
    if (m == 0)
        return FuzzyResult{};
    const qsizetype n = text.size();
    if (n < m)
        return std::nullopt;
    const char16_t* t = text.utf16();
    const char16_t* q = m_chars.data();

    // Phase 1: in-order scan. first[i] = earliest index at which q[0..i] can be matched in order.
    Scratch& s = scratch();
    s.first.resize(size_t(m));
    qsizetype qi = 0;
    for (qsizetype i = 0; i < n && qi < m; ++i)
        if (foldChar(t[i]) == q[qi])
            s.first[size_t(qi++)] = i;
    if (qi < m)
        return std::nullopt;
    qsizetype last = n - 1; // last occurrence of the final query char bounds the window
    while (foldChar(t[last]) != q[m - 1])
        --last;
    const qsizetype minIdx = s.first[0];
    const qsizetype W = last - minIdx + 1;

    if (m * W > kMaxDpCells) { // pathological sizes: keep the greedy alignment
        FuzzyResult r;
        for (qsizetype k = 0; k < m; ++k)
            r.matches.push_back(int(s.first[size_t(k)]));
        r.score = finalScore(scoreAlignment(text, r.matches, basenameStart), n);
        return r;
    }

    // Phase 2: folded text, bonuses and the first row.
    s.text.resize(size_t(W));
    s.bonus.resize(size_t(W));
    s.extra.resize(size_t(W));
    CharClass prev = minIdx > 0 ? classOf(t[minIdx - 1]) : White;
    for (qsizetype off = 0; off < W; ++off) {
        const char16_t c = t[minIdx + off];
        const CharClass cls = classOf(c);
        s.text[size_t(off)] = foldChar(c);
        s.bonus[size_t(off)] = bonusFor(prev, cls);
        s.extra[size_t(off)] = minIdx + off >= basenameStart ? kBonusBasename : 0;
        prev = cls;
    }
    const char16_t* T = s.text.data();
    const int* B = s.bonus.data();
    const int* P = s.extra.data();

    if (m == 1) {
        int best = -1;
        qsizetype bestPos = 0;
        for (qsizetype off = 0; off < W; ++off) {
            if (T[off] != q[0])
                continue;
            const int score = kScoreMatch + B[off] * kBonusFirstCharMultiplier + P[off];
            if (score > best) {
                best = score;
                bestPos = off;
            }
        }
        FuzzyResult r;
        r.score = finalScore(best, n);
        r.matches.push_back(int(minIdx + bestPos));
        return r;
    }

    s.H.resize(size_t(m * W));
    s.C.resize(size_t(m * W));
    int* H = s.H.data();
    int* C = s.C.data();
    {
        int prevH = 0;
        bool inGap = false;
        for (qsizetype off = 0; off < W; ++off) {
            if (T[off] == q[0]) {
                H[off] = kScoreMatch + B[off] * kBonusFirstCharMultiplier + P[off];
                C[off] = 1;
                inGap = false;
            } else {
                H[off] = std::max(prevH + (inGap ? kScoreGapExtension : kScoreGapStart), 0);
                C[off] = 0;
                inGap = true;
            }
            prevH = H[off];
        }
    }

    // Phase 3: fill the score matrix row by row, only from each character's first possible column.
    int maxScore = -1;
    qsizetype maxPos = 0;
    for (qsizetype i = 1; i < m; ++i) {
        int* Hrow = H + i * W;
        int* Crow = C + i * W;
        const int* Hup = Hrow - W;
        const int* Cup = Crow - W;
        const qsizetype start = s.first[size_t(i)] - minIdx;
        std::fill(Hrow, Hrow + start, 0);
        std::fill(Crow, Crow + start, 0);
        const char16_t qc = q[i];
        const bool lastRow = i == m - 1;
        bool inGap = false;
        for (qsizetype col = start; col < W; ++col) {
            const int s2 = Hrow[col - 1] + (inGap ? kScoreGapExtension : kScoreGapStart);
            int s1 = 0;
            int consecutive = 0;
            if (T[col] == qc) {
                s1 = Hup[col - 1] + kScoreMatch + P[col];
                int b = B[col];
                consecutive = Cup[col - 1] + 1;
                if (consecutive > 1) {
                    const int fb = B[col - consecutive + 1]; // bonus of the run's first character
                    if (b >= kBonusBoundary && b > fb)
                        consecutive = 1; // a stronger boundary breaks the run
                    else
                        b = std::max(b, std::max(kBonusConsecutive, fb));
                }
                if (s1 + b < s2) {
                    s1 += B[col];
                    consecutive = 0;
                } else {
                    s1 += b;
                }
            }
            Crow[col] = consecutive;
            inGap = s1 < s2;
            const int score = std::max(std::max(s1, s2), 0);
            Hrow[col] = score;
            if (lastRow && score > maxScore) {
                maxScore = score;
                maxPos = col;
            }
        }
    }

    // Phase 4: backtrack the best alignment.
    QList<int> pos(m, -1);
    {
        qsizetype i = m - 1;
        qsizetype j = maxPos;
        bool preferMatch = true;
        while (j >= 0) {
            const qsizetype I = i * W;
            const int sc = H[I + j];
            const qsizetype fi = s.first[size_t(i)] - minIdx;
            const int s1 = i > 0 && j >= fi ? H[I - W + j - 1] : 0;
            const int s2 = j > fi ? H[I + j - 1] : 0;
            if (sc > s1 && (sc > s2 || (sc == s2 && preferMatch))) {
                pos[i] = int(minIdx + j);
                if (i == 0)
                    break;
                --i;
            }
            preferMatch = C[I + j] > 1 || (I + W + j + 1 < m * W && C[I + W + j + 1] > 0);
            --j;
        }
    }
    bool valid = true;
    for (qsizetype k = 0; k < m && valid; ++k)
        valid = pos[k] >= 0 && (k == 0 || pos[k] > pos[k - 1]) && foldChar(t[pos[k]]) == q[k];
    Q_ASSERT_X(valid, "FuzzyQuery", "backtracking produced an invalid alignment");
    FuzzyResult r;
    if (!valid) { // not expected; keep results sane regardless
        r.matches.clear();
        for (qsizetype k = 0; k < m; ++k)
            r.matches.push_back(int(s.first[size_t(k)]));
        r.score = finalScore(scoreAlignment(text, r.matches, basenameStart), n);
        return r;
    }
    r.matches = std::move(pos);
    r.score = finalScore(maxScore, n);
    return r;
}

QList<MatchRange> matchIndicesToRanges(const QList<int>& indices)
{
    QList<MatchRange> out;
    for (int i : indices) {
        if (!out.isEmpty() && out.back().to == i)
            out.back().to = i + 1;
        else
            out.push_back({i, i + 1});
    }
    return out;
}

SimpleSearch::SimpleSearch(const QString& query)
{
    qsizetype i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i].isSpace())
            ++i;
        const qsizetype start = i;
        while (i < query.size() && !query[i].isSpace())
            ++i;
        if (i > start)
            m_words.push_back(query.mid(start, i - start));
    }
}

std::optional<SimpleSearchResult> SimpleSearch::match(QStringView text) const
{
    SimpleSearchResult r;
    if (m_words.isEmpty())
        return r;
    const double len = double(text.size()) + 1;
    for (const QString& w : m_words) {
        const qsizetype idx = text.indexOf(w, 0, Qt::CaseInsensitive);
        if (idx < 0)
            return std::nullopt;
        r.matches.push_back({int(idx), int(idx + w.size())});
        const bool wordStart = idx == 0 || !text[idx - 1].isLetterOrNumber();
        r.score += (wordStart ? 1.0 : 0.0) + (idx == 0 ? 0.5 : 0.0) - double(idx) / len;
    }
    std::sort(r.matches.begin(), r.matches.end(), [](const MatchRange& a, const MatchRange& b) { return a.from < b.from; });
    QList<MatchRange> merged;
    for (const MatchRange& m : r.matches) {
        if (!merged.isEmpty() && m.from <= merged.back().to)
            merged.back().to = std::max(merged.back().to, m.to);
        else
            merged.push_back(m);
    }
    r.matches = std::move(merged);
    return r;
}

} // namespace oc
