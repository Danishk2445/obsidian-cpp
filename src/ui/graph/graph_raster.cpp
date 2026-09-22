#include "ui/graph/graph_raster.h"

#include "graph/parallel.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace oc::graph_raster {

namespace {

// x·a/255 on the four 8-bit channels of a premultiplied pixel (Qt's BYTE_MUL).
inline uint byteMul(uint x, uint a)
{
    uint t = (x & 0xff00ff) * a;
    t = (t + ((t >> 8) & 0xff00ff) + 0x800080) >> 8;
    t &= 0xff00ff;
    x = ((x >> 8) & 0xff00ff) * a;
    x = (x + ((x >> 8) & 0xff00ff) + 0x800080);
    x &= 0xff00ff00;
    return x | t;
}

// Source-over of premultiplied `src` with coverage `cov` (0..255).
inline void blend(uint* d, uint src, uint cov)
{
    if (cov == 0)
        return;
    const uint s = byteMul(src, cov);
    *d = s + byteMul(*d, 255 - qAlpha(s));
}

constexpr double kFar = 1e9; // keeps double → int conversions defined

inline int clampToInt(double v) { return int(std::clamp(v, -kFar, kFar)); }

struct Target {
    uchar* bits;
    qsizetype bpl;
    int width;
    uint* row(int y) const { return reinterpret_cast<uint*>(bits + y * bpl); }
};

// Xiaolin Wu's line restricted to rows [by0, by1): at every column (or row, for steep lines) the
// two pixels around the exact line position share the coverage.
void wuLine(const Target& t, int by0, int by1, const QLineF& line, uint src)
{
    double x0 = line.x1(), y0 = line.y1(), x1 = line.x2(), y1 = line.y2();
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
        return;
    const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (!steep) {
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        const double dx = x1 - x0;
        const double g = dx > 0 ? (y1 - y0) / dx : 0;
        int cx0 = std::max(0, clampToInt(std::ceil(x0 - 0.5)));
        int cx1 = std::min(t.width - 1, clampToInt(std::floor(x1 - 0.5)));
        // Columns whose pixel pair (iy, iy + 1) can touch the band: yc ∈ [by0 − 1, by1).
        if (g != 0) {
            const double a = x0 - 0.5 + (by0 - 0.5 - y0) / g;
            const double b = x0 - 0.5 + (by1 + 0.5 - y0) / g;
            cx0 = std::max(cx0, clampToInt(std::floor(std::min(a, b))));
            cx1 = std::min(cx1, clampToInt(std::ceil(std::max(a, b))));
        } else if (y0 - 0.5 < by0 - 1 || y0 - 0.5 >= by1) {
            return;
        }
        if (cx0 > cx1)
            return;
        // 16.16 fixed point along the minor axis.
        qint64 yf = qint64(std::llround((y0 + g * (cx0 + 0.5 - x0) - 0.5) * 65536.0));
        const qint64 step = qint64(std::llround(g * 65536.0));
        for (int cx = cx0; cx <= cx1; ++cx, yf += step) {
            const int iy = int(yf >> 16);
            const uint f = uint((yf >> 8) & 0xff);
            if (iy >= by0 && iy < by1)
                blend(t.row(iy) + cx, src, 255 - f);
            if (iy + 1 >= by0 && iy + 1 < by1)
                blend(t.row(iy + 1) + cx, src, f);
        }
    } else {
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        const double g = (x1 - x0) / (y1 - y0);
        const int ry0 = std::max(by0, clampToInt(std::ceil(y0 - 0.5)));
        const int ry1 = std::min(by1 - 1, clampToInt(std::floor(y1 - 0.5)));
        if (ry0 > ry1)
            return;
        qint64 xf = qint64(std::llround(std::clamp(x0 + g * (ry0 + 0.5 - y0) - 0.5, -kFar, kFar) * 65536.0));
        const qint64 step = qint64(std::llround(g * 65536.0));
        for (int ry = ry0; ry <= ry1; ++ry, xf += step) {
            const qint64 ix = xf >> 16;
            if (ix < -1 || ix >= t.width)
                continue;
            const uint f = uint((xf >> 8) & 0xff);
            uint* row = t.row(ry);
            if (ix >= 0)
                blend(row + ix, src, 255 - f);
            if (ix + 1 < t.width)
                blend(row + ix + 1, src, f);
        }
    }
}

template <typename Fn>
void forBands(int height, Fn&& fn)
{
    const int bands = bandCount(height);
    if (bands <= 1) {
        fn(0, height);
        return;
    }
    graph::parallelFor(bands, [&](int b) { fn(height * b / bands, height * (b + 1) / bands); });
}

} // namespace

bool supportsDirect(const QImage& image)
{
    return image.format() == QImage::Format_ARGB32_Premultiplied || image.format() == QImage::Format_RGB32;
}

int bandCount(int height)
{
    const int threads = graph::parallelWorkers();
    return std::clamp(height / 32, 1, std::min(threads, 32));
}

void drawHairlines(QImage& image, const std::vector<LineBatch>& batches)
{
    if (!supportsDirect(image) || image.isNull())
        return;
    const Target t{image.bits(), image.bytesPerLine(), image.width()};
    std::vector<uint> colors;
    colors.reserve(batches.size());
    for (const LineBatch& b : batches)
        colors.push_back(qPremultiply(qRgba(qRed(b.rgb), qGreen(b.rgb), qBlue(b.rgb), std::clamp(b.alpha, 0, 255))));
    forBands(image.height(), [&](int by0, int by1) {
        for (size_t k = 0; k < batches.size(); ++k) {
            for (const QLineF& l : batches[k].lines) {
                const double top = std::min(l.y1(), l.y2()), bottom = std::max(l.y1(), l.y2());
                if (bottom + 1 < by0 || top - 1 >= by1)
                    continue;
                wuLine(t, by0, by1, l, colors[k]);
            }
        }
    });
}

void paintInBands(QImage& image, const std::function<void(QPainter& p, int y0, int y1)>& paint)
{
    if (image.isNull())
        return;
    uchar* bits = image.bits();
    const qsizetype bpl = image.bytesPerLine();
    const int width = image.width();
    const QImage::Format format = image.format();
    forBands(image.height(), [&](int y0, int y1) {
        QImage band(bits + y0 * bpl, width, y1 - y0, bpl, format); // shares the rows
        QPainter p(&band);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(0, -y0);
        paint(p, y0, y1);
    });
}

} // namespace oc::graph_raster
