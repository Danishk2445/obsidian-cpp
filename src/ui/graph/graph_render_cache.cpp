#include "ui/graph/graph_render_cache.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>

namespace oc {

// ---------------------------------------------------------------------------------------------
// CircleSpriteCache
// ---------------------------------------------------------------------------------------------
const CircleSpriteCache::Sprite& CircleSpriteCache::sprite(int radius8, QRgb rgb, int phaseX, int phaseY, int phases)
{
    const quint64 key = (quint64(radius8) << 32) | (quint64(phaseX) << 30) | (quint64(phaseY) << 28) | (quint64(phases == 4) << 27)
                        | quint64(rgb & 0xFFFFFFu);
    auto it = m_sprites.find(key);
    if (it != m_sprites.end())
        return it->second;
    const double r = radius8 / 8.0;
    Sprite s;
    s.offset = int(std::ceil(r)) + 1;
    const int size = 2 * s.offset + 2;
    s.image = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
    s.image.fill(Qt::transparent);
    QPainter p(&s.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromRgb(rgb));
    p.drawEllipse(QPointF(s.offset + double(phaseX) / phases, s.offset + double(phaseY) / phases), r, r);
    p.end();
    return m_sprites.emplace(key, std::move(s)).first->second;
}

void CircleSpriteCache::beginFrame()
{
    if (m_sprites.size() > 8192)
        m_sprites.clear();
}

void CircleSpriteCache::draw(QPainter& p, double cx, double cy, double r, QRgb rgb)
{
    const Blit b = blit(cx, cy, r, rgb);
    if (b.image) {
        p.drawImage(b.pos, *b.image);
    } else if (r > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromRgb(rgb));
        p.drawEllipse(QPointF(cx, cy), r, r);
    }
}

CircleSpriteCache::Blit CircleSpriteCache::blit(double cx, double cy, double r, QRgb rgb)
{
    if (!(r > 0) || r > kMaxSpriteRadius)
        return {};
    const int phases = r < 24 ? 4 : 1;
    double fx = std::floor(cx), fy = std::floor(cy);
    int px = int(std::floor((cx - fx) * phases + 0.5));
    int py = int(std::floor((cy - fy) * phases + 0.5));
    if (px == phases) {
        px = 0;
        fx += 1;
    }
    if (py == phases) {
        py = 0;
        fy += 1;
    }
    const Sprite& s = sprite(std::max(1, int(std::lround(r * 8))), rgb, px, py, phases);
    return {&s.image, QPoint(int(fx) - s.offset, int(fy) - s.offset)};
}

void CircleSpriteCache::clear() { m_sprites.clear(); }

// ---------------------------------------------------------------------------------------------
// LabelCache
// ---------------------------------------------------------------------------------------------
void LabelCache::setStyle(const QString& family, QRgb color)
{
    if (family == m_family && color == m_color)
        return;
    m_family = family;
    m_color = color;
    m_entries.clear();
}

const QImage* LabelCache::find(const QString& id, int px, int wrap) const
{
    const auto it = m_entries.constFind(Key{id, px, wrap});
    return it == m_entries.cend() ? nullptr : &*it;
}

QImage LabelCache::render(const QString& text, const QString& family, int px, int wrap, QRgb color)
{
    QFont font(family);
    font.setPixelSize(std::max(1, px));
    font.setHintingPreference(QFont::PreferNoHinting);
    const int flags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
    const QFontMetricsF fm(font);
    const QRectF box = fm.boundingRect(QRectF(0, 0, std::max(1, wrap), 1e6), flags, text);
    // The tight box breaks lines exactly like the wrap width: no line is wider than the box.
    const QSize size(int(std::ceil(box.width())) + 2, int(std::ceil(box.height())) + 2);
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setFont(font);
    p.setPen(QColor::fromRgb(color));
    p.drawText(QRectF(1, 1, size.width() - 2, size.height() - 2), flags, text);
    p.end();
    return img;
}

void LabelCache::prepare(const std::vector<Request>& requests)
{
    std::vector<const Request*> missing;
    for (const Request& r : requests)
        if (!m_entries.contains(Key{r.id, r.px, r.wrap}))
            missing.push_back(&r);
    if (missing.empty())
        return;
    if (m_entries.size() + qsizetype(missing.size()) > 6000)
        m_entries.clear();
    const QString family = m_family;
    const QRgb color = m_color;
    auto renderOne = [&](const Request* r) { return render(r->label, family, r->px, r->wrap, color); };
    std::vector<QImage> images;
    if (missing.size() < 8) {
        for (const Request* r : missing)
            images.push_back(renderOne(r));
    } else {
        images = QtConcurrent::blockingMapped<std::vector<QImage>>(missing, renderOne);
    }
    for (size_t i = 0; i < missing.size(); ++i)
        m_entries.insert(Key{missing[i]->id, missing[i]->px, missing[i]->wrap}, std::move(images[i]));
}

void LabelCache::clear() { m_entries.clear(); }

// ---------------------------------------------------------------------------------------------
// BoxShadowCache
// ---------------------------------------------------------------------------------------------
namespace {

QColor parseCssColor(const QString& s)
{
    static const QRegularExpression rgba(QStringLiteral(R"(rgba?\(\s*([^)]*)\))"));
    const auto m = rgba.match(s);
    if (m.hasMatch()) {
        const QStringList parts = m.captured(1).split(QLatin1Char(','));
        if (parts.size() >= 3) {
            QColor c(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
            if (parts.size() >= 4)
                c.setAlphaF(std::clamp(parts[3].trimmed().toFloat(), 0.0f, 1.0f));
            return c;
        }
    }
    return QColor(s.trimmed());
}

// Separable box blur (radius r) of a float plane, in place.
void boxBlur(std::vector<float>& plane, int w, int h, int r)
{
    if (r <= 0)
        return;
    std::vector<float> line(size_t(std::max(w, h)));
    const float norm = 1.0f / float(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        float* row = plane.data() + size_t(y) * size_t(w);
        float acc = 0;
        for (int x = -r; x <= r; ++x)
            acc += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            line[size_t(x)] = acc * norm;
            acc += row[std::clamp(x + r + 1, 0, w - 1)] - row[std::clamp(x - r, 0, w - 1)];
        }
        std::copy(line.begin(), line.begin() + w, row);
    }
    for (int x = 0; x < w; ++x) {
        float acc = 0;
        for (int y = -r; y <= r; ++y)
            acc += plane[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(x)];
        for (int y = 0; y < h; ++y) {
            line[size_t(y)] = acc * norm;
            acc += plane[size_t(std::clamp(y + r + 1, 0, h - 1)) * size_t(w) + size_t(x)]
                   - plane[size_t(std::clamp(y - r, 0, h - 1)) * size_t(w) + size_t(x)];
        }
        for (int y = 0; y < h; ++y)
            plane[size_t(y) * size_t(w) + size_t(x)] = line[size_t(y)];
    }
}

} // namespace

std::vector<BoxShadowCache::Layer> BoxShadowCache::parse(const QString& css)
{
    std::vector<Layer> out;
    QStringList parts;
    QString cur;
    int depth = 0;
    for (QChar ch : css) {
        if (ch == QLatin1Char('('))
            ++depth;
        else if (ch == QLatin1Char(')'))
            --depth;
        if (ch == QLatin1Char(',') && depth == 0) {
            parts << cur;
            cur.clear();
        } else {
            cur += ch;
        }
    }
    parts << cur;
    static const QRegularExpression colorRe(QStringLiteral(R"(rgba?\([^)]*\)|#[0-9a-fA-F]{3,8}|\b(?:transparent|black|white)\b)"));
    for (QString part : parts) {
        part = part.trimmed();
        if (part.isEmpty() || part.contains(QLatin1String("inset")) || part.contains(QLatin1String("var(")))
            continue;
        Layer l;
        const auto cm = colorRe.match(part);
        if (cm.hasMatch()) {
            l.color = parseCssColor(cm.captured(0));
            part.remove(cm.capturedStart(), cm.capturedLength());
        } else {
            l.color = QColor(0, 0, 0, 64);
        }
        const QStringList nums = part.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        double v[4] = {0, 0, 0, 0};
        for (int i = 0; i < nums.size() && i < 4; ++i) {
            QString n = nums[i];
            n.remove(QLatin1String("px"));
            v[i] = n.toDouble();
        }
        l.dx = v[0];
        l.dy = v[1];
        l.blur = v[2];
        l.spread = v[3];
        if (l.color.alpha() > 0)
            out.push_back(l);
    }
    return out;
}

void BoxShadowCache::paint(QPainter& p, const QRectF& rect, double radius, const std::vector<Layer>& layers)
{
    if (layers.empty() || rect.isEmpty())
        return;
    QString key = QStringLiteral("%1x%2r%3").arg(rect.width()).arg(rect.height()).arg(radius);
    for (const Layer& l : layers)
        key += QStringLiteral("|%1,%2,%3,%4,%5").arg(l.dx).arg(l.dy).arg(l.blur).arg(l.spread).arg(l.color.rgba());
    if (key != m_key) {
        double pad = 2;
        for (const Layer& l : layers)
            pad = std::max(pad, std::max(std::abs(l.dx), std::abs(l.dy)) + l.blur + std::abs(l.spread) + 2);
        const int w = int(std::ceil(rect.width() + 2 * pad)), h = int(std::ceil(rect.height() + 2 * pad));
        std::vector<float> r(size_t(w) * size_t(h), 0), g(r), b(r), a(r);
        QImage mask(w, h, QImage::Format_Grayscale8);
        for (const Layer& l : layers) {
            mask.fill(0);
            {
                QPainter mp(&mask);
                mp.setRenderHint(QPainter::Antialiasing);
                mp.setPen(Qt::NoPen);
                mp.setBrush(Qt::white);
                const QRectF shape = QRectF(pad + l.dx, pad + l.dy, rect.width(), rect.height()).adjusted(-l.spread, -l.spread, l.spread, l.spread);
                mp.drawRoundedRect(shape, radius + l.spread, radius + l.spread);
            }
            std::vector<float> plane(size_t(w) * size_t(h));
            for (int y = 0; y < h; ++y) {
                const uchar* line = mask.constScanLine(y);
                for (int x = 0; x < w; ++x)
                    plane[size_t(y) * size_t(w) + size_t(x)] = line[x] / 255.0f;
            }
            const double sigma = l.blur / 2;
            if (sigma > 0.25) {
                const int boxR = std::max(1, int(std::lround((std::sqrt(4 * sigma * sigma + 1) - 1) / 2)));
                for (int pass = 0; pass < 3; ++pass)
                    boxBlur(plane, w, h, boxR);
            }
            const float la = float(l.color.alphaF()), lr = float(l.color.redF()), lg = float(l.color.greenF()), lb = float(l.color.blueF());
            for (size_t i = 0; i < plane.size(); ++i) {
                const float sa = plane[i] * la; // source-over, premultiplied
                r[i] = lr * sa + r[i] * (1 - sa);
                g[i] = lg * sa + g[i] * (1 - sa);
                b[i] = lb * sa + b[i] * (1 - sa);
                a[i] = sa + a[i] * (1 - sa);
            }
        }
        m_image = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(m_image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                auto c8 = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
                line[x] = qRgba(c8(r[i]), c8(g[i]), c8(b[i]), c8(a[i]));
            }
        }
        m_origin = QPointF(-pad, -pad);
        m_key = key;
    }
    p.drawImage(rect.topLeft() + m_origin, m_image);
}

} // namespace oc
