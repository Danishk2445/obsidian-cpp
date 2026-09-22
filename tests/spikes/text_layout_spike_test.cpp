// M0 spikes: validate the QTextLayout techniques the editor engine depends on.
//  1. Inline-widget placeholder: one character whose advance is forced to an exact width via
//     absolute letter spacing (NBSP variant and U+FFFC/NoFontMerging variant).
//  2. Very long lines (the user's vault has 14k-char lines): layout / cursorToX / xToCursor timing.
//  3. QTextLayout on worker threads (for background height measurement) gives identical results.
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTextLayout>
#include <QThread>
#include <QtConcurrent>
#include <gtest/gtest.h>

namespace {

QFont testFont(int px = 16)
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(px);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

QTextOption designOption()
{
    QTextOption o;
    o.setUseDesignMetrics(true);
    o.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    return o;
}

double layoutSingleLine(QTextLayout& layout, double width = 100000)
{
    layout.setTextOption(designOption());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(width);
    line.setPosition(QPointF(0, 0));
    layout.endLayout();
    return line.naturalTextWidth();
}

double advanceOfPlaceholder(const QString& text, int pos, const QFont& font, const QList<QTextLayout::FormatRange>& formats = {})
{
    QTextLayout l(text, font);
    l.setFormats(formats);
    layoutSingleLine(l);
    const QTextLine line = l.lineAt(0);
    return line.cursorToX(pos + 1) - line.cursorToX(pos);
}

QString lorem(int chars, quint32 seed = 7)
{
    static const QStringList words = {QStringLiteral("lorem"), QStringLiteral("ipsum"), QStringLiteral("dolor"), QStringLiteral("sit"),
                                      QStringLiteral("amet"), QStringLiteral("consectetur"), QStringLiteral("adipiscing"), QStringLiteral("elit"),
                                      QStringLiteral("sed"), QStringLiteral("do"), QStringLiteral("eiusmod"), QStringLiteral("tempor"),
                                      QStringLiteral("incididunt"), QStringLiteral("ut"), QStringLiteral("labore"), QStringLiteral("magna")};
    QRandomGenerator rng(seed);
    QString s;
    s.reserve(chars + 16);
    while (s.size() < chars) {
        s += words[rng.bounded(words.size())];
        s += QLatin1Char(' ');
    }
    s.truncate(chars);
    return s;
}

} // namespace

class PlaceholderSpike : public ::testing::TestWithParam<QChar> {};

TEST_P(PlaceholderSpike, LetterSpacingReservesExactWidth)
{
    const QChar ph = GetParam();
    const QFont font = testFont();
    const QString text = QStringLiteral("ab") + ph + QStringLiteral("cd");
    QList<QTextLayout::FormatRange> none;
    QTextCharFormat baseFmt;
    if (ph == QChar(0xFFFC)) {
        QFont nf = font;
        nf.setStyleStrategy(QFont::NoFontMerging);
        baseFmt.setFont(nf);
        none = {{2, 1, baseFmt}};
    }
    const double natural = advanceOfPlaceholder(text, 2, font, none);
    for (double want : {1.0, 10.0, 16.0, 23.5, 100.0, 333.25}) {
        QTextCharFormat fmt = baseFmt;
        fmt.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        fmt.setFontLetterSpacing(want - natural);
        const double got = advanceOfPlaceholder(text, 2, font, {{2, 1, fmt}});
        EXPECT_NEAR(got, want, 0.05) << "placeholder U+" << QString::number(ph.unicode(), 16).toStdString() << " natural=" << natural;
        // Neighbours keep their natural advances.
        QTextLayout l(text, font);
        l.setFormats({{2, 1, fmt}});
        layoutSingleLine(l);
        const QTextLine line = l.lineAt(0);
        QTextLayout plain(text, font);
        plain.setFormats(none);
        layoutSingleLine(plain);
        EXPECT_NEAR(line.cursorToX(2), plain.lineAt(0).cursorToX(2), 0.01);
        EXPECT_NEAR(line.cursorToX(5) - line.cursorToX(3), plain.lineAt(0).cursorToX(5) - plain.lineAt(0).cursorToX(3), 0.05);
    }
}

INSTANTIATE_TEST_SUITE_P(Chars, PlaceholderSpike, ::testing::Values(QChar(0x00A0), QChar(0xFFFC)));

TEST(PlaceholderSpike, WidthStableAcrossPaintDeviceDpr)
{
    const QFont font = testFont();
    const QString text = QStringLiteral("ab cd");
    const double natural = advanceOfPlaceholder(text, 2, font);
    QTextCharFormat fmt;
    fmt.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    fmt.setFontLetterSpacing(40 - natural);
    for (qreal dpr : {1.0, 1.25, 2.0}) {
        QImage img(400, 100, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        QTextLayout l(text, font, &img);
        l.setFormats({{2, 1, fmt}});
        layoutSingleLine(l);
        const QTextLine line = l.lineAt(0);
        EXPECT_NEAR(line.cursorToX(3) - line.cursorToX(2), 40.0, 0.1) << "dpr " << dpr;
        QPainter p(&img);
        l.draw(&p, QPointF(0, 0)); // must not crash / assert
    }
}

TEST(LongLineSpike, Layout14kCharsIsFastEnough)
{
    const QFont font = testFont();
    const QString text = lorem(14266);
    QElapsedTimer t;
    t.start();
    QTextLayout layout(text, font);
    layout.setTextOption(designOption());
    layout.setCacheEnabled(true);
    layout.beginLayout();
    double y = 0;
    int lines = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(700);
        line.setPosition(QPointF(0, y));
        y += 24;
        ++lines;
    }
    layout.endLayout();
    const qint64 layoutUs = t.nsecsElapsed() / 1000;

    t.restart();
    double acc = 0;
    for (int i = 0; i < 2000; ++i) {
        const int pos = int(QRandomGenerator::global()->bounded(text.size()));
        const QTextLine line = layout.lineForTextPosition(pos);
        acc += line.cursorToX(pos);
    }
    const qint64 cursorUs = t.nsecsElapsed() / 1000;

    t.restart();
    for (int i = 0; i < 2000; ++i) {
        const QTextLine line = layout.lineAt(i % lines);
        acc += line.xToCursor(i % 700);
    }
    const qint64 hitUs = t.nsecsElapsed() / 1000;

    QImage img(720, 800, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    t.restart();
    // Draw only a viewport's worth of lines, as the editor will.
    for (int i = 0; i < 30 && i < lines; ++i)
        layout.lineAt(i).draw(&p, QPointF(0, 0));
    const qint64 drawUs = t.nsecsElapsed() / 1000;
    p.end();

    std::printf("[spike] 14k-char line: %d visual lines, layout %lld us, 2000x cursorToX %lld us, 2000x xToCursor %lld us, draw 30 lines %lld us (acc %g)\n",
                lines, static_cast<long long>(layoutUs), static_cast<long long>(cursorUs), static_cast<long long>(hitUs),
                static_cast<long long>(drawUs), acc);
    EXPECT_GT(lines, 100);
#ifndef OC_DEBUG
    EXPECT_LT(layoutUs, 20000);
#else
    EXPECT_LT(layoutUs, 400000);
#endif
}

TEST(WorkerThreadSpike, LayoutOnWorkersMatchesMainThread)
{
    const QFont font = testFont();
    QStringList paragraphs;
    for (int i = 0; i < 200; ++i)
        paragraphs << lorem(80 + (i * 37) % 900, quint32(i + 1));
    auto measure = [&](const QString& s) {
        QTextLayout l(s, font);
        l.setTextOption(designOption());
        l.beginLayout();
        int n = 0;
        double w = 0;
        for (;;) {
            QTextLine line = l.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(640);
            ++n;
            w += line.naturalTextWidth();
        }
        l.endLayout();
        return std::pair<int, double>(n, w);
    };
    QList<std::pair<int, double>> mainResults;
    for (const QString& s : paragraphs)
        mainResults << measure(s);
    const QList<std::pair<int, double>> threaded = QtConcurrent::blockingMapped(paragraphs, measure);
    ASSERT_EQ(threaded.size(), mainResults.size());
    for (int i = 0; i < mainResults.size(); ++i) {
        EXPECT_EQ(threaded[i].first, mainResults[i].first) << i;
        EXPECT_NEAR(threaded[i].second, mainResults[i].second, 0.01) << i;
    }
}

TEST(FontSpike, FallbackCoversSurveyScripts)
{
    // Arabic, CJK, emoji+VS16, box drawing and a Nerd Font private-use glyph must all get glyphs
    // through Qt's font merging (Obsidian relies on Chromium's per-character fallback).
    const QFont font = testFont();
    const QString samples[] = {QStringLiteral("مرحبا"), QStringLiteral("漢字かな"), QStringLiteral("👍️"),
                               QStringLiteral("┌─┐│└┘"), QString(QChar(0xE0B0)), QStringLiteral(" nbsp")};
    for (const QString& s : samples) {
        QTextLayout l(s, font);
        layoutSingleLine(l);
        const QList<QGlyphRun> runs = l.glyphRuns();
        int missing = 0;
        for (const QGlyphRun& r : runs)
            for (quint32 g : r.glyphIndexes())
                missing += g == 0 ? 1 : 0;
        std::printf("[spike] '%s': %lld runs, missing glyphs %d, fonts:", qPrintable(s), static_cast<long long>(runs.size()), missing);
        for (const QGlyphRun& r : runs)
            std::printf(" %s", qPrintable(r.rawFont().familyName()));
        std::printf("\n");
        EXPECT_EQ(missing, 0) << s.toStdString();
    }
}
