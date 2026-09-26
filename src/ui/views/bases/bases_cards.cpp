#include "ui/views/bases/bases_cards.h"

#include "render/services/image_cache.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_style.h"

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

CardsLayout::CardsLayout(BasesController* controller, QWidget* parent)
    : BaseLayout(controller, parent)
{
    setObjectName(QStringLiteral("ocBasesCards"));
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(&ImageCache::instance(), &ImageCache::imageReady, this, [this] { viewport()->update(); });
}

void CardsLayout::setData(const ViewData& data)
{
    storeData(data);
    relayout();
    viewport()->update();
}

void CardsLayout::relayout()
{
    const Style& s = basesStyle(isEmbedded());
    const BaseView* v = m_controller ? m_controller->currentView() : nullptr;
    m_imageProp.clear();
    m_contain = false;
    m_aspect = 1;
    m_size = 200;
    if (v) {
        const YamlNode image = v->option(QStringLiteral("image"));
        if (image.isString() && !image.toString().isEmpty())
            m_imageProp = propertyIdFromWritten(image.toString());
        m_contain = v->option(QStringLiteral("imageFit")).toString() == u"contain";
        const YamlNode ar = v->option(QStringLiteral("imageAspectRatio"));
        if (ar.isNumber() && ar.toNumber() > 0)
            m_aspect = ar.toNumber();
        const YamlNode size = v->option(QStringLiteral("cardSize"));
        if (size.isNumber() && size.toNumber() > 0)
            m_size = int(size.toNumber());
    }
    const double scale = currentTheme()->number(QStringLiteral("--bases-cards-scale"), 1);
    const double cardSize = m_size * (scale > 0 ? scale : 1);
    if (isEmbedded()) {
        m_padX = 1;
        m_padTop = m_padBottom = 4;
    } else {
        m_padX = m_padTop = m_padBottom = 12;
    }
    const qreal avail = std::max<qreal>(1, viewport()->width() - 2 * m_padX);
    m_perRow = std::max(1, int(std::floor(avail / cardSize)));
    m_cardWidth = (avail - s.cardGap * (m_perRow - 1)) / m_perRow;
    const QFontMetricsF label(s.cardLabelFont);
    m_titleHeight = s.cardLineHeight;
    m_propHeight = std::ceil(label.height()) + s.cardLineHeight;
    m_coverHeight = m_imageProp.isEmpty() ? 0 : m_cardWidth * m_aspect;
    const qsizetype props = m_data.columns.size();
    m_cardHeight = m_coverHeight;
    if (props > 0)
        m_cardHeight += s.cardPadTop + s.cardPadBottom + m_titleHeight;
    if (props > 1)
        m_cardHeight += (m_propHeight + s.cardPropGap) * (props - 1);
    // groups
    m_groups.clear();
    const bool grouped = v && v->groupBy;
    const qreal headingH = grouped ? s.rowHeight + 4 + 8 : 0;
    const qreal groupPadBottom = grouped ? 16 : 0;
    const qreal groupGap = 8;
    qreal y = m_padTop;
    for (int g = 0; g < m_data.groups.size(); ++g) {
        GroupGeom gg;
        gg.count = int(m_data.groups[g].entries.size());
        gg.headingTop = y;
        gg.headingHeight = headingH;
        y += headingH;
        const int rows = int(std::ceil(double(gg.count) / m_perRow));
        gg.top = y;
        gg.height = gg.count == 0 ? 0 : rows * m_cardHeight + (rows - 1) * s.cardGap;
        y += gg.height + groupPadBottom;
        m_groups << gg;
        if (g < m_data.groups.size() - 1)
            y += groupGap;
    }
    m_contentHeight = m_data.rows.isEmpty() ? 0 : int(std::ceil(y + m_padBottom));
    updateScrollBars(viewport()->width(), m_contentHeight);
    emit contentHeightChanged();
}

QRectF CardsLayout::cardRect(int group, int index) const
{
    if (group < 0 || group >= m_groups.size())
        return {};
    const Style& s = basesStyle(isEmbedded());
    const int row = index / m_perRow, col = index % m_perRow;
    const qreal x = m_padX + col * (m_cardWidth + s.cardGap) - scrollX();
    const qreal y = m_groups[group].top + row * (m_cardHeight + s.cardGap) - scrollY();
    return QRectF(x, y, m_cardWidth, m_cardHeight);
}

EntryPtr CardsLayout::entryAt(const QPoint& p) const
{
    for (int g = 0; g < m_groups.size(); ++g) {
        const QList<EntryPtr>& entries = m_data.groups[g].entries;
        for (int i = 0; i < entries.size(); ++i)
            if (cardRect(g, i).contains(p))
                return entries[i];
    }
    return nullptr;
}

void CardsLayout::paintCard(QPainter& p, const QRectF& r, const EntryPtr& e, bool hover)
{
    const Style& s = basesStyle(isEmbedded());
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;
    shape.addRoundedRect(r, s.cardRadius, s.cardRadius);
    p.fillPath(shape, s.cardsBg);
    p.setClipPath(shape);
    qreal y = r.top();
    if (!m_imageProp.isEmpty()) {
        const QRectF cover(r.left(), y, r.width(), m_coverHeight);
        p.fillRect(cover, s.cardsCoverBg);
        Value iv = e->valueOrError(m_imageProp);
        while (iv.is(ValueClass::List) && !iv.items().isEmpty())
            iv = iv.items().first();
        QString path;
        QColor color;
        if (iv.is(ValueClass::File)) {
            path = iv.filePath();
        } else if (iv.type() == ValueType::Link) {
            path = iv.resolve();
        } else if (iv.is(ValueClass::String)) {
            static const QRegularExpression hex(QStringLiteral("^#[a-fA-F0-9]{6}$"));
            if (hex.match(iv.text()).hasMatch())
                color = QColor(iv.text());
            else if (!iv.text().startsWith(QLatin1String("http")))
                path = m_controller->runner()->dataSource()->resolveLink(iv.text(), e->path());
        }
        if (color.isValid()) {
            p.fillRect(cover, color);
        } else if (!path.isEmpty()) {
            const QString abs = m_controller->imagePath(path);
            if (!abs.isEmpty()) {
                const QSize natural = ImageCache::instance().naturalSize(abs);
                if (natural.isValid() && natural.width() > 0 && natural.height() > 0) {
                    const qreal sx = cover.width() / natural.width(), sy = cover.height() / natural.height();
                    const qreal scale = m_contain ? std::min(sx, sy) : std::max(sx, sy);
                    const QSizeF sz(natural.width() * scale, natural.height() * scale);
                    const QRectF target(cover.center().x() - sz.width() / 2, cover.center().y() - sz.height() / 2, sz.width(), sz.height());
                    const std::shared_ptr<const QImage> img =
                        ImageCache::instance().image(abs, int(std::ceil(sz.width())), viewport()->devicePixelRatioF());
                    if (img && !img->isNull()) {
                        p.save();
                        p.setClipRect(cover, Qt::IntersectClip);
                        p.drawImage(target, *img);
                        p.restore();
                    }
                }
            }
        }
        y += m_coverHeight;
    }
    y += s.cardPadTop;
    const QStringList& props = m_data.columns;
    for (int i = 0; i < props.size(); ++i) {
        const QString& prop = props[i];
        const bool title = i == 0;
        const qreal h = title ? m_titleHeight : m_propHeight;
        const QRectF pr(r.left() + s.cardPadX, y, r.width() - 2 * s.cardPadX, h);
        QRectF line = pr;
        if (!title) {
            const QFontMetricsF lf(s.cardLabelFont);
            p.setFont(s.cardLabelFont);
            p.setPen(s.muted);
            const QRectF lr(pr.left() + 8, pr.top(), pr.width() - 16, std::ceil(lf.height()));
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, lf.elidedText(m_controller->base().displayName(prop), Qt::ElideRight, lr.width()));
            line = QRectF(pr.left(), lr.bottom(), pr.width(), s.cardLineHeight);
        }
        const QRectF content = line.adjusted(8, 0, -8, 0);
        PaintOptions o = m_controller->paintOptions(title ? s.cardTitleFont : s.cardFont, s.text);
        qreal w = 0;
        if (title && prop == u"file.name") {
            // the title is plain text (pointer-events: none; --link-color: --text-normal)
            p.setFont(s.cardTitleFont);
            p.setPen(s.text);
            const QString name = e->valueOrError(prop).toString();
            p.drawText(content, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(s.cardTitleFont).elidedText(name, Qt::ElideRight, content.width()));
            w = 1;
        } else if (parsePropertyId(prop).type == u"note") {
            const QString key = parsePropertyId(prop).name;
            const QVariant raw = e->rawProperty(key);
            QList<ValueHit>* hits = title ? nullptr : &m_hits;
            w = paintProperty(&p, content, raw, m_controller->propertyType(key, raw), e->path(), o, hits);
        } else {
            Value v = prop == u"file.name" ? Value::file(m_controller->runner()->dataSource(), e->path()) : e->valueOrError(prop);
            w = paintValue(&p, content, v, o, title ? nullptr : &m_hits);
        }
        if (w <= 0) {
            p.setFont(o.font);
            p.setPen(s.faint);
            p.drawText(line.adjusted(title ? 8 : 0, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("—"));
        }
        y += h + s.cardPropGap;
    }
    p.setClipping(false);
    p.setPen(QPen(hover ? s.cardsShadowHover : s.cardsShadow, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), s.cardRadius, s.cardRadius);
    p.restore();
}

void CardsLayout::paintEvent(QPaintEvent*)
{
    if (!m_controller)
        return;
    const Style& s = basesStyle(isEmbedded());
    QPainter p(viewport());
    const QRect vr = viewport()->rect();
    p.fillRect(vr, s.bgPrimary);
    m_hits.clear();
    const BaseView* v = m_controller->currentView();
    for (int g = 0; g < m_groups.size(); ++g) {
        const GroupGeom& gg = m_groups[g];
        const ViewGroup& group = m_data.groups[g];
        if (gg.headingHeight > 0) {
            const qreal top = gg.headingTop - scrollY() + 4;
            if (top < vr.height() && top + s.rowHeight > 0) {
                qreal x = m_padX - scrollX();
                const QString label = v && v->groupBy ? m_controller->base().displayName(v->groupBy->property) : QString();
                p.setFont(s.groupPropertyFont);
                p.setPen(s.muted);
                const qreal lw = QFontMetricsF(s.groupPropertyFont).horizontalAdvance(label);
                p.drawText(QRectF(x, top, lw + 1, s.rowHeight), Qt::AlignLeft | Qt::AlignVCenter, label);
                x += lw + 4;
                const QRectF vrect(x, top, vr.width() - x, s.rowHeight);
                if (!group.hasKey) {
                    p.setFont(s.groupValueFont);
                    p.setPen(s.text);
                    p.drawText(vrect, Qt::AlignLeft | Qt::AlignVCenter, tr("None"));
                } else {
                    paintValue(&p, vrect, group.key, m_controller->paintOptions(s.groupValueFont, s.text), &m_hits);
                }
            }
        }
        if (v && v->groupBy) {
            const qreal borderY = gg.top + gg.height + 16 - scrollY();
            p.fillRect(QRectF(m_padX - scrollX(), borderY - 1, vr.width() - 2 * m_padX, 1), s.border);
        }
        const int rows = int(std::ceil(double(gg.count) / m_perRow));
        const qreal rowStep = m_cardHeight + s.cardGap;
        const int firstRow = std::max(0, int(std::floor((scrollY() - gg.top) / rowStep)));
        const int lastRow = std::min(rows - 1, int(std::floor((scrollY() + vr.height() - gg.top) / rowStep)));
        for (int row = firstRow; row <= lastRow; ++row) {
            for (int col = 0; col < m_perRow; ++col) {
                const int i = row * m_perRow + col;
                if (i >= gg.count)
                    break;
                const QRectF r = cardRect(g, i);
                const EntryPtr& e = group.entries[i];
                paintCard(p, r, e, e->path() == m_hoverPath);
            }
        }
    }
}

void CardsLayout::resizeEvent(QResizeEvent* e)
{
    BaseLayout::resizeEvent(e);
    relayout();
}

void CardsLayout::scrollContentsBy(int, int) { viewport()->update(); }

void CardsLayout::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton)
        return;
    const QPoint pos = e->position().toPoint();
    const Qt::KeyboardModifiers mods = e->button() == Qt::MiddleButton ? Qt::ControlModifier : e->modifiers();
    for (const ValueHit& h : std::as_const(m_hits)) {
        if (!h.rect.contains(pos))
            continue;
        if (h.kind == ValueHit::Kind::Link)
            m_controller->openLink(h.target, h.sourcePath, mods);
        else if (h.kind == ValueHit::Kind::File)
            m_controller->openFile(h.target, mods);
        else if (h.kind == ValueHit::Kind::Url)
            m_controller->openUrl(h.target);
        else
            m_controller->openTag(h.target);
        return;
    }
    if (const EntryPtr entry = entryAt(pos))
        m_controller->openFile(entry->path(), mods);
}

void CardsLayout::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    const EntryPtr entry = entryAt(pos);
    const QString path = entry ? entry->path() : QString();
    viewport()->setCursor(entry ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (path != m_hoverPath) {
        m_hoverPath = path;
        viewport()->update();
        if (path.isEmpty()) {
            m_controller->unhoverLink(this);
        } else {
            QRectF r;
            for (int g = 0; g < m_groups.size(); ++g)
                for (int i = 0; i < m_data.groups[g].entries.size(); ++i)
                    if (m_data.groups[g].entries[i] == entry)
                        r = cardRect(g, i);
            m_controller->hoverLink(path, QString(), QRect(viewport()->mapToGlobal(r.topLeft().toPoint()), r.size().toSize()), this);
        }
    }
}

void CardsLayout::leaveEvent(QEvent* e)
{
    if (!m_hoverPath.isEmpty()) {
        m_hoverPath.clear();
        m_controller->unhoverLink(this);
        viewport()->update();
    }
    BaseLayout::leaveEvent(e);
}

void CardsLayout::contextMenuEvent(QContextMenuEvent* e)
{
    if (const EntryPtr entry = entryAt(e->pos()))
        m_controller->showFileMenu(entry->path(), e->globalPos(), this);
}

} // namespace oc::basesui
