// Spellcheck: word boundaries, the clone's custom dictionary, skipped regions and the word being typed.
// Dictionary-dependent checks skip when no en_US dictionary is installed (pacman: hunspell-en_us).
#include "editor/spell/spell_checker.h"
#include "markdown/doc_source.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

namespace {
QStringList flagged(const char* markdown, int line = 0, int skipCol = -1)
{
    md::StaticDocSource doc(Text::fromString(QString::fromUtf8(markdown)));
    LineDecoration d;
    SpellChecker::instance().decorateLine(doc, line, d, skipCol);
    const QString s = doc.text().line(line);
    QStringList out;
    for (const Mark& m : d.marks)
        if (m.style == StyleId::SpellError)
            out << s.mid(m.from, m.to - m.from);
    return out;
}
} // namespace

TEST(Spell, WordAt)
{
    int a = 0, b = 0;
    ASSERT_TRUE(SpellChecker::wordAt(u"say 'helo' now", 6, &a, &b));
    EXPECT_EQ(a, 5); // quotes are not part of the word
    EXPECT_EQ(b, 9);
    ASSERT_TRUE(SpellChecker::wordAt(u"don't", 5, &a, &b));
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 5);
    EXPECT_FALSE(SpellChecker::wordAt(u"  ", 1, &a, &b));
}

TEST(Spell, CustomDictionaryFile)
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/Custom Dictionary.txt";
    SpellChecker& sc = SpellChecker::instance();
    sc.setCustomDictionaryPath(path);
    QSignalSpy changed(&sc, &SpellChecker::dictionaryChanged);
    sc.addToDictionary("Obsidianish");
    sc.addToDictionary("zettel");
    sc.addToDictionary("zettel"); // once
    EXPECT_EQ(changed.count(), 2);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("Obsidianish\nzettel\n"));
    sc.setCustomDictionaryPath(path); // reload
    EXPECT_EQ(sc.customWords(), (QStringList{"Obsidianish", "zettel"}));
    sc.setCustomDictionaryPath(tmp.path() + "/other.txt");
    EXPECT_TRUE(sc.customWords().isEmpty());
}

TEST(Spell, FlagsAndSkips)
{
    SpellChecker& sc = SpellChecker::instance();
    if (!sc.available())
        GTEST_SKIP() << "no en_US dictionary installed";
    QTemporaryDir tmp;
    sc.setCustomDictionaryPath(tmp.path() + "/dict.txt");
    EXPECT_EQ(flagged("This is helo wrld."), (QStringList{"helo", "wrld"}));
    // code, links, tags, URLs, math, comments, HTML, block ids, ALL-CAPS and digits are skipped
    EXPECT_TRUE(flagged("`helo` [[wrld]] #tagg https://x.io/helo $wrld$ %%helo%% <spn> wrld2 NASAA ^blkid").isEmpty());
    EXPECT_EQ(flagged("[shown helo](http://x.y/wrld)"), QStringList{"helo"}); // link text is checked, the target isn't
    EXPECT_TRUE(flagged("```\nhelo wrld\n```", 1).isEmpty()); // fenced code
    // '_' emphasis and snake_case: the parts are words of their own
    EXPECT_TRUE(flagged("_italic too_, __strong words__, snake_case").isEmpty());
    EXPECT_EQ(flagged("_helo_ wrld_there"), (QStringList{"helo", "wrld"}));
    // only Latin-script words are checked against the en-US dictionary
    EXPECT_EQ(flagged("привет مرحبا שלום こんにちは 你好世界 helo"), QStringList{"helo"});
    EXPECT_EQ(flagged("don't it’s helo's"), QStringList{"helo's"});
    // the word at the cursor while typing
    EXPECT_EQ(flagged("helo wrld", 0, 9), QStringList{"helo"});
    EXPECT_EQ(flagged("helo wrld", 0, 4), QStringList{"wrld"});
    // the custom dictionary wins
    sc.addToDictionary("wrld");
    EXPECT_EQ(flagged("helo wrld"), QStringList{"helo"});
    EXPECT_FALSE(sc.suggestions("helo").isEmpty());
}
