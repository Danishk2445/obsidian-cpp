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

std::optional<std::pair<QString, int>> NestedRenderer::linkHitAt(const QPointF& local) const
{
    if (local.x() < 0 || local.y() < 0 || local.x() > m_width || local.y() > m_height)
        return std::nullopt;
    const qsizetype pos = m_layout.hitTest(local);
    const auto hit = m_decorator->linkAt(pos);
    if (!hit)
        return std::nullopt;
    using K = LivePreviewDecorator::LinkHit::Kind;
    int kind = 0;
    switch (hit->kind) {
    case K::WikiLink: kind = 0; break;
    case K::Embed: kind = 1; break;
    case K::Link: kind = 2; break;
    case K::Url: kind = 3; break;
    case K::Tag: kind = 4; break;
    }
    return std::make_pair(hit->target, kind);
}

} // namespace oc
