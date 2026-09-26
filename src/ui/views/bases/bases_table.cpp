#include "ui/views/bases/bases_table.h"

#include "core/properties/property_types.h"
#include "core/vault_session.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_menus.h"
#include "ui/views/bases/bases_style.h"
#include "ui/views/markdown/properties/property_editors.h"
#include "core/util/owned_menu.h"
#include "ui/workspace/file_menu.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

namespace {

constexpr int kResizerWidth = 6;

QString propName(const QString& id) { return parsePropertyId(id).name; }
bool isNoteProp(const QString& id) { return parsePropertyId(id).type == u"note"; }

// The icon of a property id (header, menus).
QString columnIcon(BasesController* c, const QString& id)
{
    const PropertyRef r = parsePropertyId(id);
    if (r.type == u"formula")
        return QStringLiteral("square-function");
    if (r.type == u"file")
        return QStringLiteral("info");
    return c ? c->propertyIcon(r.name) : QStringLiteral("text");
}

int rowMultiplier(const BaseView* v)
{
    if (!v)
        return 1;
    const QString h = v->option(QStringLiteral("rowHeight")).toString();
    if (h == u"medium")
        return 2;
    if (h == u"tall")
        return 4;
    if (h == u"extra")
        return 8;
    return 1;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TableLayout
// ---------------------------------------------------------------------------------------------
TableLayout::TableLayout(BasesController* controller, QWidget* parent)
    : BaseLayout(controller, parent)
{
    setObjectName(QStringLiteral("ocBasesTable"));
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!m_editor || !now)
            return;
        if (now == m_editor || m_editor->isAncestorOf(now))
            return;
        for (QWidget* w = now; w; w = w->parentWidget())
            if (w->property("ocCapturesKeys").toBool() || w->objectName() == u"ocOverlayLayer")
                return; // a suggest popup of the editor
        QTimer::singleShot(0, this, [this] { closeEditor(); });
    });
}

TableLayout::~TableLayout() = default;

PropertyValueEditor* TableLayout::editor() const { return m_editor; }

void TableLayout::setData(const ViewData& data)
{
    // keep the entry being edited
    QString editingPath;
    if (m_editor && m_editing.valid())
        if (const EntryPtr e = entryAt(m_editing))
            editingPath = e->path();
    QString activePath;
    if (m_active.valid())
        if (const EntryPtr e = entryAt(m_active))
            activePath = e->path();
    storeData(data);
    measure();
    auto find = [&](const QString& path, int column) {
        CellPos pos;
        for (int g = 0; g < m_data.groups.size(); ++g)
            for (int r = 0; r < m_data.groups[g].entries.size(); ++r)
                if (m_data.groups[g].entries[r]->path() == path) {
                    pos.group = g;
                    pos.row = r;
                    pos.column = std::min(column, int(m_columns.size()) - 1);
                    return pos;
                }
        return pos;
    };
    if (!activePath.isEmpty())
        m_active = find(activePath, m_active.column);
    else
        m_active = {};
    if (m_editor) {
        const CellPos pos = editingPath.isEmpty() ? CellPos() : find(editingPath, m_editing.column);
        if (pos.valid() && pos.column == m_editing.column) {
            m_editing = pos;
            placeEditor();
        } else {
            closeEditor();
        }
    }
    viewport()->update();
    emit contentHeightChanged();
}

void TableLayout::measure()
{
    const BaseView* v = m_controller ? m_controller->currentView() : nullptr;
    const Style& s = basesStyle(isEmbedded());
    m_columns = m_data.columns;
    m_rowHeight0 = s.rowHeight;
    const int mult = rowMultiplier(v);
    m_rowHeight = s.rowHeight * mult;
    m_multiline = mult > 1;
    m_grouped = v && v->groupBy;
    m_customWidths.clear();
    if (v) {
        const YamlNode sizes = v->option(QStringLiteral("columnSize"));
        for (const YamlNode::Entry& e : sizes.entries())
            if (e.second.isNumber() && e.second.toNumber() > 0)
                m_customWidths.insert(propertyIdFromWritten(e.first), int(std::lround(e.second.toNumber())));
    }
    // footer / group summaries: the view has summaries for a visible column
    m_footer = false;
    if (v && v->summaries)
        for (const auto& [id, name] : *v->summaries)
            if (m_columns.contains(id) || v->columns().contains(id))
                m_footer = true;
    m_groupSummaries = m_footer && m_data.groups.size() > 1;
    // content widths: the longest value (by text) of each column, rendered
    m_contentWidths.clear();
    m_headerWidths.clear();
    const QFontMetricsF hfm(s.headerFont);
    for (const QString& prop : std::as_const(m_columns)) {
        int hw = s.cellPadX * 2 + s.iconXs + 4 + int(std::ceil(hfm.horizontalAdvance(m_controller ? m_controller->base().displayName(prop) : prop)));
        if (v)
            for (const SortSpec& sp : v->sortSpecs())
                if (sp.property == prop)
                    hw += 16;
        m_headerWidths.insert(prop, hw);
        if (m_customWidths.contains(prop))
            continue;
        EntryPtr longest;
        int longestLen = 0;
        for (const EntryPtr& e : std::as_const(m_data.rows)) {
            QString text;
            if (isNoteProp(prop)) {
                const QVariant raw = e->rawProperty(propName(prop));
                text = raw.typeId() == QMetaType::QVariantList ? raw.toStringList().join(QStringLiteral("  ")) : raw.toString();
                if (raw.typeId() == QMetaType::Double)
                    text = Value::jsNumber(raw.toDouble());
            } else {
                const Value val = e->valueOrError(prop);
                text = val.isNull() ? QString() : val.toString();
            }
            if (text.size() > longestLen) {
                longestLen = int(text.size());
                longest = e;
            }
        }
        int cw = 0;
        if (longest) {
            const PaintOptions o = m_controller->paintOptions(s.cellFont, s.text);
            if (isNoteProp(prop)) {
                const QVariant raw = longest->rawProperty(propName(prop));
                cw = int(std::ceil(measureProperty(raw, m_controller->propertyType(propName(prop), raw), o)));
            } else {
                cw = int(std::ceil(measureValue(longest->valueOrError(prop), o)));
            }
            cw += 2 * s.cellPadX;
        }
        m_contentWidths.insert(prop, cw);
    }
    computeWidths();
    layoutGroups();
}

void TableLayout::computeWidths()
{
    const Style& s = basesStyle(isEmbedded());
    m_widths.clear();
    int total = 0;
    bool lastCustom = false;
    for (int i = 0; i < m_columns.size(); ++i) {
        const QString& prop = m_columns[i];
        int w;
        if (m_customWidths.contains(prop)) {
            w = std::max(s.colMinWidth, m_customWidths.value(prop));
            lastCustom = i == m_columns.size() - 1;
        } else {
            w = std::clamp(std::max(m_contentWidths.value(prop), m_headerWidths.value(prop)) + 1, s.colMinWidth, s.colMaxWidth);
            if (i == m_columns.size() - 1)
                lastCustom = false;
        }
        m_widths << w;
        total += w;
    }
    const int avail = viewport()->width() - 1;
    if (!lastCustom && !m_widths.isEmpty() && total < avail)
        m_widths.last() += avail - total;
}

void TableLayout::layoutGroups()
{
    m_groups.clear();
    const Style& s = basesStyle(isEmbedded());
    int y = headerHeight();
    for (int g = 0; g < m_data.groups.size(); ++g) {
        GroupGeom gg;
        gg.top = y;
        gg.heading = m_grouped ? m_rowHeight0 : 0;
        gg.summary = m_groupSummaries ? m_rowHeight0 : 0;
        gg.rowsTop = y + gg.heading + gg.summary;
        gg.rows = int(m_data.groups[g].entries.size());
        m_groups << gg;
        y = gg.rowsTop + gg.rows * m_rowHeight;
        if (g < m_data.groups.size() - 1)
            y += s.groupGap;
    }
    m_bodyHeight = y - headerHeight();
    updateScrollBars(totalWidth(), contentHeight());
}

int TableLayout::contentHeight() const { return headerHeight() + m_bodyHeight + (m_footer ? m_rowHeight0 : 0); }

int TableLayout::totalWidth() const
{
    int t = 0;
    for (int w : m_widths)
        t += w;
    return t;
}

int TableLayout::columnLeft(int column) const
{
    int x = 0;
    for (int i = 0; i < column && i < m_widths.size(); ++i)
        x += m_widths[i];
    return x - scrollX();
}

int TableLayout::groupTop(int group) const { return group >= 0 && group < m_groups.size() ? m_groups[group].top : 0; }

int TableLayout::rowViewportTop(int group, int row) const
{
    if (group < 0 || group >= m_groups.size())
        return 0;
    return m_groups[group].rowsTop + row * m_rowHeight - scrollY();
}

int TableLayout::footerTop() const
{
    const int natural = headerHeight() + m_bodyHeight - scrollY();
    return std::min(natural, viewport()->height() - m_rowHeight0);
}

QRect TableLayout::cellRect(int group, int row, int column) const
{
    if (column < 0 || column >= m_widths.size())
        return {};
    return QRect(columnLeft(column), rowViewportTop(group, row), m_widths[column], m_rowHeight);
}

QRect TableLayout::headerRect(int column) const
{
    if (column < 0 || column >= m_widths.size())
        return {};
    return QRect(columnLeft(column), 0, m_widths[column], headerHeight());
}

QRect TableLayout::footerRect(int column) const
{
    if (!m_footer || column < 0 || column >= m_widths.size())
        return {};
    return QRect(columnLeft(column), footerTop(), m_widths[column], m_rowHeight0);
}

EntryPtr TableLayout::entryAt(const CellPos& pos) const
{
    if (pos.group < 0 || pos.group >= m_data.groups.size())
        return nullptr;
    const QList<EntryPtr>& rows = m_data.groups[pos.group].entries;
    return pos.row >= 0 && pos.row < rows.size() ? rows[pos.row] : nullptr;
}

TableLayout::CellPos TableLayout::cellAt(const QPoint& p) const
{
    CellPos pos;
    if (p.y() < headerHeight())
        return pos;
    if (m_footer && p.y() >= footerTop())
        return pos;
    const int y = p.y() + scrollY();
    for (int g = 0; g < m_groups.size(); ++g) {
        const GroupGeom& gg = m_groups[g];
        if (y < gg.rowsTop || y >= gg.rowsTop + gg.rows * m_rowHeight)
            continue;
        pos.group = g;
        pos.row = (y - gg.rowsTop) / m_rowHeight;
        break;
    }
    if (pos.group < 0)
        return CellPos();
    for (int c = 0; c < m_widths.size(); ++c) {
        const int left = columnLeft(c);
        if (p.x() >= left && p.x() < left + m_widths[c]) {
            pos.column = c;
            return pos;
        }
    }
    return CellPos();
}

int TableLayout::headerColumnAt(const QPoint& p) const
{
    if (p.y() < 0 || p.y() >= headerHeight())
        return -1;
    for (int c = 0; c < m_widths.size(); ++c) {
        const int left = columnLeft(c);
        if (p.x() >= left && p.x() < left + m_widths[c])
            return c;
    }
    return -1;
}

int TableLayout::footerColumnAt(const QPoint& p) const
{
    if (!m_footer || p.y() < footerTop() || p.y() >= footerTop() + m_rowHeight0)
        return -1;
    for (int c = 0; c < m_widths.size(); ++c) {
        const int left = columnLeft(c);
        if (p.x() >= left && p.x() < left + m_widths[c])
            return c;
    }
    return -1;
}

int TableLayout::resizerAt(const QPoint& p) const
{
    if (p.y() < 0 || p.y() >= headerHeight())
        return -1;
    for (int c = 0; c < m_widths.size(); ++c) {
        const int right = columnLeft(c) + m_widths[c];
        if (p.x() >= right - kResizerWidth + 1 && p.x() <= right + 1)
            return c;
    }
    return -1;
}

const ValueHit* TableLayout::hitAt(const QPoint& p) const
{
    for (const ValueHit& h : m_hits)
        if (h.rect.adjusted(-1, -2, 1, 2).contains(p))
            return &h;
    return nullptr;
}

bool TableLayout::isEditable(const EntryPtr& e, const QString& prop) const
{
    if (!e || !isNoteProp(prop) || (m_controller && m_controller->isReadOnly()))
        return false;
    return e->path().endsWith(QLatin1String(".md"), Qt::CaseInsensitive);
}

// ---- painting -------------------------------------------------------------------------------
void TableLayout::paintCell(QPainter& p, const QRect& r, const EntryPtr& e, const QString& prop, int column)
{
    Q_UNUSED(column);
    const Style& s = basesStyle(isEmbedded());
    const QRect content = r.adjusted(s.cellPadX, 0, -s.cellPadX, 0);
    PaintOptions o = m_controller->paintOptions(s.cellFont, s.text);
    p.save();
    p.setClipRect(r.adjusted(0, 0, -1, 0));
    if (isNoteProp(prop)) {
        const bool md = e->path().endsWith(QLatin1String(".md"), Qt::CaseInsensitive);
        if (!md) {
            p.fillRect(r, s.cellDisabledBg);
        } else {
            const QString key = propName(prop);
            const QVariant raw = e->rawProperty(key);
            const QString type = m_controller->propertyType(key, raw);
            if (type == u"number")
                o.align = PaintOptions::Align::Right;
            else if (type == u"checkbox")
                o.align = PaintOptions::Align::Center;
            QRect cr = content;
            if (m_multiline)
                cr = QRect(content.left(), r.top(), content.width(), m_rowHeight0);
            paintProperty(&p, cr, raw, type, e->path(), o, &m_hits);
        }
    } else {
        Value v;
        if (prop == u"file.name") {
            v = Value::file(m_controller->runner()->dataSource(), e->path());
        } else {
            v = e->valueOrError(prop);
        }
        if (v.type() == ValueType::Number)
            o.align = PaintOptions::Align::Right;
        QRect cr = content;
        if (m_multiline)
            cr = QRect(content.left(), r.top(), content.width(), m_rowHeight0);
        paintValue(&p, cr, v, o, &m_hits);
    }
    p.restore();
}

void TableLayout::paintHeader(QPainter& p)
{
    const Style& s = basesStyle(isEmbedded());
    const int h = headerHeight();
    const int width = viewport()->width();
    p.fillRect(QRect(0, 0, width, h), s.headerBg);
    const BaseView* v = m_controller->currentView();
    for (int c = 0; c < m_columns.size(); ++c) {
        const QRect r = headerRect(c);
        if (r.right() < 0 || r.left() > width)
            continue;
        const QString& prop = m_columns[c];
        if (c == m_hoverHeader || c == m_pressHeader)
            p.fillRect(r, s.headerBgHover);
        QString sortDir;
        if (v)
            for (const SortSpec& sp : v->sortSpecs())
                if (sp.property == prop) {
                    sortDir = sp.direction;
                    break;
                }
        int x = r.left() + s.cellPadX;
        drawIcon(&p, QRectF(x, r.top(), s.iconXs, h), columnIcon(m_controller, prop), s.iconXs, s.headerText);
        x += s.iconXs + 4;
        p.setFont(s.headerFont);
        p.setPen(s.headerText);
        const int right = r.right() - s.cellPadX - (sortDir.isEmpty() ? 0 : 16);
        const QString name = m_controller->base().displayName(prop);
        p.drawText(QRect(x, r.top(), std::max(0, right - x), h), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetricsF(s.headerFont).elidedText(name, Qt::ElideRight, std::max(0, right - x)));
        if (!sortDir.isEmpty())
            drawIcon(&p, QRectF(r.right() - 6 - s.iconXs, r.top(), s.iconXs, h),
                     sortDir == u"ASC" ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"), s.iconXs, s.muted);
        // column border (the cell's -1px box shadow) and the resizer
        if (c > 0)
            p.fillRect(QRect(r.left() - 1, r.top(), 1, h), s.tableBorder);
        if (c == m_hoverResizer || c == m_resizing) {
            QColor rc = m_resizing == c ? s.interactiveAccent : currentTheme()->color(QStringLiteral("--divider-color-hover"), s.borderHover);
            p.fillRect(QRect(r.right() - kResizerWidth + 2, r.top(), kResizerWidth, h), rc);
        }
    }
    // bottom border
    p.fillRect(QRect(0, h - 1, width, 1), s.tableBorder);
}

QString TableLayout::summaryText(const QString& prop, const QList<EntryPtr>& entries, QString* name) const
{
    const BaseView* v = m_controller->currentView();
    const QString key = v ? v->summaryOf(prop) : QString();
    if (name)
        *name = key;
    if (key.isEmpty())
        return {};
    QList<Value> values;
    values.reserve(entries.size());
    for (const EntryPtr& e : entries)
        values << e->valueOrError(prop);
    const Value r = summaryValue(m_controller->base(), m_controller->runner()->dataSource(), values, key);
    return r.isNull() ? QStringLiteral("Empty") : r.toString();
}

void TableLayout::paintSummaryRow(QPainter& p, int y, const QList<EntryPtr>& entries, bool footer)
{
    const Style& s = basesStyle(isEmbedded());
    const int width = viewport()->width();
    const int h = m_rowHeight0;
    p.fillRect(QRect(0, y, std::min(width, totalWidth() - scrollX()), h), footer ? s.summaryBg : s.groupBg);
    for (int c = 0; c < m_columns.size(); ++c) {
        const QRect r(columnLeft(c), y, m_widths[c], h);
        if (r.right() < 0 || r.left() > width)
            continue;
        if (footer && c == m_hoverFooter)
            p.fillRect(r, currentTheme()->color(QStringLiteral("--bases-table-summary-background-hover"), s.hover));
        QString name;
        const QString value = summaryText(m_columns[c], entries, &name);
        if (!name.isEmpty()) {
            QFont nf = s.cellFont;
            const QFont vf = s.uiSmall;
            const int vw = int(std::ceil(QFontMetricsF(vf).horizontalAdvance(value)));
            const int nw = int(std::ceil(QFontMetricsF(nf).horizontalAdvance(name)));
            int x = r.right() - s.cellPadX - vw;
            p.setFont(vf);
            p.setPen(s.muted);
            p.drawText(QRect(x, y, vw + 1, h), Qt::AlignLeft | Qt::AlignVCenter, value);
            x -= 6 + nw;
            p.setFont(nf);
            p.setPen(s.muted);
            p.drawText(QRect(std::max(r.left() + 4, x), y, std::min(nw, r.width() - vw - 12) + 1, h), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetricsF(nf).elidedText(name, Qt::ElideRight, std::max(0, std::min(nw, r.width() - vw - 12))));
        } else if (footer && c == m_hoverFooter) {
            p.setFont(s.uiSmaller);
            p.setPen(s.faint);
            p.drawText(r.adjusted(s.cellPadX, 0, -s.cellPadX, 0), Qt::AlignRight | Qt::AlignVCenter, tr("Summary"));
            drawIcon(&p, QRectF(r.left() + 4, y, s.iconXs, h), QStringLiteral("chevrons-up-down"), s.iconXs, s.faint);
        }
        if (c > 0)
            p.fillRect(QRect(r.left() - 1, y, 1, h), s.tableBorder);
    }
    p.fillRect(QRect(0, y, std::min(width, totalWidth() - scrollX()), 1), s.tableBorder);
}

void TableLayout::paintEvent(QPaintEvent*)
{
    if (!m_controller)
        return;
    const Style& s = basesStyle(isEmbedded());
    QPainter p(viewport());
    const QRect vr = viewport()->rect();
    m_hits.clear();
    // the container: group background, the rows on the primary background
    p.fillRect(vr, s.groupBg);
    const int tableRight = std::min(vr.width(), totalWidth() - scrollX());
    for (int g = 0; g < m_groups.size(); ++g) {
        const GroupGeom& gg = m_groups[g];
        const ViewGroup& group = m_data.groups[g];
        const int top = gg.top - scrollY();
        const int bottom = gg.rowsTop + gg.rows * m_rowHeight - scrollY();
        if (bottom < 0 || top > vr.height())
            continue;
        if (gg.heading > 0) {
            // group heading: property name + value
            int x = 8 - scrollX();
            const QString propLabel = m_controller->currentView() && m_controller->currentView()->groupBy
                                          ? m_controller->base().displayName(m_controller->currentView()->groupBy->property)
                                          : QString();
            p.setFont(s.groupPropertyFont);
            p.setPen(s.muted);
            const int pw = int(std::ceil(QFontMetricsF(s.groupPropertyFont).horizontalAdvance(propLabel)));
            p.drawText(QRect(x, top, pw + 1, gg.heading), Qt::AlignLeft | Qt::AlignVCenter, propLabel);
            x += pw + 4;
            const QRect vrect(x, top, std::max(0, vr.width() - x - 8), gg.heading);
            if (!group.hasKey) {
                p.setFont(s.groupValueFont);
                p.setPen(s.text);
                p.drawText(vrect, Qt::AlignLeft | Qt::AlignVCenter, tr("None"));
            } else {
                PaintOptions o = m_controller->paintOptions(s.groupValueFont, s.text);
                paintValue(&p, vrect, group.key, o, &m_hits);
            }
        }
        if (gg.summary > 0)
            paintSummaryRow(p, top + gg.heading, group.entries, false);
        // rows
        const int rowsTop = gg.rowsTop - scrollY();
        p.fillRect(QRect(0, rowsTop, tableRight, gg.rows * m_rowHeight), s.bgPrimary);
        const int first = std::max(0, (headerHeight() - rowsTop) / std::max(1, m_rowHeight));
        const int last = std::min(gg.rows - 1, (vr.height() - rowsTop) / std::max(1, m_rowHeight));
        for (int r = first; r <= last; ++r) {
            const int y = rowsTop + r * m_rowHeight;
            const EntryPtr& e = group.entries[r];
            for (int c = 0; c < m_columns.size(); ++c) {
                const QRect cr(columnLeft(c), y, m_widths[c], m_rowHeight);
                if (cr.right() < 0 || cr.left() > vr.width())
                    continue;
                if (m_editor && m_editing == CellPos{g, r, c})
                    continue;
                paintCell(p, cr, e, m_columns[c], c);
                if (c > 0)
                    p.fillRect(QRect(cr.left() - 1, y, 1, m_rowHeight), s.tableBorder);
            }
            p.fillRect(QRect(0, y - (r == 0 ? 0 : 1), tableRight, 1), s.tableBorder);
        }
        p.fillRect(QRect(0, rowsTop + gg.rows * m_rowHeight, tableRight, 1), s.tableBorder);
    }
    // the active cell / the editor's focus ring
    auto ring = [&](const QRect& r, const QColor& c) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 2, 2);
        p.restore();
    };
    if (m_editor && m_editing.valid())
        ring(m_editor->geometry(), s.cellFocusRing);
    else if (m_active.valid() && hasFocus())
        ring(cellRect(m_active.group, m_active.row, m_active.column), s.cellActiveRing);
    // sticky header and footer on top
    paintHeader(p);
    if (m_footer)
        paintSummaryRow(p, footerTop(), m_data.rows, true);
}

// ---- geometry / scrolling ----------------------------------------------------------------------
void TableLayout::resizeEvent(QResizeEvent* e)
{
    BaseLayout::resizeEvent(e);
    computeWidths();
    updateScrollBars(totalWidth(), contentHeight());
    placeEditor();
}

void TableLayout::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    placeEditor();
    viewport()->update();
}

void TableLayout::scrollToCell(const CellPos& pos)
{
    if (!pos.valid() || isEmbedded())
        return;
    const QRect r = cellRect(pos.group, pos.row, pos.column);
    if (r.top() < headerHeight())
        verticalScrollBar()->setValue(verticalScrollBar()->value() - (headerHeight() - r.top()));
    else if (r.bottom() > viewport()->height() - (m_footer ? m_rowHeight0 : 0))
        verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height() + (m_footer ? m_rowHeight0 : 0) + 1);
    if (r.left() < 0)
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + r.left());
    else if (r.right() > viewport()->width())
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + r.right() - viewport()->width() + 1);
}

void TableLayout::setActiveCell(const CellPos& pos)
{
    m_active = pos;
    scrollToCell(pos);
    viewport()->update();
}

// ---- editing ----------------------------------------------------------------------------------
bool TableLayout::editCell(const CellPos& pos)
{
    const EntryPtr e = entryAt(pos);
    if (!e || pos.column < 0 || pos.column >= m_columns.size())
        return false;
    const QString prop = m_columns[pos.column];
    if (!isEditable(e, prop))
        return false;
    closeEditor();
    const QString key = propName(prop);
    const QVariant raw = e->rawProperty(key);
    const QString type = m_controller->propertyType(key, raw);
    if (type == u"unknown")
        return false;
    PropertyEditorContext ctx;
    QPointer<BasesController> c = m_controller;
    const QString path = e->path();
    ctx.session = [c] { return c ? c->session() : nullptr; };
    ctx.sourcePath = [path] { return path; };
    ctx.key = key;
    ctx.openLink = [c, path](const QString& target, int kind, bool newLeaf) {
        if (!c)
            return;
        if (kind == 3)
            c->openUrl(target);
        else
            c->openLink(target, path, newLeaf ? Qt::ControlModifier : Qt::NoModifier);
    };
    ctx.openTag = [c](const QString& tag) {
        if (c)
            c->openTag(tag);
    };
    ctx.hoverLink = [c, path](const QString& linktext, const QRect& globalRect, QWidget* hoverParent) {
        if (!c)
            return;
        if (linktext.isEmpty())
            c->unhoverLink(hoverParent);
        else
            c->hoverLink(linktext, path, globalRect, hoverParent);
    };
    ctx.isResolved = [c, path](const QString& linkpath) { return c && c->isResolved(linkpath, path); };
    ctx.notice = [c](const QString& m) {
        if (c)
            c->notice(m);
    };
    PropertyValueEditor* ed = createPropertyEditor(type, raw, ctx, viewport());
    if (!ed)
        return false;
    ed->setProperty("ocBasesCellEditor", true);
    ed->setAutoFillBackground(true);
    QPalette pal = ed->palette();
    pal.setColor(QPalette::Window, basesStyle(isEmbedded()).bgPrimary);
    ed->setPalette(pal);
    m_editor = ed;
    m_editing = pos;
    m_active = pos;
    connect(ed, &PropertyValueEditor::changed, this, [this, c, path, key](const QVariant& value) {
        if (c)
            c->writeProperty(path, key, value);
    });
    connect(ed, &PropertyValueEditor::blurRequested, this, [this] {
        closeEditor();
        setFocus(Qt::OtherFocusReason);
    });
    connect(ed, &PropertyValueEditor::heightChanged, this, &TableLayout::placeEditor);
    placeEditor();
    ed->show();
    if (type != u"checkbox")
        ed->focusEditor(QStringLiteral("end"));
    else
        ed->setFocus(Qt::OtherFocusReason);
    viewport()->update();
    return true;
}

void TableLayout::placeEditor()
{
    if (!m_editor || !m_editing.valid())
        return;
    const QRect r = cellRect(m_editing.group, m_editing.row, m_editing.column);
    if (r.isEmpty()) {
        closeEditor();
        return;
    }
    const int h = std::max(r.height(), m_editor->heightForWidth(r.width()));
    m_editor->setGeometry(r.left(), r.top(), r.width(), h);
    m_editor->raise();
}

void TableLayout::closeEditor()
{
    if (!m_editor)
        return;
    PropertyValueEditor* ed = m_editor;
    m_editor = nullptr;
    m_editing = {};
    ed->hide();
    ed->deleteLater();
    viewport()->update();
}

// ---- menus ------------------------------------------------------------------------------------
void TableLayout::saveColumnWidth(const QString& prop, int width)
{
    m_controller->editView([&](BaseView& v) {
        YamlNode sizes = v.option(QStringLiteral("columnSize"));
        if (!sizes.isMap())
            sizes = YamlNode::map();
        // Obsidian saves the custom widths of the view's columns
        YamlNode out = YamlNode::map();
        for (const QString& col : std::as_const(m_columns)) {
            if (col == prop) {
                if (width > 0)
                    out.set(col, YamlNode::number(width));
                continue;
            }
            if (const YamlNode* n = sizes.find(col))
                out.set(col, *n);
            else if (const YamlNode* n2 = sizes.find(writtenPropertyId(col)))
                out.set(col, *n2);
        }
        v.setOption(QStringLiteral("columnSize"), out.entries().isEmpty() ? YamlNode() : out);
    });
}

void TableLayout::showHeaderMenu(int column, const QPoint& globalPos)
{
    if (column < 0 || column >= m_columns.size())
        return;
    const QString prop = m_columns[column];
    const PropertyRef ref = parsePropertyId(prop);
    const BaseView* v = m_controller->currentView();
    QString dir;
    if (v)
        for (const SortSpec& sp : v->sortSpecs())
            if (sp.property == prop)
                dir = sp.direction;
    QPointer<BasesController> c = m_controller;
    QList<MenuEntry> items;
    auto sortItem = [&](const QString& title, const QString& icon, const QString& d) {
        MenuEntry e{QStringLiteral("sort"), title, icon, [c, prop, d] {
                        if (c)
                            c->editView([&](BaseView& view) { view.setSortProperty(prop, d); });
                    }};
        e.checkable = d != u"NONE";
        e.checked = d == dir;
        e.warning = d == u"NONE";
        return e;
    };
    items << sortItem(tr("Sort A → Z"), QStringLiteral("arrow-down-wide-narrow"), QStringLiteral("ASC"));
    items << sortItem(tr("Sort Z → A"), QStringLiteral("arrow-up-narrow-wide"), QStringLiteral("DESC"));
    if (!dir.isEmpty())
        items << sortItem(tr("Clear sort"), QStringLiteral("eraser"), QStringLiteral("NONE"));
    items << MenuEntry{QStringLiteral("action"), tr("Hide column"), QStringLiteral("eye-off"), [c, prop] {
                           if (c)
                               c->editView([&](BaseView& view) {
                                   QStringList order = view.columns();
                                   order.removeAll(prop);
                                   view.order = order;
                               });
                       }};
    QPointer<TableLayout> self = this;
    items << MenuEntry{QStringLiteral("action"), tr("Summarize..."), QStringLiteral("calculator"), [self, column, globalPos] {
                           if (self)
                               self->showSummaryMenu(column, self->m_data.rows, globalPos);
                       }};
    static const QStringList noGroup = {QStringLiteral("file.file"), QStringLiteral("file.path"), QStringLiteral("file.ctime"),
                                        QStringLiteral("file.mtime"), QStringLiteral("file.name"), QStringLiteral("file.fullname"),
                                        QStringLiteral("file.basename")};
    const bool aliases = ref.type == u"note" && m_controller->propertyType(ref.name, QVariant()) == u"aliases";
    if (!noGroup.contains(prop) && !aliases)
        items << MenuEntry{QStringLiteral("action"), tr("Group by this property"), QStringLiteral("group"), [c, prop] {
                               if (c)
                                   c->editView([&](BaseView& view) { view.groupBy = SortSpec{prop, QStringLiteral("ASC")}; });
                           }};
    items << MenuEntry{QStringLiteral("danger"), ref.type == u"formula" ? tr("Edit formula...") : tr("Edit property..."), QStringLiteral("edit-3"),
                       [c, prop] {
                           if (c)
                               openEditPropertyMenu(c, prop);
                       }};
    MenuLayout layout;
    layout.sections = {QStringLiteral("action"), QStringLiteral("action.changeType"), QStringLiteral("resize"), QStringLiteral("sort"),
                       QString(), QStringLiteral("danger")};
    if (ref.type == u"note" && m_controller->session()) {
        layout.submenus.append({QStringLiteral("action.changeType"), {tr("Property type"), QStringLiteral("info")}});
        const QString current = m_controller->propertyType(ref.name, QVariant());
        PropertyTypeManager* types = PropertyTypeManager::of(m_controller->session());
        for (const props::PropertyType& t : props::menuPropertyTypes(ref.name)) {
            MenuEntry e{QStringLiteral("action.changeType"), t.name, t.icon, [types, name = ref.name, id = t.id] {
                            if (types)
                                types->setType(name, id);
                        }};
            e.checkable = true;
            e.checked = t.id == current;
            items << e;
        }
    }
    QMenu* menu = ownedMenu(this);
    buildSectionedMenu(menu, items, layout);
    menu->popup(globalPos);
}

void TableLayout::showSummaryMenu(int column, const QList<EntryPtr>& entries, const QPoint& globalPos)
{
    if (column < 0 || column >= m_columns.size())
        return;
    QList<Value> values;
    for (const EntryPtr& e : entries)
        values << e->valueOrError(m_columns[column]);
    auto* anchor = new QWidget(viewport());
    anchor->setGeometry(QRect(viewport()->mapFromGlobal(globalPos), QSize(1, 1)));
    const QRect fr = footerRect(column);
    if (!fr.isEmpty())
        anchor->setGeometry(fr);
    anchor->show();
    MenuPanel* panel = openSummaryMenu(m_controller, m_columns[column], values, anchor);
    if (panel)
        connect(panel, &MenuPanel::closed, anchor, &QObject::deleteLater);
    else
        anchor->deleteLater();
}

// ---- input ------------------------------------------------------------------------------------
bool TableLayout::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
        return true;
    return BaseLayout::viewportEvent(e);
}

void TableLayout::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;
    if (e->button() == Qt::LeftButton) {
        const int rz = resizerAt(pos);
        if (rz >= 0) {
            m_resizing = rz;
            m_resizeStartX = pos.x();
            m_resizeStartWidth = m_widths[rz];
            viewport()->update();
            return;
        }
        const int hc = headerColumnAt(pos);
        if (hc >= 0) {
            m_pressHeader = hc;
            viewport()->update();
            return;
        }
        const int fc = footerColumnAt(pos);
        if (fc >= 0) {
            showSummaryMenu(fc, m_data.rows, e->globalPosition().toPoint());
            return;
        }
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        if (const ValueHit* hit = hitAt(pos)) {
            const ValueHit h = *hit;
            const Qt::KeyboardModifiers mods = e->button() == Qt::MiddleButton ? Qt::ControlModifier : e->modifiers();
            switch (h.kind) {
            case ValueHit::Kind::Link:
                m_controller->openLink(h.target, h.sourcePath, mods);
                break;
            case ValueHit::Kind::File:
                m_controller->openFile(h.target, mods);
                break;
            case ValueHit::Kind::Url:
                m_controller->openUrl(h.target);
                break;
            case ValueHit::Kind::Tag:
                m_controller->openTag(h.target);
                break;
            }
            return;
        }
    }
    if (e->button() != Qt::LeftButton)
        return;
    const CellPos cell = cellAt(pos);
    if (!cell.valid()) {
        closeEditor();
        return;
    }
    m_active = cell;
    const EntryPtr entry = entryAt(cell);
    const QString prop = m_columns.value(cell.column);
    if (isEditable(entry, prop)) {
        const QString key = propName(prop);
        const QVariant raw = entry->rawProperty(key);
        if (m_controller->propertyType(key, raw) == u"checkbox") {
            // a click on the checkbox toggles it (Obsidian's checkbox widget)
            const QRect r = cellRect(cell.group, cell.row, cell.column);
            const int size = 18;
            if (QRect(r.center().x() - size, r.center().y() - size, 2 * size, 2 * size).contains(pos))
                m_controller->writeProperty(entry->path(), key, !raw.toBool());
            viewport()->update();
            return;
        }
        editCell(cell);
        return;
    }
    closeEditor();
    viewport()->update();
}

void TableLayout::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_resizing >= 0) {
        const Style& s = basesStyle(isEmbedded());
        const int w = std::max(s.colMinWidth, m_resizeStartWidth + pos.x() - m_resizeStartX);
        m_customWidths.insert(m_columns[m_resizing], w);
        computeWidths();
        updateScrollBars(totalWidth(), contentHeight());
        placeEditor();
        viewport()->update();
        return;
    }
    const int rz = resizerAt(pos);
    const int hc = rz >= 0 ? -1 : headerColumnAt(pos);
    const int fc = footerColumnAt(pos);
    if (rz != m_hoverResizer || hc != m_hoverHeader || fc != m_hoverFooter) {
        m_hoverResizer = rz;
        m_hoverHeader = hc;
        m_hoverFooter = fc;
        viewport()->update();
    }
    const ValueHit* hit = rz < 0 && hc < 0 ? hitAt(pos) : nullptr;
    viewport()->setCursor(rz >= 0 ? Qt::SplitHCursor : hit || hc >= 0 || fc >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    const QString target = hit && (hit->kind == ValueHit::Kind::Link || hit->kind == ValueHit::Kind::File) ? hit->target : QString();
    if (target != m_hoverTarget) {
        m_hoverTarget = target;
        if (target.isEmpty()) {
            m_controller->unhoverLink(this);
        } else {
            const QRect gr(viewport()->mapToGlobal(hit->rect.topLeft().toPoint()), hit->rect.size().toSize());
            m_controller->hoverLink(target, hit->sourcePath, gr, this);
        }
    }
}

void TableLayout::mouseReleaseEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_resizing >= 0) {
        const int col = m_resizing;
        m_resizing = -1;
        const QString prop = m_columns.value(col);
        const int w = m_customWidths.value(prop, m_widths.value(col));
        viewport()->update();
        if (!prop.isEmpty() && w != m_resizeStartWidth)
            saveColumnWidth(prop, w);
        return;
    }
    if (m_pressHeader >= 0 && e->button() == Qt::LeftButton) {
        const int col = m_pressHeader;
        m_pressHeader = -1;
        viewport()->update();
        if (headerColumnAt(pos) == col && (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            const QString prop = m_columns.value(col);
            m_controller->editView([&](BaseView& v) { v.setSortProperty(prop, QStringLiteral("TOGGLE")); });
        }
    }
}

void TableLayout::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int rz = resizerAt(e->position().toPoint());
    if (rz >= 0) {
        m_customWidths.remove(m_columns[rz]);
        saveColumnWidth(m_columns[rz], 0); // Obsidian's "Reset column size"
        return;
    }
    mousePressEvent(e);
}

void TableLayout::leaveEvent(QEvent* e)
{
    m_hoverHeader = m_hoverResizer = m_hoverFooter = -1;
    if (!m_hoverTarget.isEmpty()) {
        m_hoverTarget.clear();
        m_controller->unhoverLink(this);
    }
    viewport()->update();
    BaseLayout::leaveEvent(e);
}

void TableLayout::contextMenuEvent(QContextMenuEvent* e)
{
    const QPoint pos = e->pos();
    const int hc = headerColumnAt(pos);
    if (hc >= 0) {
        showHeaderMenu(hc, e->globalPos());
        return;
    }
    const int fc = footerColumnAt(pos);
    if (fc >= 0) {
        showSummaryMenu(fc, m_data.rows, e->globalPos());
        return;
    }
    if (const ValueHit* hit = hitAt(pos)) {
        if (hit->kind == ValueHit::Kind::File) {
            m_controller->showFileMenu(hit->target, e->globalPos(), this);
            return;
        }
        if (hit->kind == ValueHit::Kind::Link) {
            const QString target = m_controller->runner()->dataSource()->resolveLink(hit->target.section(u'#', 0, 0), hit->sourcePath);
            if (!target.isEmpty()) {
                m_controller->showFileMenu(target, e->globalPos(), this);
                return;
            }
        }
    }
    const CellPos cell = cellAt(pos);
    if (const EntryPtr entry = entryAt(cell))
        m_controller->showFileMenu(entry->path(), e->globalPos(), this);
}

void TableLayout::keyPressEvent(QKeyEvent* e)
{
    if (m_editor) {
        if (e->key() == Qt::Key_Escape) {
            closeEditor();
            setFocus();
            return;
        }
        BaseLayout::keyPressEvent(e);
        return;
    }
    CellPos pos = m_active;
    auto groupRows = [&](int g) { return g >= 0 && g < m_groups.size() ? m_groups[g].rows : 0; };
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Tab:
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (pos.valid())
            editCell(pos);
        return;
    case Qt::Key_Escape:
        if (pos.valid()) {
            m_active = {};
            viewport()->update();
            return;
        }
        BaseLayout::keyPressEvent(e);
        return;
    default:
        BaseLayout::keyPressEvent(e);
        return;
    }
    if (!pos.valid()) {
        for (int g = 0; g < m_groups.size(); ++g)
            if (m_groups[g].rows > 0) {
                setActiveCell(CellPos{g, 0, 0});
                return;
            }
        return;
    }
    if (e->key() == Qt::Key_Up) {
        if (pos.row > 0) {
            --pos.row;
        } else {
            for (int g = pos.group - 1; g >= 0; --g)
                if (groupRows(g) > 0) {
                    pos.group = g;
                    pos.row = groupRows(g) - 1;
                    break;
                }
        }
    } else if (e->key() == Qt::Key_Down) {
        if (pos.row < groupRows(pos.group) - 1) {
            ++pos.row;
        } else {
            for (int g = pos.group + 1; g < m_groups.size(); ++g)
                if (groupRows(g) > 0) {
                    pos.group = g;
                    pos.row = 0;
                    break;
                }
        }
    } else if (e->key() == Qt::Key_Left) {
        pos.column = std::max(0, pos.column - 1);
    } else {
        pos.column = std::min(int(m_columns.size()) - 1, pos.column + 1);
    }
    setActiveCell(pos);
}

// ---- state ------------------------------------------------------------------------------------
QJsonObject TableLayout::ephemeralState() const
{
    QJsonObject o = BaseLayout::ephemeralState();
    if (m_active.valid())
        o.insert(QStringLiteral("activeCell"), QJsonObject{{QStringLiteral("groupIdx"), m_active.group},
                                                           {QStringLiteral("row"), m_active.row},
                                                           {QStringLiteral("column"), m_active.column}});
    return o;
}

void TableLayout::setEphemeralState(const QJsonObject& state)
{
    BaseLayout::setEphemeralState(state);
    const QJsonObject a = state.value(QStringLiteral("activeCell")).toObject();
    if (!a.isEmpty())
        m_active = CellPos{a.value(QStringLiteral("groupIdx")).toInt(), a.value(QStringLiteral("row")).toInt(),
                           a.value(QStringLiteral("column")).toInt()};
}

} // namespace oc::basesui
