// Raw HTML rendering cost in Live Preview and Reading view: a note full of inline HTML (kbd / sub / span
// colours / <br>), a 2000-line paragraph of <br>-separated lines (tags pair across the whole leaf) and HTML
// blocks. Budgets are asserted in optimized builds with OC_PERF_ASSERT=1.
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

using namespace oc;

namespace {

QByteArray htmlDoc()
{
    QString out = QStringLiteral("# HTML heavy\n\n");
    for (int i = 0; i < 3000; ++i) {
        out += QStringLiteral("Press <kbd>Ctrl</kbd> + <kbd>%1</kbd>, H<sub>2</sub>O, x<sup>%1</sup>, <span style=\"color: #e93147\">red %1</span>, "
                              "<mark>mark</mark> and <a href=\"https://example.com/%1\">a link</a><br>second line <b>bold</b>.\n\n")
                   .arg(i);
        if (i % 50 == 0)
            out += QStringLiteral("<p align=\"center\">Centered <b>block</b> %1<br>line two</p>\n\n<table>\n<tr><th>K</th><th>V</th></tr>\n"
                                  "<tr><td><kbd>A</kbd></td><td>%1</td></tr>\n</table>\n\n")
                       .arg(i);
    }
    // One huge paragraph: every line of it carries an element, and all of them pair within one leaf.
    const bool plain = qEnvironmentVariableIntValue("OC_HTML_PERF_PLAIN") == 1; // baseline: the same without HTML
    for (int i = 0; i < 2000; ++i)
        out += (plain ? QStringLiteral("verse %1 with *some* `keys`  \n") : QStringLiteral("verse %1 with <i>some</i> <kbd>keys</kbd><br>\n")).arg(i);
    out += QStringLiteral("\nend\n");
    return out.toUtf8();
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
    s.p95 = v[std::min(v.size() - 1, size_t(v.size() * 0.95))];
    s.max = v.back();
    return s;
}
bool assertPerf() { return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1; }

} // namespace

TEST(HtmlPerf, LivePreviewAndReading)
{
    ThemeManager themes;
    themes.setMode(ThemeManager::Mode::Dark);
    themes.apply();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray bytes = htmlDoc();
    {
        QFile f(dir.path() + "/html.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    LocalFileStorage storage(dir.path());
    auto doc = std::make_shared<Document>(&storage, QStringLiteral("html.md"));
    ASSERT_TRUE(doc->load());

    EditorView view;
    view.resize(1200, 900);
    view.setMode(EditorView::Mode::LivePreview);
    QElapsedTimer t;
    t.start();
    view.setDocument(doc);
    view.show();
    QImage img(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    view.viewport()->render(&img);
    const double firstPaintMs = t.nsecsElapsed() / 1e6;
    std::printf("[perf] html: %lld bytes, %d lines; setDocument+first paint (live) %.1f ms\n", static_cast<long long>(bytes.size()),
                doc->text().lineCount(), firstPaintMs);

    auto scrollFrames = [&](int fromLine) {
        view.setScrollTop(view.layout().lineTop(fromLine));
        std::vector<double> frames;
        for (int i = 0; i < 100; ++i) {
            t.restart();
            view.setScrollTop(view.scrollTop() + 450);
            view.viewport()->render(&img);
            frames.push_back(t.nsecsElapsed() / 1e6);
        }
        return stats(frames);
    };
    const Stats live = scrollFrames(doc->text().lineCount() / 4);
    std::printf("[perf] html scroll half-page frame (live): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", live.p50, live.p95, live.max);

    // Moving the cursor through the huge paragraph reveals / hides elements line by line.
    const int bigPara = doc->text().lineCount() - 1000;
    view.setCursorPosition(doc->text().lineStart(bigPara));
    view.scrollToPos(view.cursorPosition(), true);
    view.setFocus();
    QApplication::processEvents();
    std::vector<double> moves;
    for (int i = 0; i < 100; ++i) {
        t.restart();
        QTest::keyClick(&view, Qt::Key_Down);
        view.viewport()->render(&img);
        moves.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats mv = stats(moves);
    std::printf("[perf] html cursor down in a 2000-line paragraph (live): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", mv.p50, mv.p95, mv.max);

    std::vector<double> keys;
    for (int i = 0; i < 100; ++i) {
        t.restart();
        QTest::keyClick(&view, Qt::Key(Qt::Key_A + (i % 26)));
        view.viewport()->render(&img);
        keys.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats ks = stats(keys);
    std::printf("[perf] html keystroke in a 2000-line paragraph (live): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", ks.p50, ks.p95, ks.max);

    t.restart();
    view.setMode(EditorView::Mode::Reading);
    view.viewport()->render(&img);
    const double readingMs = t.nsecsElapsed() / 1e6;
    const Stats reading = scrollFrames(doc->text().lineCount() / 2);
    std::printf("[perf] html switch to reading + paint %.1f ms; scroll frame (reading): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", readingMs,
                reading.p50, reading.p95, reading.max);

    if (assertPerf()) {
        EXPECT_LT(firstPaintMs, 400.0);
        EXPECT_LT(live.p95, 16.0);
        EXPECT_LT(reading.p95, 16.0);
        EXPECT_LT(mv.p95, 16.0);
        // Typing re-parses the whole 80 KB paragraph; the same paragraph without HTML (OC_HTML_PERF_PLAIN=1) costs
        // as much (~20-30 ms), so this only guards the HTML part.
        EXPECT_LT(ks.p95, 40.0);
    }
}
