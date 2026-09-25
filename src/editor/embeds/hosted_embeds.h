#pragma once

// Embeds of registered file types (PDF, audio, video) inside rendered Markdown.
//
// EmbedWidget is the inline widget the Live Preview / Reading decorator puts in place of
// "![[doc.pdf]]": it reserves the provider's size in the line and paints a static rendering.
// HostedEmbeds (one per EditorView, created by the view) keeps the providers' live QWidgets over
// those placeholders: widgets are created only for embeds laid out within one screen of the
// viewport, follow scrolling and relayout synchronously, are matched to embeds by content
// (file + subpath, the nearest previous position first) so edits, reveals and mode switches keep
// them (a playing video keeps playing), stay pooled hidden for a moment when their embed
// disappears (serving the next identical embed that comes into range), and are destroyed once
// pooled for kPoolMs (unless busy, e.g. playing media).

#include "editor/embeds/embed_registry.h"
#include "render/layout/decoration.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>

namespace oc {

class EditorView;

class EmbedWidget : public InlineWidget {
public:
    EmbedWidget(EmbedRequest request, std::shared_ptr<EmbedProvider> provider, qreal maxWidth);

    QSizeF size(const LineMetrics& lm) const override { Q_UNUSED(lm); return m_size; }
    qreal ascent(const LineMetrics& lm) const override { Q_UNUSED(lm); return m_size.height(); } // on the baseline
    void paint(QPainter* p, const QRectF& rect, const LineMetrics& lm) const override;
    quint64 signature() const override { return m_signature; }
    QString accessibleText() const override { return {}; }

    const EmbedRequest& request() const { return m_request; }
    const std::shared_ptr<EmbedProvider>& provider() const { return m_provider; }

private:
    EmbedRequest m_request;
    std::shared_ptr<EmbedProvider> m_provider;
    QSizeF m_size;
    quint64 m_signature = 0;
};

class HostedEmbeds : public QObject {
    Q_OBJECT
public:
    explicit HostedEmbeds(EditorView* view);
    ~HostedEmbeds() override;

    static HostedEmbeds* of(EditorView* view);

    void sync();         // create / position / retire widgets now
    void scheduleSync(); // coalesced, after pending events

    struct Hosted {
        QString key;
        QPointer<QWidget> widget;
        QRect rect;           // viewport coordinates
        int line = -1;        // document line of the embed (-1 while pooled)
        qsizetype docFrom = 0; // document offset of the embed's markdown
        bool pooled = false;  // its embed disappeared; hidden, kept for a moment
        QElapsedTimer pooledSince;
    };
    const QList<Hosted>& hosted() const { return m_hosted; }
    QList<QWidget*> liveWidgets() const; // shown widgets (tests)
    int createdCount() const { return m_created; }

    // A hosted widget that sets this dynamic property to true (e.g. playing media) is not destroyed
    // when scrolled far away or when its embed disappears; it is only hidden.
    static constexpr const char* kBusyProperty = "ocEmbedBusy";
    static constexpr int kPoolMs = 1500; // how long a widget without embed stays pooled

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Found {
        const EmbedWidget* embed = nullptr;
        QRect rect;
        int line = -1;
        qsizetype docFrom = 0;
    };
    QList<Found> collect(double y0, double y1) const;
    QWidget* create(const Found& f);
    void retire(qsizetype index);

    QPointer<EditorView> m_view;
    QList<Hosted> m_hosted;
    bool m_pending = false;
    bool m_syncing = false;
    int m_created = 0;
    QMetaObject::Connection m_registryConn;
};

} // namespace oc
