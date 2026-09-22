// Perf gates for the block parser on in-test generated documents shaped like the user's vault
// (a 5.5 MB / 64k-line note with 7-14k char lines, a 1.4 MB note with ~1300 fences, a 13.9k-item
// tab-indented list). Label "perf". Timing assertions are active in optimized (NDEBUG) builds unless
// OC_PERF_ASSERT=0, and forced on with OC_PERF_ASSERT=1; other runs only print the numbers.
#include "markdown/syntax_tree.h"
#include "text/changeset.h"
#include "text/text.h"

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <gtest/gtest.h>

#include <algorithm>

using namespace oc;
using namespace oc::md;

namespace {

bool assertPerf()
{
    bool set = false;
    const int v = qEnvironmentVariableIntValue("OC_PERF_ASSERT", &set);
#ifdef NDEBUG
    return !set || v != 0;
#else
    return set && v == 1;
#endif
}

const char* kWords[] = {"the", "of", "and", "kernel", "config", "mount", "partition", "package", "driver", "network", "server",
                        "client", "module", "service", "script", "write", "people", "number", "water", "first", "long", "time",
                        "**bold**", "`code`", "[[Some note]]", "#tag"};

QString sentence(QRandomGenerator& r, int n)
{
    QString s;
    for (int i = 0; i < n; ++i) {
        s += QLatin1StringView(kWords[r.bounded(int(std::size(kWords)))]);
        s += QLatin1Char(' ');
    }
    s[0] = s[0].toUpper();
    s.chop(1);
    return s + QLatin1Char('.');
}

void listBlock(QRandomGenerator& r, QString& out, int items)
{
    int depth = 0;
    for (int i = 0; i < items; ++i) {
        depth = std::clamp(depth + int(r.bounded(3)) - 1, 0, 3);
        out += QString(depth, u'\t') + (r.bounded(10) == 0 ? QStringLiteral("1. ") : QStringLiteral("- ")) + sentence(r, 3 + int(r.bounded(10)))
               + QLatin1Char('\n');
    }
}

// ~5.5 MB, ~64k lines: headings, tab-indented lists, paragraphs, some 7-14k char lines, a few
// quotes and tables, no code (like the survey's giant note).
QString giantDoc()
{
    QRandomGenerator r(42);
    QString out;
    out.reserve(5'600'000);
    while (out.size() < 5'500'000) {
        const int k = int(r.bounded(1000));
        if (k < 20) {
            out += QStringLiteral("# ") + sentence(r, 4) + QStringLiteral("\n\n");
        } else if (k < 300) {
            listBlock(r, out, 3 + int(r.bounded(15)));
            out += QLatin1Char('\n');
        } else if (k < 305) {
            QString para;
            const int target = 7000 + int(r.bounded(7266));
            while (para.size() < target)
                para += sentence(r, 12) + QLatin1Char(' ');
            out += para.left(target) + QStringLiteral("\n\n");
        } else if (k < 310) {
            out += QStringLiteral("> ") + sentence(r, 8) + QStringLiteral("\n> ") + sentence(r, 6) + QStringLiteral("\n\n");
        } else if (k < 312) {
            out += QStringLiteral("| a | b |\n|---|---|\n| ") + sentence(r, 2) + QStringLiteral(" | x |\n\n");
        } else {
            out += sentence(r, 5 + int(r.bounded(40))) + QLatin1Char('\n');
        }
    }
    return out;
}

// ~1.4 MB with ~1300 fenced blocks (mostly without a language).
QString fencesDoc()
{
    QRandomGenerator r(7);
    static const char* langs[] = {"", "", "", "", "bash", "c", "ini", "sh"};
    QString out;
    for (int f = 0; f < 1303; ++f) {
        out += sentence(r, 10 + int(r.bounded(30))) + QStringLiteral("\n\n```") + QLatin1StringView(langs[r.bounded(8)]) + QLatin1Char('\n');
        for (int i = 0, n = 3 + int(r.bounded(25)); i < n; ++i)
            out += QString(int(r.bounded(3)) * 2, u' ') + sentence(r, 3 + int(r.bounded(8))) + QLatin1Char('\n');
        out += QStringLiteral("```\n\n");
    }
    return out;
}

// ~13.9k tab-indented list items, depth <= 4, a few blank-line separated runs.
QString listsDoc()
{
    QRandomGenerator r(9);
    QString out;
    for (int block = 0; block < 14; ++block) {
        listBlock(r, out, 990 + int(r.bounded(20)));
        out += QLatin1Char('\n');
    }
    return out;
}

struct Stats {
    double p50 = 0, p95 = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    Stats s;
    if (v.empty())
        return s;
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
    s.max = v.back();
    return s;
}

double ms(const QElapsedTimer& t) { return double(t.nsecsElapsed()) / 1e6; }

// One edit: returns the update time in ms (excluding building the new Text).
double timedEdit(SyntaxTree& tree, Text& text, qsizetype from, qsizetype to, const QString& insert, const ParseBudget& budget = {},
                 TreeUpdate* out = nullptr)
{
    const ChangeSet cs = ChangeSet::of(ChangeSpec{from, to, insert}, text.length());
    const Text next = cs.apply(text);
    QElapsedTimer t;
    t.start();
    const TreeUpdate u = tree.update(next, cs.desc(), budget);
    const double dt = ms(t);
    text = next;
    if (out)
        *out = u;
    return dt;
}

void expectEquivalentToFullParse(const SyntaxTree& tree, const Text& text, const char* what)
{
    SyntaxTree full;
    full.parse(text);
    QString why;
    EXPECT_TRUE(SyntaxTree::equivalent(tree, full, &why)) << what << ": " << qPrintable(why);
}

int findLine(const Text& text, int from, auto&& pred)
{
    for (int l = from; l < text.lineCount(); ++l)
        if (pred(l))
            return l;
    return -1;
}

} // namespace

TEST(ParserPerf, FullParse)
{
    struct Doc {
        const char* name;
        QString s;
        double budgetMs;
    };
    const Doc docs[] = {{"giant", giantDoc(), 120.0}, {"fences", fencesDoc(), 40.0}, {"lists", listsDoc(), 30.0}};
    for (const Doc& d : docs) {
        const Text text = Text::fromString(d.s);
        double best = 1e9;
        SyntaxTree tree;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t;
            t.start();
            tree.parse(text);
            best = std::min(best, ms(t));
        }
        std::printf("[perf] full parse %-6s: %.2f MB, %d lines, %d nodes: %.1f ms (%.1f MB/s)\n", d.name, double(d.s.size()) / 1e6,
                    text.lineCount(), tree.nodeCount(), best, double(d.s.size()) / 1e3 / best);
        if (assertPerf()) {
            EXPECT_LT(best, d.budgetMs) << d.name;
        }
    }
}

TEST(ParserPerf, TypingInParagraphs)
{
    Text text = Text::fromString(giantDoc());
    SyntaxTree tree;
    tree.parse(text);

    // A normal paragraph line in the middle of the document.
    const int line = findLine(text, text.lineCount() / 2, [&](int l) {
        return tree.role(l) == LineRole::Paragraph && tree.containerMarkers(l).isEmpty() && text.lineLength(l) > 40 && text.lineLength(l) < 400;
    });
    ASSERT_GE(line, 0);
    std::vector<double> keys;
    qsizetype pos = text.lineStart(line) + 10;
    for (int i = 0; i < 300; ++i, ++pos)
        keys.push_back(timedEdit(tree, text, pos, pos, QString(QChar(u'a' + i % 26))));
    for (int i = 0; i < 100; ++i)
        keys.push_back(timedEdit(tree, text, pos - 1, pos, QString())), --pos;
    const Stats ks = stats(keys);
    std::printf("[perf] char typed/deleted in a paragraph: p50 %.3f ms, p95 %.3f ms, max %.3f ms\n", ks.p50, ks.p95, ks.max);

    // Inside a 7-14k char line.
    const int longLine = findLine(text, 0, [&](int l) { return text.lineLength(l) > 7000; });
    ASSERT_GE(longLine, 0);
    std::vector<double> longKeys;
    pos = text.lineStart(longLine) + 3000;
    for (int i = 0; i < 200; ++i, ++pos)
        longKeys.push_back(timedEdit(tree, text, pos, pos, QString(QChar(u'a' + i % 26))));
    const Stats ls = stats(longKeys);
    std::printf("[perf] char typed in a %d-char line: p50 %.3f ms, p95 %.3f ms, max %.3f ms\n", text.lineLength(longLine), ls.p50, ls.p95, ls.max);

    // Enter / Backspace (line count changes: every later node shifts).
    std::vector<double> enters;
    for (int i = 0; i < 50; ++i) {
        const qsizetype p = text.lineStart(line) + 5;
        enters.push_back(timedEdit(tree, text, p, p, QStringLiteral("\n")));
        enters.push_back(timedEdit(tree, text, p, p + 1, QString()));
    }
    const Stats es = stats(enters);
    std::printf("[perf] Enter/Backspace in a paragraph (mid-document): p50 %.3f ms, p95 %.3f ms, max %.3f ms\n", es.p50, es.p95, es.max);

    expectEquivalentToFullParse(tree, text, "after typing");
    if (assertPerf()) {
        EXPECT_LT(ks.p95, 0.3);
        EXPECT_LT(ls.p95, 0.3);
        EXPECT_LT(es.p95, 2.0);
    }
}

TEST(ParserPerf, BigTabIndentedList)
{
    Text text = Text::fromString(listsDoc());
    SyntaxTree tree;
    tree.parse(text);
    QRandomGenerator r(3);
    std::vector<double> inserts, typing;
    for (int i = 0; i < 100; ++i) {
        const int l = int(r.bounded(text.lineCount() - 1));
        const qsizetype p = text.lineStart(l);
        inserts.push_back(timedEdit(tree, text, p, p, QString(int(r.bounded(4)), u'\t') + QStringLiteral("- inserted item\n")));
        const qsizetype q = text.lineEnd(l);
        typing.push_back(timedEdit(tree, text, q, q, QStringLiteral("x")));
    }
    const Stats is = stats(inserts), ts = stats(typing);
    int items = 0;
    for (int id = 0; id < tree.nodeCount(); ++id)
        items += tree.node(id).type == NodeType::ListItem ? 1 : 0;
    std::printf("[perf] %d-item list: line inserted p50 %.3f ms, p95 %.3f ms, max %.3f ms; char typed p50 %.3f ms, p95 %.3f ms\n", items,
                is.p50, is.p95, is.max, ts.p50, ts.p95);
    expectEquivalentToFullParse(tree, text, "after list edits");
    if (assertPerf()) {
        EXPECT_LT(is.p95, 1.0);
        EXPECT_LT(ts.p95, 0.3);
    }
}

TEST(ParserPerf, FenceOpenedAtTopOfGiantDocument)
{
    Text text = Text::fromString(giantDoc());
    SyntaxTree tree;
    tree.parse(text);
    auto resumeAll = [&](const char* what) {
        int steps = 0;
        double total = 0, worst = 0;
        for (bool more = true; more; ++steps) {
            QElapsedTimer t;
            t.start();
            more = tree.resume(text, 4'000'000);
            const double dt = ms(t);
            total += dt;
            worst = std::max(worst, dt);
        }
        std::printf("[perf]   %s: resume finished in %d steps, %.1f ms total, worst step %.2f ms\n", what, steps, total, worst);
        return worst;
    };
    // Type ``` on a new first line: the third backtick turns the whole document into code.
    double open = 0;
    TreeUpdate u;
    timedEdit(tree, text, 0, 0, QStringLiteral("\n"));
    for (int i = 0; i < 3; ++i)
        open = timedEdit(tree, text, i, i, QStringLiteral("`"), ParseBudget{}, &u);
    std::printf("[perf] \"```\" typed at the top of the giant note: budgeted update %.2f ms (complete=%d, validUpTo=%d of %d)\n", open,
                u.complete, tree.validUpTo(), tree.lineCount());
    EXPECT_EQ(tree.role(0), LineRole::FenceOpen);
    const double worstOpen = u.complete ? 0 : resumeAll("fence open");
    expectEquivalentToFullParse(tree, text, "fence opened");

    // Typing the info string is cheap again (same fence state on the next line).
    const double info = timedEdit(tree, text, 3, 3, QStringLiteral("j"));
    std::printf("[perf] info string char typed while everything is code: %.3f ms\n", info);

    // Closing it again (delete the fence line).
    const double close = timedEdit(tree, text, 0, 5, QString(), ParseBudget{}, &u);
    std::printf("[perf] fence line deleted: budgeted update %.2f ms (complete=%d)\n", close, u.complete);
    const double worstClose = u.complete ? 0 : resumeAll("fence removed");
    expectEquivalentToFullParse(tree, text, "fence removed");

    // "$$" opened in the middle while a resume is pending, then typed into again.
    const int mid = text.lineCount() / 2;
    const double math = timedEdit(tree, text, text.lineStart(mid), text.lineStart(mid), QStringLiteral("$$\n"), ParseBudget{}, &u);
    const double again = timedEdit(tree, text, text.lineStart(mid) + 2, text.lineStart(mid) + 2, QStringLiteral(" x"), ParseBudget{}, &u);
    std::printf("[perf] \"$$\" line inserted mid-document: %.2f ms; typing on it while incomplete: %.3f ms\n", math, again);
    if (!u.complete)
        resumeAll("math open");
    expectEquivalentToFullParse(tree, text, "math opened");

    if (assertPerf()) {
        EXPECT_LT(open, 5.0);
        EXPECT_LT(close, 5.0);
        EXPECT_LT(math, 5.0);
        EXPECT_LT(info, 0.3);
        EXPECT_LT(worstOpen, 8.0);
        EXPECT_LT(worstClose, 8.0);
    }
}
