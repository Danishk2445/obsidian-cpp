#include "core/util/word_count.h"

#include <gtest/gtest.h>

using namespace oc;

TEST(WordCount, ObsidianRules)
{
    EXPECT_EQ(countWords(u""), 0);
    EXPECT_EQ(countWords(u"hello world"), 2);
    EXPECT_EQ(countWords(u"don't stop-me now’s"), 3);
    EXPECT_EQ(countWords(u"foo_bar abc123"), 4);           // '_' and digits split words
    EXPECT_EQ(countWords(u"1,234.56 and 7"), 3);            // numbers with separators
    EXPECT_EQ(countWords(u"- item\n- second"), 4);          // list bullets count, like Obsidian
    EXPECT_EQ(countWords(u"**bold** `code` [[link]]"), 3);
    EXPECT_EQ(countWords(u"日本語のテキスト"), 8);            // each ideograph/kana is a word
    EXPECT_EQ(countWords(u"한국어 문장"), 2);                  // Hangul forms words
    EXPECT_EQ(countWords(u"naïve café"), 2);
    EXPECT_EQ(countWords(u"😀 ✓ → ---"), 1);                // symbols aren't words; "---" is
    EXPECT_EQ(countWords(u"x.y"), 2);
}

TEST(WordCount, FrontmatterExcludedForNotes)
{
    const QString note = QStringLiteral("---\ntags: [a, b]\ntitle: x\n---\nTwo words");
    const TextCounts c = countNote(note);
    EXPECT_EQ(c.words, 2);
    EXPECT_EQ(c.characters, 9);
    EXPECT_EQ(countNote(u"---\nnot closed").words, 3);
    EXPECT_EQ(countNote(u"no frontmatter").characters, 14);
    EXPECT_EQ(countNote(u"---\r\na: 1\r\n---\r\nhi").words, 1);
    EXPECT_EQ(countSelection(u"---\na: b\n---").words, 4);
}
