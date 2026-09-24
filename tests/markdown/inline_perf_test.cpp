// Perf gates of md::parseInline() (label "perf"; assertions only in optimized builds (NDEBUG), other
// builds just report the numbers).
#include "markdown/inline_tree.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace oc;
using namespace oc::md;

namespace {

const char* kWords[] = {"the", "of", "and", "kernel", "config", "mount", "partition", "package", "driver", "network", "server",
                        "client", "module", "service", "script", "write", "people", "number", "water", "first", "long", "time"};

// Survey-shaped inline text: plain words with bold, code, wikilinks, tags, URLs, shell '$' and emphasis.
QString sentence(QRandomGenerator& r, int words)
{
    QString s;
    for (int i = 0; i < words; ++i) {
        if (i)
            s += QLatin1Char(' ');
        const int k = r.bounded(100);
        const QString w = QString::fromLatin1(kWords[r.bounded(int(std::size(kWords)))]);
        if (k < 6)
            s += QStringLiteral("**") + w + QStringLiteral("**");
        else if (k < 10)
            s += QLatin1Char('`') + w + QStringLiteral(" -x`");
        else if (k < 12)
            s += QStringLiteral("[[") + w + QStringLiteral("]]");
        else if (k < 14)
            s += QLatin1Char('#') + w;
        else if (k < 16)
            s += QStringLiteral("https://example.com/") + w;
        else if (k < 18)
            s += QLatin1Char('$') + w.toUpper();
        else if (k < 20)
            s += QLatin1Char('*') + w + QLatin1Char('*');
        else if (k < 21)
            s += QStringLiteral("<kbd>") + w + QStringLiteral("</kbd>");
        else
            s += w;
    }
    return s;
}

double medianMicros(std::vector<qint64>& ns)
{
    std::sort(ns.begin(), ns.end());
    return ns[ns.size() / 2] / 1000.0;
}

#ifdef NDEBUG
constexpr bool kAssert = true;
#else
constexpr bool kAssert = false;
#endif

} // namespace

TEST(InlinePerf, GiantParagraph14k)
{
    QRandomGenerator r(7);
    QString text;
    while (text.size() < 14000)
        text += sentence(r, 12) + QStringLiteral(". ");
    text.truncate(14000);
    std::vector<qint64> ns;
    int nodes = 0;
    for (int rep = 0; rep < 30; ++rep) {
        QElapsedTimer t;
        t.start();
        const InlineTreePtr tree = parseInline(text);
        ns.push_back(t.nsecsElapsed());
        nodes = tree->size();
    }
    const double us = medianMicros(ns);
    std::printf("[inline-perf] 14k-char paragraph: %.0f us (median of 30), %d nodes\n", us, nodes);
    if (kAssert) {
        EXPECT_LT(us, 2000.0);
    }
}

TEST(InlinePerf, TypicalLine)
{
    const QString line = QStringLiteral("Mount the **partition** with `mount -o rw` and see [[Disk Setup#fstab|fstab]] #linux");
    std::vector<qint64> ns;
    for (int batch = 0; batch < 20; ++batch) {
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 1000; ++i) {
            const InlineTreePtr tree = parseInline(line);
            ASSERT_GT(tree->size(), 0);
        }
        ns.push_back(t.nsecsElapsed() / 1000);
    }
    const double us = medianMicros(ns);
    std::printf("[inline-perf] typical %d-char line: %.2f us\n", int(line.size()), us);
    if (kAssert) {
        EXPECT_LT(us, 20.0);
    }
}

TEST(InlinePerf, TenThousandShortLines)
{
    QRandomGenerator r(11);
    QStringList lines;
    for (int i = 0; i < 10000; ++i)
        lines << QStringLiteral("- ") + sentence(r, 3 + r.bounded(10));
    QElapsedTimer t;
    t.start();
    qsizetype nodes = 0;
    for (const QString& l : lines)
        nodes += parseInline(l)->size();
    const double ms = t.nsecsElapsed() / 1e6;
    std::printf("[inline-perf] 10,000 short lines: %.1f ms (%lld nodes)\n", ms, qlonglong(nodes));
    if (kAssert) {
        EXPECT_LT(ms, 100.0);
    }
}

TEST(InlinePerf, PathologicalInputsStayLinear)
{
    // Unclosed openers of every kind (quadratic traps in naive parsers).
    const QStringList patterns = {QStringLiteral("*a "), QStringLiteral("_a "), QStringLiteral("[a "), QStringLiteral("![a "), QStringLiteral("`a ``b "),
                                  QStringLiteral("$a "), QStringLiteral("%%a "), QStringLiteral("[[a "), QStringLiteral("<a "), QStringLiteral("**a_ "),
                                  QStringLiteral("~~a "), QStringLiteral("==a "), QStringLiteral("^[a "), QStringLiteral("a@b "), QStringLiteral("www.a ")};
    for (const QString& p : patterns) {
        const QString text = p.repeated(4000);
        QElapsedTimer t;
        t.start();
        const InlineTreePtr tree = parseInline(text);
        const double ms = t.nsecsElapsed() / 1e6;
        std::printf("[inline-perf] %-8s x4000 (%lld chars): %.2f ms\n", qPrintable(p.trimmed()), qlonglong(text.size()), ms);
        if (kAssert) {
            EXPECT_LT(ms, 20.0) << qPrintable(p);
        }
        EXPECT_EQ(tree->content().size(), text.size());
    }
    // Mixed traps: many open brackets followed by many links, unterminated titles, nested emphasis.
    const QStringList mixed = {QStringLiteral("[").repeated(4000) + QStringLiteral("[a](b) ").repeated(4000),
                               QStringLiteral("[a](b \"").repeated(4000), QStringLiteral("[a](<b").repeated(4000),
                               QStringLiteral("*a **a ***a ").repeated(3000), QStringLiteral("a**b").repeated(4000),
                               QStringLiteral("<!-- ").repeated(4000), QStringLiteral("<a b=\"").repeated(4000),
                               QStringLiteral("![[").repeated(4000) + QStringLiteral("]]"), QStringLiteral("$$a \\$").repeated(4000),
                               QStringLiteral("[x]").repeated(4000)};
    const QList<LinkRefDefinition> refs = {LinkRefDefinition{QStringLiteral("x"), QStringLiteral("/x"), QString(), 0}};
    for (const QString& text : mixed) {
        QElapsedTimer t;
        t.start();
        const InlineTreePtr tree = parseInline(text, refs);
        const double ms = t.nsecsElapsed() / 1e6;
        std::printf("[inline-perf] %-12s... (%lld chars): %.2f ms\n", qPrintable(text.left(12)), qlonglong(text.size()), ms);
        if (kAssert) {
            EXPECT_LT(ms, 20.0) << qPrintable(text.left(12));
        }
    }
}
