#include "text/codec.h"
#include "text/text.h"

#include <QRandomGenerator>
#include <gtest/gtest.h>

using namespace oc;

TEST(Text, EmptyHasOneLine)
{
    Text t;
    EXPECT_EQ(t.lineCount(), 1);
    EXPECT_EQ(t.length(), 0);
    EXPECT_EQ(t.lineEol(0), Eol::None);
}

TEST(Text, SplitsAndRecordsEols)
{
    Text t = Text::fromString(u"a\nbb\r\nccc\rdd");
    ASSERT_EQ(t.lineCount(), 4);
    EXPECT_EQ(t.line(0), "a");
    EXPECT_EQ(t.line(1), "bb");
    EXPECT_EQ(t.line(2), "ccc");
    EXPECT_EQ(t.line(3), "dd");
    EXPECT_EQ(t.lineEol(0), Eol::LF);
    EXPECT_EQ(t.lineEol(1), Eol::CRLF);
    EXPECT_EQ(t.lineEol(2), Eol::CR);
    EXPECT_EQ(t.lineEol(3), Eol::None);
    EXPECT_EQ(t.length(), 1 + 1 + 2 + 1 + 3 + 1 + 2);
    EXPECT_EQ(t.toString(), "a\nbb\nccc\ndd");
    EXPECT_EQ(t.lineStart(2), 5);
    EXPECT_EQ(t.lineEnd(2), 8);
    EXPECT_EQ(t.lineAt(0), 0);
    EXPECT_EQ(t.lineAt(1), 0); // end of line 0
    EXPECT_EQ(t.lineAt(2), 1);
    EXPECT_EQ(t.lineAt(t.length()), 3);
    EXPECT_EQ(t.charAt(1), QChar('\n'));
    EXPECT_EQ(t.slice(3, 7), "b\ncc");
}

TEST(Text, TrailingNewlineMakesEmptyLastLine)
{
    Text t = Text::fromString(u"x\n");
    ASSERT_EQ(t.lineCount(), 2);
    EXPECT_EQ(t.line(1), "");
    EXPECT_TRUE(t.hasFinalNewline() == false); // last line (empty) has no terminator after it
    EXPECT_EQ(t.lineEol(0), Eol::LF);
}

TEST(Text, ReplaceKeepsOtherEols)
{
    Text t = Text::fromString(u"one\r\ntwo\r\nthree");
    Text u = t.replace(t.lineStart(1) + 1, t.lineStart(1) + 2, u"W\nX", Eol::CRLF);
    EXPECT_EQ(u.toString(), "one\ntW\nXo\nthree");
    ASSERT_EQ(u.lineCount(), 4);
    EXPECT_EQ(u.lineEol(0), Eol::CRLF);
    EXPECT_EQ(u.lineEol(1), Eol::CRLF); // new break uses the given EOL
    EXPECT_EQ(u.lineEol(2), Eol::CRLF); // keeps the terminator that followed the replaced range
    EXPECT_EQ(u.lineEol(3), Eol::None);
    // original untouched (immutable)
    EXPECT_EQ(t.toString(), "one\ntwo\nthree");
}

TEST(Text, ReplaceJoinsLines)
{
    Text t = Text::fromString(u"ab\ncd\nef");
    Text u = t.replace(1, 5, u"");
    EXPECT_EQ(u.toString(), "a\nef");
    EXPECT_EQ(u.lineCount(), 2);
}

TEST(Text, ManyChunksRandomEditsMatchQString)
{
    QRandomGenerator rng(42);
    QString model;
    for (int i = 0; i < 3000; ++i)
        model += QStringLiteral("line %1 text\n").arg(i);
    Text t = Text::fromString(model);
    ASSERT_EQ(t.toString(), model);
    for (int step = 0; step < 2000; ++step) {
        const qsizetype len = model.size();
        const qsizetype from = rng.bounded(int(len + 1));
        const qsizetype to = std::min<qsizetype>(len, from + rng.bounded(40));
        QString ins;
        const int kind = rng.bounded(4);
        if (kind == 0)
            ins = QStringLiteral("x");
        else if (kind == 1)
            ins = QStringLiteral("\n");
        else if (kind == 2)
            ins = QStringLiteral("a\nb\nc");
        if (rng.bounded(50) == 0) { // big insert
            for (int k = 0; k < 700; ++k)
                ins += QStringLiteral("big %1\n").arg(k);
        }
        model.replace(from, to - from, ins);
        t = t.replace(from, to, ins);
        ASSERT_EQ(t.length(), model.size()) << "step " << step;
        if (step % 97 == 0) {
            ASSERT_EQ(t.toString(), model) << "step " << step;
        }
        // random probes
        for (int p = 0; p < 5; ++p) {
            const qsizetype pos = rng.bounded(int(model.size() + 1));
            const int line = t.lineAt(pos);
            ASSERT_LE(t.lineStart(line), pos);
            ASSERT_GE(t.lineEnd(line), pos);
        }
    }
    EXPECT_EQ(t.toString(), model);
    EXPECT_EQ(t.lineCount(), model.count(QLatin1Char('\n')) + 1);
}

TEST(Codec, RoundTripsArbitraryBytes)
{
    QRandomGenerator rng(1234);
    for (int iter = 0; iter < 500; ++iter) {
        QByteArray bytes;
        const int n = rng.bounded(200);
        for (int i = 0; i < n; ++i) {
            const int kind = rng.bounded(10);
            if (kind < 5)
                bytes += char('a' + rng.bounded(26));
            else if (kind == 5)
                bytes += '\n';
            else if (kind == 6)
                bytes += "\r\n";
            else if (kind == 7)
                bytes += '\r';
            else if (kind == 8)
                bytes += "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x91\x8D"; // é € 👍
            else
                bytes += char(rng.bounded(256)); // anything, incl. invalid UTF-8
        }
        if (rng.bounded(4) == 0)
            bytes.prepend("\xEF\xBB\xBF");
        const DecodedFile d = decodeFile(bytes);
        EXPECT_EQ(encodeFile(d.text, d.bom), bytes) << "iter " << iter;
    }
}

TEST(Codec, FlagsAndFinalNewline)
{
    {
        const QByteArray b("no final newline\n\tindented  \nlast");
        const DecodedFile d = decodeFile(b);
        EXPECT_FALSE(d.bom);
        EXPECT_FALSE(d.hadInvalidUtf8);
        EXPECT_EQ(d.text.lineCount(), 3);
        EXPECT_EQ(d.text.line(1), "\tindented  ");
        EXPECT_EQ(encodeFile(d.text, d.bom), b);
    }
    {
        const QByteArray b("\xEF\xBB\xBFhi\xFF\xFEthere\x00x", 13);
        const DecodedFile d = decodeFile(b);
        EXPECT_TRUE(d.bom);
        EXPECT_TRUE(d.hadInvalidUtf8);
        EXPECT_TRUE(d.hasNul);
        EXPECT_EQ(encodeFile(d.text, d.bom), b);
    }
}

TEST(Codec, EditedLineKeepsNeighbouringBytes)
{
    const QByteArray b("a\r\nb\nc\r\nd");
    DecodedFile d = decodeFile(b);
    Text t = d.text.replace(d.text.lineStart(1), d.text.lineEnd(1), u"B!");
    EXPECT_EQ(encodeFile(t, false), QByteArray("a\r\nB!\nc\r\nd"));
}

TEST(Codec, HashIsStable)
{
    EXPECT_EQ(contentHash("abc"), contentHash(QByteArray("abc")));
    EXPECT_NE(contentHash("abc"), contentHash("abd"));
    EXPECT_EQ(contentHash("").size(), 32);
}
