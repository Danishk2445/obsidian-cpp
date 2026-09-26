// Raw HTML rendering cost in Live Preview and Reading view: a note full of inline HTML (kbd / sub / span
// colours / <br>), a 2000-line paragraph of <br>-separated lines (tags pair across the whole leaf) and HTML
// blocks, measured against the same note written without HTML in the same run (so the gates hold on a busy
// machine). Budgets are asserted in optimized builds with OC_PERF_ASSERT=1.
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

// `html` false: the same structure with Markdown formatting instead of HTML.
QByteArray note(bool html)
{
    QString out = QStringLiteral("# HTML heavy\n\n");
    for (int i = 0; i < 3000; ++i) {
        out += (html ? QStringLiteral("Press <kbd>Ctrl</kbd> + <kbd>%1</kbd>, H<sub>2</sub>O, x<sup>%1</sup>, <span style=\"color: #e93147\">red %1</span>, "
                                      "<mark>mark</mark> and <a href=\"https://example.com/%1\">a link</a><br>second line <b>bold</b>.\n\n")
                     : QStringLiteral("Press `Ctrl` + `%1`, H2O, x^%1, *red %1*, ==mark== and [a link](https://example.com/%1)  \n"
                                      "second line **bold**.\n\n"))
                   .arg(i);
        if (i % 50 == 0)
            out += (html ? QStringLiteral("<p align=\"center\">Centered <b>block</b> %1<br>line two</p>\n\n<table>\n<tr><th>K</th><th>V</th></tr>\n"
                                          "<tr><td><kbd>A</kbd></td><td>%1</td></tr>\n</table>\n\n")
                         : QStringLiteral("Centered **block** %1  \nline two\n\n| K | V |\n| - | - |\n| `A` | %1 |\n\n"))
                       .arg(i);
    }
    // One huge paragraph: every line of it carries elements, and all of them pair within one leaf.
    for (int i = 0; i < 2000; ++i)
        out += (html ? QStringLiteral("verse %1 with <i>some</i> <kbd>keys</kbd><br>\n") : QStringLiteral("verse %1 with *some* `keys`  \n")).arg(i);
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

struct Measured {
    double firstPaint = 0, reading = 0;
    Stats live, move, key, readingScroll;
};

Measured measure(const QString& dirPath, bool html)
{
    Measured r;
    const QString name = html ? QStringLiteral("html.md") : QStringLiteral("plain.md");
    {
        QFile f(dirPath + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            return r;
        f.write(note(html));
    }
    LocalFileStorage storage(dirPath);
    auto doc = std::make_shared<Document>(&storage, name);
    if (!doc->load())
        return r;
    EditorView view;
    view.resize(1200, 900);
    view.setMode(EditorView::Mode::LivePreview);
    QElapsedTimer t;
    t.start();
    view.setDocument(doc);
    view.show();
    QImage img(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    view.viewport()->render(&img);
    r.firstPaint = t.nsecsElapsed() / 1e6;

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
    r.live = scrollFrames(doc->text().lineCount() / 4);

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
    r.move = stats(moves);

    std::vector<double> keys;
    for (int i = 0; i < 100; ++i) {
        t.restart();
        QTest::keyClick(&view, Qt::Key(Qt::Key_A + (i % 26)));
        view.viewport()->render(&img);
        keys.push_back(t.nsecsElapsed() / 1e6);
    }
    r.key = stats(keys);

    t.restart();
    view.setMode(EditorView::Mode::Reading);
    view.viewport()->render(&img);
    r.reading = t.nsecsElapsed() / 1e6;
    r.readingScroll = scrollFrames(doc->text().lineCount() / 2);
    return r;
}

void print(const char* label, const Measured& r)
{
    std::printf("[perf] %s: first paint (live) %.1f ms; scroll frame live p95 %.2f / reading p95 %.2f ms; cursor down in a 2000-line "
                "paragraph p50 %.2f p95 %.2f ms; keystroke there p50 %.2f p95 %.2f ms; switch to reading + paint %.1f ms\n",
                label, r.firstPaint, r.live.p95, r.readingScroll.p95, r.move.p50, r.move.p95, r.key.p50, r.key.p95, r.reading);
}

} // namespace

TEST(HtmlPerf, LivePreviewAndReading)
{
    ThemeManager themes;
    themes.setMode(ThemeManager::Mode::Dark);
    themes.apply();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Measured plain = measure(dir.path(), false);
    const Measured html = measure(dir.path(), true);
    print("markdown", plain);
    print("html", html);
    ASSERT_GT(html.firstPaint, 0);

    if (assertPerf()) {
        // HTML blocks are laid out when they come into view, not when the note opens.
        EXPECT_LT(html.firstPaint, plain.firstPaint * 2 + 20.0);
        EXPECT_LT(html.reading, plain.reading * 2 + 20.0);
        EXPECT_LT(html.live.p95, 16.0);
        EXPECT_LT(html.readingScroll.p95, 16.0);
        // Typing / moving in the huge paragraph re-decorates (and re-parses) it; HTML may cost a bit more than
        // the Markdown equivalent, never a multiple of it.
        EXPECT_LT(html.move.p50, plain.move.p50 * 1.5 + 3.0);
        EXPECT_LT(html.key.p50, plain.key.p50 * 1.5 + 5.0);
    }
}
