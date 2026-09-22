#include "ui/graph/graph_render_cache.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextOption>

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
    auto it = m_sprites.constFind(key);
    if (it != m_sprites.cend())
        return *it;
    if (m_sprites.size() > 8192)
        m_sprites.clear();
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
    return *m_sprites.insert(key, std::move(s));
}

void CircleSpriteCache::draw(QPainter& p, double cx, double cy, double r, QRgb rgb)
{
    if (!(r > 0))
        return;
    if (r > kMaxSpriteRadius) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromRgb(rgb));
        p.drawEllipse(QPointF(cx, cy), r, r);
        return;
    }
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
    p.drawImage(QPoint(int(fx) - s.offset, int(fy) - s.offset), s.image);
}

void CircleSpriteCache::clear() { m_sprites.clear(); }

// ---------------------------------------------------------------------------------------------
// LabelCache
// ---------------------------------------------------------------------------------------------
const LabelCache::Entry& LabelCache::get(const QString& id, const QString& label, const QString& family, double fontPx, double wrapWidth)
{
    const Key key{id, int(std::lround(fontPx * 2)), int(std::lround(wrapWidth))};
    auto it = m_entries.constFind(key);
    if (it != m_entries.cend())
        return *it;
    if (m_entries.size() > 4000)
        m_entries.clear();
    Entry e;
    e.font = QFont(family);
    e.font.setPixelSize(std::max(1, int(std::lround(fontPx))));
    e.font.setHintingPreference(QFont::PreferNoHinting);
    e.width = wrapWidth;
    e.text.setText(label);
    e.text.setTextFormat(Qt::PlainText);
    e.text.setTextWidth(wrapWidth);
    QTextOption option(Qt::AlignHCenter);
    option.setWrapMode(QTextOption::WordWrap);
    e.text.setTextOption(option);
    e.text.setPerformanceHint(QStaticText::AggressiveCaching);
    e.text.prepare(QTransform(), e.font);
    return *m_entries.insert(key, std::move(e));
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
