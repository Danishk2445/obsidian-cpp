// Widgets rendering the workspace model (splits, tab groups, tab headers, view headers) and the tab
// drag & drop engine. Sizes and colors follow Obsidian 1.13's app.css (values only):
//   .workspace-tab-header-container   40px, padding 0 8px, border-bottom 1px --tab-outline-color
//   .workspace-tab-header-container-inner  margin 6px -5px -1px, padding 1px 15px 0
//   .mod-root .workspace-tab-header   width --tab-width (200) shrinking, padding 1px 3px 3.5px,
//                                     active: radius 6 6 0 0 + 6px inverted curves, outline 1px
//   .workspace-tab-header-inner       radius 4, padding 0 3px 0 6px (root), gap 2px
//   sidedock tabs                     icon only (18px), 25px tall, padding 0 8px, gap 3px
//   .view-header                      40px, padding 0 12px, gap 8px, title 13px centered

#include "core/vault/vault.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace_p.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

#include <cmath>

namespace oc {

namespace {
constexpr int kHeaderHeight = 40;
constexpr double kTabTop = 7;       // container-inner margin-top 6 + padding-top 1
constexpr double kTabWidth = 200;   // --tab-width
constexpr double kMinTabWidth = 48; // below 3rem Obsidian hides inactive close buttons
constexpr double kMinTabBox = 6;    // root tabs: min-width 0 + padding 0 3px; the strip only scrolls below
constexpr int kDragThreshold = 6;
constexpr int kMinPaneSize = 200; // Obsidian's minimum pane size while resizing
constexpr int kMinSideWidth = 200;

QColor tok(const ThemePtr& t, const char* name) { return t->color(QLatin1String(name)); }

QPixmap icon(const QString& name, int size, const QColor& color, double stroke, qreal dpr)
{
    return IconProvider::instance().pixmap(name, size, color, stroke, dpr);
}

QIcon menuIcon(const QString& name) { return IconProvider::instance().icon(name, 16, 2.0); }

bool isEmptyOrMarkdown(const WorkspaceLeaf* leaf)
{
    const View* v = leaf ? leaf->view() : nullptr;
    const QString type = v ? v->viewType() : QString();
    return type.isEmpty() || type == u"empty" || type == u"markdown";
}

void execMenuBelow(QMenu& menu, QWidget* anchor, bool alignRight)
{
    const QSize sz = menu.sizeHint();
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height()));
    if (alignRight)
        pos.rx() += anchor->width() - sz.width();
    menu.exec(pos);
}

// Obsidian's .drag-ghost.mod-leaf: 13px medium white text on rgba(0,0,0,.85), radius 4, padding 6 8,
// icon + 6px gap, max width 300, shadow 0 2px 8px.
QPixmap makeGhost(const WorkspaceLeaf* leaf, qreal dpr)
{
    const ThemePtr t = currentTheme();
    QFont f = t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13));
    f.setWeight(QFont::Medium);
    const QFontMetricsF fm(f);
    QString text = leaf->displayText().trimmed();
    if (text.size() > 60)
        text = text.left(60) + QStringLiteral("…");
    const int iconSz = 16, padX = 8, padY = 6, gap = 6, shadow = 8;
    const double textW = std::min(fm.horizontalAdvance(text), 300.0 - 2 * padX - iconSz - gap);
    const QSizeF box(padX + iconSz + gap + textW + padX, padY * 2 + std::max<double>(iconSz, fm.height()));
    QPixmap pm(QSize(int(std::ceil((box.width() + 2 * shadow) * dpr)), int(std::ceil((box.height() + 2 * shadow) * dpr))));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(shadow, shadow, box.width(), box.height());
    for (int i = 4; i >= 1; --i) {
        QColor sc = tok(t, "--background-modifier-box-shadow");
        sc.setAlphaF(sc.alphaF() * 0.35);
        QPainterPath sp;
        sp.addRoundedRect(r.adjusted(-i, -i + 2, i, i + 2), 4 + i, 4 + i);
        p.fillPath(sp, sc);
    }
    QPainterPath bg;
    bg.addRoundedRect(r, 4, 4);
    p.fillPath(bg, tok(t, "--drag-ghost-background").isValid() ? tok(t, "--drag-ghost-background") : QColor(0, 0, 0, 217));
    QColor fg = tok(t, "--drag-ghost-text-color");
    if (!fg.isValid())
        fg = Qt::white;
    p.drawPixmap(QPointF(r.left() + padX, r.center().y() - iconSz / 2.0), icon(leaf->icon(), iconSz, fg, 2.0, dpr));
    p.setFont(f);
    p.setPen(fg);
    const QRectF tr(r.left() + padX + iconSz + gap, r.top(), textW, r.height());
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, textW));
    return pm;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// ResizeHandle
// ---------------------------------------------------------------------------------------------

ResizeHandle::ResizeHandle(Line line, QWidget* parent)
    : QWidget(parent)
    , m_line(line)
{
    setAttribute(Qt::WA_Hover);
    setCursor(line == Line::Bottom ? Qt::SplitVCursor : Qt::SplitHCursor);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void ResizeHandle::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    if (m_hover || m_dragging) {
        p.fillRect(rect(), tok(t, "--divider-color-hover"));
        return;
    }
    const QColor c = tok(t, "--divider-color");
    switch (m_line) {
    case Line::Right:
        p.fillRect(QRect(width() - 1, 0, 1, height()), c);
        break;
    case Line::Left:
        p.fillRect(QRect(0, 0, 1, height()), c);
        break;
    case Line::Bottom:
        p.fillRect(QRect(0, height() - 1, width(), 1), c);
        break;
    }
}

void ResizeHandle::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void ResizeHandle::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

void ResizeHandle::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    update();
    if (onDragStart)
        onDragStart(e->globalPosition().toPoint());
    e->accept();
}

void ResizeHandle::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && onDrag)
        onDrag(e->globalPosition().toPoint());
}

void ResizeHandle::mouseReleaseEvent(QMouseEvent* e)
{
    if (!m_dragging || e->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    update();
    if (onDragFinished)
        onDragFinished();
}

// ---------------------------------------------------------------------------------------------
// SplitWidget
// ---------------------------------------------------------------------------------------------

SplitWidget::SplitWidget(WorkspaceImpl* d, WorkspaceSplit* split)
    : QWidget(d->widget)
    , d(d)
    , m_split(split)
{
    hide();
}

void SplitWidget::sync()
{
    if (!m_split)
        return;
    for (WorkspaceItem* c : std::as_const(m_split->children)) {
        QWidget* w = c->widget();
        if (w->parentWidget() != this)
            w->setParent(this);
        w->show();
    }
    const ResizeHandle::Line line = m_split->vertical ? ResizeHandle::Line::Right : ResizeHandle::Line::Bottom;
    const int want = std::max(0, int(m_split->children.size()) - 1);
    // Recreate the handles when the direction changed (their line and cursor depend on it).
    if (!m_handles.isEmpty() && (m_handles.first()->cursor().shape() == Qt::SplitVCursor) == m_split->vertical) {
        qDeleteAll(m_handles);
        m_handles.clear();
    }
    while (m_handles.size() > want)
        delete m_handles.takeLast();
    while (m_handles.size() < want) {
        auto* h = new ResizeHandle(line, this);
        const int index = int(m_handles.size());
        h->onDragStart = [this](QPoint g) { startHandleDrag(g); };
        h->onDrag = [this, index](QPoint g) { dragHandle(index, g); };
        h->onDragFinished = [this] {
            m_dragSizes.clear();
            d->q->requestSaveLayout();
            emit d->q->layoutChanged();
        };
        m_handles.push_back(h);
        h->show();
    }
    if (m_split->isSidedock() && m_split->children.isEmpty()) {
        auto* empty = findChild<QLabel*>(QStringLiteral("ocSidedockEmpty"), Qt::FindDirectChildrenOnly);
        if (!empty) {
            empty = new QLabel(tr("The sidebar is empty, try dragging a tab here."), this);
            empty->setObjectName(QStringLiteral("ocSidedockEmpty"));
            empty->setProperty("ocText", QStringLiteral("muted"));
            empty->setWordWrap(true);
            empty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        }
        const ThemePtr t = currentTheme();
        empty->setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13)));
        empty->show();
    } else if (auto* empty = findChild<QLabel*>(QStringLiteral("ocSidedockEmpty"), Qt::FindDirectChildrenOnly)) {
        empty->hide();
    }
    relayout();
}

void SplitWidget::relayout()
{
    if (!m_split)
        return;
    const auto& ch = m_split->children;
    const int n = int(ch.size());
    if (auto* empty = findChild<QLabel*>(QStringLiteral("ocSidedockEmpty"), Qt::FindDirectChildrenOnly); empty && empty->isVisible())
        empty->setGeometry(30, 20, std::max(0, width() - 60), std::max(0, height() - 40));
    if (n == 0)
        return;
    // Children are flex items (flex: 1 0 0) whose flex-grow is their dimension, or 1 when unset.
    auto weight = [](const WorkspaceItem* c) { return c->dimension > 0 ? c->dimension : 1.0; };
    double total = 0;
    for (const WorkspaceItem* c : ch)
        total += weight(c);
    const bool vertical = m_split->vertical;
    const int extent = vertical ? width() : height();
    const int top = d->frameless() ? 0 : kHeaderHeight; // --divider-vertical-height
    int pos = 0;
    double acc = 0;
    for (int i = 0; i < n; ++i) {
        acc += weight(ch[i]);
        const int end = i == n - 1 ? extent : int(std::lround(acc / total * extent));
        const QRect r = vertical ? QRect(pos, 0, std::max(0, end - pos), height()) : QRect(0, pos, width(), std::max(0, end - pos));
        ch[i]->widget()->setGeometry(r);
        if (i < n - 1 && i < m_handles.size()) {
            ResizeHandle* h = m_handles[i];
            h->setGeometry(vertical ? QRect(end - 3, top, 3, std::max(0, height() - top)) : QRect(0, end - 3, width(), 3));
            h->raise();
        }
        pos = end;
    }
}

void SplitWidget::startHandleDrag(QPoint globalPos)
{
    if (!m_split)
        return;
    const QPoint local = mapFromGlobal(globalPos);
    m_dragSizes.clear();
    for (const WorkspaceItem* c : std::as_const(m_split->children))
        m_dragSizes.push_back(m_split->vertical ? c->widget()->width() : c->widget()->height());
    m_dragOrigin = m_split->vertical ? local.x() : local.y();
}

void SplitWidget::dragHandle(int index, QPoint globalPos)
{
    if (!m_split)
        return;
    const auto& ch = m_split->children;
    const int n = int(ch.size());
    if (index < 0 || index >= n - 1)
        return;
    const bool vertical = m_split->vertical;
    const QPoint local = mapFromGlobal(globalPos);
    if (m_dragSizes.size() != n)
        startHandleDrag(globalPos);
    const int delta = (vertical ? local.x() : local.y()) - m_dragOrigin;
    QList<int> sizes = m_dragSizes;
    // Obsidian: the pane next to the handle absorbs the change; the panes on the shrinking side give
    // up space nearest first, down to 200px each.
    auto shrink = [&](const QList<int>& order, int amount) {
        int done = 0;
        for (int i : order) {
            const int floor = std::min(kMinPaneSize, m_dragSizes[i]);
            const int take = std::min(std::max(0, sizes[i] - floor), amount - done);
            sizes[i] -= take;
            done += take;
            if (done >= amount)
                break;
        }
        return done;
    };
    if (delta > 0) {
        QList<int> after;
        for (int i = index + 1; i < n; ++i)
            after.push_back(i);
        sizes[index] += shrink(after, delta);
    } else if (delta < 0) {
        QList<int> before;
        for (int i = index; i >= 0; --i)
            before.push_back(i);
        sizes[index + 1] += shrink(before, -delta);
    }
    double total = 0;
    for (int s : sizes)
        total += s;
    if (total <= 0)
        return;
    for (int i = 0; i < n; ++i)
        ch[i]->dimension = sizes[i] * 100.0 / total;
    relayout();
}

void SplitWidget::resizeEvent(QResizeEvent*) { relayout(); }

void SplitWidget::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.fillRect(rect(), m_split && m_split->isSidedock() ? tok(t, "--background-secondary") : tok(t, "--background-primary"));
}

// ---------------------------------------------------------------------------------------------
// TabHeaderBar
// ---------------------------------------------------------------------------------------------

TabHeaderBar::TabHeaderBar(WorkspaceImpl* d, WorkspaceTabs* tabs, QWidget* parent)
    : QWidget(parent)
    , d(d)
    , m_group(tabs)
{
    setObjectName(QStringLiteral("ocTabHeader"));
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    // .workspace-tab-header-new-tab / -tab-list .clickable-icon: padding 4px, 18px icon.
    m_newTab = new IconButton(QStringLiteral("plus"), tr("New tab"), this);
    m_newTab->setButtonSize({26, 26});
    m_tabList = new IconButton(QStringLiteral("chevron-down"), QString(), this);
    m_tabList->setButtonSize({26, 26});
    connect(m_newTab, &IconButton::clicked, this, [this] { this->d->appendNewTab(m_group); });
    connect(m_tabList, &IconButton::clicked, this, [this] { showTabList(); });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            relayout();
            update();
        });
}

void TabHeaderBar::setSidedock(bool sidedock)
{
    if (m_sidedock == sidedock)
        return;
    m_sidedock = sidedock;
    relayout();
    update();
}

void TabHeaderBar::setTop(bool top)
{
    if (m_top == top)
        return;
    m_top = top;
    if (!top)
        setProperty("ocDragRegion", false);
    else
        refreshDragRegion();
    update();
}

void TabHeaderBar::setRightReserve(int px)
{
    if (m_reserve == px)
        return;
    m_reserve = px;
    relayout();
    update();
}

WorkspaceLeaf* TabHeaderBar::leafAt(int index) const { return m_group ? m_group->leaves.value(index) : nullptr; }

void TabHeaderBar::detachModel()
{
    m_group = nullptr;
    m_tabs.clear();
    m_hoverTab = m_pressTab = -1;
    m_dragging = false;
}

bool TabHeaderBar::closeVisible(int index) const
{
    if (m_sidedock || !m_group || index < 0 || index >= m_tabs.size())
        return false;
    const WorkspaceLeaf* leaf = leafAt(index);
    if (!leaf)
        return false;
    if (leaf->isPinned())
        return true; // the pin status icon sits where the close button would be
    if (index == m_group->currentTab)
        return true;
    return index == m_hoverTab && m_tabs[index].rect.width() >= kMinTabWidth;
}

void TabHeaderBar::relayout()
{
    if (!m_group)
        return;
    const int n = int(m_group->leaves.size());
    m_tabs.resize(n);
    const int cur = m_group->currentTab;
    // The rightmost top header leaves room for the floating sidebar toggle + window buttons.
    const double padL = 8, padR = m_reserve > 0 ? m_reserve : 8;
    auto scrollIntoView = [&](double tabL, double tabR, double visible, double maxScroll) {
        if (cur != m_snappedTab) {
            if (tabL < m_scroll)
                m_scroll = tabL;
            else if (tabR > m_scroll + visible)
                m_scroll = tabR - visible;
            m_snappedTab = cur;
        }
        m_scroll = std::clamp(m_scroll, 0.0, maxScroll);
    };
    if (m_sidedock) {
        m_newTab->hide();
        m_tabList->hide();
        // Icon tabs 34x25, 3px apart; the strip scrolls when it doesn't fit.
        const double avail = std::max(0.0, width() - padL - padR);
        const double stripW = n > 0 ? n * 34 + (n - 1) * 3 : 0;
        if (cur >= 0 && cur < n)
            scrollIntoView(cur * 37.0, cur * 37.0 + 34, avail, std::max(0.0, stripW - avail));
        else
            m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, stripW - avail));
        m_clipL = padL;
        m_clipR = padL + avail;
        double x = padL - m_scroll;
        for (int i = 0; i < n; ++i) {
            TabGeom& g = m_tabs[i];
            g.rect = QRectF(x, kTabTop, 34, 25);
            g.inner = g.rect;
            g.icon = QRectF(g.rect.center().x() - 9, g.rect.center().y() - 9, 18, 18);
            g.title = {};
            g.close = {};
            g.showIcon = true;
            x += 34 + 3;
        }
        refreshDragRegion();
        return;
    }
    m_newTab->show();
    m_tabList->show();
    const double content = width() - padL - padR;
    // inner (tabs) margin box = tabs + 20; "+" margin box 34; tab list margin box 30.
    const double availStrip = std::max(0.0, content - 20 - 34 - 30);
    // .mod-root .workspace-tab-header: flex 1 1 0, width --tab-width, min-width 0 — the tabs share the
    // strip, up to 200px each, and keep shrinking when it's too narrow.
    double tw = n > 0 ? std::min(kTabWidth, availStrip / n) : kTabWidth;
    if (m_lockedWidth > 0)
        tw = std::min(tw, m_lockedWidth);
    tw = std::max(tw, kMinTabBox);
    const double stripW = n * tw;
    const double visibleStrip = std::min(stripW, availStrip);
    if (cur >= 0 && cur < n)
        scrollIntoView(cur * tw, cur * tw + tw, visibleStrip, std::max(0.0, stripW - visibleStrip));
    else
        m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, stripW - visibleStrip));
    // .workspace-tab-header-container-inner: margin 0 -5px, padding 0 15px, overflow hidden.
    m_clipL = padL - 5;
    m_clipR = padL - 5 + visibleStrip + 30;
    const double x0 = padL + 10 - m_scroll;
    for (int i = 0; i < n; ++i) {
        TabGeom& g = m_tabs[i];
        g.rect = QRectF(x0 + i * tw, kTabTop, tw, kHeaderHeight - kTabTop);
        g.inner = g.rect.adjusted(3, 1, -3, -3.5);
        const double cl = g.inner.left() + 6, cr = g.inner.right() - 3, cy = g.inner.center().y();
        g.showIcon = !isEmptyOrMarkdown(leafAt(i));
        double titleL = cl;
        if (g.showIcon) {
            g.icon = QRectF(cl, cy - 7, 14, 14);
            titleL = cl + 14 + 4 + 2;
        } else {
            g.icon = {};
        }
        g.close = QRectF(cr - 20, cy - 10, 20, 20).intersected(g.inner); // the inner clips (overflow hidden)
        g.title = QRectF(titleL, g.inner.top(), std::max(0.0, cr - titleL), g.inner.height());
    }
    const int newTabX = int(std::lround(padL + visibleStrip + 20 - 4));
    m_newTab->move(newTabX, 8);
    m_tabList->move(int(std::lround(width() - padR - 4 - 26)), 8);
    refreshDragRegion();
}

void TabHeaderBar::refreshDragRegion()
{
    // Tabs can move under a still pointer (new tab, closed tab, resize): re-evaluate what's under it
    // so a click on a tab never starts a window move (the frame controller reads the property on press).
    if (underMouse())
        updateHover(mapFromGlobal(QCursor::pos()));
}

QRect TabHeaderBar::tabRect(int index) const
{
    return index >= 0 && index < m_tabs.size() ? m_tabs[index].rect.toAlignedRect() : QRect();
}

QRect TabHeaderBar::closeRect(int index) const
{
    return index >= 0 && index < m_tabs.size() ? m_tabs[index].close.toAlignedRect() : QRect();
}

int TabHeaderBar::tabAt(const QPoint& pos) const
{
    // Tabs scrolled out of the strip aren't hit.
    if (pos.x() < m_clipL || pos.x() >= m_clipR)
        return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].rect.contains(pos))
            return i;
    return -1;
}

TabHeaderBar::InsertLocation TabHeaderBar::insertLocation(int x) const
{
    // Obsidian's getTabInsertLocation(): a 10px band centered on the gap nearest to x.
    InsertLocation loc;
    const int n = int(m_tabs.size());
    if (n == 0) {
        loc.rect = QRect(m_sidedock ? 3 : 13, int(kTabTop), 10, m_sidedock ? 25 : kHeaderHeight - int(kTabTop));
        return loc;
    }
    for (int i = 0; i < n; ++i) {
        const QRectF r = m_tabs[i].rect;
        if (i == n - 1 || x <= r.right()) {
            QRectF band(r.left(), r.top(), 10, r.height());
            loc.index = i;
            if (x > r.center().x()) {
                ++loc.index;
                band.moveLeft(r.right());
            }
            band.translate(-5, 0);
            loc.rect = band.toAlignedRect();
            break;
        }
    }
    return loc;
}

void TabHeaderBar::paintRootTab(QPainter& p, int index, const TabGeom& g)
{
    const ThemePtr t = currentTheme();
    const WorkspaceLeaf* leaf = leafAt(index);
    if (!leaf)
        return;
    const bool active = index == m_group->currentTab;
    const bool hovered = index == m_hoverTab;
    const bool focused = d->windowFocused();
    const bool groupActive = d->isGroupActive(m_group);
    const qreal dpr = devicePixelRatioF();

    if (active) {
        const double x0 = g.rect.left(), x1 = g.rect.right(), y0 = g.rect.top(), y1 = kHeaderHeight;
        const double r = 6, c = t->px(QStringLiteral("--tab-curve"), -1, 6);
        QPainterPath shape;
        shape.moveTo(x0 - c, y1);
        shape.arcTo(QRectF(x0 - 2 * c, y1 - 2 * c, 2 * c, 2 * c), 270, 90);
        shape.lineTo(x0, y0 + r);
        shape.arcTo(QRectF(x0, y0, 2 * r, 2 * r), 180, -90);
        shape.lineTo(x1 - r, y0);
        shape.arcTo(QRectF(x1 - 2 * r, y0, 2 * r, 2 * r), 90, -90);
        shape.lineTo(x1, y1 - c);
        shape.arcTo(QRectF(x1, y1 - 2 * c, 2 * c, 2 * c), 180, 90);
        shape.closeSubpath();
        p.fillPath(shape, tok(t, "--tab-background-active"));
        // 1px outline outside the tab (box-shadow ring) and inside the curves (inset ring).
        QPainterPath outline;
        const double h = 0.5;
        outline.moveTo(x0 - c, y1 - h);
        outline.arcTo(QRectF(x0 - 2 * c + h, y1 - 2 * c + h, 2 * c - 2 * h, 2 * c - 2 * h), 270, 90);
        outline.lineTo(x0 - h, y0 + r);
        outline.arcTo(QRectF(x0 - h, y0 - h, 2 * r + 2 * h, 2 * r + 2 * h), 180, -90);
        outline.lineTo(x1 - r, y0 - h);
        outline.arcTo(QRectF(x1 - 2 * r - h, y0 - h, 2 * r + 2 * h, 2 * r + 2 * h), 90, -90);
        outline.lineTo(x1 + h, y1 - c);
        outline.arcTo(QRectF(x1 + h, y1 - 2 * c + h, 2 * c - 2 * h, 2 * c - 2 * h), 180, 90);
        QPen pen(tok(t, "--tab-outline-color"), t->px(QStringLiteral("--tab-outline-width"), -1, 1));
        p.strokePath(outline, pen);
    } else {
        if (hovered) {
            QPainterPath hp;
            hp.addRoundedRect(g.inner, 4, 4);
            p.fillPath(hp, tok(t, "--background-modifier-hover"));
        }
        // 1px divider (20px tall) at the tab's right edge; hidden under/next to the active tab.
        const double dx = std::round(g.rect.right());
        p.fillRect(QRectF(dx, std::round(g.inner.center().y() - 10), 1, 20), tok(t, "--tab-divider-color"));
    }

    // The contents are clipped by .workspace-tab-header-inner (overflow hidden) in narrow tabs.
    p.save();
    p.setClipRect(g.inner, Qt::IntersectClip);
    // Title / icon colors (body.is-focused / .mod-active variants).
    QColor textColor;
    if (focused)
        textColor = active ? (groupActive ? tok(t, "--tab-text-color-focused-active-current") : tok(t, "--tab-text-color-focused-active"))
                           : tok(t, "--tab-text-color-focused");
    else
        textColor = active ? tok(t, "--tab-text-color-active") : tok(t, "--tab-text-color");

    if (g.showIcon) {
        QColor ic = (focused && active && groupActive) ? textColor : (hovered ? tok(t, "--icon-color-hover") : tok(t, "--icon-color"));
        p.setOpacity(hovered || (focused && active && groupActive) ? 1.0 : t->number(QStringLiteral("--icon-opacity"), 0.85));
        p.drawPixmap(g.icon.topLeft(), icon(leaf->icon(), 14, ic, 2.0, dpr));
        p.setOpacity(1.0);
    }

    const bool showClose = closeVisible(index);
    QRectF titleRect = g.title;
    if (showClose)
        titleRect.setRight(g.close.left() - 2);
    QFont f = t->interfaceFont(t->px(QStringLiteral("--tab-font-size"), -1, 13));
    if (qobject_cast<const PlaceholderView*>(leaf->view()))
        f.setItalic(true); // .workspace-tab-header.mod-unknown
    p.setFont(f);
    p.setPen(textColor);
    const QFontMetricsF fm(f);
    p.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(leaf->displayText(), Qt::ElideRight, std::max(0.0, titleRect.width())));

    if (showClose) {
        const bool overClose = hovered && m_hoverClose;
        if (leaf->isPinned()) {
            if (overClose && active) {
                QPainterPath bp;
                bp.addRoundedRect(g.close, 4, 4);
                p.fillPath(bp, tok(t, "--background-modifier-active-hover"));
            }
            p.drawPixmap(QPointF(g.close.right() - 18, g.close.top() + 2), icon(QStringLiteral("pin"), 16, tok(t, "--text-accent"), 2.0, dpr));
        } else {
            if (overClose && active) {
                QPainterPath bp;
                bp.addRoundedRect(g.close, 4, 4);
                p.fillPath(bp, tok(t, "--background-modifier-hover"));
            }
            QColor cc = overClose ? tok(t, "--tab-text-color-focused-active-current")
                                  : (active ? tok(t, "--tab-text-color-focused-active") : tok(t, "--tab-text-color-focused"));
            p.drawPixmap(QPointF(g.close.right() - 18, g.close.top() + 2), icon(QStringLiteral("x"), 16, cc, 2.0, dpr));
        }
    }
    p.restore();
}

void TabHeaderBar::paintSideTab(QPainter& p, int index, const TabGeom& g)
{
    const ThemePtr t = currentTheme();
    const WorkspaceLeaf* leaf = leafAt(index);
    if (!leaf)
        return;
    const bool active = index == m_group->currentTab;
    const bool hovered = index == m_hoverTab;
    if (active || hovered) {
        QPainterPath bp;
        bp.addRoundedRect(g.rect, 4, 4);
        p.fillPath(bp, tok(t, "--background-modifier-hover"));
    }
    QColor ic = active ? tok(t, "--icon-color-focused") : (hovered ? tok(t, "--icon-color-hover") : tok(t, "--icon-color"));
    p.setOpacity(active || hovered ? 1.0 : t->number(QStringLiteral("--icon-opacity"), 0.85));
    p.drawPixmap(g.icon.topLeft(), icon(leaf->icon(), 18, ic, t->px(QStringLiteral("--icon-l-stroke-width"), -1, 1.75), devicePixelRatioF()));
    p.setOpacity(1.0);
}

void TabHeaderBar::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const bool focused = d->windowFocused();
    // .workspace-tabs.mod-top uses the title bar colors (brighter while the window is focused).
    const QColor bg = m_top ? (focused ? tok(t, "--titlebar-background-focused") : tok(t, "--titlebar-background"))
                            : tok(t, "--tab-container-background");
    p.fillRect(rect(), bg);
    p.fillRect(QRectF(0, kHeaderHeight - 1, width(), 1), tok(t, "--tab-outline-color"));
    // The tab strip scrolls inside .workspace-tab-header-container-inner (overflow hidden).
    if (!m_group)
        return;
    p.save();
    p.setClipRect(QRectF(m_clipL, 0, std::max(0.0, m_clipR - m_clipL), height()));
    if (m_sidedock) {
        for (int i = 0; i < m_tabs.size(); ++i)
            paintSideTab(p, i, m_tabs[i]);
        p.restore();
        return;
    }
    const int cur = m_group->currentTab;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (i != cur)
            paintRootTab(p, i, m_tabs[i]);
    if (cur >= 0 && cur < m_tabs.size())
        paintRootTab(p, cur, m_tabs[cur]);
    p.restore();
}

void TabHeaderBar::resizeEvent(QResizeEvent*)
{
    m_snappedTab = -2; // re-reveal the current tab at the new width
    relayout();
}

void TabHeaderBar::updateHover(const QPoint& pos)
{
    const int i = tabAt(pos);
    const bool overClose = i >= 0 && !m_sidedock && m_tabs[i].close.contains(pos);
    if (i != m_hoverTab || overClose != m_hoverClose) {
        m_hoverTab = i;
        m_hoverClose = overClose;
        update();
    }
    // Empty header space of a top tab group drags the frameless window (WindowFrameController).
    setProperty("ocDragRegion", m_top && i < 0);
}

void TabHeaderBar::mousePressEvent(QMouseEvent* e)
{
    updateHover(e->pos());
    const int i = tabAt(e->pos());
    m_pressTab = i;
    m_pressButton = e->button();
    m_pressPos = e->pos();
    m_dragging = false;
    m_pressClose = i >= 0 && closeVisible(i) && m_tabs[i].close.contains(e->pos());
    e->accept();
}

void TabHeaderBar::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton) || m_pressTab < 0 || m_pressButton != Qt::LeftButton || m_pressClose) {
        updateHover(e->pos());
        return;
    }
    const QPoint delta = e->pos() - m_pressPos;
    if (!m_dragging && delta.x() * delta.x() + delta.y() * delta.y() >= kDragThreshold * kDragThreshold) {
        if (WorkspaceLeaf* leaf = leafAt(m_pressTab)) {
            m_dragging = true;
            d->beginDrag(leaf, e->globalPosition().toPoint());
        }
    }
    if (m_dragging)
        d->updateDrag(e->globalPosition().toPoint());
}

void TabHeaderBar::mouseReleaseEvent(QMouseEvent* e)
{
    const int pressed = m_pressTab;
    m_pressTab = -1;
    if (m_dragging) {
        m_dragging = false;
        d->endDrag(e->globalPosition().toPoint(), true);
        return;
    }
    const int i = tabAt(e->pos());
    if (i < 0 || i != pressed)
        return;
    WorkspaceLeaf* leaf = leafAt(i);
    if (!leaf)
        return;
    if (e->button() == Qt::MiddleButton && m_pressButton == Qt::MiddleButton) {
        leaf->detach();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (m_pressClose && m_tabs[i].close.contains(e->pos())) {
        if (leaf->isPinned()) {
            leaf->setPinned(false);
        } else {
            // Obsidian lockTabWidths(): the next tab slides under the pointer instead of the strip
            // re-flowing, until the pointer leaves the header.
            m_lockedWidth = m_tabs[i].rect.width();
            leaf->detach();
        }
        return;
    }
    d->q->setActiveLeaf(leaf, true);
}

void TabHeaderBar::leaveEvent(QEvent* e)
{
    m_hoverTab = -1;
    m_hoverClose = false;
    if (m_lockedWidth > 0) {
        m_lockedWidth = -1;
        relayout();
    }
    update();
    QWidget::leaveEvent(e);
}

void TabHeaderBar::wheelEvent(QWheelEvent* e)
{
    const QPoint delta = e->angleDelta();
    const int step = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
    m_scroll -= step / 2.0;
    relayout(); // clamps the scroll offset
    update();
    e->accept();
}

void TabHeaderBar::contextMenuEvent(QContextMenuEvent* e)
{
    const int i = tabAt(e->pos());
    if (i >= 0)
        showTabMenu(i, e->globalPos());
}

void TabHeaderBar::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange)
        update();
    QWidget::changeEvent(e);
}

bool TabHeaderBar::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = tabAt(he->pos());
        if (WorkspaceLeaf* leaf = leafAt(i); leaf && i >= 0) {
            QString tip;
            if (closeVisible(i) && m_tabs[i].close.contains(he->pos()))
                tip = leaf->isPinned() ? tr("Unpin") : tr("Close");
            else
                tip = leaf->displayText().left(100);
            QToolTip::showText(he->globalPos(), tip, this, tabRect(i));
        } else {
            QToolTip::hideText();
            e->ignore();
        }
        return true;
    }
    return QWidget::event(e);
}

void TabHeaderBar::showTabMenu(int index, const QPoint& globalPos)
{
    QPointer<WorkspaceLeaf> leaf = leafAt(index);
    if (!leaf)
        return;
    WorkspaceImpl* dd = d;
    QMenu menu(this);
    menu.addAction(menuIcon(QStringLiteral("x")), tr("Close"), &menu, [leaf] {
        if (leaf)
            leaf->detach();
    });
    if (!m_sidedock) {
        const QList<WorkspaceLeaf*> all = m_group->leaves;
        bool others = false, after = false;
        int unpinned = 0;
        for (int i = 0; i < all.size(); ++i) {
            if (all[i]->isPinned())
                continue;
            ++unpinned;
            others = others || all[i] != leaf;
            after = after || i > index;
        }
        if (others)
            menu.addAction(menuIcon(QStringLiteral("x")), tr("Close others"), &menu, [dd, leaf] { dd->closeOthers(leaf); });
        if (after)
            menu.addAction(menuIcon(QStringLiteral("x")), tr("Close tabs after"), &menu, [dd, leaf] { dd->closeAfter(leaf); });
        if (unpinned > 0 && !(unpinned == 1 && !leaf->isPinned()))
            menu.addAction(menuIcon(QStringLiteral("x")), tr("Close all"), &menu, [dd, group = m_group] { dd->closeAll(group); });
        if (leaf->view() && leaf->view()->viewType() != u"empty") {
            menu.addSeparator();
            menu.addAction(menuIcon(QStringLiteral("pin")), leaf->isPinned() ? tr("Unpin") : tr("Pin"), &menu, [leaf] {
                if (leaf)
                    leaf->setPinned(!leaf->isPinned());
            });
        }
    }
    menu.addSeparator();
    QAction* right = menu.addAction(menuIcon(QStringLiteral("separator-vertical")), tr("Split right"), &menu, [dd, leaf] {
        if (leaf)
            dd->q->duplicateLeaf(leaf, Workspace::NewLeaf::SplitRight);
    });
    right->setEnabled(!m_sidedock);
    menu.addAction(menuIcon(QStringLiteral("separator-horizontal")), tr("Split down"), &menu, [dd, leaf] {
        if (leaf)
            dd->q->duplicateLeaf(leaf, Workspace::NewLeaf::SplitDown);
    });
    if (leaf->view()) {
        const int before = int(menu.actions().size());
        menu.addSeparator();
        leaf->view()->populatePaneMenu(&menu);
        if (menu.actions().size() == before + 1)
            menu.removeAction(menu.actions().last());
    }
    menu.exec(globalPos);
}

void TabHeaderBar::showTabList()
{
    if (!m_group)
        return;
    WorkspaceImpl* dd = d;
    QMenu menu(this);
    int unpinned = 0;
    for (const WorkspaceLeaf* l : std::as_const(m_group->leaves))
        unpinned += l->isPinned() ? 0 : 1;
    if (unpinned > 0) {
        menu.addAction(menuIcon(QStringLiteral("x")), tr("Close all"), &menu, [dd, group = m_group] { dd->closeAll(group); });
        menu.addSeparator();
    }
    for (int i = 0; i < m_group->leaves.size(); ++i) {
        QPointer<WorkspaceLeaf> leaf = m_group->leaves[i];
        QAction* a = menu.addAction(menuIcon(leaf->icon()), leaf->displayText(), &menu, [dd, leaf] {
            if (leaf)
                dd->q->setActiveLeaf(leaf, true);
        });
        a->setCheckable(true);
        a->setChecked(i == m_group->currentTab);
    }
    execMenuBelow(menu, m_tabList, true);
}

// ---------------------------------------------------------------------------------------------
// TabGroupWidget
// ---------------------------------------------------------------------------------------------

TabGroupWidget::TabGroupWidget(WorkspaceImpl* d, WorkspaceTabs* tabs)
    : QWidget(d->widget)
    , d(d)
    , m_tabs(tabs)
    , m_header(new TabHeaderBar(d, tabs, this))
{
    setObjectName(QStringLiteral("ocTabGroup"));
    hide();
}

void TabGroupWidget::sync()
{
    if (!m_tabs)
        return;
    WorkspaceSplit* top = d->topSplit(m_tabs);
    m_header->setSidedock(top && top->isSidedock());
    WorkspaceLeaf* cur = m_tabs->current();
    for (WorkspaceLeaf* l : std::as_const(m_tabs->leaves)) {
        LeafWidget* lw = d->leafWidget(l);
        if (!lw)
            continue;
        if (lw->parentWidget() != this)
            lw->setParent(this);
        lw->refresh();
        lw->setVisible(l == cur);
    }
    layoutChildren();
    m_header->relayout();
    m_header->update();
}

void TabGroupWidget::releaseLeaves()
{
    for (QObject* o : children()) {
        if (auto* lw = qobject_cast<LeafWidget*>(o)) {
            lw->hide();
            lw->setParent(d->widget);
        }
    }
}

void TabGroupWidget::detachModel()
{
    m_tabs = nullptr;
    m_header->detachModel();
}

void TabGroupWidget::layoutChildren()
{
    m_header->setGeometry(0, 0, width(), kHeaderHeight);
    if (!m_tabs)
        return;
    for (WorkspaceLeaf* l : std::as_const(m_tabs->leaves))
        if (LeafWidget* lw = d->leafWidget(l); lw && lw->parentWidget() == this)
            lw->setGeometry(0, kHeaderHeight, width(), std::max(0, height() - kHeaderHeight));
}

void TabGroupWidget::resizeEvent(QResizeEvent*) { layoutChildren(); }

void TabGroupWidget::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    WorkspaceSplit* top = d->topSplit(m_tabs);
    p.fillRect(rect(), top && top->isSidedock() ? tok(t, "--background-secondary") : tok(t, "--background-primary"));
}

// ---------------------------------------------------------------------------------------------
// ViewHeaderTitle
// ---------------------------------------------------------------------------------------------

ViewHeaderTitle::ViewHeaderTitle(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void ViewHeaderTitle::setParts(const QStringList& folders, const QString& title)
{
    if (folders == m_folders && title == m_title)
        return;
    m_folders = folders;
    m_title = title;
    updateGeometry();
    update();
}

void ViewHeaderTitle::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

QString ViewHeaderTitle::text() const
{
    QStringList parts = m_folders;
    parts.push_back(m_title);
    return parts.join(QStringLiteral(" / "));
}

QSize ViewHeaderTitle::sizeHint() const { return {200, kHeaderHeight}; }
QSize ViewHeaderTitle::minimumSizeHint() const { return {0, kHeaderHeight}; }

void ViewHeaderTitle::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QFont f = t->interfaceFont(t->px(QStringLiteral("--file-header-font-size"), -1, 13));
    p.setFont(f);
    const QFontMetricsF fm(f);
    const QColor muted = tok(t, "--text-muted"), faint = tok(t, "--text-faint");
    const QColor titleColor = m_active ? tok(t, "--text-normal") : muted;
    const double avail = width();
    // .view-header-breadcrumb: padding 2px 4px; separator "/" padding 2px 1px; gap 4px before the title.
    double parentW = 0;
    for (const QString& folder : m_folders)
        parentW += fm.horizontalAdvance(folder) + 8 + fm.horizontalAdvance(QStringLiteral("/")) + 2;
    const double gap = m_folders.isEmpty() ? 0 : 4;
    double titleW = fm.horizontalAdvance(m_title);
    // The parent path shrinks first (flex-shrink 100), then the title is elided.
    double shownParent = std::min(parentW, std::max(0.0, avail - titleW - gap));
    if (shownParent < 24)
        shownParent = 0; // not even an ellipsis fits
    const double shownGap = shownParent > 0 ? gap : 0;
    titleW = std::min(titleW, std::max(0.0, avail - shownParent - shownGap));
    double x = std::round((avail - (shownParent + shownGap + titleW)) / 2);
    const double cy = height() / 2.0;
    const double lineH = fm.height();
    if (shownParent > 0) {
        p.save();
        p.setClipRect(QRectF(x, 0, shownParent, height()));
        double px = x;
        const bool elideParent = shownParent < parentW;
        for (const QString& folder : m_folders) {
            p.setPen(muted);
            p.drawText(QRectF(px + 4, cy - lineH / 2, fm.horizontalAdvance(folder) + 1, lineH), Qt::AlignVCenter | Qt::AlignLeft, folder);
            px += fm.horizontalAdvance(folder) + 8;
            p.setPen(faint);
            p.drawText(QRectF(px + 1, cy - lineH / 2, fm.horizontalAdvance(QStringLiteral("/")) + 1, lineH), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("/"));
            px += fm.horizontalAdvance(QStringLiteral("/")) + 2;
        }
        p.restore();
        if (elideParent) {
            // fade the cut-off end with an ellipsis
            const double ew = fm.horizontalAdvance(QStringLiteral("…"));
            p.fillRect(QRectF(x + shownParent - ew - 2, 0, ew + 2, height()), tok(t, "--background-primary"));
            p.setPen(muted);
            p.drawText(QRectF(x + shownParent - ew - 2, cy - lineH / 2, ew + 2, lineH), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("…"));
        }
        x += shownParent + shownGap;
    }
    p.setPen(titleColor);
    p.drawText(QRectF(x, cy - lineH / 2, titleW + 1, lineH), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(m_title, Qt::ElideRight, titleW + 0.5));
}

// ---------------------------------------------------------------------------------------------
// ViewHeader
// ---------------------------------------------------------------------------------------------

ViewHeader::ViewHeader(WorkspaceImpl* d, WorkspaceLeaf* leaf, QWidget* parent)
    : QWidget(parent)
    , d(d)
    , m_leaf(leaf)
{
    setObjectName(QStringLiteral("ocViewHeader"));
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(12, 0, 12, 0);
    h->setSpacing(8);
    auto* nav = new QWidget(this);
    auto* nl = new QHBoxLayout(nav);
    nl->setContentsMargins(0, 0, 0, 0);
    nl->setSpacing(0);
    auto makeButton = [](const QString& iconName, const QString& tip, QWidget* parent) {
        // .clickable-icon in the view header: 16px icon (2px stroke), padding 4px 6px.
        auto* b = new IconButton(iconName, tip, parent);
        b->setButtonSize({28, 24});
        b->setIconSize(16);
        b->setStrokeWidth(2.0);
        return b;
    };
    m_back = makeButton(QStringLiteral("arrow-left"), tr("Navigate back"), nav);
    m_forward = makeButton(QStringLiteral("arrow-right"), tr("Navigate forward"), nav);
    nl->addWidget(m_back);
    nl->addWidget(m_forward);
    h->addWidget(nav, 0, Qt::AlignVCenter);
    m_title = new ViewHeaderTitle(this);
    h->addWidget(m_title, 1);
    m_actions = new QWidget(this);
    auto* al = new QHBoxLayout(m_actions);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(0);
    m_more = makeButton(QStringLiteral("ellipsis-vertical"), tr("More options"), m_actions);
    al->addWidget(m_more);
    h->addWidget(m_actions, 0, Qt::AlignVCenter);

    connect(m_back, &IconButton::clicked, this, [this] { m_leaf->goBack(); });
    connect(m_forward, &IconButton::clicked, this, [this] { m_leaf->goForward(); });
    connect(m_more, &IconButton::clicked, this, [this] { showMoreMenu(); });
    m_back->installEventFilter(this);
    m_forward->installEventFilter(this);
    connect(leaf, &WorkspaceLeaf::historyChanged, this, &ViewHeader::refresh);
    connect(leaf, &WorkspaceLeaf::viewChanged, this, &ViewHeader::refreshActions);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    refreshActions();
    refresh();
}

void ViewHeader::refresh()
{
    View* v = m_leaf->view();
    m_back->setEnabled(m_leaf->canGoBack());
    m_forward->setEnabled(m_leaf->canGoForward());
    if (auto* fv = qobject_cast<FileView*>(v)) {
        const QString path = fv->file() ? fv->file()->path() : fv->state().value(QStringLiteral("file")).toString();
        QStringList folders = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!folders.isEmpty())
            folders.removeLast();
        m_title->setParts(folders, fv->displayText());
    } else {
        m_title->setParts({}, v ? v->displayText() : QString());
    }
    // .is-focused .workspace-leaf.mod-active .view-header-title { color: --text-normal }
    m_title->setActive(d->active == m_leaf && d->windowFocused());
}

void ViewHeader::refreshActions()
{
    for (const QPointer<QWidget>& w : std::as_const(m_viewActions))
        if (w)
            w->deleteLater();
    m_viewActions.clear();
    auto* al = static_cast<QHBoxLayout*>(m_actions->layout());
    if (View* v = m_leaf->view()) {
        int i = 0;
        for (QWidget* w : v->headerActions()) {
            if (!w)
                continue;
            w->setParent(m_actions);
            al->insertWidget(i++, w);
            w->show();
            m_viewActions.push_back(w);
        }
    }
    refresh();
}

void ViewHeader::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    const bool focusedActive = d->active == m_leaf && d->windowFocused();
    p.fillRect(rect(), focusedActive ? tok(t, "--file-header-background-focused") : tok(t, "--file-header-background"));
}

bool ViewHeader::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::ContextMenu && (obj == m_back || obj == m_forward)) {
        showHistoryMenu(obj == m_back);
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

void ViewHeader::showMoreMenu()
{
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    WorkspaceImpl* dd = d;
    QMenu menu(this);
    menu.addAction(menuIcon(QStringLiteral("x")), tr("Close"), &menu, [leaf] {
        if (leaf)
            leaf->detach();
    });
    if (leaf->view() && leaf->view()->viewType() != u"empty")
        menu.addAction(menuIcon(QStringLiteral("pin")), leaf->isPinned() ? tr("Unpin") : tr("Pin"), &menu, [leaf] {
            if (leaf)
                leaf->setPinned(!leaf->isPinned());
        });
    menu.addSeparator();
    menu.addAction(menuIcon(QStringLiteral("separator-vertical")), tr("Split right"), &menu, [dd, leaf] {
        if (leaf)
            dd->q->duplicateLeaf(leaf, Workspace::NewLeaf::SplitRight);
    });
    menu.addAction(menuIcon(QStringLiteral("separator-horizontal")), tr("Split down"), &menu, [dd, leaf] {
        if (leaf)
            dd->q->duplicateLeaf(leaf, Workspace::NewLeaf::SplitDown);
    });
    if (View* v = leaf->view()) {
        const int before = int(menu.actions().size());
        menu.addSeparator();
        v->populatePaneMenu(&menu);
        if (menu.actions().size() == before + 1)
            menu.removeAction(menu.actions().last());
    }
    execMenuBelow(menu, m_more, true);
}

void ViewHeader::showHistoryMenu(bool back)
{
    const auto items = d->historyItems(m_leaf, back);
    if (items.isEmpty())
        return;
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    WorkspaceImpl* dd = d;
    QMenu menu(this);
    for (int i = 0; i < items.size(); ++i) {
        QString title = items[i].first;
        if (title.size() > 50)
            title = title.left(50) + QStringLiteral("…");
        const int steps = back ? -(i + 1) : i + 1;
        menu.addAction(menuIcon(items[i].second.isEmpty() ? QStringLiteral("file") : items[i].second), title, &menu, [dd, leaf, steps] {
            if (leaf)
                dd->goHistory(leaf, steps);
        });
    }
    execMenuBelow(menu, back ? m_back : m_forward, false);
}

// ---------------------------------------------------------------------------------------------
// LeafWidget
// ---------------------------------------------------------------------------------------------

LeafWidget::LeafWidget(WorkspaceImpl* d, WorkspaceLeaf* leaf)
    : QWidget(d->widget)
    , d(d)
    , m_leaf(leaf)
{
    setObjectName(QStringLiteral("ocLeaf"));
    hide();
}

void LeafWidget::setView(View* view)
{
    m_view = view;
    if (view) {
        if (view->parentWidget() != this)
            view->setParent(this);
        view->show();
    }
    if (m_header)
        m_header->refreshActions();
    refresh();
}

void LeafWidget::refresh()
{
    const bool inRoot = m_leaf->tabs() && !m_leaf->isInSidedock();
    const bool want = inRoot && m_view && m_view->showsViewHeader();
    if (want && !m_header)
        m_header = new ViewHeader(d, m_leaf, this);
    if (m_header) {
        m_header->setVisible(want);
        if (want)
            m_header->refresh();
    }
    layoutChildren();
    update();
}

void LeafWidget::layoutChildren()
{
    int y = 0;
    if (m_header && !m_header->isHidden()) {
        m_header->setGeometry(0, 0, width(), kHeaderHeight);
        y = kHeaderHeight;
    }
    if (m_view)
        m_view->setGeometry(0, y, width(), std::max(0, height() - y));
}

void LeafWidget::resizeEvent(QResizeEvent*) { layoutChildren(); }

void LeafWidget::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.fillRect(rect(), m_leaf->isInSidedock() ? tok(t, "--background-secondary") : tok(t, "--background-primary"));
}

// ---------------------------------------------------------------------------------------------
// WorkspaceWidget
// ---------------------------------------------------------------------------------------------

WorkspaceWidget::WorkspaceWidget(WorkspaceImpl* d)
    : QWidget(nullptr)
    , d(d)
    , m_leftHandle(new ResizeHandle(ResizeHandle::Line::Right, this))
    , m_rightHandle(new ResizeHandle(ResizeHandle::Line::Left, this))
{
    setObjectName(QStringLiteral("ocWorkspace"));
    // Obsidian's sidedock resize: dragging below 50px collapses, past 200px re-expands, and the
    // width is clamped to [200, 80% of the workspace].
    auto dragSide = [this](Workspace::Side side, QPoint g) {
        WorkspaceSplit* s = side == Workspace::Side::Left ? this->d->left : this->d->right;
        const QPoint local = mapFromGlobal(g);
        const int size = side == Workspace::Side::Left ? local.x() + 2 : width() - local.x() + 2;
        if (!s->collapsed && size < 50) {
            this->d->q->setSideCollapsed(side, true);
        } else if (s->collapsed && size >= kMinSideWidth) {
            this->d->q->setSideCollapsed(side, false);
        } else if (!s->collapsed) {
            s->width = std::clamp(size, kMinSideWidth, std::max(kMinSideWidth, int(width() * 0.8)));
            relayout();
        }
    };
    m_leftHandle->onDrag = [dragSide](QPoint g) { dragSide(Workspace::Side::Left, g); };
    m_rightHandle->onDrag = [dragSide](QPoint g) { dragSide(Workspace::Side::Right, g); };
    auto finished = [this] {
        this->d->q->requestSaveLayout();
        emit this->d->q->layoutChanged();
    };
    m_leftHandle->onDragFinished = finished;
    m_rightHandle->onDragFinished = finished;
}

QRect WorkspaceWidget::sideRect(Workspace::Side side) const
{
    WorkspaceSplit* s = side == Workspace::Side::Left ? d->left : d->right;
    return s && s->w && s->w->isVisible() ? s->w->geometry() : QRect();
}

void WorkspaceWidget::relayout()
{
    if (!d->root || !d->left || !d->right)
        return;
    const int W = width(), H = height();
    int lw = d->left->collapsed ? 0 : d->left->width;
    int rw = d->right->collapsed ? 0 : d->right->width;
    // Keep some room for the root split when the window is narrow.
    const int maxSides = std::max(0, W - 100);
    if (lw + rw > maxSides && lw + rw > 0) {
        const double k = double(maxSides) / (lw + rw);
        lw = int(lw * k);
        rw = int(rw * k);
    }
    d->left->w->setGeometry(0, 0, lw, H);
    d->left->w->setVisible(lw > 0);
    d->right->w->setGeometry(W - rw, 0, rw, H);
    d->right->w->setVisible(rw > 0);
    d->root->w->setGeometry(lw, 0, std::max(0, W - lw - rw), H);
    d->root->w->show();
    const int top = d->frameless() ? 0 : kHeaderHeight;
    m_leftHandle->setGeometry(lw - 3, top, 3, std::max(0, H - top));
    m_leftHandle->setVisible(lw > 0);
    m_leftHandle->raise();
    m_rightHandle->setGeometry(W - rw, top, 3, std::max(0, H - top));
    m_rightHandle->setVisible(rw > 0);
    m_rightHandle->raise();
}

void WorkspaceWidget::resizeEvent(QResizeEvent*) { relayout(); }

void WorkspaceWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-secondary")));
}

// ---------------------------------------------------------------------------------------------
// DropOverlay
// ---------------------------------------------------------------------------------------------

DropOverlay::DropOverlay(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocDropOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    m_anim.setDuration(100); // transition: all 100ms ease-in-out
    m_anim.setEasingCurve(QEasingCurve::InOutQuad);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_shown = v.toRectF();
        update();
    });
    hide();
}

void DropOverlay::setTarget(const QRect& rect)
{
    if (rect == m_target)
        return;
    m_target = rect;
    m_anim.stop();
    if (rect.isNull()) {
        m_shown = {};
    } else if (m_shown.isNull() || !QApplication::isEffectEnabled(Qt::UI_AnimateCombo)) {
        m_shown = rect;
    } else {
        m_anim.setStartValue(m_shown);
        m_anim.setEndValue(QRectF(rect));
        m_anim.start();
    }
    update();
}

void DropOverlay::setGhost(const QPixmap& ghost, const QPoint& pos)
{
    m_ghost = ghost;
    m_ghostPos = pos;
    update();
}

void DropOverlay::moveGhost(const QPoint& pos)
{
    if (pos == m_ghostPos)
        return;
    m_ghostPos = pos;
    update();
}

void DropOverlay::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!m_shown.isNull()) {
        // .workspace-drop-overlay:before — inset 3px, --radius-m, --interactive-accent at 50%.
        QColor c = tok(t, "--interactive-accent");
        c.setAlphaF(0.5);
        QPainterPath path;
        const double radius = t->px(QStringLiteral("--radius-m"), -1, 8);
        const QRectF r = m_shown.adjusted(3, 3, -3, -3);
        path.addRoundedRect(r, std::min(radius, r.width() / 2), std::min(radius, r.height() / 2));
        p.fillPath(path, c);
    }
    if (!m_ghost.isNull())
        p.drawPixmap(m_ghostPos - QPoint(8, 8), m_ghost); // the ghost's top-left corner sits at the pointer
}

// ---------------------------------------------------------------------------------------------
// Workspace::Impl — drag & drop
// ---------------------------------------------------------------------------------------------

DropOverlay* Workspace::Impl::ensureOverlay()
{
    QWidget* top = widget ? widget->window() : nullptr;
    if (!top)
        return nullptr;
    if (!overlay || overlay->parentWidget() != top) {
        delete overlay.data();
        overlay = new DropOverlay(top);
    }
    overlay->setGeometry(top->rect());
    overlay->raise();
    overlay->show();
    return overlay;
}

Workspace::Impl::DropTarget Workspace::Impl::dropTargetAt(const WorkspaceLeaf* dragged, const QPoint& globalPos) const
{
    DropTarget t;
    if (!widget || !root)
        return t;
    // Obsidian's getDropLocation(): the ribbon (with the left sidebar toggle) drops into the left
    // sidedock, the right sidebar toggle into the right one; a collapsed sidedock highlights its toggle.
    if (MainWindow* mw = window()) {
        auto over = [&](const QWidget* w) { return w && w->isVisible() && w->rect().contains(w->mapFromGlobal(globalPos)); };
        Ribbon* r = mw->ribbon();
        QWidget* rightToggle = mw->rightSidebarToggle();
        const bool overRibbon = over(r);
        if (overRibbon || over(rightToggle)) {
            WorkspaceSplit* side = overRibbon ? left : right;
            if (side->children.size() == 1 && side->children.first()->isTabs()
                && side->children.first()->asTabs()->leaves == QList<WorkspaceLeaf*>{const_cast<WorkspaceLeaf*>(dragged)})
                return t; // its only tab: nothing would change
            t.kind = DropTarget::Kind::Side;
            t.side = side;
            if (side->collapsed) {
                QWidget* toggle = overRibbon ? static_cast<QWidget*>(r->sidebarToggle()) : rightToggle;
                t.rect = QRect(widget->mapFromGlobal(toggle->mapToGlobal(QPoint(0, 0))), toggle->size());
            } else {
                t.rect = side->w->geometry();
            }
            return t;
        }
    }
    const QPoint p = widget->mapFromGlobal(globalPos);
    if (!widget->rect().contains(p))
        return t;
    WorkspaceSplit* area = nullptr;
    for (WorkspaceSplit* s : {left, right, root}) {
        if (s->isSidedock() && s->collapsed)
            continue;
        if (s->w->isVisible() && s->w->geometry().contains(p)) {
            area = s;
            break;
        }
    }
    if (!area)
        return t;
    if (area->children.isEmpty()) {
        t.kind = DropTarget::Kind::Side;
        t.side = area;
        t.rect = area->w->geometry();
        return t;
    }
    WorkspaceTabs* tabs = nullptr;
    QRect R;
    forEachTabs(area, [&](WorkspaceTabs* tb) {
        if (tabs || !tb->w->isVisible())
            return;
        const QRect r(tb->w->mapTo(widget, QPoint(0, 0)), tb->w->size());
        if (r.contains(p)) {
            tabs = tb;
            R = r;
        }
    });
    if (!tabs)
        return t;
    if (tabs->leaves.size() == 1 && tabs->leaves.first() == dragged)
        return t; // Obsidian shows nothing when hovering the dragged tab's own single-tab group
    // Obsidian's getDropDirection().
    const bool inSidedock = area->isSidedock();
    const double fx = p.x() - R.x(), fy = p.y() - R.y();
    const double a = std::abs(fx) / R.width(), s = std::abs(fy) / R.height();
    const double l = std::abs(p.x() - (R.x() + R.width())) / double(R.width());
    const double c = std::abs(p.y() - (R.y() + R.height())) / double(R.height());
    DropEdge edge = DropEdge::Center;
    double h = 1;
    if (s < 0.33 && s < h) {
        h = s;
        if (fy < kHeaderHeight)
            h = 0; // over the tab header: only "top" (upper third of the header) or a tab insert
        edge = fy <= kHeaderHeight / 3.0 ? DropEdge::Top : DropEdge::Center;
    }
    if (!inSidedock && a < 0.33 && a < h) {
        h = a;
        edge = DropEdge::Left;
    }
    if (!inSidedock && l < 0.33 && l < h) {
        h = l;
        edge = DropEdge::Right;
    }
    if (c < 0.33 && c < h) {
        h = c;
        edge = DropEdge::Bottom;
    }
    t.kind = DropTarget::Kind::Tabs;
    t.tabs = tabs;
    t.edge = edge;
    if (edge == DropEdge::Center) {
        const TabHeaderBar::InsertLocation loc = tabs->w->header()->insertLocation(int(fx));
        t.index = loc.index;
        t.rect = loc.rect.translated(R.topLeft());
    } else {
        const bool horizontalBand = edge == DropEdge::Left || edge == DropEdge::Right;
        const int size = std::max((horizontalBand ? R.width() : R.height()) / 3, 40);
        switch (edge) {
        case DropEdge::Left:
            t.rect = QRect(R.x(), R.y(), size, R.height());
            break;
        case DropEdge::Right:
            t.rect = QRect(R.right() + 1 - size, R.y(), size, R.height());
            break;
        case DropEdge::Top:
            t.rect = QRect(R.x(), R.y(), R.width(), size);
            break;
        case DropEdge::Bottom:
            t.rect = QRect(R.x(), R.bottom() + 1 - size, R.width(), size);
            break;
        case DropEdge::Center:
            break;
        }
    }
    return t;
}

bool Workspace::Impl::applyDrop(WorkspaceLeaf* leaf, const DropTarget& target)
{
    if (!leaf || !leaf->m_tabs || !target.isValid())
        return false;
    bool focus = true;
    if (target.kind == DropTarget::Kind::Side) {
        moveLeafToSide(leaf, target.side);
        focus = !target.side->collapsed;
    } else if (target.edge == DropEdge::Center) {
        moveLeaf(leaf, target.tabs, target.index);
    } else if (!splitLeafTo(leaf, target.tabs, target.edge)) {
        return false;
    }
    if (focus)
        q->setActiveLeaf(leaf, true);
    return true;
}

void Workspace::Impl::beginDrag(WorkspaceLeaf* leaf, const QPoint& globalPos)
{
    if (!leaf || drag.active)
        return;
    drag.leaf = leaf;
    drag.active = true;
    drag.target = {};
    QApplication::setOverrideCursor(Qt::ClosedHandCursor); // body.is-grabbing
    if (DropOverlay* o = ensureOverlay())
        o->setGhost(makeGhost(leaf, o->devicePixelRatioF()), o->mapFromGlobal(globalPos));
    updateDrag(globalPos);
}

void Workspace::Impl::updateDrag(const QPoint& globalPos)
{
    if (!drag.active || !drag.leaf)
        return;
    drag.target = dropTargetAt(drag.leaf, globalPos);
    if (overlay) {
        QRect r;
        if (drag.target.isValid()) {
            QWidget* top = overlay->parentWidget();
            r = QRect(widget->mapTo(top, drag.target.rect.topLeft()), drag.target.rect.size());
        }
        overlay->setTarget(r);
        overlay->moveGhost(overlay->mapFromGlobal(globalPos));
    }
}

void Workspace::Impl::endDrag(const QPoint& globalPos, bool commit)
{
    if (!drag.active)
        return;
    drag.active = false;
    QApplication::restoreOverrideCursor();
    if (overlay) {
        overlay->setTarget({});
        overlay->setGhost({}, {});
        overlay->hide();
    }
    QPointer<WorkspaceLeaf> leaf = drag.leaf;
    drag.leaf = nullptr;
    drag.target = {};
    if (!commit || !leaf || destroying)
        return;
    const DropTarget t = dropTargetAt(leaf, globalPos);
    applyDrop(leaf, t);
}

} // namespace oc
