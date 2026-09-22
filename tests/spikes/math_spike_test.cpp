// M0 spike: JKQTMathText builds against Qt 6.11 / GCC 16 and renders typical formulas quickly.
#include <jkqtmathtext/jkqtmathtext.h>

#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <gtest/gtest.h>

TEST(MathSpike, RendersFormulas)
{
    JKQTMathText mt;
    mt.useXITS();
    mt.setFontSizePixels(16);
    mt.setFontColor(QColor(0xda, 0xda, 0xda));
    const QString formulas[] = {
        QStringLiteral("$\\frac{a}{b} + \\sqrt{x^2+y^2}$"),
        QStringLiteral("$\\sum_{i=1}^{n} i^2 = \\frac{n(n+1)(2n+1)}{6}$"),
        QStringLiteral("$\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}$"),
        QStringLiteral("$\\mathbb{R}^n \\to \\mathcal{L}(V, W)$"),
        QStringLiteral("$\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}$"),
        QStringLiteral("$HOME and $"), // what Obsidian renders for an accidental shell-variable "math" span
    };
    QImage img(600, 200, QImage::Format_ARGB32_Premultiplied);
    for (const QString& f : formulas) {
        img.fill(Qt::transparent);
        QElapsedTimer t;
        t.start();
        const bool ok = mt.parse(f);
        QPainter p(&img);
        const QSizeF size = mt.getSize(p);
        mt.draw(p, QPointF(10, 10 + mt.getAscent(p)));
        p.end();
        const qint64 us = t.nsecsElapsed() / 1000;
        std::printf("[spike] math %-58s ok=%d size=%.1fx%.1f %lld us errors=%s\n", qPrintable(f), ok, size.width(), size.height(),
                    static_cast<long long>(us), qPrintable(mt.getErrorList().join(QStringLiteral("; "))));
        EXPECT_TRUE(ok) << f.toStdString();
        EXPECT_GT(size.width(), 5);
        EXPECT_GT(size.height(), 5);
        // something was drawn
        int painted = 0;
        for (int y = 0; y < img.height(); y += 2)
            for (int x = 0; x < img.width(); x += 2)
                painted += qAlpha(img.pixel(x, y)) > 0 ? 1 : 0;
        EXPECT_GT(painted, 10) << f.toStdString();
    }
}
