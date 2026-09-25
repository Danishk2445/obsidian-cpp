#pragma once

// Painting pieces of the canvas view: the theme colors of Obsidian's .canvas-* CSS, canvas color
// strings ("1".."6" presets, CSS colors), and the contents of cards (rendered Markdown of text cards
// and notes, images, embedded canvases as minimaps, placeholders).

#include "core/canvas/canvas_model.h"
#include "editor/embeds/embed_registry.h"
#include "render/layout/style_sheet.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QPainter;

namespace oc {

class LivePreviewDecorator;
class NestedRenderer;
class Text;

namespace canvasui {

// Theme values used by the canvas (Obsidian's canvas CSS variables), rebuilt on theme changes.
struct Palette {
    QColor background;     // --canvas-background
    QColor dots;           // --canvas-dot-pattern
    QColor card;           // --background-primary
    QColor cardDark;       // --background-secondary (card menu in dark mode)
    QColor defaultColor;   // --canvas-color (#c0c0c0): unthemed borders and edges
    QColor presets[7];     // --canvas-color-1..6 (index 0 unused)
    QColor accent;         // --color-accent
    QColor textNormal, textMuted, textFaint, textOnAccent, textOnAccentInverted;
    QColor labelColor;     // --canvas-card-label-color
    QColor border;         // --background-modifier-border
    QColor borderHover;    // --background-modifier-border-hover
    QColor interactive, interactiveHover; // --interactive-normal / -hover
    QColor h1;             // --inline-title-color
    bool dark = false;
    double uiFontPx = 15;  // --font-ui-medium: the canvas' base font size
    double edgeLabelPx = 20; // --font-ui-large
    QString uiFamily;
    QString textFamily;
    quint64 generation = 0;

    static const Palette& current(); // for the current theme (cached per generation)
};

// A node or edge color: `themed` when a color is set ("is-themed"), `color` the --canvas-color.
struct CanvasColor {
    QColor color;
    bool themed = false;
};
CanvasColor resolveColor(const QString& value, const Palette& p);
// Parses a CSS color: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb()/rgba(), hsl(), names, and Obsidian's
// "r, g, b" form. Invalid -> invalid QColor.
QColor parseCssColor(const QString& value);
// Obsidian's "light color" test for the group label text (dark text on light colors).
bool isLightColor(const QColor& c);
QColor withAlpha(QColor c, double alpha); // color-mix(in oklch, c N%, transparent) ~ c with alpha N

QFont uiFont(double px, int weight = 400);

// ---- card contents ---------------------------------------------------------------------------

// A link in a card: target and kind (as EditorView::linkActivated: 0 wikilink, 1 embed, 2 markdown
// link, 3 url, 4 tag).
struct CardLink {
    QString target;
    int kind = 0;
};

// What a card shows inside its container. Coordinates are canvas units relative to the content rect.
class CardContent {
public:
    virtual ~CardContent() = default;
    // Paints into `rect` (the card's inner rect), scrolled by `scroll` (content px).
    virtual void paint(QPainter* p, const QRectF& rect, double scroll) = 0;
    // Height of the scrollable content at `width` (0 when it does not scroll).
    virtual double contentHeight(double width) { Q_UNUSED(width); return 0; }
    // Link target under `pos` (relative to the content rect of `size`, which is scrolled by `scroll`).
    virtual std::optional<CardLink> linkAt(const QPointF& pos, const QSizeF& size, double scroll)
    {
        Q_UNUSED(pos);
        Q_UNUSED(size);
        Q_UNUSED(scroll);
        return std::nullopt;
    }
    // Natural aspect ratio (width / height) for media, 0 otherwise.
    virtual double aspectRatio() const { return 0; }
    virtual bool isLoading() const { return false; }
};

// Rendered Markdown (reading view) with Obsidian's card padding: 0 16px, vertical spacers of up to
// 16px (smaller on short cards). `title` adds a note's inline title.
class MarkdownContent : public CardContent {
public:
    MarkdownContent(const Text& text, StyleSheetPtr styles, const LivePreviewDecorator* resolver, const QString& title = {});
    ~MarkdownContent() override;
    void paint(QPainter* p, const QRectF& rect, double scroll) override;
    double contentHeight(double width) override;
    std::optional<CardLink> linkAt(const QPointF& pos, const QSizeF& size, double scroll) override;
    // The vertical offset of the rendered text in a card of `height` whose content is `contentHeight`.
    static double topPadding(double height, double contentHeight);

private:
    double titleHeight(double width) const;
    std::unique_ptr<NestedRenderer> m_renderer;
    QString m_title;
};

// An image, centered and scaled to fit (loaded through the ImageCache).
class ImageContent : public CardContent {
public:
    explicit ImageContent(QString absPath);
    void paint(QPainter* p, const QRectF& rect, double scroll) override;
    double aspectRatio() const override;
    bool isLoading() const override;

private:
    QString m_path;
    QSize m_natural;
};

// Obsidian's canvas embed (".canvas-minimap"): the embedded canvas' groups, edges and cards as shapes.
class MinimapContent : public CardContent {
public:
    explicit MinimapContent(canvas::CanvasData data);
    void paint(QPainter* p, const QRectF& rect, double scroll) override;

private:
    canvas::CanvasData m_data;
};

// A centered icon and text (other files, web pages, missing files).
class PlaceholderContent : public CardContent {
public:
    PlaceholderContent(QString icon, QString title, QString subtitle = {});
    void paint(QPainter* p, const QRectF& rect, double scroll) override;

private:
    QString m_icon, m_title, m_subtitle;
};

// A file shown by an embed provider of the EmbedRegistry (PDF, audio, video): its static rendering.
// The canvas hosts the provider's live widget over it while the card is on screen.
class EmbedContent : public CardContent {
public:
    EmbedContent(std::shared_ptr<EmbedProvider> provider, EmbedRequest request);
    void paint(QPainter* p, const QRectF& rect, double scroll) override;
    double aspectRatio() const override; // videos keep their picture's ratio when resized
    const EmbedRequest& request() const { return m_request; }
    EmbedProvider* provider() const { return m_provider.get(); }

private:
    std::shared_ptr<EmbedProvider> m_provider;
    EmbedRequest m_request;
};

// Obsidian's embed of a canvas in a note ("![[board.canvas]]"): the file name and a minimap of the
// canvas, at most --embed-canvas-max-height (400px) tall; a click opens the canvas. Registered for the
// "canvas" extension by registerCanvasView().
void registerCanvasEmbed();

} // namespace canvasui
} // namespace oc
