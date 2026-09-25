#include "editor/embeds/hosted_embeds.h"

#include "editor/editor_view.h"

#include <QEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QTimer>

#include <cmath>

namespace oc {

namespace {
constexpr int kMaxPooled = 8;                       // beyond this, pooled idle widgets go at once
constexpr const char* kEditableProperty = "ocEmbedEditable"; // Live Preview: the "edit" action applies
} // namespace

// ---------------------------------------------------------------------------------------------
// EmbedWidget
// ---------------------------------------------------------------------------------------------
EmbedWidget::EmbedWidget(EmbedRequest request, std::shared_ptr<EmbedProvider> provider, qreal maxWidth)
    : m_request(std::move(request))
    , m_provider(std::move(provider))
{
    const QSizeF s = m_provider->size(m_request, std::max<qreal>(1, maxWidth));
    m_size = QSizeF(std::max(1.0, std::round(s.width())), std::max(1.0, std::round(s.height())));
    m_signature = qHashMulti(0x656d6264, m_request.key(), m_request.width, m_request.height, m_size.width(), m_size.height(),
                             m_provider->signature(m_request));
}

void EmbedWidget::paint(QPainter* p, const QRectF& rect, const LineMetrics& lm) const
{
    Q_UNUSED(lm);
    p->save();
    m_provider->paint(p, QRectF(rect.topLeft(), m_size), m_request);
    p->restore();
}

// ---------------------------------------------------------------------------------------------
// HostedEmbeds
// ---------------------------------------------------------------------------------------------
HostedEmbeds::HostedEmbeds(EditorView* view)
    : QObject(view)
    , m_view(view)
{
    setObjectName(QStringLiteral("ocHostedEmbeds"));
    view->viewport()->installEventFilter(this);
    connect(view, &EditorView::scrolled, this, &HostedEmbeds::sync);
    connect(view, &EditorView::modeChanged, this, &HostedEmbeds::scheduleSync);
    m_registryConn = connect(&EmbedRegistry::instance(), &EmbedRegistry::changed, this, [this](const QString& absPath) {
        // The file's rendering changed (modified, dimensions known...): re-decorate if it is embedded here.
        EditorView* v = m_view;
        if (!v || !v->document())
            return;
        bool shown = false;
        for (const Hosted& h : std::as_const(m_hosted))
            shown |= h.key.startsWith(absPath + QLatin1Char('\n'));
        const double top = v->scrollTop(), height = v->viewport()->height();
        for (const Found& f : collect(top - height, top + 2 * height))
            shown |= f.embed->request().absPath == absPath;
        if (shown)
            v->refreshDecorations();
    });
}

HostedEmbeds::~HostedEmbeds() { disconnect(m_registryConn); }

HostedEmbeds* HostedEmbeds::of(EditorView* view) { return view ? view->findChild<HostedEmbeds*>(QString(), Qt::FindDirectChildrenOnly) : nullptr; }

QList<QWidget*> HostedEmbeds::liveWidgets() const
{
    QList<QWidget*> out;
    for (const Hosted& h : m_hosted)
        if (h.widget && !h.pooled)
            out << h.widget;
    return out;
}

bool HostedEmbeds::eventFilter(QObject* obj, QEvent* ev)
{
    Q_UNUSED(obj);
    switch (ev->type()) {
    case QEvent::Paint:
    case QEvent::Resize:
    case QEvent::Show:
        // Layout may have changed (edits, reveal, folding, width): reposition after this paint.
        scheduleSync();
        break;
    default:
        break;
    }
    return false;
}

void HostedEmbeds::scheduleSync()
{
    if (m_pending)
        return;
    m_pending = true;
    QMetaObject::invokeMethod(this, &HostedEmbeds::sync, Qt::QueuedConnection);
}

QList<HostedEmbeds::Found> HostedEmbeds::collect(double y0, double y1) const
{
    QList<Found> out;
    EditorView* v = m_view;
    if (!v || !v->document())
        return out;
    DocLayout& layout = v->layout();
    const int count = layout.lineCount();
    if (count == 0)
        return out;
    const Text& text = layout.text();
    const auto [clipFrom, clipTo] = v->visibleLineRange();
    for (int line = layout.lineAtY(std::max(0.0, y0)); line < count; ++line) {
        const double top = layout.lineTop(line);
        if (top > y1)
            break;
        if (layout.isHidden(line)) {
            line = std::max(line, layout.hiddenRunEnd(line) - 1); // skip a folded run
            continue;
        }
        if (line < clipFrom || (clipTo >= 0 && line > clipTo))
            continue;
        // Only units the view already measured: measuring here would move content above the viewport.
        if (!layout.isLaidOut(line) || layout.isInBlock(line))
            continue;
        const DocLayout::UnitLayout* ul = layout.layoutFor(line);
        if (!ul || ul->deco.hidden)
            continue;
        const auto& slotList = ul->block.widgets();
        for (qsizetype i = 0; i < slotList.size() && i < ul->widgetRects.size(); ++i) {
            const auto* ew = dynamic_cast<const EmbedWidget*>(slotList[i].widget.get());
            if (!ew)
                continue;
            if (ul->widgetRects[i].isEmpty())
                continue;
            const QRectF docRect = ul->widgetRects[i].translated(0, top);
            const QPointF tl = v->docToViewport(docRect.topLeft());
            Found f;
            f.embed = ew;
            f.rect = QRect(int(std::lround(tl.x())), int(std::lround(tl.y())), int(std::lround(docRect.width())),
                           int(std::lround(docRect.height())));
            f.line = line;
            f.docFrom = text.lineStart(line);
            for (const DisplayBlock::Segment& seg : ul->block.segments()) {
                if (seg.kind == DisplayBlock::Segment::Kind::Widget && seg.dispFrom == slotList[i].dispPos) {
                    f.docFrom += seg.docFrom;
                    break;
                }
            }
            out.push_back(f);
        }
    }
    return out;
}

QWidget* HostedEmbeds::create(const Found& f)
{
    EditorView* v = m_view;
    EmbedContext ctx;
    ctx.request = f.embed->request();
    ctx.view = v;
    ctx.livePreview = v->mode() == EditorView::Mode::LivePreview && !v->isReadOnly();
    auto slot = std::make_shared<QPointer<QWidget>>();
    QPointer<HostedEmbeds> self(this);
    ctx.revealSource = [self, slot] {
        // Obsidian's "Edit this block": select the embed's markdown (head at its start) and focus.
        if (!self || !*slot || !self->m_view)
            return;
        EditorView* view = self->m_view;
        for (const Hosted& h : std::as_const(self->m_hosted)) {
            if (h.widget != *slot || h.pooled || !view->document())
                continue;
            const Text& t = view->document()->text();
            qsizetype end = h.docFrom;
            // the embed's markdown ends at the next "]]" / ")" on its line
            const qsizetype lineEnd = t.lineEnd(t.lineAt(h.docFrom));
            const QString rest = t.slice(h.docFrom, lineEnd);
            const qsizetype close = rest.startsWith(QLatin1String("![[")) ? rest.indexOf(QLatin1String("]]")) : rest.indexOf(QLatin1Char(')'));
            end = close >= 0 ? h.docFrom + close + (rest.startsWith(QLatin1String("![[")) ? 2 : 1) : lineEnd;
            view->setFocus(Qt::OtherFocusReason);
            view->setSelection(Selection::single(end, h.docFrom), true);
            break;
        }
    };
    QWidget* w = f.embed->provider()->createWidget(ctx, v->viewport());
    if (!w)
        return nullptr;
    *slot = w;
    ++m_created;
    if (!w->testAttribute(Qt::WA_SetCursor))
        w->setCursor(Qt::ArrowCursor); // the viewport's I-beam would be inherited otherwise
    w->setProperty(kEditableProperty, ctx.livePreview);
    w->setGeometry(f.rect);
    w->show();
    return w;
}

void HostedEmbeds::retire(qsizetype index)
{
    if (QWidget* w = m_hosted[index].widget) {
        w->hide();
        w->deleteLater();
    }
    m_hosted.removeAt(index);
}

void HostedEmbeds::sync()
{
    m_pending = false;
    EditorView* v = m_view;
    if (!v || m_syncing)
        return;
    if (m_hosted.isEmpty() && EmbedRegistry::instance().isEmpty())
        return;
    const QScopedValueRollback guard(m_syncing, true);
    const double top = v->scrollTop();
    const double height = std::max(1, v->viewport()->height());
    // Live widgets for embeds within one screen of the viewport.
    const QList<Found> found = collect(top - height, top + 2 * height);
    const bool editable = v->mode() == EditorView::Mode::LivePreview && !v->isReadOnly();

    QList<bool> claimed(m_hosted.size(), false);
    for (const Found& f : found) {
        const QString key = f.embed->request().key();
        // the widget of the same content that was nearest (identical embeds keep their own widgets;
        // one whose embed left the range serves the next identical embed)
        qsizetype idx = -1;
        for (qsizetype i = 0; i < m_hosted.size(); ++i) {
            if (claimed[i] || !m_hosted[i].widget || m_hosted[i].key != key)
                continue;
            if (idx < 0 || std::abs(m_hosted[i].docFrom - f.docFrom) < std::abs(m_hosted[idx].docFrom - f.docFrom))
                idx = i;
        }
        if (idx < 0) {
            QWidget* w = create(f);
            if (!w)
                continue;
            Hosted h;
            h.key = key;
            h.widget = w;
            h.rect = f.rect;
            m_hosted.push_back(h);
            claimed.push_back(true);
            idx = m_hosted.size() - 1;
        } else {
            claimed[idx] = true;
        }
        Hosted& h = m_hosted[idx];
        h.line = f.line;
        h.docFrom = f.docFrom;
        h.pooled = false;
        h.rect = f.rect;
        QWidget* w = h.widget;
        if (w->geometry() != f.rect)
            w->setGeometry(f.rect);
        if (w->property(kEditableProperty).toBool() != editable)
            w->setProperty(kEditableProperty, editable);
        if (w->isHidden())
            w->show();
    }

    // Widgets without an embed in range: hide them, keep them a moment (a reveal toggling back, a
    // relayout, scrolling back), then destroy them unless they are busy (playing media).
    int pooled = 0;
    bool waiting = false;
    for (qsizetype i = m_hosted.size() - 1; i >= 0; --i) {
        if (claimed.value(i))
            continue;
        Hosted& h = m_hosted[i];
        if (!h.widget) {
            m_hosted.removeAt(i);
            continue;
        }
        if (!h.pooled) {
            h.pooled = true; // (docFrom kept: the nearest identical embed gets it back)
            h.line = -1;
            h.pooledSince.start();
            h.widget->hide();
        }
        const bool busy = h.widget->property(kBusyProperty).toBool();
        if (busy)
            continue;
        if (h.pooledSince.elapsed() >= kPoolMs || ++pooled > kMaxPooled)
            retire(i);
        else
            waiting = true;
    }
    if (waiting)
        QTimer::singleShot(kPoolMs + 20, this, &HostedEmbeds::scheduleSync);
}

} // namespace oc
