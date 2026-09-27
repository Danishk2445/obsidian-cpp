#include "ui/views/bases/bases_list.h"

#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_style.h"

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

ListLayout::ListLayout(BasesController* controller, QWidget* parent)
    : BaseLayout(controller, parent)
{
    setObjectName(QStringLiteral("ocBasesList"));
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void ListLayout::setData(const ViewData& data)
{
    storeData(data);
    relayout();
    viewport()->update();
}

// Obsidian's W5.render(): empty values (null, [], "", {}) are left out.
QStringList ListLayout::visibleProps(const EntryPtr& e) const
{
    QStringList out;
    for (const QString& prop : m_data.columns) {
        const Value v = e->valueOrError(prop);
        if (v.isNull())
            continue;
        if (v.is(ValueClass::List) && v.items().isEmpty())
            continue;
        if (v.is(ValueClass::String) && v.text().isEmpty())
            continue;
        if (v.type() == ValueType::Object && v.objectData()->keys.isEmpty())
            continue;
        out << prop;
    }
    return out;
}

int ListLayout::itemHeight(const EntryPtr& e) const
{
    const Style& s = basesStyle(isEmbedded());
    const qsizetype props = visibleProps(e).size();
    if (props == 0)
        return 0;
    const int lines = m_indent ? int(props) : 1;
    return lines * m_lineHeight + 2 * s.listSpacing;
}

void ListLayout::relayout()
{
    const Style& s = basesStyle(isEmbedded());
    const BaseView* v = m_controller ? m_controller->currentView() : nullptr;
    m_markers = QStringLiteral("bullet");
    m_separator = QStringLiteral(", ");
    m_indent = false;
    if (v) {
        const QString markers = v->option(QStringLiteral("markers")).toString();
        if (markers == u"bullet" || markers == u"number" || markers == u"none")
            m_markers = markers;
        const YamlNode sep = v->option(QStringLiteral("separator"));
        if (!sep.isNull())
            m_separator = sep.isString() ? sep.toString() : scalarText(sep);
        if (m_separator.isEmpty())
            m_separator = QStringLiteral(", ");
        m_indent = v->option(QStringLiteral("indentProperties")).toBool();
    }
    const bool grouped = v && v->groupBy;
    m_lineHeight = int(std::ceil(QFontMetricsF(s.listFont).height() * 1.5));
    if (isEmbedded()) {
        m_padLeft = m_padRight = m_padTop = m_padBottom = 0;
    } else {
        m_padTop = grouped ? 0 : 8;
        m_padLeft = grouped ? 16 : 8;
        m_padRight = 16;
        m_padBottom = 60;
    }
    m_groups.clear();
    int y = m_padTop;
    for (int g = 0; g < m_data.groups.size(); ++g) {
        GroupGeom gg;
        gg.headingTop = y;
        gg.headingHeight = grouped ? s.rowHeight : 0;
        y += gg.headingHeight;
        for (const EntryPtr& e : m_data.groups[g].entries) {
            const int h = itemHeight(e);
            gg.itemTops << y;
            gg.itemHeights << h;
            y += h;
        }
        m_groups << gg;
    }
    m_contentHeight = m_data.rows.isEmpty() ? 0 : y + m_padBottom;
    if (m_contentHeight < 100 && !m_data.rows.isEmpty())
        m_contentHeight = std::max(m_contentHeight, isEmbedded() ? y : 100); // .bases-list-container min-height
    updateScrollBars(viewport()->width(), m_contentHeight);
    emit contentHeightChanged();
}

QRect ListLayout::itemRect(int group, int index) const
{
    if (group < 0 || group >= m_groups.size() || index < 0 || index >= m_groups[group].itemTops.size())
        return {};
    return QRect(m_padLeft - scrollX(), m_groups[group].itemTops[index] - scrollY(), viewport()->width() - m_padLeft - m_padRight,
                 m_groups[group].itemHeights[index]);
}

EntryPtr ListLayout::entryAt(const QPoint& p) const
{
    for (int g = 0; g < m_groups.size(); ++g)
        for (int i = 0; i < m_groups[g].itemTops.size(); ++i)
            if (itemRect(g, i).contains(p))
                return m_data.groups[g].entries[i];
    return nullptr;
}

void ListLayout::paintEvent(QPaintEvent*)
{
    if (!m_controller)
        return;
    const Style& s = basesStyle(isEmbedded());
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QRect vr = viewport()->rect();
    p.fillRect(vr, s.bgPrimary);
    m_hits.clear();
    const BaseView* v = m_controller->currentView();
    const QFontMetricsF fm(s.listFont);
    const qreal ch = fm.horizontalAdvance(u'0');
    const qreal indent = currentTheme()->px(QStringLiteral("--list-indent-editing"), s.em, s.em * 0.75);
    const qreal contentLeft = 2 * ch + indent; // margin-inline-start: 2ch + padding-inline-start
    int number = 0;
    for (int g = 0; g < m_groups.size(); ++g) {
        const GroupGeom& gg = m_groups[g];
        const ViewGroup& group = m_data.groups[g];
        number = 0;
        if (gg.headingHeight > 0) {
            const int top = gg.headingTop - scrollY();
            if (top < vr.height() && top + gg.headingHeight > 0) {
                qreal x = m_padLeft - scrollX();
                const QString label = v && v->groupBy ? m_controller->base().displayName(v->groupBy->property) : QString();
                p.setFont(s.groupPropertyFont);
                p.setPen(s.muted);
                const qreal lw = QFontMetricsF(s.groupPropertyFont).horizontalAdvance(label);
                p.drawText(QRectF(x, top, lw + 1, gg.headingHeight), Qt::AlignLeft | Qt::AlignVCenter, label);
                x += lw + 4;
                const QRectF vrect(x, top, vr.width() - x, gg.headingHeight);
                if (!group.hasKey) {
                    p.setFont(s.groupValueFont);
                    p.setPen(s.text);
                    p.drawText(vrect, Qt::AlignLeft | Qt::AlignVCenter, tr("None"));
                } else {
                    paintValue(&p, vrect, group.key, m_controller->paintOptions(s.groupValueFont, s.text), &m_hits);
                }
            }
        }
        for (int i = 0; i < gg.itemTops.size(); ++i) {
            if (gg.itemHeights[i] == 0)
                continue;
            ++number;
            const QRect r = itemRect(g, i);
            if (r.bottom() < 0 || r.top() > vr.height())
                continue;
            const EntryPtr& e = group.entries[i];
            const QStringList props = visibleProps(e);
            const qreal x0 = r.left() + contentLeft;
            const qreal lineTop = r.top() + s.listSpacing;
            // marker
            if (m_markers == u"bullet") {
                const qreal d = std::max<qreal>(4, s.em * 0.3);
                p.setPen(Qt::NoPen);
                p.setBrush(s.listMarker);
                p.drawEllipse(QPointF(x0 - s.em * 0.4, lineTop + m_lineHeight / 2.0), d / 2, d / 2);
            } else if (m_markers == u"number") {
                p.setFont(s.cellFont);
                p.setPen(s.listMarker);
                const QString num = QString::number(number) + u'.';
                p.drawText(QRectF(x0 - s.em * 3 - 2, lineTop, s.em * 3 - 2, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter, num);
            }
            const qreal right = r.right();
            if (!m_indent) {
                qreal x = x0;
                for (int k = 0; k < props.size() && x < right; ++k) {
                    if (k > 0) {
                        p.setFont(s.cellFont);
                        p.setPen(s.muted);
                        const qreal sw = QFontMetricsF(s.cellFont).horizontalAdvance(m_separator);
                        p.drawText(QRectF(x, lineTop, sw + 1, m_lineHeight), Qt::AlignLeft | Qt::AlignVCenter, m_separator);
                        x += sw;
                    }
                    const QString& prop = props[k];
                    const Value val = prop == u"file.name" ? Value::file(m_controller->runner()->dataSource(), e->path()) : e->valueOrError(prop);
                    const PaintOptions o = m_controller->paintOptions(s.listFont, s.text);
                    const qreal w = measureValue(val, o);
                    paintValue(&p, QRectF(x, lineTop, std::max<qreal>(0, right - x), m_lineHeight), val, o, &m_hits);
                    x += w;
                }
            } else {
                for (int k = 0; k < props.size(); ++k) {
                    const QString& prop = props[k];
                    const qreal y = lineTop + k * m_lineHeight;
                    qreal x = x0;
                    if (k > 0) {
                        x += indent + ch * 2;
                        p.setFont(s.cellFont);
                        p.setPen(s.listMarker);
                        p.drawText(QRectF(x - s.em * 0.8, y, s.em * 0.8, m_lineHeight), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("-"));
                    }
                    const Value val = prop == u"file.name" ? Value::file(m_controller->runner()->dataSource(), e->path()) : e->valueOrError(prop);
                    paintValue(&p, QRectF(x, y, std::max<qreal>(0, right - x), m_lineHeight), val, m_controller->paintOptions(s.listFont, s.text),
                               &m_hits);
                }
            }
        }
    }
}

void ListLayout::resizeEvent(QResizeEvent* e)
{
    BaseLayout::resizeEvent(e);
    updateScrollBars(viewport()->width(), m_contentHeight);
}

void ListLayout::scrollContentsBy(int, int) { viewport()->update(); }

void ListLayout::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton)
        return;
    const QPoint pos = e->position().toPoint();
    const Qt::KeyboardModifiers mods = e->button() == Qt::MiddleButton ? Qt::ControlModifier : e->modifiers();
    for (const ValueHit& h : std::as_const(m_hits)) {
        if (!h.rect.adjusted(-1, -2, 1, 2).contains(pos))
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
}

void ListLayout::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    const ValueHit* hit = nullptr;
    for (const ValueHit& h : std::as_const(m_hits))
        if (h.rect.adjusted(-1, -2, 1, 2).contains(pos))
            hit = &h;
    viewport()->setCursor(hit ? Qt::PointingHandCursor : Qt::IBeamCursor);
    const QString target = hit && (hit->kind == ValueHit::Kind::Link || hit->kind == ValueHit::Kind::File) ? hit->target : QString();
    if (target != m_hoverTarget) {
        m_hoverTarget = target;
        if (target.isEmpty())
            m_controller->unhoverLink(this);
        else
            m_controller->hoverLink(target, hit->sourcePath, QRect(viewport()->mapToGlobal(hit->rect.topLeft().toPoint()), hit->rect.size().toSize()),
                                    this);
    }
}

void ListLayout::leaveEvent(QEvent* e)
{
    if (!m_hoverTarget.isEmpty()) {
        m_hoverTarget.clear();
        m_controller->unhoverLink(this);
    }
    BaseLayout::leaveEvent(e);
}

void ListLayout::contextMenuEvent(QContextMenuEvent* e)
{
    for (const ValueHit& h : std::as_const(m_hits)) {
        if (!h.rect.contains(e->pos()))
            continue;
        if (h.kind == ValueHit::Kind::File) {
            m_controller->showFileMenu(h.target, e->globalPos(), this);
            return;
        }
    }
    if (const EntryPtr entry = entryAt(e->pos()))
        m_controller->showFileMenu(entry->path(), e->globalPos(), this);
}

} // namespace oc::basesui
