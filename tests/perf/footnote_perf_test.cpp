// Footnotes cost: a note with 3000 references, 2000 definitions and inline footnotes, measured against the same
// note without footnote syntax in the same run. Live Preview (typing, first paint) must not pay for the
// Reading-view numbering; Reading view builds it once (switching mode, the footnotes section) and then scrolls
// and follows references like any note. Budgets are asserted in optimized builds with OC_PERF_ASSERT=1.
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

QByteArray note(bool footnotes)
{
    QString out = QStringLiteral("# Footnotes heavy\n\n");
    for (int i = 0; i < 3000; ++i) {
        out += footnotes ? QStringLiteral("Paragraph %1 cites a source[^s%2] and adds an aside^[aside %1] before it ends.\n\n").arg(i).arg(i % 2000)
                         : QStringLiteral("Paragraph %1 cites a source (s%2) and adds an aside (aside %1) before it ends.\n\n").arg(i).arg(i % 2000);
    }
    for (int i = 0; i < 2000; ++i)
        out += footnotes ? QStringLiteral("[^s%1]: Source %1, with *emphasis* and a [[link]].\n").arg(i)
                         : QStringLiteral("- s%1: Source %1, with *emphasis* and a [[link]].\n").arg(i);
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
    double firstPaint = 0, reading = 0, follow = 0;
    Stats key, scroll;
};

Measured measure(const QString& dirPath, bool footnotes)
{
    Measured r;
    const QString name = footnotes ? QStringLiteral("fn.md") : QStringLiteral("plain.md");
    {
        QFile f(dirPath + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            return r;
        f.write(note(footnotes));
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

    // typing in the middle of the note
    const int mid = doc->text().lineCount() / 3;
    view.setCursorPosition(doc->text().lineStart(mid) + 5);
    view.scrollToPos(view.cursorPosition(), true);
    view.setFocus();
    QApplication::processEvents();
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

    view.setScrollTop(view.layout().lineTop(doc->text().lineCount() / 2));
    std::vector<double> frames;
    for (int i = 0; i < 100; ++i) {
        t.restart();
        view.setScrollTop(view.scrollTop() + 450);
        view.viewport()->render(&img);
        frames.push_back(t.nsecsElapsed() / 1e6);
    }
    r.scroll = stats(frames);

    if (footnotes) { // follow a reference down to its footnote and back
        t.restart();
        view.scrollToAnchor(QStringLiteral("fn-1500"));
        view.viewport()->render(&img);
        view.scrollToAnchor(QStringLiteral("fnref-1500"));
        view.viewport()->render(&img);
        r.follow = t.nsecsElapsed() / 1e6;
    }
    return r;
}

void print(const char* label, const Measured& r)
{
    std::printf("[perf] %s: first paint (live) %.1f ms; keystroke p50 %.2f p95 %.2f ms; switch to reading + paint %.1f ms; "
                "scroll frame (reading) p50 %.2f p95 %.2f ms; follow a footnote and back %.1f ms\n",
                label, r.firstPaint, r.key.p50, r.key.p95, r.reading, r.scroll.p50, r.scroll.p95, r.follow);
}

} // namespace

TEST(FootnotePerf, LivePreviewAndReading)
{
    ThemeManager themes;
    themes.setMode(ThemeManager::Mode::Dark);
    themes.apply();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Measured plain = measure(dir.path(), false);
    const Measured fn = measure(dir.path(), true);
    print("markdown", plain);
    print("footnotes", fn);
    ASSERT_GT(fn.firstPaint, 0);

    if (assertPerf()) {
        // the editor doesn't number footnotes
        EXPECT_LT(fn.firstPaint, plain.firstPaint * 2 + 20.0);
        EXPECT_LT(fn.key.p50, plain.key.p50 * 1.5 + 1.0);
        EXPECT_LT(fn.key.p95, 16.0);
        // Reading view: numbering + the 3000-item footnotes section once, then ordinary frames
        EXPECT_LT(fn.reading, 400.0);
        EXPECT_LT(fn.scroll.p95, 16.0);
        EXPECT_LT(fn.follow, 100.0);
    }
}
