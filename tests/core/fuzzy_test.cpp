#include "core/fuzzy/fuzzy.h"

#include <QRandomGenerator>
#include <gtest/gtest.h>

#include <algorithm>

using namespace oc;

namespace {

double score(const QString& query, const QString& text)
{
    const auto r = FuzzyQuery(query).match(text);
    return r ? r->score : -1e9;
}

double pathScore(const QString& query, const QString& path)
{
    const auto r = FuzzyQuery(query).matchPath(path);
    return r ? r->score : -1e9;
}

QList<int> indices(const QString& query, const QString& text)
{
    const auto r = FuzzyQuery(query).match(text);
    return r ? r->matches : QList<int>{-1};
}

bool isSubsequenceCI(const QString& q, const QString& t)
{
    qsizetype i = 0;
    for (QChar c : t)
        if (i < q.size() && c.toCaseFolded() == q[i].toCaseFolded())
            ++i;
    return i == q.size();
}

} // namespace

TEST(Fuzzy, CharactersMustAppearInOrderCaseInsensitively)
{
    EXPECT_TRUE(FuzzyQuery("abc").match(u"xAxBxCx").has_value());
    EXPECT_TRUE(FuzzyQuery("ABC").match(u"abc").has_value());
    EXPECT_FALSE(FuzzyQuery("abc").match(u"acb").has_value());
    EXPECT_FALSE(FuzzyQuery("abcd").match(u"abc").has_value());
    EXPECT_FALSE(FuzzyQuery("x").match(u"").has_value());
    EXPECT_EQ(indices("abc", "a_b_c"), (QList<int>{0, 2, 4}));
    EXPECT_EQ(indices("ÜBER", "Das über"), (QList<int>{4, 5, 6, 7})); // Unicode case folding
    EXPECT_FALSE(FuzzyQuery("uber").match(u"über").has_value());    // no diacritic folding
}

TEST(Fuzzy, EmptyQueryMatchesEverythingAndSpacesAreIgnored)
{
    const auto r = FuzzyQuery("").match(u"anything");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->score, 0);
    EXPECT_TRUE(r->matches.isEmpty());
    EXPECT_TRUE(FuzzyQuery("   ").isEmpty());
    EXPECT_TRUE(FuzzyQuery("proj alpha").matchPath(u"Projects/Alpha").has_value());
    EXPECT_TRUE(FuzzyQuery("daily note").match(u"Daily note").has_value());
}

TEST(Fuzzy, PrefersConsecutiveAndWordStartMatches)
{
    EXPECT_GT(score("note", "Note"), score("note", "N o t e"));
    EXPECT_GT(score("note", "my note"), score("note", "manyotherte"));
    // Word boundaries after separators and at the start beat mid-word hits.
    EXPECT_EQ(indices("ds", "dashboards daily-standup"), (QList<int>{11, 17}));
    EXPECT_EQ(indices("fb", "xfxb foo_bar"), (QList<int>{5, 9}));
    // camelCase humps
    EXPECT_EQ(indices("gf", "gxfx getFile"), (QList<int>{5, 8}));
    EXPECT_GT(score("gf", "getFile"), score("gf", "golfer"));
    // start of text
    EXPECT_GT(score("a", "alpha"), score("a", "beta"));
}

TEST(Fuzzy, ShorterTargetsSlightlyPreferred)
{
    EXPECT_GT(score("alpha", "Alpha"), score("alpha", "Alpha project plan"));
    // ...but only slightly: a better alignment wins over length.
    EXPECT_GT(score("abc", "abc with a very long tail of other words"), score("abc", "a_b_c"));
}

TEST(Fuzzy, MatchPathPrefersTheBasename)
{
    EXPECT_GT(pathScore("alpha", "notes/Alpha"), pathScore("alpha", "Alpha/notes"));
    EXPECT_GT(pathScore("plan", "Plan"), pathScore("plan", "Projects/Alpha/Plan"));
    const auto r = FuzzyQuery("notes").matchPath(u"Notes/Archive/Notes");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->matches, (QList<int>{14, 15, 16, 17, 18})); // highlight the file name, not the folder
    // After '/' is a word boundary.
    EXPECT_EQ(FuzzyQuery("dt").matchPath(u"Daily/Todo")->matches, (QList<int>{0, 6}));
}

TEST(Fuzzy, HighlightRanges)
{
    EXPECT_EQ(matchIndicesToRanges({0, 1, 2, 5, 7, 8}), (QList<MatchRange>{{0, 3}, {5, 6}, {7, 9}}));
    EXPECT_TRUE(matchIndicesToRanges({}).isEmpty());
}

TEST(Fuzzy, RandomisedMatchesAreValidAlignments)
{
    QRandomGenerator rng(1234);
    const QString alphabet = QStringLiteral("abcAB-_/ .1");
    for (int iter = 0; iter < 20000; ++iter) {
        QString text, query;
        for (int i = 0, n = rng.bounded(0, 24); i < n; ++i)
            text += alphabet[rng.bounded(alphabet.size())];
        for (int i = 0, n = rng.bounded(1, 6); i < n; ++i)
            query += alphabet[rng.bounded(alphabet.size())];
        const QString effective = QString(query).remove(u' ');
        const FuzzyQuery fq(query);
        for (bool path : {false, true}) {
            const auto r = path ? fq.matchPath(text) : fq.match(text);
            ASSERT_EQ(r.has_value(), isSubsequenceCI(effective, text)) << text.toStdString() << " / " << query.toStdString();
            if (!r)
                continue;
            ASSERT_EQ(r->matches.size(), effective.size());
            for (qsizetype k = 0; k < r->matches.size(); ++k) {
                const int i = r->matches[k];
                ASSERT_TRUE(i >= 0 && i < text.size());
                ASSERT_EQ(text[i].toCaseFolded(), effective[k].toCaseFolded());
                if (k > 0) {
                    ASSERT_GT(i, r->matches[k - 1]);
                }
            }
        }
    }
}

TEST(SimpleSearch, AllWordsMustAppearInAnyOrder)
{
    const SimpleSearch s("world HELLO");
    const auto r = s.match(u"Hello, wide world");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->matches, (QList<MatchRange>{{0, 5}, {12, 17}}));
    EXPECT_FALSE(s.match(u"hello there").has_value());
    EXPECT_TRUE(SimpleSearch("  ").match(u"x").has_value());
    EXPECT_EQ(SimpleSearch("ab abc").match(u"xabcx")->matches, (QList<MatchRange>{{1, 4}})); // overlapping merged
    EXPECT_GT(SimpleSearch("note").match(u"note taking")->score, SimpleSearch("note").match(u"keynote")->score);
    EXPECT_EQ(SimpleSearch("a  b\tc").words(), (QStringList{"a", "b", "c"}));
}
