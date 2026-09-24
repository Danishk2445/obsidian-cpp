#pragma once

// Caches behind GraphCanvas: pre-rasterised node circles, laid-out labels and the controls
// panel's box shadow. All sizes are device pixels.

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QString>

#include <unordered_map>
#include <vector>

class QPainter;

namespace oc {

// Antialiased filled circles, one sprite per (radius in 1/8 px, colour, sub-pixel phase), blitted
// 1:1 at integer positions. Small circles get 4×4 sub-pixel phases so motion stays smooth.
class CircleSpriteCache {
public:
    // Where a circle's sprite goes; `image` is null for circles too large for a sprite (draw those
    // with drawEllipse). Images stay valid until the next beginFrame() or clear(), so blits may run
    // on other threads.
    struct Blit {
        const QImage* image = nullptr;
        QPoint pos;
    };
    // Once per frame, before blit(): trims the cache when it has grown too large.
    void beginFrame();
    Blit blit(double cx, double cy, double r, QRgb rgb);
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

    std::unordered_map<quint64, Sprite> m_sprites; // node-based: references survive rehashing
};

// Rasterised node labels: one premultiplied image per (node id, font pixel size, wrap width) in the
// label colour, word-wrapped and centred like Pixi's Text (align center, wordWrapWidth), 1 px of
// padding around the text box. Missing images are rendered on the thread pool (prepare()); a
// frame then only blits.
class LabelCache {
public:
    struct Request {
        QString id;
        QString label;
        int px = 0;
        int wrap = 0;
    };
    // Sets the family/colour; changing either drops every image.
    void setStyle(const QString& family, QRgb color);
    const QImage* find(const QString& id, int px, int wrap) const;
    // Renders the requests that are not cached yet (in parallel when there are many).
    void prepare(const std::vector<Request>& requests);
    void clear();
    int size() const { return int(m_entries.size()); }

    // Thread-safe rendering of one label.
    static QImage render(const QString& text, const QString& family, int px, int wrap, QRgb color);

private:
    struct Key {
        QString id;
        int px;   // font px
        int wrap; // wrap width (px)
        friend bool operator==(const Key&, const Key&) = default;
    };
    friend size_t qHash(const Key& k, size_t seed) { return qHashMulti(seed, k.id, k.px, k.wrap); }
    QHash<Key, QImage> m_entries;
    QString m_family;
    QRgb m_color = 0;
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
