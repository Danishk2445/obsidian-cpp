#include "ui/views/canvas/canvas_render.h"

#include "core/canvas/canvas_geometry.h"
#include "editor/live_preview_decorator.h"
#include "editor/widgets/nested_renderer.h"
#include "render/services/image_cache.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "text/text.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QWidget>

#include <cmath>

namespace oc::canvasui {

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------------------------

const Palette& Palette::current()
{
    static Palette p;
    const ThemePtr t = currentTheme();
    const quint64 gen = t ? t->generation() : 0;
    if (p.generation == gen && p.card.isValid())
        return p;
    p = Palette{};
    p.generation = gen;
    if (!t)
        return p;
    auto c = [&](const char* name, QColor fallback) { return t->color(QString::fromLatin1(name), fallback); };
    p.dark = t->isDark();
    p.background = c("--canvas-background", p.dark ? QColor(0x1e1e1e) : Qt::white);
    p.dots = c("--canvas-dot-pattern", p.dark ? QColor(0x333333) : QColor(0xe4e4e4));
    p.card = c("--background-primary", p.background);
    p.cardDark = c("--background-secondary", p.card);
    p.defaultColor = c("--canvas-color", QColor(0xc0c0c0));
    static const QColor fallback[7] = {QColor(), QColor(0xe93147), QColor(0xec7500), QColor(0xe0ac00),
                                       QColor(0x08b94e), QColor(0x00bfbc), QColor(0x7852ee)};
    for (int i = 1; i <= 6; ++i)
        p.presets[i] = t->color(QStringLiteral("--canvas-color-%1").arg(i), fallback[i]);
    p.accent = c("--color-accent", QColor(0x8a5cf5));
    p.textNormal = c("--text-normal", p.dark ? QColor(0xdadada) : QColor(0x222222));
    p.textMuted = c("--text-muted", QColor(0x999999));
    p.textFaint = c("--text-faint", QColor(0x666666));
    p.textOnAccent = c("--text-on-accent", Qt::white);
    p.textOnAccentInverted = c("--text-on-accent-inverted", Qt::black);
    p.labelColor = c("--canvas-card-label-color", p.textFaint);
    p.border = c("--background-modifier-border", QColor(0x363636));
    p.borderHover = c("--background-modifier-border-hover", p.border);
    p.interactive = c("--interactive-normal", p.card);
    p.interactiveHover = c("--interactive-hover", p.card);
    p.h1 = c("--inline-title-color", p.textNormal);
    p.uiFontPx = t->px(u"--font-ui-medium"_s, -1, 15);
    p.edgeLabelPx = t->px(u"--font-ui-large"_s, -1, 20);
    p.uiFamily = t->interfaceFamily();
    p.textFamily = t->textFamily();
    return p;
}

QFont uiFont(double px, int weight)
{
    const ThemePtr t = currentTheme();
    QFont f = t ? t->interfaceFont(px) : QFont();
    if (!t)
        f.setPixelSize(std::max(1, int(std::lround(px))));
    f.setWeight(QFont::Weight(weight));
    return f;
}

// ---------------------------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------------------------

QColor parseCssColor(const QString& raw)
{
    const QString v = raw.trimmed();
    if (v.isEmpty())
        return {};
    static const QRegularExpression triplet(uR"(^(\d+)\s*,\s*(\d+)\s*,\s*(\d+)$)"_s);
    if (const auto m = triplet.match(v); m.hasMatch())
        return QColor(std::min(255, m.captured(1).toInt()), std::min(255, m.captured(2).toInt()), std::min(255, m.captured(3).toInt()));
    if (v.startsWith(u'#')) {
        const QString hex = v.mid(1);
        bool ok = false;
        const uint n = hex.toUInt(&ok, 16);
        if (!ok)
            return {};
        switch (hex.size()) {
        case 3: return QColor(((n >> 8) & 0xF) * 17, ((n >> 4) & 0xF) * 17, (n & 0xF) * 17);
        case 4: return QColor(((n >> 12) & 0xF) * 17, ((n >> 8) & 0xF) * 17, ((n >> 4) & 0xF) * 17, (n & 0xF) * 17);
        case 6: return QColor((n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF);
        case 8: return QColor((n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF); // CSS order: rgba
        default: return {};
        }
    }
    static const QRegularExpression fn(uR"(^(rgba?|hsla?)\(\s*([^)]*)\)$)"_s, QRegularExpression::CaseInsensitiveOption);
    if (const auto m = fn.match(v); m.hasMatch()) {
        QStringList parts = m.captured(2).split(QRegularExpression(uR"([\s,/]+)"_s), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            return {};
        auto num = [](QString s, double scale) {
            const bool pct = s.endsWith(u'%');
            if (pct)
                s.chop(1);
            if (s.endsWith(u"deg"))
                s.chop(3);
            const double d = s.toDouble();
            return pct ? d / 100 * scale : d;
        };
        const double a = parts.size() > 3 ? num(parts[3], 1) : 1;
        if (m.captured(1).startsWith(u"rgb", Qt::CaseInsensitive))
            return QColor::fromRgbF(float(num(parts[0], 255) / 255), float(num(parts[1], 255) / 255), float(num(parts[2], 255) / 255),
                                    float(std::clamp(a, 0.0, 1.0)));
        double h = std::fmod(num(parts[0], 360), 360);
        if (h < 0)
            h += 360;
        return QColor::fromHslF(float(h / 360), float(std::clamp(num(parts[1], 1), 0.0, 1.0)), float(std::clamp(num(parts[2], 1), 0.0, 1.0)),
                                float(std::clamp(a, 0.0, 1.0)));
    }
    const QColor named = QColor::fromString(v);
    return named.isValid() ? named : QColor();
}

CanvasColor resolveColor(const QString& value, const Palette& p)
{
    const QString v = value.trimmed();
    if (v.isEmpty())
        return {p.defaultColor, false};
    static const QRegularExpression digits(u"^\\d+$"_s);
    if (digits.match(v).hasMatch()) {
        const int n = v.toInt();
        // An unknown preset keeps the default color but is still "themed" (Obsidian).
        return {n >= 1 && n <= 6 ? p.presets[n] : p.defaultColor, true};
    }
    const QColor c = parseCssColor(v);
    if (!c.isValid())
        return {p.defaultColor, true};
    return {c, true};
}

bool isLightColor(const QColor& c)
{
    return 0.299 * c.redF() + 0.587 * c.greenF() + 0.114 * c.blueF() > 0.6;
}

QColor withAlpha(QColor c, double alpha)
{
    c.setAlphaF(float(std::clamp(c.alphaF() * alpha, 0.0, 1.0)));
    return c;
}

// ---------------------------------------------------------------------------------------------
// MarkdownContent
// ---------------------------------------------------------------------------------------------

namespace {
constexpr double kPadX = 16; // .markdown-preview-view padding: 0 var(--size-4-4)

QFont titleFont(double* size, double* lineHeight, double* marginBottom)
{
    const ThemePtr t = currentTheme();
    const double base = t ? t->px(u"--font-text-size"_s, -1, 16) : 16;
    const double s = t ? t->px(u"--inline-title-size"_s, base, base * 1.618) : base * 1.618;
    QFont f = t ? t->textFont(s) : QFont();
    f.setWeight(QFont::Weight(int(t ? t->number(u"--inline-title-weight"_s, 700) : 700)));
    f.setLetterSpacing(QFont::AbsoluteSpacing, -0.015 * s);
    if (size)
        *size = s;
    if (lineHeight)
        *lineHeight = s * (t ? t->number(u"--inline-title-line-height"_s, 1.2) : 1.2);
    if (marginBottom)
        *marginBottom = t ? t->px(u"--inline-title-margin-bottom"_s, s, s * 0.5) : s * 0.5;
    return f;
}
} // namespace

MarkdownContent::MarkdownContent(const Text& text, StyleSheetPtr styles, const LivePreviewDecorator* resolver, const QString& title)
    : m_renderer(std::make_unique<NestedRenderer>(text, std::move(styles), 1, resolver))
    , m_title(title)
{
}

MarkdownContent::~MarkdownContent() = default;

double MarkdownContent::titleHeight(double width) const
{
    if (m_title.isEmpty())
        return 0;
    double lh = 0, mb = 0;
    const QFont f = titleFont(nullptr, &lh, &mb);
    const QFontMetricsF fm(f);
    const QRectF r = fm.boundingRect(QRectF(0, 0, std::max(1.0, width), 1e6), Qt::TextWordWrap, m_title);
    const int lines = std::max(1, int(std::lround(r.height() / fm.lineSpacing())));
    return lines * lh + mb;
}

double MarkdownContent::contentHeight(double width)
{
    const double w = std::max(1.0, width - 2 * kPadX);
    return titleHeight(w) + m_renderer->layout(w);
}

double MarkdownContent::topPadding(double h, double contentH)
{
    // The ::before/::after spacers of the card (flex 1 1 0, max 16px, min min(10% - 3px, 16px)).
    const double minSpacer = std::clamp(h * 0.1 - 3, 0.0, 16.0);
    const double share = h / 3;
    if (share >= contentH)
        return std::min(16.0, share);
    return std::clamp((h - contentH) / 2, minSpacer, 16.0);
}

void MarkdownContent::paint(QPainter* p, const QRectF& rect, double scroll)
{
    const double w = std::max(1.0, rect.width() - 2 * kPadX);
    const double total = contentHeight(rect.width());
    const double top = rect.top() + topPadding(rect.height(), total) - scroll;
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    double y = top;
    if (!m_title.isEmpty()) {
        double lh = 0, mb = 0;
        const QFont f = titleFont(nullptr, &lh, &mb);
        p->setFont(f);
        p->setPen(Palette::current().h1);
        const double th = titleHeight(w) - mb;
        QTextOption opt;
        opt.setWrapMode(QTextOption::WordWrap);
        p->drawText(QRectF(rect.left() + kPadX, y, w, th), m_title, opt);
        y += th + mb;
    }
    m_renderer->layout(w);
    m_renderer->paint(p, QPointF(rect.left() + kPadX, y), rect);
    p->restore();
}

std::optional<CardLink> MarkdownContent::linkAt(const QPointF& pos, const QSizeF& size, double scroll)
{
    const double w = std::max(1.0, size.width() - 2 * kPadX);
    const double total = contentHeight(size.width());
    const double top = topPadding(size.height(), total) - scroll + titleHeight(w);
    m_renderer->layout(w);
    const auto hit = m_renderer->linkHitAt(QPointF(pos.x() - kPadX, pos.y() - top));
    if (!hit)
        return std::nullopt;
    return CardLink{hit->first, hit->second};
}

// ---------------------------------------------------------------------------------------------
// ImageContent
// ---------------------------------------------------------------------------------------------

ImageContent::ImageContent(QString absPath)
    : m_path(std::move(absPath))
{
    m_natural = ImageCache::instance().naturalSize(m_path);
}

double ImageContent::aspectRatio() const
{
    return m_natural.isValid() && m_natural.height() > 0 ? double(m_natural.width()) / m_natural.height() : 0;
}

bool ImageContent::isLoading() const { return !m_natural.isValid(); }

void ImageContent::paint(QPainter* p, const QRectF& rect, double scroll)
{
    Q_UNUSED(scroll);
    if (!m_natural.isValid() || m_natural.isEmpty())
        return;
    QSizeF s = QSizeF(m_natural).scaled(rect.size(), Qt::KeepAspectRatio);
    const QRectF target(rect.center().x() - s.width() / 2, rect.center().y() - s.height() / 2, s.width(), s.height());
    const double scale = std::abs(p->combinedTransform().m11());
    // Buckets of 128 device px so zooming reuses decoded images.
    int want = int(std::ceil(target.width() * scale / 128.0)) * 128;
    want = std::clamp(want, 32, std::max(32, m_natural.width()));
    const std::shared_ptr<const QImage> img = ImageCache::instance().image(m_path, want, 1.0);
    if (!img)
        return; // imageReady() triggers a repaint
    p->save();
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->drawImage(target, *img);
    p->restore();
}

// ---------------------------------------------------------------------------------------------
// MinimapContent
// ---------------------------------------------------------------------------------------------

MinimapContent::MinimapContent(canvas::CanvasData data)
    : m_data(std::move(data))
{
}

namespace {
void paintMinimap(QPainter* p, const QRectF& rect, const canvas::CanvasData& m_data);
} // namespace

void MinimapContent::paint(QPainter* p, const QRectF& rect, double scroll)
{
    Q_UNUSED(scroll);
    paintMinimap(p, rect, m_data);
}

namespace {
void paintMinimap(QPainter* p, const QRectF& rect, const canvas::CanvasData& m_data)
{
    using namespace oc::canvas;
    const Palette& pal = Palette::current();
    QRectF box = allNodesBounds(m_data);
    if (box.isNull())
        return;
    const double pad = std::max(box.width(), box.height()) / 20;
    box.adjust(-pad, -pad, pad, pad);
    const QRectF area = rect.adjusted(4, 4, -4, -4); // .canvas-minimap padding
    if (area.width() <= 0 || area.height() <= 0 || box.width() <= 0 || box.height() <= 0)
        return;
    const double k = std::min(area.width() / box.width(), area.height() / box.height());
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    p->translate(area.center());
    p->scale(k, k);
    p->translate(-box.center());
    p->setRenderHint(QPainter::Antialiasing);
    auto drawNode = [&](const Node& n) {
        const CanvasColor c = resolveColor(n.color, pal);
        const QColor base = c.themed ? c.color : pal.border;
        p->setPen(QPen(base, 5));
        p->setBrush(withAlpha(base, c.themed ? 0.5 : 0.65));
        p->drawRoundedRect(n.rect(), 20, 20);
    };
    for (const Node& n : m_data.nodes)
        if (n.isShown() && n.isGroup())
            drawNode(n);
    const double stroke = std::sqrt(box.width() / 10);
    for (const Edge& e : m_data.edges) {
        if (!m_data.isEdgeShown(e))
            continue;
        const QRectF a = m_data.node(e.fromNode)->rect(), b = m_data.node(e.toNode)->rect();
        // Obsidian's minimap draws the bezier between the side anchors themselves.
        const QPointF s = sideAnchor(a, e.fromSide), t = sideAnchor(b, e.toSide);
        const double o = std::clamp(QLineF(s, t).length() / 2, 70.0, 150.0);
        QPainterPath path(s);
        path.cubicTo(s + sideDirection(e.fromSide) * o, t + sideDirection(e.toSide) * o, t);
        const CanvasColor c = resolveColor(e.color, pal);
        p->setPen(QPen(c.themed ? c.color : QColor(0xc0c0c0), stroke));
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
    }
    for (const Node& n : m_data.nodes)
        if (n.isShown() && !n.isGroup())
            drawNode(n);
    p->restore();
}
} // namespace

// ---------------------------------------------------------------------------------------------
// PlaceholderContent
// ---------------------------------------------------------------------------------------------

PlaceholderContent::PlaceholderContent(QString icon, QString title, QString subtitle)
    : m_icon(std::move(icon))
    , m_title(std::move(title))
    , m_subtitle(std::move(subtitle))
{
}

void PlaceholderContent::paint(QPainter* p, const QRectF& rect, double scroll)
{
    Q_UNUSED(scroll);
    const Palette& pal = Palette::current();
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    const double iconPx = std::min(48.0, std::min(rect.width(), rect.height()) * 0.3);
    const QFont f = uiFont(pal.uiFontPx);
    const QFontMetricsF fm(f);
    const double textW = std::max(10.0, rect.width() - 24);
    // The title wraps (at most 3 lines), the subtitle stays on one line.
    const QRectF titleBox = fm.boundingRect(QRectF(0, 0, textW, fm.lineSpacing() * 3), Qt::AlignHCenter | Qt::TextWordWrap, m_title);
    const double titleH = std::min(titleBox.height(), fm.lineSpacing() * 3);
    const double textH = titleH + (m_subtitle.isEmpty() ? 0 : fm.height() + 4);
    const double total = iconPx + 12 + textH;
    double y = rect.center().y() - total / 2;
    if (iconPx >= 8 && !m_icon.isEmpty()) {
        const double scale = std::max(1.0, std::abs(p->combinedTransform().m11()));
        const QPixmap pm = IconProvider::instance().pixmap(m_icon, int(std::ceil(iconPx)), pal.textFaint, 1.5, scale);
        p->drawPixmap(QRectF(rect.center().x() - iconPx / 2, y, iconPx, iconPx), pm, QRectF(pm.rect()));
    }
    y += iconPx + 12;
    p->setFont(f);
    p->setPen(pal.textMuted);
    p->drawText(QRectF(rect.left() + 12, y, textW, titleH + 1), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_title);
    if (!m_subtitle.isEmpty()) {
        p->setPen(pal.textFaint);
        const QRectF l2(rect.left() + 12, y + titleH + 4, textW, fm.height());
        p->drawText(l2, Qt::AlignHCenter | Qt::AlignTop, fm.elidedText(m_subtitle, Qt::ElideMiddle, l2.width()));
    }
    p->restore();
}

// ---------------------------------------------------------------------------------------------
// EmbedContent
// ---------------------------------------------------------------------------------------------

EmbedContent::EmbedContent(std::shared_ptr<EmbedProvider> provider, EmbedRequest request)
    : m_provider(std::move(provider))
    , m_request(std::move(request))
{
}

void EmbedContent::paint(QPainter* p, const QRectF& rect, double scroll)
{
    Q_UNUSED(scroll);
    if (!m_provider)
        return;
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    m_provider->paint(p, rect, m_request);
    p->restore();
}

double EmbedContent::aspectRatio() const
{
    // Obsidian: videos keep their picture's ratio (like images); PDFs and audio resize freely.
    static const QStringList video = {u"mp4"_s, u"webm"_s, u"ogv"_s, u"mov"_s, u"mkv"_s};
    if (!m_provider || !video.contains(m_request.extension))
        return 0;
    const QSizeF s = m_provider->size(m_request, 1000);
    return s.height() > 0 && s.width() > 0 && s.height() != 42 ? s.width() / s.height() : 0;
}

// ---------------------------------------------------------------------------------------------
// Canvas embeds in notes
// ---------------------------------------------------------------------------------------------

namespace {
constexpr double kEmbedCanvasMaxHeight = 400; // --embed-canvas-max-height

// Parsed canvases by path + mtime (embeds repaint often).
std::shared_ptr<const canvas::CanvasData> embeddedCanvas(const QString& absPath)
{
    struct Entry {
        qint64 mtime = -1;
        std::shared_ptr<const canvas::CanvasData> data;
    };
    static QHash<QString, Entry> cache;
    const QFileInfo fi(absPath);
    const qint64 mtime = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : -1;
    Entry& e = cache[absPath];
    if (!e.data || e.mtime != mtime) {
        e.mtime = mtime;
        QFile f(absPath);
        std::optional<canvas::CanvasData> d = f.open(QIODevice::ReadOnly) ? canvas::parseCanvas(f.readAll()) : std::nullopt;
        e.data = std::make_shared<const canvas::CanvasData>(d ? std::move(*d) : canvas::CanvasData{});
    }
    return e.data;
}

double titleRowHeight()
{
    const ThemePtr t = currentTheme();
    const double px = t ? t->px(u"--font-text-size"_s, -1, 16) : 16;
    return std::ceil(px * 1.5) + 8; // .embed-title: one line + padding-bottom var(--size-4-2)
}

class CanvasEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override
    {
        const double w = std::max<qreal>(80, maxWidth);
        const std::shared_ptr<const canvas::CanvasData> data = embeddedCanvas(request.absPath);
        double h = 150;
        const QRectF box = canvas::allNodesBounds(*data);
        if (!box.isEmpty())
            h = std::clamp((w - 8) * box.height() / box.width() + 8, 60.0, kEmbedCanvasMaxHeight);
        if (request.display == EmbedRequest::Display::Popover)
            return QSizeF(std::min<qreal>(450, w), std::min(400.0, h + titleRowHeight()));
        return QSizeF(w, h + titleRowHeight());
    }
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override
    {
        const Palette& pal = Palette::current();
        const ThemePtr t = currentTheme();
        const double px = t ? t->px(u"--font-text-size"_s, -1, 16) : 16;
        // .embed-title: the canvas icon and the file's name
        const double rowH = titleRowHeight() - 8;
        const double iconPx = std::round(px);
        const double dpr = std::max(1.0, std::abs(p->combinedTransform().m11()));
        const QPixmap icon = IconProvider::instance().pixmap(u"layout-dashboard"_s, int(iconPx), pal.textMuted, 1.75, dpr);
        p->drawPixmap(QRectF(rect.left(), rect.top() + (rowH - iconPx) / 2, iconPx, iconPx), icon, QRectF(icon.rect()));
        QFont f = t ? t->textFont(px) : QFont();
        f.setWeight(QFont::DemiBold);
        p->setFont(f);
        p->setPen(pal.textNormal);
        const QRectF titleRect(rect.left() + iconPx + 4, rect.top(), rect.width() - iconPx - 4, rowH);
        const QString name = QFileInfo(request.absPath).completeBaseName();
        p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(f).elidedText(name, Qt::ElideRight, titleRect.width()));
        const QRectF map(rect.left(), rect.top() + titleRowHeight(), rect.width(), rect.height() - titleRowHeight());
        paintMinimap(p, map, *embeddedCanvas(request.absPath));
    }
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
    quint64 signature(const EmbedRequest& request) const override
    {
        const QFileInfo fi(request.absPath);
        return quint64(fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0);
    }
};

// The live embed: the same painting; a click opens the canvas (Mod: in a new tab).
class CanvasEmbedWidget : public QWidget {
public:
    CanvasEmbedWidget(const CanvasEmbedProvider* provider, EmbedContext context, QWidget* parent)
        : QWidget(parent)
        , m_provider(provider)
        , m_ctx(std::move(context))
    {
        setObjectName(u"ocCanvasEmbed"_s);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        m_provider->paint(&p, QRectF(rect()), m_ctx.request);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()))
            EmbedRegistry::instance().open(this, m_ctx.request, (e->modifiers() & Qt::ControlModifier) ? u"tab"_s : QString());
    }

private:
    const CanvasEmbedProvider* m_provider;
    EmbedContext m_ctx;
};

QWidget* CanvasEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const
{
    return new CanvasEmbedWidget(this, context, parent);
}
} // namespace

void registerCanvasEmbed()
{
    static const std::shared_ptr<CanvasEmbedProvider> provider = std::make_shared<CanvasEmbedProvider>();
    EmbedRegistry& r = EmbedRegistry::instance();
    if (r.provider(u"canvas"_s) != provider)
        r.registerExtensions({u"canvas"_s}, provider);
}

} // namespace oc::canvasui
