#include "editor/widgets/nested_renderer.h"

#include "editor/live_preview_decorator.h"

#include <QPainter>

namespace oc {

NestedRenderer::NestedRenderer(const Text& text, StyleSheetPtr styles, int depth, const LivePreviewDecorator* parent)
    : m_source(std::make_unique<md::StaticDocSource>(text))
    , m_decorator(std::make_unique<LivePreviewDecorator>(m_source.get()))
{
    m_decorator->setStyleSheet(styles.get());
    m_decorator->setReading(true);
    m_decorator->setNestingDepth(depth);
    if (parent) {
        m_decorator->isResolved = parent->isResolved;
        m_decorator->resolveAttachment = parent->resolveAttachment;
    }
    m_layout.setDecorator(m_decorator.get());
    m_layout.setStyleSheet(std::move(styles));
    m_layout.setText(m_source->text());
}

NestedRenderer::~NestedRenderer() = default;

qreal NestedRenderer::layout(qreal width)
{
    if (width == m_width)
        return m_height;
    m_width = width;
    m_decorator->setContentWidth(width);
    m_layout.setContentWidth(width);
    m_layout.ensureLaidOut(0, 1e9); // nested content is small; measure all of it
    m_height = m_layout.totalHeight();
    return m_height;
}

void NestedRenderer::paint(QPainter* p, const QPointF& origin, const QRectF& clip)
{
    p->save();
    p->translate(origin);
    DocLayout::PaintContext ctx;
    ctx.drawSelection = false;
    ctx.clip = clip.translated(-origin);
    ctx.rightEdge = m_width;
    m_layout.paint(p, ctx);
    p->restore();
}

qsizetype NestedRenderer::hitTest(const QPointF& local) const { return m_layout.hitTest(local); }

QString NestedRenderer::linkAt(const QPointF& local) const
{
    const qsizetype pos = m_layout.hitTest(local);
    auto hit = m_decorator->linkAt(pos);
    return hit ? hit->target : QString();
}

} // namespace oc
