// Widgets rendering the workspace model (splits, tab groups, tab headers, view headers) and the tab
// drag & drop engine. Sizes and colors follow Obsidian 1.13's app.css (values only):
//   .workspace-tab-header-container   40px, padding 0 8px, border-bottom 1px --tab-outline-color
//   .workspace-tab-header-container-inner  margin 6px -5px -1px, padding 1px 15px 0
//   .mod-root .workspace-tab-header   width --tab-width (200) shrinking, padding 1px 3px 3.5px,
//                                     active: radius 6 6 0 0 + 6px inverted curves, outline 1px
//   .workspace-tab-header-inner       radius 4, padding 0 3px 0 6px (root), gap 2px
//   sidedock tabs                     icon only (18px), 25px tall, padding 0 8px, gap 3px
//   .view-header                      40px, padding 0 12px, gap 8px, title 13px centered
//   .mod-stacked (stacked tabs)       header row: container-inner padding 0 0 0 12px (only "+" and the tab
//                                     list); spines --tab-stacked-header-width wide, --background-primary,
//                                     box-shadow -1px 0 0 --tab-outline-color + --tab-stacked-shadow; spine
//                                     inner padding 8px 8px 16px, gap 2px: close (16px icon, 2px padding),
//                                     icon (14px, 4px padding, radius 4, hover bg), title (vertical-lr,
//                                     --tab-stacked-font-size / -weight, padding 4px 0, --text-normal)

#include "core/util/busy.h"
#include "core/util/owned_menu.h"
#include "core/vault/vault.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/shell/main_window.h"
#include "ui/shell/popout_window.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace_p.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QToolTip>
#include <QWheelEvent>
#include <QWindow>

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
constexpr int kMinPopoutSizeForDrop = 600; // Obsidian's WorkspaceWindow: at least 600x600
constexpr double kStackedMaxPaneFloor = 300; // updateSlidingTabs(): max pane width = max(300, visible - spines)
constexpr double kWheelStep = 60;            // px per wheel notch when scrolling a stacked strip
constexpr int kStackedInsetTop = 8, kStackedInsetSide = 8, kStackedInsetBottom = 16; // spine inner padding
constexpr int kStackedGap = 2;                // --size-2-1

QColor tok(const ThemePtr& t, const char* name) { return t->color(QLatin1String(name)); }

// --tab-stacked-shadow ("-8px 0 8px 0 rgba(0, 0, 0, 0.05)"): the x offset, blur and color that matter for a
// spine's shadow on the pane before it.
struct SpineShadow {
    double dx = -8, blur = 8, spread = 0;
    QColor color = QColor(0, 0, 0, 13);
    // How far it reaches left of the spine (the widget painting it is that wide).
    int extent() const { return std::max(1, int(std::ceil(std::max(0.0, spread - dx) + blur))); }
};

SpineShadow spineShadow(const ThemePtr& t)
{
    SpineShadow s;
    const CssValue v = t->value(QStringLiteral("--tab-stacked-shadow"));
    if (v.kind != CssValue::List || v.commaList || v.items.size() < 3)
        return s;
    QList<double> lengths;
    for (const CssValue& item : v.items) {
        if (item.kind == CssValue::Color)
            s.color = item.color;
        else if (item.kind == CssValue::Length || item.kind == CssValue::Number)
            lengths.push_back(item.kind == CssValue::Length ? item.len.resolve(t->textSize()) : item.num);
    }
    if (lengths.size() >= 3) {
        s.dx = lengths[0];
        s.blur = std::max(0.0, lengths[2]);
        s.spread = lengths.size() >= 4 ? lengths[3] : 0;
    }
    return s;
}

// --tab-stacked-text-transform: "rotate(0deg)" (Obsidian's RTL layout uses 180deg).
double stackedTitleRotation(const ThemePtr& t)
{
    static const QRegularExpression re(QStringLiteral(R"(rotate\(\s*(-?[\d.]+)\s*deg\s*\))"));
    const QRegularExpressionMatch m = re.match(t->raw(QStringLiteral("--tab-stacked-text-transform")));
    return m.hasMatch() ? m.captured(1).toDouble() : 0.0;
}

// The standard normal distribution function (a blurred box-shadow edge is a Gaussian step).
double normalCdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// The shadow a spine casts on the pane before it (drawn over that pane, left of the spine; transparent for
// the mouse). Obsidian: box-shadow: -1px 0 0 0 var(--tab-outline-color), var(--tab-stacked-shadow).
class StackShadow : public QWidget {
public:
    explicit StackShadow(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const ThemePtr t = currentTheme();
        const SpineShadow s = spineShadow(t);
        QPainter p(this);
        const double w = width();
        // The shadow box is the spine shifted by dx and grown by spread; its blurred left edge (sigma =
        // blur / 2) is what shows outside the spine.
        const double edge = w + s.dx - s.spread;
        if (s.color.alpha() > 0 && edge < w) {
            QLinearGradient g(0, 0, w, 0);
            const int steps = 12;
            for (int i = 0; i <= steps; ++i) {
                const double x = w * i / steps;
                const double k = s.blur > 0 ? normalCdf((x - edge) / (s.blur / 2)) : (x >= edge ? 1.0 : 0.0);
                QColor c = s.color;
                c.setAlphaF(s.color.alphaF() * k);
                g.setColorAt(double(i) / steps, c);
            }
            p.fillRect(rect(), g);
        }
        p.fillRect(QRectF(w - 1, 0, 1, height()), tok(t, "--tab-outline-color"));
    }
};

QPixmap icon(const QString& name, int size, const QColor& color, double stroke, qreal dpr)
{
    return IconProvider::instance().pixmap(name, size, color, stroke, dpr);
}

QIcon menuIcon(const QString& name) { return IconProvider::instance().icon(name, 16, 2.0); }

// Views that still fill a QMenu directly (View::populatePaneMenu) add their items after the sections.
void appendLegacyPaneMenu(QMenu* menu, View* view)
{
    const qsizetype before = menu->actions().size();
    if (before > 0)
        menu->addSeparator();
    view->populatePaneMenu(menu);
    if (before > 0 && menu->actions().size() == before + 1)
        menu->removeAction(menu->actions().last());
}

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

void TabHeaderBar::setStacked(bool stacked)
{
    if (m_stacked == stacked)
        return;
    m_stacked = stacked;
    m_hoverTab = m_pressTab = -1;
    m_hoverClose = false;
    m_lockedWidth = -1;
    m_snappedTab = -2;
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
    if (m_stacked) {
        // The tabs are the group's spines: .mod-stacked .workspace-tab-header-container-inner is empty
        // (padding-left 12px, no margins), "+" follows it (margin-left -4px).
        m_tabs.clear();
        m_scroll = 0;
        m_clipL = m_clipR = 0;
        m_newTab->move(int(padL) + 12 - 4, 8);
        m_tabList->move(int(std::lround(width() - padR - 4 - 26)), 8);
        refreshDragRegion();
        return;
    }
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
    const bool focused = d->windowFocused(this);
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
    const bool focused = d->windowFocused(this);
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
    // Obsidian's onOpenTabHeaderMenu: the view's onTabMenu (close items, pin) then its
    // onPaneMenu(menu, "tab-header"), in the pane menu's sections.
    WorkspaceImpl* dd = d;
    QList<MenuEntry> items;
    items << MenuEntry{QStringLiteral("close"), tr("Close"), QStringLiteral("x"), [leaf] {
                           if (leaf)
                               leaf->detach();
                       }};
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
            items << MenuEntry{QStringLiteral("close"), tr("Close others"), QStringLiteral("x"), [dd, leaf] { dd->closeOthers(leaf); }};
        if (after)
            items << MenuEntry{QStringLiteral("close"), tr("Close tabs after"), QStringLiteral("x"), [dd, leaf] { dd->closeAfter(leaf); }};
        if (unpinned > 0 && !(unpinned == 1 && !leaf->isPinned()))
            items << MenuEntry{QStringLiteral("close"), tr("Close all"), QStringLiteral("x"), [dd, group = m_group] { dd->closeAll(group); }};
        if (leaf->view() && leaf->view()->viewType() != u"empty")
            items << MenuEntry{QStringLiteral("pane"), leaf->isPinned() ? tr("Unpin") : tr("Pin"), QStringLiteral("pin"), [leaf] {
                                   if (leaf)
                                       leaf->setPinned(!leaf->isPinned());
                               }};
    }
    // Obsidian's ItemView.onTabMenu(): every tab (sidebar ones too) can move to a new window.
    items << MenuEntry{QStringLiteral("open"), tr("Move to new window"), QStringLiteral("picture-in-picture"), [dd, leaf] {
                           if (leaf)
                               dd->q->moveLeafToPopout(leaf);
                       }};
    if (View* v = leaf->view())
        v->paneMenuEntries(items, QStringLiteral("tab-header"));
    QMenu& menu = *ownedMenu(this); // Close / Delete file may destroy this tab bar
    buildSectionedMenu(&menu, items, paneMenuLayout());
    if (View* v = leaf->view())
        appendLegacyPaneMenu(&menu, v);
    menu.exec(globalPos);
}

void TabHeaderBar::showTabList()
{
    if (!m_group)
        return;
    WorkspaceImpl* dd = d;
    QMenu& menu = *ownedMenu(this); // "Close all" may close this tab group, or the pop-out window it is in
    // Obsidian's sections: action ("Stack tabs" / "Unstack tabs", not in the sidebars), close, tab list.
    if (!m_sidedock) {
        const bool stacked = m_group->stacked;
        menu.addAction(menuIcon(QStringLiteral("layers")), stacked ? tr("Unstack tabs") : tr("Stack tabs"), &menu,
                       [dd, group = QPointer<TabGroupWidget>(qobject_cast<TabGroupWidget*>(parentWidget())), stacked] {
                           if (group && group->tabs())
                               dd->setStacked(group->tabs(), !stacked);
                       });
        menu.addSeparator();
    }
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
    // A stacked strip that overflows scrolls with Obsidian's styled 12px scroll bar (app.qss).
    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_hbar->setObjectName(QStringLiteral("ocStackScrollBar"));
    m_hbar->hide();
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (v != m_scroll)
            setStackScroll(v);
    });
    m_scrollAnim = new QVariantAnimation(this);
    m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_scrollAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        if (!m_stacked || !m_tabs)
            return;
        m_scroll = v.toInt();
        layoutStacked();
    });
    connect(m_scrollAnim, &QAbstractAnimation::stateChanged, this, [this](QAbstractAnimation::State state) {
        const bool running = state == QAbstractAnimation::Running;
        if (running == m_animBusy)
            return;
        m_animBusy = running;
        if (running)
            Busy::begin();
        else
            Busy::end();
    });
    hide();
}

TabGroupWidget::~TabGroupWidget()
{
    stopScrollAnimation(); // (releases oc::Busy while this widget is still whole)
}

void TabGroupWidget::sync()
{
    if (!m_tabs)
        return;
    WorkspaceSplit* top = d->topSplit(m_tabs);
    const bool sidedock = top && top->isSidedock();
    m_header->setSidedock(sidedock);
    const bool stacked = m_tabs->stacked && !sidedock;
    const bool entering = stacked && !m_stacked;
    if (stacked != m_stacked) {
        m_stacked = stacked;
        stopScrollAnimation();
        m_scroll = 0;
        m_revealed = nullptr;
        m_revealPending = false;
    }
    m_header->setStacked(stacked);
    WorkspaceLeaf* cur = m_tabs->current();
    for (WorkspaceLeaf* l : std::as_const(m_tabs->leaves)) {
        LeafWidget* lw = d->leafWidget(l);
        if (!lw)
            continue;
        if (lw->parentWidget() != this)
            lw->setParent(this);
        lw->refresh();
        if (!stacked)
            lw->setVisible(l == cur);
    }
    syncSpines();
    restack();
    layoutChildren(); // (stacked: also which panes show)
    // Obsidian's selectTabIndex() / setStacked(): the (new) current tab scrolls into view.
    if (stacked && cur && cur != m_revealed) {
        m_revealed = cur;
        revealTab(m_tabs->currentTab, !entering);
    }
    if (!stacked)
        m_revealed = nullptr;
    m_header->relayout();
    updateTabs();
}

void TabGroupWidget::updateTabs()
{
    m_header->update();
    for (StackedTabHeader* s : std::as_const(m_spines))
        s->update();
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
    stopScrollAnimation();
    m_tabs = nullptr;
    m_header->detachModel();
    for (StackedTabHeader* s : std::as_const(m_spines))
        s->detach();
}

void TabGroupWidget::syncSpines()
{
    const QList<WorkspaceLeaf*> leaves = m_stacked && m_tabs ? m_tabs->leaves : QList<WorkspaceLeaf*>();
    QList<StackedTabHeader*> old = std::exchange(m_spines, {});
    for (WorkspaceLeaf* l : leaves) {
        auto it = std::find_if(old.begin(), old.end(), [l](const StackedTabHeader* s) { return s->leaf() == l; });
        StackedTabHeader* s = nullptr;
        if (it != old.end()) {
            s = *it;
            old.erase(it);
        } else {
            s = new StackedTabHeader(d, this, l);
            s->show();
        }
        m_spines.push_back(s);
    }
    // Deleted later: a spine may be handling the event that took its leaf away (its close button, a drop).
    for (StackedTabHeader* s : std::as_const(old)) {
        s->detach();
        s->deleteLater();
    }
    while (m_shadows.size() > m_spines.size())
        delete m_shadows.takeLast();
    while (m_shadows.size() < m_spines.size()) {
        auto* shadow = new StackShadow(this);
        shadow->show();
        m_shadows.push_back(shadow);
    }
}

void TabGroupWidget::restack()
{
    if (!m_stacked || !m_tabs)
        return;
    // The strip's DOM order: later spines and panes paint over earlier ones; a spine's shadow lies over the
    // pane before it, under the spine. The scroll bar stays on top.
    QList<QWidget*> want;
    for (int i = 0; i < m_spines.size(); ++i) {
        want << m_shadows.value(i) << m_spines[i];
        if (LeafWidget* lw = d->leafWidget(m_tabs->leaves.value(i)); lw && lw->parentWidget() == this)
            want << lw;
    }
    want << m_hbar;
    const QSet<QWidget*> ours(want.cbegin(), want.cend());
    QList<QWidget*> have;
    for (QObject* o : children())
        if (o->isWidgetType() && ours.contains(static_cast<QWidget*>(o)))
            have << static_cast<QWidget*>(o);
    if (have == want)
        return;
    for (QWidget* w : std::as_const(want))
        w->raise();
}

int TabGroupWidget::stackHeaderWidth() const
{
    return std::max(1, int(std::lround(currentTheme()->px(QStringLiteral("--tab-stacked-header-width"), -1, kHeaderHeight))));
}

TabGroupWidget::StackMetrics TabGroupWidget::stackMetrics() const
{
    // Obsidian's updateSlidingTabs(): panes are --tab-stacked-pane-width, at most max(300, visible width -
    // all spines) and at least that / n (min-width wins).
    StackMetrics m;
    const ThemePtr t = currentTheme();
    const int n = m_tabs ? int(m_tabs->leaves.size()) : 0;
    m.header = stackHeaderWidth();
    m.client = width();
    const double maxPane = std::max(kStackedMaxPaneFloor, m.client - n * m.header);
    const double minPane = n > 0 ? maxPane / n : maxPane;
    m.pane = std::max(minPane, std::min(t->px(QStringLiteral("--tab-stacked-pane-width"), -1, 700), maxPane));
    m.content = n * (m.header + m.pane);
    m.maxScroll = std::max(0, int(std::ceil(m.content - m.client - 0.01)));
    m.barHeight = m.maxScroll > 0 ? std::max(1, int(std::lround(t->px(QStringLiteral("--scrollbar-height"), -1, 12)))) : 0;
    m.areaHeight = std::max(0, height() - kHeaderHeight - m.barHeight);
    return m;
}

int TabGroupWidget::stackMaxScroll() const { return m_stacked && m_tabs ? stackMetrics().maxScroll : 0; }

double TabGroupWidget::stackPaneWidth() const { return stackMetrics().pane; }

void TabGroupWidget::layoutChildren()
{
    m_header->setGeometry(0, 0, width(), kHeaderHeight);
    if (!m_tabs)
        return;
    if (m_stacked) {
        layoutStacked();
        return;
    }
    m_hbar->hide();
    for (WorkspaceLeaf* l : std::as_const(m_tabs->leaves))
        if (LeafWidget* lw = d->leafWidget(l); lw && lw->parentWidget() == this)
            lw->setGeometry(0, kHeaderHeight, width(), std::max(0, height() - kHeaderHeight));
}

void TabGroupWidget::layoutStacked()
{
    const StackMetrics m = stackMetrics();
    const int n = int(m_tabs->leaves.size());
    m_scroll = std::clamp(m_scroll, 0, m.maxScroll);
    if (m.maxScroll > 0) {
        const QSignalBlocker block(m_hbar);
        m_hbar->setRange(0, m.maxScroll);
        m_hbar->setPageStep(std::max(1, int(m.client)));
        m_hbar->setSingleStep(int(kWheelStep / 3));
        m_hbar->setValue(m_scroll);
        m_hbar->setGeometry(0, height() - m.barHeight, width(), m.barHeight);
        m_hbar->show();
    } else {
        m_hbar->hide();
    }
    const int top = kHeaderHeight;
    const int shadowExtent = spineShadow(currentTheme()).extent();
    const int cur = m_tabs->currentTab;
    const double H = m.header, L = m.pane, scroll = m_scroll, client = m.client;
    auto px = [](double v) { return int(std::lround(v)); };
    double x = 0; // natural position in the strip
    for (int i = 0; i < n; ++i) {
        // A spine sticks at the left after the spines before it and at the right before the spines after
        // it (left wins).
        const double sx = std::max(std::min(x - scroll, client - (n - i) * H), i * H);
        if (StackedTabHeader* s = m_spines.value(i))
            s->setGeometry(px(sx), top, px(sx + H) - px(sx), m.areaHeight);
        if (QWidget* shadow = m_shadows.value(i)) {
            shadow->setGeometry(px(sx) - shadowExtent, top, shadowExtent, m.areaHeight);
            shadow->setVisible(px(sx) > 0);
        }
        x += H;
        // A pane sticks at the left after its own spine. Obsidian's onContainerScroll() hides the panes
        // out of view or entirely covered by the ones after them, but never the current one.
        const double lx = std::max(x - scroll, (i + 1) * H);
        if (LeafWidget* lw = d->leafWidget(m_tabs->leaves[i]); lw && lw->parentWidget() == this) {
            lw->setGeometry(px(lx), top, px(lx + L) - px(lx), m.areaHeight);
            const bool hidden = i != cur && (x + L <= scroll || x >= scroll + client);
            lw->setVisible(!hidden);
        }
        x += L;
    }
}

void TabGroupWidget::setStackScroll(int x)
{
    stopScrollAnimation();
    if (!m_stacked || !m_tabs)
        return;
    const int v = std::clamp(x, 0, stackMaxScroll());
    if (v == m_scroll)
        return;
    m_scroll = v;
    layoutStacked();
}

void TabGroupWidget::revealTab(int index, bool animate)
{
    if (!m_stacked || !m_tabs || index < 0 || index >= m_tabs->leaves.size())
        return;
    if (!isVisible() || width() <= 0) {
        m_revealPending = true; // once the group is shown with its size
        return;
    }
    m_revealPending = false;
    const StackMetrics m = stackMetrics();
    const int n = int(m_tabs->leaves.size());
    // Obsidian's scrollIntoView(): scrolls between these show the whole pane and the spines after it.
    const double least = n * m.header + (index + 1) * m.pane - m.client;
    const double most = index * m.pane;
    double target = m_scroll;
    if (m_scroll < least)
        target = least;
    else if (m_scroll > most)
        target = most;
    const int to = std::clamp(int(std::lround(target)), 0, m.maxScroll);
    if (animate)
        animateScrollTo(to);
    else
        setStackScroll(to);
}

void TabGroupWidget::animateScrollTo(int x)
{
    // The browser's scrollTo({behavior: "smooth"}). Headless (offscreen) runs jump to the end, so tests and
    // screenshots see the final state unless they enable combo animations.
    const bool smooth = QApplication::isEffectEnabled(Qt::UI_AnimateCombo) || QGuiApplication::platformName() != u"offscreen";
    if (!smooth || !isVisible()) {
        setStackScroll(x);
        return;
    }
    x = std::clamp(x, 0, stackMaxScroll());
    if (m_scrollAnim->state() == QAbstractAnimation::Running && m_scrollAnim->endValue().toInt() == x)
        return;
    stopScrollAnimation();
    const int distance = std::abs(x - m_scroll);
    if (distance == 0)
        return;
    m_scrollAnim->setDuration(std::clamp(int(std::sqrt(double(distance)) * 12), 150, 450));
    m_scrollAnim->setStartValue(m_scroll);
    m_scrollAnim->setEndValue(x);
    m_scrollAnim->start();
}

void TabGroupWidget::stopScrollAnimation()
{
    if (m_scrollAnim && m_scrollAnim->state() != QAbstractAnimation::Stopped)
        m_scrollAnim->stop();
}

TabHeaderBar::InsertLocation TabGroupWidget::insertLocation(int x) const
{
    if (!m_stacked)
        return m_header->insertLocation(x); // (the header row sits at the group's origin)
    // Obsidian's getTabInsertLocation() over the spines (tabHeaderEls, where they stick): a 10px band on the
    // edge nearest to x of the first spine not left of x.
    TabHeaderBar::InsertLocation loc;
    const int n = int(m_spines.size());
    if (n == 0) {
        loc.rect = QRect(13, kHeaderHeight, 10, std::max(0, height() - kHeaderHeight));
        return loc;
    }
    for (int i = 0; i < n; ++i) {
        const QRectF r = m_spines[i]->geometry();
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

void TabGroupWidget::resizeEvent(QResizeEvent*)
{
    layoutChildren();
    if (m_revealPending && m_tabs)
        revealTab(m_tabs->currentTab, false);
}

void TabGroupWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_revealPending && m_tabs) {
        layoutChildren();
        revealTab(m_tabs->currentTab, false);
    }
}

void TabGroupWidget::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    WorkspaceSplit* top = d->topSplit(m_tabs);
    p.fillRect(rect(), top && top->isSidedock() ? tok(t, "--background-secondary") : tok(t, "--background-primary"));
}

void TabGroupWidget::wheelEvent(QWheelEvent* e)
{
    // A stacked strip scrolls with horizontal wheels (touchpads), and with Shift + wheel like the browser;
    // vertical wheels belong to the panes.
    if (!m_stacked || stackMaxScroll() <= 0) {
        e->ignore();
        return;
    }
    const QPoint pixels = e->pixelDelta(), angle = e->angleDelta();
    double dx = !pixels.isNull() ? pixels.x() : angle.x() / 120.0 * kWheelStep;
    const double dy = !pixels.isNull() ? pixels.y() : angle.y() / 120.0 * kWheelStep;
    if ((e->modifiers() & Qt::ShiftModifier) && std::abs(dy) > std::abs(dx)) {
        dx = dy;
    } else if (std::abs(dx) <= std::abs(dy)) {
        e->ignore();
        return;
    }
    setStackScroll(m_scroll - int(std::lround(dx)));
    e->accept();
}

// ---------------------------------------------------------------------------------------------
// StackedTabHeader
// ---------------------------------------------------------------------------------------------

StackedTabHeader::StackedTabHeader(WorkspaceImpl* d, TabGroupWidget* group, WorkspaceLeaf* leaf)
    : QWidget(group)
    , d(d)
    , m_group(group)
    , m_leaf(leaf)
{
    setObjectName(QStringLiteral("ocStackedTab"));
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void StackedTabHeader::detach()
{
    m_leaf = nullptr;
    m_group = nullptr;
    m_pressed = m_dragging = false;
    hide();
}

int StackedTabHeader::index() const
{
    return m_group && m_group->tabs() && m_leaf ? int(m_group->tabs()->leaves.indexOf(m_leaf.data())) : -1;
}

QRect StackedTabHeader::closeRect() const
{
    // First in the spine's column: .workspace-tab-header-inner-close-button (or the pin status icon), a 16px
    // icon with 2px padding, centered in the 24px content box.
    return QRect(width() / 2 - 10, kStackedInsetTop, 20, 20);
}

QRect StackedTabHeader::iconRect() const
{
    // .workspace-tab-header-inner-icon (order 2): a 14px icon with 4px padding.
    return QRect(width() / 2 - 11, kStackedInsetTop + 20 + kStackedGap, 22, 22);
}

QRect StackedTabHeader::titleRect() const
{
    // .workspace-tab-header-inner-title (order 3): the rest of the column, down to the 16px bottom padding.
    const int top = iconRect().bottom() + 1 + kStackedGap;
    return QRect(kStackedInsetSide, top, std::max(0, width() - 2 * kStackedInsetSide), std::max(0, height() - kStackedInsetBottom - top));
}

void StackedTabHeader::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), tok(t, "--background-primary"));
    const WorkspaceLeaf* leaf = m_leaf;
    if (!leaf)
        return;
    const qreal dpr = devicePixelRatioF();
    const bool current = m_group && m_group->tabs() && m_group->tabs()->current() == leaf;
    auto hoverBox = [&](const QRectF& r, const char* color) {
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        p.fillPath(path, tok(t, color));
    };
    const QRectF close = closeRect();
    if (leaf->isPinned()) {
        // The pin status icon replaces the close button (hover background only on the current tab).
        if (m_hoverClose && current)
            hoverBox(close, "--background-modifier-active-hover");
        p.drawPixmap(close.topLeft() + QPointF(2, 2), icon(QStringLiteral("pin"), 16, tok(t, "--text-accent"), 2.0, dpr));
    } else {
        if (m_hoverClose)
            hoverBox(close, "--background-modifier-hover");
        p.drawPixmap(close.topLeft() + QPointF(2, 2), icon(QStringLiteral("x"), 16, tok(t, "--tab-text-color-focused"), 2.0, dpr));
    }
    // Stacked spines show every view's icon (notes too); hovering the spine brightens it.
    const QRectF ir = iconRect();
    if (m_hoverIcon)
        hoverBox(ir, "--background-modifier-hover");
    p.setOpacity(m_hover ? t->number(QStringLiteral("--icon-opacity-hover"), 1.0) : t->number(QStringLiteral("--icon-opacity"), 0.85));
    const QColor ic = m_hover ? tok(t, "--icon-color-hover") : tok(t, "--icon-color");
    p.drawPixmap(ir.topLeft() + QPointF(4, 4), icon(leaf->icon(), 14, ic, t->px(QStringLiteral("--icon-xs-stroke-width"), -1, 2.0), dpr));
    p.setOpacity(1.0);

    const QRectF title = QRectF(titleRect()).adjusted(0, 4, 0, -4); // padding 4px 0
    if (title.height() <= 0)
        return;
    QFont f = t->interfaceFont(t->px(QStringLiteral("--tab-stacked-font-size"), -1, 13));
    f.setWeight(QFont::Weight(std::clamp(int(t->number(QStringLiteral("--tab-stacked-font-weight"), 400)), 1, 1000)));
    if (qobject_cast<const PlaceholderView*>(leaf->view()))
        f.setItalic(true); // .workspace-tab-header.mod-unknown
    p.setFont(f);
    p.setPen(tok(t, "--text-normal"));
    const QFontMetricsF fm(f);
    const QString text = fm.elidedText(leaf->displayText(), Qt::ElideRight, title.height());
    const double lh = fm.height();
    p.save();
    p.setClipRect(QRectF(0, title.top(), width(), title.height())); // overflow hidden
    // --tab-stacked-text-transform turns the title box about its center (Obsidian's RTL layout: 180deg).
    if (const double angle = stackedTitleRotation(t); angle != 0) {
        p.translate(title.center());
        p.rotate(angle);
        p.translate(-title.center());
    }
    // writing-mode vertical-lr, text-orientation mixed: the line turned a quarter clockwise, from the top
    // (text-align start), centered across the spine.
    p.translate(title.center().x(), title.top());
    p.rotate(90);
    p.drawText(QRectF(0, -lh / 2, title.height(), lh), Qt::AlignLeft | Qt::AlignVCenter, text);
    p.restore();
}

void StackedTabHeader::updateHover(const QPoint& pos)
{
    const bool inside = rect().contains(pos);
    const bool overClose = inside && closeRect().contains(pos);
    const bool overIcon = inside && iconRect().contains(pos);
    if (inside != m_hover || overClose != m_hoverClose || overIcon != m_hoverIcon) {
        m_hover = inside;
        m_hoverClose = overClose;
        m_hoverIcon = overIcon;
        update();
    }
    // .workspace-tab-header-inner-icon { cursor: grab }
    if (overIcon)
        setCursor(Qt::OpenHandCursor);
    else
        unsetCursor();
}

void StackedTabHeader::mousePressEvent(QMouseEvent* e)
{
    updateHover(e->pos());
    m_pressed = true;
    m_pressButton = e->button();
    m_pressPos = e->pos();
    m_dragging = false;
    m_pressClose = closeRect().contains(e->pos());
    e->accept();
}

void StackedTabHeader::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton) || !m_pressed || m_pressButton != Qt::LeftButton || m_pressClose) {
        updateHover(e->pos());
        return;
    }
    const QPoint delta = e->pos() - m_pressPos;
    if (!m_dragging && delta.x() * delta.x() + delta.y() * delta.y() >= kDragThreshold * kDragThreshold) {
        WorkspaceLeaf* leaf = m_leaf;
        if (leaf && leaf->tabs()) {
            m_dragging = true;
            QPointer<StackedTabHeader> self = this;
            d->beginDrag(leaf, e->globalPosition().toPoint()); // (may become a native drag: nested loop)
            if (!self)
                return;
        }
    }
    if (m_dragging)
        d->updateDrag(e->globalPosition().toPoint());
}

void StackedTabHeader::mouseReleaseEvent(QMouseEvent* e)
{
    const bool pressed = std::exchange(m_pressed, false);
    if (m_dragging) {
        m_dragging = false;
        d->endDrag(e->globalPosition().toPoint(), true);
        return;
    }
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    if (!pressed || !leaf || !leaf->tabs() || !rect().contains(e->pos()))
        return;
    if (e->button() == Qt::MiddleButton && m_pressButton == Qt::MiddleButton) {
        leaf->detach(); // Obsidian's auxclick on a tab header
        return;
    }
    if (e->button() != Qt::LeftButton || m_pressButton != Qt::LeftButton)
        return;
    if (m_pressClose && closeRect().contains(e->pos())) {
        if (leaf->isPinned())
            leaf->setPinned(false);
        else
            leaf->detach();
        return;
    }
    QPointer<TabGroupWidget> group = m_group;
    d->q->setActiveLeaf(leaf, true); // a newly current tab scrolls into view (the group's sync)
    // The current tab's spine reveals its pane too.
    if (group && leaf && group->tabs() && leaf->tabs() == group->tabs())
        group->revealTab(int(group->tabs()->leaves.indexOf(leaf.data())), true);
}

void StackedTabHeader::leaveEvent(QEvent* e)
{
    m_hover = m_hoverClose = m_hoverIcon = false;
    unsetCursor();
    update();
    QWidget::leaveEvent(e);
}

void StackedTabHeader::contextMenuEvent(QContextMenuEvent* e)
{
    const int i = index();
    if (i >= 0 && m_group)
        m_group->header()->showTabMenu(i, e->globalPos());
}

bool StackedTabHeader::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        // --no-tooltip on stacked headers: only the close button / pin explain themselves.
        auto* he = static_cast<QHelpEvent*>(e);
        if (m_leaf && closeRect().contains(he->pos())) {
            QToolTip::showText(he->globalPos(), m_leaf->isPinned() ? tr("Unpin") : tr("Close"), this, closeRect());
        } else {
            QToolTip::hideText();
            e->ignore();
        }
        return true;
    }
    return QWidget::event(e);
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
    m_title->setActive(d->active == m_leaf && d->windowFocused(this));
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
    const bool focusedActive = d->active == m_leaf && d->windowFocused(this);
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
    // Obsidian's onMoreOptions: the view's onPaneMenu(menu, "more-options") in the pane menu's sections
    // (desktop: no Close / Pin here, those are on the tab header).
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    QMenu& menu = *ownedMenu(this); // Delete file / Move file may close this leaf
    if (View* v = leaf ? leaf->view() : nullptr) {
        QList<MenuEntry> items;
        v->paneMenuEntries(items, QStringLiteral("more-options"));
        buildSectionedMenu(&menu, items, paneMenuLayout());
        appendLegacyPaneMenu(&menu, v);
    }
    if (!menu.isEmpty())
        execMenuBelow(menu, m_more, true);
    else
        menu.deleteLater();
}

void ViewHeader::showHistoryMenu(bool back)
{
    const auto items = d->historyItems(m_leaf, back);
    if (items.isEmpty())
        return;
    QPointer<WorkspaceLeaf> leaf = m_leaf;
    WorkspaceImpl* dd = d;
    QMenu& menu = *ownedMenu(this);
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
    const bool want = inRoot && m_view && m_view->showsViewHeader() && d->showViewHeader;
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
// Tab drags follow the mouse inside the application (no QDrag) across all of its windows: the target
// is found from the global pointer position, and a release outside every window moves the tab to a
// new pop-out there (Obsidian). On Wayland windows don't know where they are on screen, so the drag
// stays in the window it started in and turns into a native drag (QDrag) once the pointer leaves it;
// our windows accept it through NativeTabDrop, an event filter on their QWindow.

namespace {
const char* const kLeafMime = "application/x-obsidian-cpp-leaf";

bool globalPositionsRelateWindows()
{
    static const bool reliable = !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    return reliable;
}

class NativeTabDrop : public QObject {
public:
    NativeTabDrop(WorkspaceImpl* d, QObject* parent)
        : QObject(parent)
        , d(d)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        const QEvent::Type type = ev->type();
        if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::DragLeave && type != QEvent::Drop)
            return false;
        QWidget* top = d->destroying ? nullptr : d->topForWindowHandle(obj);
        if (!top)
            return false;
        if (type == QEvent::DragLeave) {
            if (d->nativeDragging)
                d->showDragFeedback({}, nullptr, nullptr);
            return false;
        }
        auto* de = static_cast<QDropEvent*>(ev);
        if (!de->mimeData() || !de->mimeData()->hasFormat(QString::fromLatin1(kLeafMime)))
            return false;
        WorkspaceLeaf* leaf = d->leafById(QString::fromUtf8(de->mimeData()->data(QString::fromLatin1(kLeafMime))));
        if (!leaf)
            return false;
        // QWindow coordinates are the top-level widget's.
        const QPoint global = top->mapToGlobal(de->position().toPoint());
        const WorkspaceImpl::DropTarget target = d->dropTargetAt(leaf, global, top);
        if (type == QEvent::Drop) {
            d->hideOverlays();
            if (d->nativeDragging) {
                // Applied once QDrag::exec() returned: nothing is deleted under the drag's event loop.
                d->nativeDropAt = {top, global};
            } else {
                d->applyDrop(leaf, target);
            }
        } else {
            d->showDragFeedback(target, top, nullptr);
        }
        // Accepted anywhere in our windows: only a release outside all of them opens a new window.
        de->setDropAction(Qt::MoveAction);
        de->accept();
        return true;
    }

private:
    WorkspaceImpl* d;
};
} // namespace

DropOverlay* Workspace::Impl::ensureOverlay() { return ensureOverlay(mainTop()); }

DropOverlay* Workspace::Impl::ensureOverlay(QWidget* top)
{
    if (!top)
        return nullptr;
    QPointer<DropOverlay>* slot = nullptr;
    if (top == mainTop())
        slot = &overlay;
    else if (WorkspaceSplit* win = windowSplit(top))
        slot = &win->dropOverlay;
    if (!slot)
        return nullptr;
    if (!*slot || (*slot)->parentWidget() != top) {
        delete slot->data();
        *slot = new DropOverlay(top);
    }
    DropOverlay* o = *slot;
    o->setGeometry(top->rect());
    o->raise();
    o->show();
    return o;
}

void Workspace::Impl::hideOverlays()
{
    auto clear = [](DropOverlay* o) {
        if (!o)
            return;
        o->setTarget({});
        o->setGhost({}, {});
        o->hide();
    };
    clear(overlay);
    for (WorkspaceSplit* win : std::as_const(windows))
        clear(win->dropOverlay);
    drag.ghostTop = nullptr;
}

void Workspace::Impl::showDragFeedback(const DropTarget& target, QWidget* top, const QPoint* ghostGlobal)
{
    QWidget* highlightTop = target.host ? target.host->window() : nullptr;
    QList<QPair<QWidget*, DropOverlay*>> all{{mainTop(), overlay}};
    for (WorkspaceSplit* win : std::as_const(windows))
        all.push_back({win->popout, win->dropOverlay});
    for (const auto& [w, o] : all) {
        if (!w || w == top || w == highlightTop || !o)
            continue;
        o->setTarget({});
        o->setGhost({}, {});
        o->hide();
    }
    if (highlightTop && highlightTop != top)
        if (DropOverlay* o = ensureOverlay(highlightTop)) // (the ghost shows in `top`)
            o->setTarget(QRect(target.host->mapTo(highlightTop, target.rect.topLeft()), target.rect.size()));
    DropOverlay* o = top ? ensureOverlay(top) : nullptr;
    if (!o)
        return;
    o->setTarget(highlightTop == top ? QRect(target.host->mapTo(top, target.rect.topLeft()), target.rect.size()) : QRect());
    if (ghostGlobal) {
        if (drag.ghostTop != top) {
            o->setGhost(drag.ghost, o->mapFromGlobal(*ghostGlobal));
            drag.ghostTop = top;
        } else {
            o->moveGhost(o->mapFromGlobal(*ghostGlobal));
        }
    }
}

QWidget* Workspace::Impl::topForWindowHandle(const QObject* window) const
{
    if (!window)
        return nullptr;
    if (QWidget* m = mainTop(); m && m->windowHandle() == window)
        return m;
    for (const WorkspaceSplit* win : windows)
        if (win->popout && win->popout->windowHandle() == window)
            return win->popout;
    return nullptr;
}

QWidget* Workspace::Impl::windowAt(const QPoint& globalPos) const
{
    QList<QWidget*> ours;
    if (QWidget* m = mainTop(); m && m->isVisible())
        ours.push_back(m);
    for (const WorkspaceSplit* win : windows)
        if (win->popout && win->popout->isVisible() && !win->popout->isMinimized())
            ours.push_back(win->popout);
    // The window system knows the stacking order (X11, Windows, macOS)...
    if (QWidget* at = QApplication::topLevelAt(globalPos); at && ours.contains(at->window()))
        return at->window();
    // ...else prefer the focused window, then the one the drag started in.
    auto contains = [&](const QWidget* w) { return w && ours.contains(w) && w->frameGeometry().contains(globalPos); };
    if (QWidget* a = QApplication::activeWindow(); contains(a))
        return a;
    if (contains(drag.source))
        return drag.source;
    for (QWidget* w : std::as_const(ours))
        if (contains(w))
            return w;
    return nullptr;
}

Workspace::Impl::DropTarget Workspace::Impl::dropTargetAt(const WorkspaceLeaf* dragged, const QPoint& globalPos, QWidget* in) const
{
    DropTarget t;
    if (!widget || !root)
        return t;
    QWidget* top = in ? in : windowAt(globalPos);
    if (!top) {
        // Outside every window: Obsidian moves the tab to a new window at the drop point.
        t.kind = DropTarget::Kind::NewWindow;
        t.globalPos = globalPos;
        return t;
    }
    if (WorkspaceSplit* win = windowSplit(top)) {
        if (!win->w || !win->w->rect().contains(win->w->mapFromGlobal(globalPos)))
            return t;
        return dropTargetInArea(dragged, win, win->w, globalPos);
    }
    if (top != mainTop())
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
            t.host = widget;
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
    return dropTargetInArea(dragged, area, widget, globalPos);
}

Workspace::Impl::DropTarget Workspace::Impl::dropTargetInArea(const WorkspaceLeaf* dragged, WorkspaceSplit* area, QWidget* host,
                                                              const QPoint& globalPos) const
{
    DropTarget t;
    t.host = host;
    const QPoint p = host->mapFromGlobal(globalPos);
    if (area->children.isEmpty()) {
        t.kind = DropTarget::Kind::Side;
        t.side = area;
        t.rect = area->w == host ? host->rect() : area->w->geometry();
        return t;
    }
    WorkspaceTabs* tabs = nullptr;
    QRect R;
    forEachTabs(area, [&](WorkspaceTabs* tb) {
        if (tabs || !tb->w->isVisible())
            return;
        const QRect r(tb->w->mapTo(host, QPoint(0, 0)), tb->w->size());
        if (r.contains(p)) {
            tabs = tb;
            R = r;
        }
    });
    if (!tabs)
        return {};
    if (tabs->leaves.size() == 1 && tabs->leaves.first() == dragged)
        return {}; // Obsidian shows nothing when hovering the dragged tab's own single-tab group
    // Obsidian's getDropDirection().
    const bool inSidedock = area->isSidedock();
    const bool stacked = tabs->w->isStacked();
    const double fx = p.x() - R.x(), fy = p.y() - R.y();
    const bool overHeader = fy < kHeaderHeight;
    // A stacked group splits left / right only from its header row: its strip takes tabs between spines.
    const bool noSides = inSidedock || (stacked && !overHeader);
    const double a = std::abs(fx) / R.width(), s = std::abs(fy) / R.height();
    const double l = std::abs(p.x() - (R.x() + R.width())) / double(R.width());
    const double c = std::abs(p.y() - (R.y() + R.height())) / double(R.height());
    DropEdge edge = DropEdge::Center;
    double h = 1;
    if (s < 0.33 && s < h) {
        h = s;
        if (overHeader && !stacked)
            h = 0; // over the tab header: only "top" (upper third of the header) or a tab insert
        if (fy <= kHeaderHeight / 3.0) {
            edge = DropEdge::Top;
        } else {
            edge = DropEdge::Center;
            if (stacked)
                h = 1;
        }
    }
    if (!noSides && a < 0.33 && a < h) {
        h = a;
        edge = DropEdge::Left;
    }
    if (!noSides && l < 0.33 && l < h) {
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
        const TabHeaderBar::InsertLocation loc = tabs->w->insertLocation(int(fx));
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
    if (target.kind == DropTarget::Kind::NewWindow) {
        // Obsidian: a pop-out at the drop point, as large as the leaf (at least 600x600).
        const LeafWidget* lw = leafWidget(leaf);
        const QSize s = lw && lw->isVisible() ? lw->size() : QSize(800, 600);
        const QSize size(std::max(s.width(), kMinPopoutSizeForDrop), std::max(s.height(), kMinPopoutSizeForDrop));
        if (!moveToWindow(leaf, QRect(target.globalPos, size), true))
            return false;
    } else if (target.kind == DropTarget::Kind::Side) {
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
    drag.source = topOf(leaf);
    drag.ghostTop = nullptr;
    QApplication::setOverrideCursor(Qt::ClosedHandCursor); // body.is-grabbing
    const qreal dpr = drag.source ? drag.source->devicePixelRatioF() : qApp->devicePixelRatio();
    drag.ghost = makeGhost(leaf, dpr);
    updateDrag(globalPos);
}

void Workspace::Impl::updateDrag(const QPoint& globalPos)
{
    if (!drag.active || !drag.leaf)
        return;
    QWidget* in = nullptr;
    if (!globalPositionsRelateWindows()) {
        QWidget* src = drag.source;
        if (src && !src->rect().contains(src->mapFromGlobal(globalPos))) {
            startNativeDrag(); // left its window: the window system carries the tab from here
            return;
        }
        in = src;
    }
    drag.target = dropTargetAt(drag.leaf, globalPos, in);
    // The highlight shows in the target's window, the ghost in the window under the pointer (outside
    // every window: the one the drag started in).
    QWidget* ghostTop = drag.target.host ? drag.target.host->window() : (in ? in : windowAt(globalPos));
    if (!ghostTop)
        ghostTop = drag.source;
    showDragFeedback(drag.target, ghostTop, &globalPos);
}

void Workspace::Impl::endDrag(const QPoint& globalPos, bool commit)
{
    if (!drag.active)
        return;
    drag.active = false;
    QApplication::restoreOverrideCursor();
    hideOverlays();
    QPointer<WorkspaceLeaf> leaf = drag.leaf;
    QWidget* in = globalPositionsRelateWindows() ? nullptr : drag.source.data();
    drag.leaf = nullptr;
    drag.target = {};
    drag.source = nullptr;
    drag.ghost = {};
    if (!commit || !leaf || destroying)
        return;
    const DropTarget t = dropTargetAt(leaf, globalPos, in);
    applyDrop(leaf, t);
}

void Workspace::Impl::installNativeDrop()
{
    if (!nativeDrop)
        nativeDrop = new NativeTabDrop(this, q);
    QList<QWidget*> tops{mainTop()};
    for (const WorkspaceSplit* win : std::as_const(windows))
        tops.push_back(win->popout);
    for (QWidget* top : std::as_const(tops)) {
        if (!top)
            continue;
        top->setAcceptDrops(true);
        if (QWindow* handle = top->windowHandle()) {
            handle->removeEventFilter(nativeDrop);
            handle->installEventFilter(nativeDrop);
        }
    }
}

void Workspace::Impl::startNativeDrag()
{
    QPointer<WorkspaceLeaf> leaf = drag.leaf;
    QWidget* source = drag.source;
    const QPixmap ghost = drag.ghost;
    // The in-app drag ends here.
    drag.active = false;
    QApplication::restoreOverrideCursor();
    hideOverlays();
    drag.leaf = nullptr;
    drag.target = {};
    drag.source = nullptr;
    drag.ghost = {};
    if (!leaf || !source || destroying || nativeDragging)
        return;
    installNativeDrop();
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kLeafMime), leaf->id().toUtf8());
    QPointer<QDrag> qd = new QDrag(q); // parented to the workspace: the source window may go meanwhile
    qd->setMimeData(mime);
    qd->setPixmap(ghost);
    qd->setHotSpot(QPoint(8, 8));
    QPointer<Workspace> alive = q;
    nativeDragging = true;
    nativeDropAt = {};
    const Qt::DropAction result = qd->exec(Qt::MoveAction, Qt::MoveAction);
    if (!alive)
        return;
    nativeDragging = false;
    hideOverlays();
    for (const QPointer<PopoutWindow>& p : std::exchange(pendingDisposal, {}))
        if (p)
            p->deleteLater();
    delete qd.data();
    const auto [dropTop, dropGlobal] = std::exchange(nativeDropAt, {});
    if (dropTop && leaf && leaf->m_tabs && !destroying) {
        applyDrop(leaf, dropTargetAt(leaf, dropGlobal, dropTop)); // dropped in one of our windows
        return;
    }
    if (result == Qt::IgnoreAction && leaf && leaf->m_tabs && !destroying) {
        // Released outside every window (the window system can't tell where: it places the window).
        DropTarget t;
        t.kind = DropTarget::Kind::NewWindow;
        t.globalPos = QCursor::pos();
        applyDrop(leaf, t);
    }
}

} // namespace oc
