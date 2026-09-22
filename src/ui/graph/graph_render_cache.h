#pragma once

// Caches behind GraphCanvas: pre-rasterised node circles, laid-out labels and the controls
// panel's box shadow. All sizes are device pixels.

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QStaticText>
#include <QString>

#include <vector>

class QPainter;

namespace oc {

// Antialiased filled circles, one sprite per (radius in 1/8 px, colour, sub-pixel phase), blitted
// 1:1 at integer positions. Small circles get 4×4 sub-pixel phases so motion stays smooth.
class CircleSpriteCache {
public:
    // Draws a circle of radius r centred at (cx, cy) in the painter's (device-pixel) space.
    void draw(QPainter& p, double cx, double cy, double r, QRgb rgb);
    void clear();
    int size() const { return int(m_sprites.size()); }

    static constexpr double kMaxSpriteRadius = 96; // larger circles are drawn directly

private:
    struct Sprite {
        QImage image;
        int offset = 0; // image pixel whose top-left corner is the integer part of the centre
    };
    const Sprite& sprite(int radius8, QRgb rgb, int phaseX, int phaseY, int phases);

    QHash<quint64, Sprite> m_sprites;
};

// QStaticText per (node id, font pixel size, wrap width), prepared with the label font.
class LabelCache {
public:
    struct Entry {
        QStaticText text;
        QFont font;
        double width = 0; // wrap width used for layout
    };
    const Entry& get(const QString& id, const QString& label, const QString& family, double fontPx, double wrapWidth);
    void clear();
    int size() const { return int(m_entries.size()); }

private:
    struct Key {
        QString id;
        int px2;   // font px × 2
        int wrap;  // wrap width (px)
        friend bool operator==(const Key&, const Key&) = default;
    };
    friend size_t qHash(const Key& k, size_t seed) { return qHashMulti(seed, k.id, k.px2, k.wrap); }
    QHash<Key, Entry> m_entries;
};

// CSS box-shadow layers (e.g. --shadow-s) rendered around a rounded rectangle.
class BoxShadowCache {
public:
    struct Layer {
        double dx = 0, dy = 0, blur = 0, spread = 0;
        QColor color;
    };
    static std::vector<Layer> parse(const QString& css);

    // Paints the shadow of `rect` (device pixels) with corner radius `radius`.
    void paint(QPainter& p, const QRectF& rect, double radius, const std::vector<Layer>& layers);
    void clear() { m_key.clear(); }

private:
    QString m_key;
    QImage m_image;
    QPointF m_origin; // image position relative to rect.topLeft()
};

} // namespace oc
