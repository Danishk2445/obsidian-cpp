// Incremental parsing == full parsing, under thousands of random edit sequences biased towards
// structure-changing tokens (fences, list markers, tabs, quotes, pipes, $$, %%, ---, ===,
// callouts, blank lines, line breaks), with and without tiny budgets (pause + resume, edits while
// a parse is still incomplete). Also checks that TreeUpdate ranges cover every line whose role,
// content column or container markers changed, and that `structural` / `headingsChanged` are not
// under-reported.
#include "markdown/md_engine.h"
#include "markdown/syntax_tree.h"
#include "text/changeset.h"
#include "text/text.h"

#include <QRandomGenerator>
#include <QStringList>

#include <gtest/gtest.h>

using namespace oc;
using namespace oc::md;

namespace {

class DocGen {
public:
    explicit DocGen(quint32 seed) : r(seed) {}
    QRandomGenerator r;
    int pick(int n) { return int(r.bounded(n)); }

    QString line()
    {
        static const char* t[] = {"",
                                  "",
                                  "  ",
                                  "\t",
                                  "plain words here",
                                  "more *text* and `code`",
                                  "- item",
                                  "- item two",
                                  "\t- nested item",
                                  "\t\t- deeper item",
                                  "\t\t\t- deepest",
                                  "  - spaced item",
                                  "* star item",
                                  "1. first",
                                  "2) second",
                                  "- [ ] task",
                                  "- [x] done task",
                                  "  continuation",
                                  "> quote",
                                  ">greentext",
                                  "> > nested quote",
                                  ">",
                                  "> [!note] Title",
                                  "> [!tip]- Folded",
                                  "> [!warning|meta]+",
                                  "```",
                                  "```js",
                                  "~~~",
                                  "````",
                                  "    indented code",
                                  "$$",
                                  "$$ x^2 $$",
                                  "%%",
                                  "%% inline %%",
                                  "---",
                                  "===",
                                  "***",
                                  "# Heading",
                                  "## Sub heading ##",
                                  "| a | b |",
                                  "|---|:-:|",
                                  "| 1 | 2 |",
                                  "a | b",
                                  "[ref]: /url \"title\"",
                                  "[^1]: footnote text",
                                  "    footnote continuation",
                                  "<div>",
                                  "</div>",
                                  "<!-- comment",
                                  "-->",
                                  "title: yaml"};
        return QString::fromLatin1(t[pick(int(std::size(t)))]);
    }

    QString doc()
    {
        QStringList lines;
        if (pick(8) == 0)
            lines << QStringLiteral("---") << QStringLiteral("title: x") << QStringLiteral("---");
        const int n = 3 + pick(35);
        for (int i = 0; i < n; ++i) {
            if (pick(5) == 0) {
                // a run of list items with tab nesting (the vault's shape)
                int depth = 0;
                for (int k = 0, m = 2 + pick(6); k < m; ++k) {
                    depth = std::clamp(depth + pick(3) - 1, 0, 3);
                    lines << QString(depth, u'\t') + (pick(6) == 0 ? QStringLiteral("1. ") : QStringLiteral("- ")) + QStringLiteral("entry %1").arg(k);
                }
                continue;
            }
            lines << line();
        }
        QString s = lines.join(QLatin1Char('\n'));
        if (pick(2))
            s += QLatin1Char('\n');
        return s;
    }

    ChangeSet edit(const Text& t)
    {
        const qsizetype len = t.length();
        const int kind = pick(10);
        auto posAt = [&]() -> qsizetype {
            const int l = pick(t.lineCount());
            const qsizetype ls = t.lineStart(l);
            switch (pick(3)) {
            case 0:
                return ls;
            case 1:
                return t.lineEnd(l);
            default:
                return ls + pick(t.lineLength(l) + 1);
            }
        };
        static const char* tok[] = {"```", "~~~", "- ", "\t", "> ", "|", "$$", "%%", "---", "===", "[!note]", "\n", "\n\n", "1. ", "# ",
                                    "[x]: /u\n", "<div>\n", " ", "a", "*", ">", "    ", "[ ] ", "-->", "\n---\n", "| a |\n|---|\n", "`"};
        if (kind <= 4) {
            const qsizetype p = posAt();
            return ChangeSet::of(ChangeSpec{p, p, QString::fromLatin1(tok[pick(int(std::size(tok)))])}, len);
        }
        if (kind <= 6 && len > 0) {
            const qsizetype a = pick(int(len + 1));
            const qsizetype b = std::min<qsizetype>(len, a + 1 + pick(pick(4) == 0 ? 60 : 4));
            return ChangeSet::of(ChangeSpec{a, b, QString()}, len);
        }
        if (kind == 7) {
            // replace a whole line with a template line
            const int l = pick(t.lineCount());
            return ChangeSet::of(ChangeSpec{t.lineStart(l), t.lineEnd(l), line()}, len);
        }
        if (kind == 8) {
            // paste a few lines
            const qsizetype p = t.lineStart(pick(t.lineCount()));
            QStringList ls;
            for (int i = 0, n = 1 + pick(4); i < n; ++i)
                ls << line();
            return ChangeSet::of(ChangeSpec{p, p, ls.join(QLatin1Char('\n')) + QLatin1Char('\n')}, len);
        }
        // multi-range change (two cursors typing)
        qsizetype a = posAt(), b = posAt();
        if (a > b)
            std::swap(a, b);
        if (a == b)
            return ChangeSet::of(ChangeSpec{a, a, QStringLiteral("x")}, len);
        return ChangeSet::of(QList<ChangeSpec>{{a, a, QString::fromLatin1(tok[pick(int(std::size(tok)))])}, {b, b, QStringLiteral("\n")}}, len);
    }
};

// Structural invariants through the public API only.
QString checkTree(const SyntaxTree& t)
{
    const int N = t.nodeCount();
    if (N < 1 || t.node(0).type != NodeType::Document || t.node(0).fromLine != 0 || t.node(0).toLine != t.lineCount())
        return QStringLiteral("bad document node");
    for (int id = 1; id < N; ++id) {
        const BlockNode& n = t.node(id);
        const int p = t.parent(id);
        if (p < 0 || p >= id)
            return QStringLiteral("node %1: parent %2").arg(id).arg(p);
        const BlockNode& pn = t.node(p);
        if (n.fromLine >= n.toLine || n.fromLine < pn.fromLine || n.toLine > pn.toLine)
            return QStringLiteral("node %1 [%2,%3) vs parent [%4,%5)").arg(id).arg(n.fromLine).arg(n.toLine).arg(pn.fromLine).arg(pn.toLine);
        if (n.depth != pn.depth + 1)
            return QStringLiteral("node %1 depth").arg(id);
        if (t.node(id - 1).fromLine > n.fromLine)
            return QStringLiteral("node %1 preorder").arg(id);
        if (isLeaf(n.type) && t.firstChild(id) >= 0)
            return QStringLiteral("leaf %1 has children").arg(id);
    }
    // child/sibling links agree with parents, siblings don't overlap
    for (int id = 0; id < N; ++id) {
        int prevEnd = -1;
        for (int c = t.firstChild(id); c >= 0; c = t.nextSibling(c)) {
            if (t.parent(c) != id)
                return QStringLiteral("child %1 of %2 has parent %3").arg(c).arg(id).arg(t.parent(c));
            if (t.node(c).fromLine < prevEnd)
                return QStringLiteral("siblings overlap at %1").arg(c);
            prevEnd = t.node(c).toLine;
        }
    }
    return {};
}

// Touch every query so ASan/UBSan see transient (paused) trees too.
void exercise(const SyntaxTree& t, const Text& text)
{
    for (int l = 0; l < t.lineCount(); ++l) {
        (void)t.role(l);
        (void)t.contentColumn(l);
        (void)t.containerMarkers(l);
        (void)t.leafAt(l);
        (void)t.ancestorsAt(l);
        (void)t.tableCells(l, text);
        (void)t.tableCellContent(l, 0, text);
    }
    for (int id = 0; id < t.nodeCount(); ++id) {
        (void)t.heading(id);
        (void)t.fence(id);
        (void)t.list(id);
        (void)t.listItem(id);
        (void)t.callout(id);
        (void)t.table(id);
        (void)t.footnoteDef(id);
        (void)t.htmlBlockKind(id);
        if (isLeaf(t.node(id).type))
            (void)t.leafContent(id, text);
    }
    (void)t.headings();
    (void)t.frontmatter();
    (void)t.firstNodeAtOrAfter(t.lineCount() / 2);
}

struct LineSig {
    int role;
    int col;
    QString markers;
    bool operator==(const LineSig&) const = default;
};

LineSig sig(const SyntaxTree& t, int l)
{
    QString m;
    for (const ContainerMarker& c : t.containerMarkers(l))
        m += QStringLiteral("%1:%2-%3,").arg(int(c.kind)).arg(c.from).arg(c.to);
    return {int(t.role(l)), t.contentColumn(l), m};
}

struct HeadingSig {
    int level, from, to;
    QString text;
    bool operator==(const HeadingSig&) const = default;
};

std::vector<HeadingSig> headingSigs(const SyntaxTree& t, const Text& text)
{
    std::vector<HeadingSig> out;
    for (int id : t.headings())
        out.push_back({t.heading(id).level, t.node(id).fromLine, t.node(id).toLine, t.leafContent(id, text).text});
    return out;
}

bool debugSessions() { return qEnvironmentVariableIsSet("OC_INC_DEBUG"); }

// Parser diagnostics since the last reset (which incremental paths the edits exercised).
struct PathCounts {
    quint64 runs, full, resyncs, lookahead, pauses, pausesBeforeSplit, sealed, lines;
};

PathCounts takeCounters()
{
    detail::Counters& c = detail::counters();
    const PathCounts p{c.updates.exchange(0), c.fullReparses.exchange(0), c.resyncs.exchange(0), c.lookaheadResyncs.exchange(0),
                       c.pauses.exchange(0), c.pausesBeforeSplit.exchange(0), c.sealedRestarts.exchange(0), c.linesParsed.exchange(0)};
    printf("runs %llu (full-parse fallbacks %llu, sealed restarts %llu), resyncs %llu (after a closing line %llu), pauses %llu "
           "(before an older split %llu), %.1f lines re-parsed per run\n",
           static_cast<unsigned long long>(p.runs), static_cast<unsigned long long>(p.full), static_cast<unsigned long long>(p.sealed),
           static_cast<unsigned long long>(p.resyncs), static_cast<unsigned long long>(p.lookahead), static_cast<unsigned long long>(p.pauses),
           static_cast<unsigned long long>(p.pausesBeforeSplit), p.runs ? double(p.lines) / double(p.runs) : 0.0);
    return p;
}

struct Stats {
    int edits = 0, paused = 0, resumes = 0, editsWhileIncomplete = 0;
};

// Runs one random edit session; returns "" or a failure description.
QString session(quint32 seed, int edits, int budgetMode, Stats& stats)
{
    DocGen g(seed);
    Text text = Text::fromString(g.doc());
    SyntaxTree tree;
    tree.parse(text);
    QString log;
    for (int e = 0; e < edits; ++e) {
        const SyntaxTree before = tree;
        const Text beforeText = text;
        const bool wasComplete = tree.validUpTo() == tree.lineCount();
        const ChangeSet cs = g.edit(text);
        const Text next = cs.apply(text);
        ParseBudget budget;
        if (budgetMode == 1 || (budgetMode == 2 && g.pick(2))) {
            budget.maxNanos = 0;
            budget.ensureLine = g.pick(3) == 0 ? g.pick(next.lineCount() + 1) : -1;
        }
        log += QStringLiteral("edit %1: %2\n").arg(e).arg(cs.desc().debugString());
        if (debugSessions())
            fprintf(stderr, "=== edit %d (%s) budget %lld ensure %d\n--- text before:\n%s\n--- tree before:\n%s--- text after:\n%s\n", e,
                    qPrintable(cs.desc().debugString()), budget.maxNanos, budget.ensureLine, qPrintable(beforeText.toString()),
                    qPrintable(tree.dump(beforeText)), qPrintable(next.toString()));
        const TreeUpdate u = tree.update(next, cs.desc(), budget);
        if (debugSessions())
            fprintf(stderr, "--- after update (complete=%d validUpTo=%d):\n%s", u.complete, tree.validUpTo(), qPrintable(tree.dump(next)));
        text = next;
        ++stats.edits;
        if (!wasComplete)
            ++stats.editsWhileIncomplete;
        if (tree.lineCount() != text.lineCount())
            return log + QStringLiteral("line count mismatch");
        if (const QString why = checkTree(tree); !why.isEmpty())
            return log + QStringLiteral("invariants after update: ") + why;
        exercise(tree, text);
        if (!u.complete) {
            ++stats.paused;
            if (u.complete != (tree.validUpTo() == tree.lineCount()))
                return log + QStringLiteral("complete flag disagrees with validUpTo");
            // Sometimes keep editing while incomplete; otherwise resume in small steps.
            if (budgetMode == 2 && g.pick(2))
                continue;
            int guard = 0;
            TreeUpdate ru;
            while (tree.resume(text, g.pick(2) ? 0 : 20'000, &ru)) {
                ++stats.resumes;
                if (debugSessions())
                    fprintf(stderr, "--- after resume step (validUpTo=%d):\n%s", tree.validUpTo(), qPrintable(tree.dump(text)));
                if (const QString why = checkTree(tree); !why.isEmpty())
                    return log + QStringLiteral("invariants after resume: ") + why;
                if (++guard > 100000)
                    return log + QStringLiteral("resume does not terminate");
            }
            exercise(tree, text);
        }
        if (tree.validUpTo() != tree.lineCount())
            continue;
        SyntaxTree full;
        full.parse(text);
        QString why;
        if (!SyntaxTree::equivalent(tree, full, &why))
            return log + QStringLiteral("NOT EQUIVALENT: %1\n--- before:\n%2\n--- after:\n%3\n--- incremental:\n%4\n--- full:\n%5")
                             .arg(why, beforeText.toString(), text.toString(), tree.dump(text), full.dump(text));
        // TreeUpdate must cover every line whose signature changed (only checked for single,
        // complete updates from a complete tree).
        if (wasComplete && u.complete) {
            for (int l = 0; l < text.lineCount(); ++l) {
                if (l >= u.fromLine && l < u.toLine)
                    continue;
                const int ol = l < u.fromLine ? l : l - u.lineDelta;
                if (ol < 0 || ol >= before.lineCount())
                    continue;
                // lines inside the edited text itself are covered by the text change
                qsizetype fa = -1, ta = -1;
                cs.desc().iterChangedRanges([&](qsizetype a, qsizetype b, qsizetype, qsizetype) {
                    if (fa < 0)
                        fa = a;
                    ta = b;
                });
                const int r0 = beforeText.lineAt(fa), e0 = beforeText.lineAt(ta);
                if (ol >= r0 && ol <= e0)
                    continue;
                if (!(sig(before, ol) == sig(tree, l)))
                    return log + QStringLiteral("line %1 changed outside TreeUpdate [%2,%3) delta %4\n%5\n%6")
                                     .arg(l)
                                     .arg(u.fromLine)
                                     .arg(u.toLine)
                                     .arg(u.lineDelta)
                                     .arg(before.dump(beforeText), tree.dump(text));
            }
            if (!u.headingsChanged && headingSigs(before, beforeText) != headingSigs(tree, text))
                return log + QStringLiteral("headings changed but headingsChanged == false");
            if (!u.structural && before.nodeCount() != tree.nodeCount())
                return log + QStringLiteral("node count changed but structural == false");
        }
    }
    // finish
    while (tree.resume(text, 1'000'000)) {
    }
    SyntaxTree full;
    full.parse(text);
    QString why;
    if (!SyntaxTree::equivalent(tree, full, &why))
        return log + QStringLiteral("final NOT EQUIVALENT: ") + why;
    return {};
}

int envInt(const char* name, int def)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok ? v : def;
}

} // namespace

// Replays one session: OC_INC_SEED=<seed> OC_INC_MODE=<0|1|2> OC_INC_EDITS=<n> [OC_INC_DEBUG=1 for dumps].
TEST(Incremental, ReplaySeed)
{
    if (!qEnvironmentVariableIsSet("OC_INC_SEED"))
        GTEST_SKIP() << "set OC_INC_SEED to replay one session";
    Stats st;
    const QString fail = session(quint32(envInt("OC_INC_SEED", 0)), envInt("OC_INC_EDITS", 25), envInt("OC_INC_MODE", 1), st);
    EXPECT_TRUE(fail.isEmpty()) << qPrintable(fail);
}

TEST(Incremental, RandomEditsDefaultBudget)
{
    takeCounters();
    Stats st;
    const int sessions = envInt("OC_INC_SESSIONS", 600);
    for (int s = 0; s < sessions; ++s) {
        const QString fail = session(quint32(1000 + s), 25, 0, st);
        ASSERT_TRUE(fail.isEmpty()) << "seed " << 1000 + s << "\n" << qPrintable(fail);
    }
    printf("%d edits\n", st.edits);
    const PathCounts pc = takeCounters();
    EXPECT_GT(pc.lookahead, 0u);
    EXPECT_GT(pc.sealed, 0u);
    EXPECT_LT(pc.full * 20, pc.runs); // full-parse fallbacks stay rare
}

TEST(Incremental, RandomEditsZeroBudgetAndResume)
{
    takeCounters();
    Stats st;
    const int sessions = envInt("OC_INC_SESSIONS", 600);
    for (int s = 0; s < sessions; ++s) {
        const QString fail = session(quint32(50000 + s), 25, 1, st);
        ASSERT_TRUE(fail.isEmpty()) << "seed " << 50000 + s << "\n" << qPrintable(fail);
    }
    printf("%d edits, %d paused, %d resume steps\n", st.edits, st.paused, st.resumes);
    EXPECT_GT(st.paused, 0);
    takeCounters();
}

TEST(Incremental, RandomEditsWhileIncomplete)
{
    takeCounters();
    Stats st;
    const int sessions = envInt("OC_INC_SESSIONS", 600);
    for (int s = 0; s < sessions; ++s) {
        const QString fail = session(quint32(90000 + s), 30, 2, st);
        ASSERT_TRUE(fail.isEmpty()) << "seed " << 90000 + s << "\n" << qPrintable(fail);
    }
    printf("%d edits, %d paused, %d edits while incomplete, %d resume steps\n", st.edits, st.paused, st.editsWhileIncomplete, st.resumes);
    EXPECT_GT(st.editsWhileIncomplete, 0);
    EXPECT_GT(takeCounters().pausesBeforeSplit, 0u);
}

TEST(Incremental, VerifyModeEnvironmentVariable)
{
    // OC_VERIFY_INCREMENTAL makes every completed update verify itself (qFatal on mismatch).
    qputenv("OC_VERIFY_INCREMENTAL", "1");
    Stats st;
    for (int s = 0; s < 60; ++s) {
        const QString fail = session(quint32(700000 + s), 20, s % 3, st);
        ASSERT_TRUE(fail.isEmpty()) << qPrintable(fail);
    }
    qunsetenv("OC_VERIFY_INCREMENTAL");
}

// Regression: a budgeted update pauses at line 1, leaving the old list (cloned from line 1) as
// stale structure; the stored state of line 2 still has the old item open although its range ended
// at line 1 (trailing blank lines). resume() then paused at line 2 and looked the old open nodes up
// from that state; the item was missing, the list clone was dropped without re-parenting its
// child, and the child became its own parent.
TEST(Incremental, ResumePausesInsideTrimmedTrailingBlankLines)
{
    const QStringList docs = {QStringLiteral("- a\n\n\n- b"), QStringLiteral("1. first\n\n       code\n\n\n1. next\n2. more"),
                              QStringLiteral("> - a\n>\n>\n> - b\n\nafter")};
    for (const QString& doc : docs) {
        for (const QString& insert : {QStringLiteral("```"), QStringLiteral("$$"), QStringLiteral("> [!tip] x")}) {
            Text text = Text::fromString(doc);
            SyntaxTree tree;
            tree.parse(text);
            const ChangeSet cs = ChangeSet::of(ChangeSpec{0, text.lineLength(0), insert}, text.length());
            text = cs.apply(text);
            TreeUpdate u = tree.update(text, cs.desc(), ParseBudget{-1, 0});
            ASSERT_TRUE(checkTree(tree).isEmpty()) << qPrintable(checkTree(tree));
            exercise(tree, text);
            int steps = 0;
            while (!u.complete) {
                u.complete = !tree.resume(text, 0, &u);
                ASSERT_TRUE(checkTree(tree).isEmpty()) << qPrintable(doc) << " / " << qPrintable(insert) << " step " << steps << ": "
                                                       << qPrintable(checkTree(tree)) << "\n"
                                                       << qPrintable(tree.dump(text));
                exercise(tree, text);
                ASSERT_LT(++steps, 1000);
            }
            SyntaxTree full;
            full.parse(text);
            QString why;
            EXPECT_TRUE(SyntaxTree::equivalent(tree, full, &why)) << qPrintable(doc) << " / " << qPrintable(insert) << ": " << qPrintable(why);
        }
    }
}

TEST(Incremental, LargeDocumentStructuralEdits)
{
    // Big tab-indented lists and many fences: edits far apart, plus a fence opened at the top
    // (everything below changes) with a tiny budget and resume.
    QStringList lines;
    for (int i = 0; i < 3000; ++i) {
        switch (i % 50) {
        case 0:
            lines << QStringLiteral("# Section %1").arg(i) << QString();
            break;
        case 10:
            lines << QStringLiteral("```bash") << QStringLiteral("echo %1").arg(i) << QStringLiteral("```");
            break;
        case 20:
            lines << QStringLiteral("| a | b |") << QStringLiteral("|---|---|") << QStringLiteral("| %1 | x |").arg(i) << QString();
            break;
        default:
            lines << QString(i % 4, u'\t') + QStringLiteral("- item %1").arg(i);
        }
    }
    Text text = Text::fromString(lines.join(QLatin1Char('\n')));
    SyntaxTree tree;
    tree.parse(text);
    auto check = [&](const char* what) {
        SyntaxTree full;
        full.parse(text);
        QString why;
        ASSERT_TRUE(SyntaxTree::equivalent(tree, full, &why)) << what << ": " << qPrintable(why);
    };
    auto apply = [&](qsizetype from, qsizetype to, const QString& ins, ParseBudget b = {}) {
        const ChangeSet cs = ChangeSet::of(ChangeSpec{from, to, ins}, text.length());
        text = cs.apply(text);
        TreeUpdate u = tree.update(text, cs.desc(), b);
        int guard = 0;
        while (!u.complete && guard++ < 1000000)
            u.complete = !tree.resume(text, 50'000);
    };
    apply(0, 0, QStringLiteral("```\n"), ParseBudget{-1, 0});
    check("fence at top");
    apply(0, 4, QString(), ParseBudget{-1, 0});
    check("fence removed");
    const qsizetype mid = text.lineStart(1500);
    apply(mid, mid, QStringLiteral("\t\t- inserted\n"));
    check("list line inserted");
    apply(text.lineStart(10), text.lineStart(10), QStringLiteral("> "));
    check("quote marker");
    apply(text.lineStart(2000), text.lineStart(2003), QString());
    check("lines deleted");
    apply(text.lineStart(700), text.lineStart(700), QStringLiteral("$$\n"), ParseBudget{800, 0});
    check("math opened with ensureLine");
}
