#pragma once

#include "markdown/doc_source.h"
#include "render/layout/doc_layout.h"

#include <functional>
#include <memory>
#include <optional>

namespace oc {

class LivePreviewDecorator;

// Read-only rendering of a piece of Markdown (callout bodies, embeds, hover previews): its own
// parsed snapshot + a reading-mode decorator + a DocLayout. Positions of the nested content can be
// mapped back to the source through `lineMap` (nested line -> source line) and `colOffset`.
class NestedRenderer {
public:
    NestedRenderer(const Text& text, StyleSheetPtr styles, int depth, const LivePreviewDecorator* parent);
    ~NestedRenderer();

    qreal layout(qreal width);
    void paint(QPainter* p, const QPointF& origin, const QRectF& clip);
    qsizetype hitTest(const QPointF& local) const;      // nested document offset
    QString linkAt(const QPointF& local) const;
    // The link under `local`: its target and kind (as EditorView::linkActivated: 0 wikilink, 1 embed,
    // 2 markdown link, 3 url, 4 tag).
    std::optional<std::pair<QString, int>> linkHitAt(const QPointF& local) const;
    const Text& text() const { return m_source->text(); }

private:
    std::unique_ptr<md::StaticDocSource> m_source;
    std::unique_ptr<LivePreviewDecorator> m_decorator;
    mutable DocLayout m_layout;
    qreal m_width = -1;
    qreal m_height = 0;
};

} // namespace oc
