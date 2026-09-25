// Vim commands on a large note (label "perf"): jumps, edits, search and a global substitute must stay
// interactive. Budgets are for the perf preset (Release).
#include "editor/vim_test_support.h"

#include <QElapsedTimer>

using namespace oc;
using namespace oc::test;

namespace {

TEST(VimPerf, LargeDocument)
{
    static ThemeManager themes;
    themes.apply();
    VimController::resetGlobalState();
    VimHarness h;
    QString text;
    for (int i = 0; i < 60000; ++i)
        text += QStringLiteral("- item %1 with some words and a needle%2 here\n").arg(i).arg(i % 100 == 0 ? QStringLiteral("X") : QString());
    h.open(u'|' + text);
    struct Step {
        const char* keys;
        int budgetMs;
    };
    const Step steps[] = {
        {"G", 100},      {"gg", 100},       {"30000G", 100},  {"dd", 100},       {"u", 100},       {"/needleX<CR>", 300},
        {"n", 300},      {"N", 300},        {"*", 300},       {"yyp", 100},      {"5>>", 100},     {"}", 100},
        {"M", 200},      {"ciwfoo<Esc>", 150}, {".", 150},    {":%s/needleX/pin/g<CR>", 1500}, {"u", 1500},
    };
    for (const Step& s : steps) {
        QElapsedTimer t;
        t.start();
        h.keys(QString::fromLatin1(s.keys));
        const qint64 ms = t.elapsed();
        EXPECT_LT(ms, s.budgetMs) << s.keys;
        std::printf("vim perf: %-24s %5lld ms\n", s.keys, static_cast<long long>(ms));
    }
    EXPECT_EQ(h.doc->text().lineCount(), 60002);
}

} // namespace
