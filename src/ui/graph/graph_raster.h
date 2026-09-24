#pragma once

// Fast paths behind GraphCanvas for frames with thousands of links: antialiased hairlines
// (Xiaolin Wu) blended straight into a 32-bit QImage, and band-parallel QPainter work. Rows are
// split into horizontal bands handled by the thread pool; every pixel belongs to exactly one band
// and each band draws in submission order, so the result is identical to a sequential pass.

#include <QImage>
#include <QLineF>
#include <QRgb>

#include <functional>
#include <vector>

class QPainter;

namespace oc::graph_raster {

struct LineBatch {
    QRgb rgb = 0;   // 0xRRGGBB
    int alpha = 255; // 0..255
    std::vector<QLineF> lines; // device pixels
};

// True when `image` can be written directly (ARGB32_Premultiplied or RGB32).
bool supportsDirect(const QImage& image);

// Draws 1 px antialiased lines, batch by batch, source-over (Wu: two pixels per step whose
// coverages sum to one). Lines are clipped to the image.
void drawHairlines(QImage& image, const std::vector<LineBatch>& batches);

// Runs `paint` for each horizontal band of `image` on the thread pool with a QPainter clipped to
// the band (device coordinates stay those of the whole image). Anything thread-safe to draw with
// QPainter (paths, polygons, images; no shared QStaticText) may be drawn.
void paintInBands(QImage& image, const std::function<void(QPainter& p, int y0, int y1)>& paint);

// Band count used for `height` rows (1 when not worth splitting).
int bandCount(int height);

} // namespace oc::graph_raster
