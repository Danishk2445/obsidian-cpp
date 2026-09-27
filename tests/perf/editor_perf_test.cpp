// Perf gates for the editor on survey-shaped giant documents (label "perf"; assertions only in
// optimized builds with OC_PERF_ASSERT=1 so debug/ASan runs just report numbers).
#include "core/documents/document.h"
#include "core/documents/local_storage.h"
#include "editor/editor_view.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <algorithm>

using namespace oc;

namespace {
const char* kWords[] = {"the", "of", "and", "kernel", "config", "mount", "partition", "package", "driver", "network", "server",
                        "client", "module", "service", "script", "write", "people", "number", "water", "first", "long", "time"};

QByteArray sentence(QRandomGenerator& r, int n)
{
    QByteArray s;
    for (int i = 0; i < n; ++i) {
        s += kWords[r.bounded(int(std::size(kWords)))];
        s += ' ';
    }
    s[0] = char(std::toupper(s[0]));
    s.chop(1);
    return s + '.';
}

// ~5.5 MB, ~64k lines: headings, tab-indented lists, paragraphs, a few 7-14k char lines.
QByteArray giantDoc()
{
    QRandomGenerator r(42);
    QByteArray out;
    out.reserve(5'600'000);
    while (out.size() < 5'500'000) {
        const int k = r.bounded(1000);
        if (k < 20) {
            out += "# " + sentence(r, 4) + "\n\n";
        } else if (k < 300) {
            int depth = 0;
            for (int i = 0, n = 3 + r.bounded(15); i < n; ++i) {
                depth = std::clamp(depth + r.bounded(3) - 1, 0, 3);
                out += QByteArray(depth, '\t') + "- " + sentence(r, 3 + r.bounded(10)) + "\n";
            }
            out += "\n";
        } else if (k < 305) {
            QByteArray para;
            const int target = 7000 + r.bounded(7266);
            while (para.size() < target)
                para += sentence(r, 12) + ' ';
            out += para.left(target) + "\n\n";
        } else {
            out += sentence(r, 5 + r.bounded(40)) + "\n";
        }
    }
    return out;
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

TEST(EditorPerf, GiantDocument)
{
    ThemeManager themes;
    themes.setMode(ThemeManager::Mode::Dark);
    themes.apply();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray bytes = giantDoc();
    {
        QFile f(dir.path() + "/giant.md");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    LocalFileStorage storage(dir.path());

    QElapsedTimer t;
    t.start();
    auto doc = std::make_shared<Document>(&storage, QStringLiteral("giant.md"));
    ASSERT_TRUE(doc->load());
    const double loadMs = t.nsecsElapsed() / 1e6;

    EditorView view;
    view.resize(1200, 900);
    t.restart();
    view.setDocument(doc);
    view.show();
    QImage img(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    view.viewport()->render(&img);
    const double firstPaintMs = t.nsecsElapsed() / 1e6;
    std::printf("[perf] giant: %lld bytes, %d lines; load+parse %.1f ms; setDocument+first paint %.1f ms\n",
                static_cast<long long>(bytes.size()), doc->text().lineCount(), loadMs, firstPaintMs);

    // Keystrokes in a normal line in the middle of the document.
    const int mid = doc->text().lineCount() / 2;
    int line = mid;
    while (doc->text().lineLength(line) < 20 || doc->text().lineLength(line) > 400)
        ++line;
    view.setCursorPosition(doc->text().lineStart(line) + 10);
    view.scrollToPos(view.cursorPosition(), true);
    view.setFocus();
    QApplication::processEvents();
    std::vector<double> keys;
    for (int i = 0; i < 200; ++i) {
        t.restart();
        QTest::keyClick(&view, Qt::Key(Qt::Key_A + (i % 26)));
        view.viewport()->render(&img);
        keys.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats ks = stats(keys);
    std::printf("[perf] keystroke (normal line): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", ks.p50, ks.p95, ks.max);

    // Keystrokes inside a 14k-char line.
    int longLine = -1;
    for (int l = 0; l < doc->text().lineCount(); ++l)
        if (doc->text().lineLength(l) > 7000) {
            longLine = l;
            break;
        }
    ASSERT_GE(longLine, 0);
    view.setCursorPosition(doc->text().lineStart(longLine) + 3000);
    view.scrollToPos(view.cursorPosition(), true);
    QApplication::processEvents();
    std::vector<double> longKeys;
    for (int i = 0; i < 100; ++i) {
        t.restart();
        QTest::keyClick(&view, Qt::Key(Qt::Key_A + (i % 26)));
        view.viewport()->render(&img);
        longKeys.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats ls = stats(longKeys);
    std::printf("[perf] keystroke (%d-char line): p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", doc->text().lineLength(longLine), ls.p50, ls.p95, ls.max);

    // Scrolling page by page through a region.
    view.setScrollTop(view.layout().lineTop(mid));
    std::vector<double> frames;
    for (int i = 0; i < 150; ++i) {
        t.restart();
        view.setScrollTop(view.scrollTop() + 450);
        view.viewport()->render(&img);
        frames.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats fs = stats(frames);
    std::printf("[perf] scroll half-page frame: p50 %.2f ms, p95 %.2f ms, max %.2f ms\n", fs.p50, fs.p95, fs.max);

    t.restart();
    QTest::keyClick(&view, Qt::Key_End, Qt::ControlModifier);
    view.viewport()->render(&img);
    const double jumpEndMs = t.nsecsElapsed() / 1e6;
    std::printf("[perf] Ctrl+End jump + paint: %.2f ms\n", jumpEndMs);

    // Reading view with strict line breaks: the whole document's paragraph runs (text units over several lines),
    // then scrolling through them.
    view.setStrictLineBreaks(true);
    t.restart();
    view.setMode(EditorView::Mode::Reading);
    view.setScrollTop(view.layout().lineTop(mid));
    view.viewport()->render(&img);
    const double strictMs = t.nsecsElapsed() / 1e6;
    std::vector<double> strictFrames;
    for (int i = 0; i < 60; ++i) {
        t.restart();
        view.setScrollTop(view.scrollTop() + 450);
        view.viewport()->render(&img);
        strictFrames.push_back(t.nsecsElapsed() / 1e6);
    }
    const Stats ss = stats(strictFrames);
    std::printf("[perf] Reading view, strict line breaks: switch + paint %.2f ms; scroll frame p50 %.2f ms, p95 %.2f ms\n", strictMs,
                ss.p50, ss.p95);

    if (assertPerf()) {
        EXPECT_LT(loadMs + firstPaintMs, 250.0 + 150.0); // first paint budget (load incl. full parse)
        EXPECT_LT(ks.p95, 8.0);
        EXPECT_LT(ls.p95, 16.0);
        EXPECT_LT(fs.p95, 16.0);
    }
}
