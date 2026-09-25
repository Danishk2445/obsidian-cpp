#pragma once
// Media fixtures generated at test time: multi-page PDFs (QPdfWriter), a synthesized WAV, PNG
// images and (when ffmpeg is installed) tiny MP4 videos. Plus an app fixture with the media views
// registered, and screenshot helpers (OC_SHOT_DIR).
#include "app_test_support.h"

#include "editor/embeds/hosted_embeds.h"
#include "render/theme/theme.h"
#include "ui/views/media/media_views.h"

#include "core/util/busy.h"

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QHash>
#include <QTemporaryDir>
#include <QDataStream>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>

#include <cmath>

namespace oc::test {

inline ThemeManager* mediaThemes()
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = [] {
            auto* m = new ThemeManager(qApp);
            FontSettings fonts;
            fonts.interfaceFamily = Theme::firstInstalled({QStringLiteral("Inter")}, QString());
            fonts.textFamily = fonts.interfaceFamily;
            m->setFonts(fonts);
            m->setMode(ThemeManager::Mode::Dark);
            m->apply();
            return m;
        }();
        return tm;
    }
    return ThemeManager::instance();
}

inline void setDarkTheme(bool dark)
{
    ThemeManager* tm = mediaThemes();
    tm->setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
    tm->apply();
    QApplication::processEvents();
}

// A PDF of `pages` pages (A4 portrait, or landscape) with "Page N" headings, "Needle N" lines and
// "needle" / "needlework" on page 2 (for case / whole-word search).
inline QByteArray makePdf(int pages, bool landscape = false)
{
    QByteArray bytes;
    {
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        QPdfWriter w(&buf);
        w.setPageSize(QPageSize(QPageSize::A4));
        w.setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
        w.setResolution(72);
        w.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter p(&w);
        for (int i = 0; i < pages; ++i) {
            if (i)
                w.newPage();
            QFont big(QStringLiteral("DejaVu Sans"));
            big.setPixelSize(28);
            p.setFont(big);
            p.drawText(QPointF(60, 90), QStringLiteral("Page %1 heading").arg(i + 1));
            QFont f(QStringLiteral("DejaVu Sans"));
            f.setPixelSize(14);
            p.setFont(f);
            p.drawText(QPointF(60, 140), QStringLiteral("Needle %1 in the haystack").arg(i + 1));
            if (i == 1) {
                p.drawText(QPointF(60, 170), QStringLiteral("a needle and needlework"));
            }
            for (int l = 0; l < 20; ++l)
                p.drawText(QPointF(60, 210 + l * 22), QStringLiteral("Lorem ipsum dolor sit amet, line %1 of page %2.").arg(l + 1).arg(i + 1));
            p.fillRect(QRectF(60, 700, 200, 60), QColor(0x20, 0x70, 0xd0));
        }
    }
    return bytes;
}

// 16-bit mono PCM sine.
inline QByteArray makeWav(double seconds, int rate = 22050)
{
    const int n = int(seconds * rate);
    QByteArray pcm(n * 2, Qt::Uninitialized);
    auto* s = reinterpret_cast<qint16*>(pcm.data());
    for (int i = 0; i < n; ++i)
        s[i] = qint16(6000 * std::sin(2 * M_PI * 440 * i / rate));
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData("RIFF", 4);
    ds << quint32(36 + pcm.size());
    ds.writeRawData("WAVE", 4);
    ds.writeRawData("fmt ", 4);
    ds << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
    ds.writeRawData("data", 4);
    ds << quint32(pcm.size());
    ds.writeRawData(pcm.constData(), pcm.size());
    return out;
}

inline QByteArray makePng(QSize size, QColor color)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    QPainter p(&img);
    p.setPen(QPen(Qt::white, 4));
    p.drawEllipse(QRect(QPoint(0, 0), size).adjusted(10, 10, -10, -10));
    p.end();
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

inline QString ffmpegPath() { return QStandardPaths::findExecutable(QStringLiteral("ffmpeg")); }

// A 1.5 s 320×180 test pattern with a sine track (or audio only). Empty when ffmpeg is missing.
inline bool makeVideo(const QString& path, bool audioOnly = false, QSize size = QSize(320, 180))
{
    const QString ff = ffmpegPath();
    if (ff.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QStringList args{QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")};
    if (!audioOnly)
        args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i")
             << QStringLiteral("testsrc=size=%1x%2:rate=15").arg(size.width()).arg(size.height());
    args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i") << QStringLiteral("sine=frequency=440") << QStringLiteral("-t")
         << QStringLiteral("1.5");
    if (!audioOnly)
        args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-shortest") << path;
    QProcess proc;
    proc.start(ff, args);
    return proc.waitForFinished(30000) && proc.exitCode() == 0 && QFileInfo(path).size() > 0;
}

// Generated once per process (ffmpeg is slow to start): the bytes of v.mp4 / an audio-only mp4.
inline QByteArray videoBytes(bool audioOnly = false)
{
    static QTemporaryDir dir;
    static QHash<bool, QByteArray> cache;
    if (!cache.contains(audioOnly)) {
        const QString path = dir.path() + (audioOnly ? "/voice.mp4" : "/v.mp4");
        cache.insert(audioOnly, dir.isValid() && makeVideo(path, audioOnly) ? readFile(path) : QByteArray());
    }
    return cache.value(audioOnly);
}

// The app fixture with the media views registered.
class MediaAppFixture : public AppFixture {
protected:
    void SetUp() override
    {
        mediaThemes();
        AppFixture::SetUp();
        registerMediaViews(*registry);
    }
    static bool hasVideo() { return !videoBytes().isEmpty(); }
    QString shotDir() const { return QString::fromLocal8Bit(qgetenv("OC_SHOT_DIR")); }
    bool shot(const QString& name)
    {
        if (shotDir().isEmpty())
            return false;
        QCursor::setPos(-10000, -10000);
        QApplication::processEvents();
        window->repaint();
        return window->grab().save(shotDir() + "/" + name);
    }
    // Wait until background work (PDF renders, media probes, loading media) settled.
    static void settle(int ms = 4000)
    {
        QElapsedTimer t;
        t.start();
        int quiet = 0;
        while (t.elapsed() < ms && quiet < 5) {
            QTest::qWait(20);
            quiet = Busy::idle() ? quiet + 1 : 0;
        }
    }
};

} // namespace oc::test
