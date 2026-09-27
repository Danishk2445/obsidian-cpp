#include "ui/views/bases/bases_widgets.h"

#include "ui/overlay/overlay_layer.h"
#include "ui/views/bases/bases_render.h"
#include "ui/views/bases/bases_style.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <cmath>

namespace oc::basesui {

namespace {
constexpr int kMenuGap = 4; // dm(..., {gap: 4})

QWidget* overlayFor(QWidget* w)
{
    if (!w)
        return nullptr;
    // the layer of the anchor's window (the main window's or a pop-out's)
    if (OverlayLayer* layer = OverlayLayer::of(w))
        return layer;
    return w->window();
}
} // namespace

// ---------------------------------------------------------------------------------------------
// TextIconButton
// ---------------------------------------------------------------------------------------------
TextIconButton::TextIconButton(const QString& icon, const QString& label, QWidget* parent)
    : QAbstractButton(parent)
    , m_icon(icon)
    , m_label(label)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void TextIconButton::setIconName(const QString& icon)
{
    m_icon = icon;
    update();
}

void TextIconButton::setLabel(const QString& label)
{
    m_label = label;
    updateGeometry();
    update();
}

void TextIconButton::setAuxIcon(const QString& icon)
{
    m_aux = icon;
    updateGeometry();
    update();
}

void TextIconButton::setBadge(const QString& badge)
{
    m_badge = badge;
    updateGeometry();
    update();
}

void TextIconButton::setLabelVisible(bool on)
{
    if (m_labelVisible == on)
        return;
    m_labelVisible = on;
    updateGeometry();
    update();
}

void TextIconButton::setActive(bool on)
{
    m_active = on;
    update();
}

void TextIconButton::setMenuOpen(bool on)
{
    m_menuOpen = on;
    update();
}

void TextIconButton::setMuted(bool on)
{
    m_muted = on;
    updateGeometry();
    update();
}

void TextIconButton::setError(bool on)
{
    m_error = on;
    update();
}

QFont TextIconButton::labelFont() const
{
    const Style& s = basesStyle();
    return m_muted ? s.uiSmaller : s.uiSmall;
}

QSize TextIconButton::sizeHint() const
{
    const Style& s = basesStyle();
    const QFontMetricsF fm(labelFont());
    int w = 2 * s.buttonPad;
    const bool showIcon = !m_icon.isEmpty() && !m_muted;
    if (showIcon)
        w += s.iconS;
    const bool label = m_labelVisible && !m_label.isEmpty();
    if (label) {
        if (showIcon)
            w += s.buttonPad;
        w += int(std::ceil(fm.horizontalAdvance(m_label))) + 2; // padding-inline-end --size-2-1
    }
    if (!m_aux.isEmpty())
        w += s.buttonPad + s.iconXs;
    if (!m_badge.isEmpty())
        w += s.buttonPad + int(std::ceil(QFontMetricsF(s.uiSmaller).horizontalAdvance(m_badge)));
    const int h = std::max(s.iconS, int(std::ceil(fm.height()))) + 2 * s.buttonPad;
    return {m_fullWidth ? std::max(w, 50) : w, h};
}

QSize TextIconButton::minimumSizeHint() const
{
    const Style& s = basesStyle();
    const QSize h = sizeHint();
    return {std::min(h.width(), s.iconS + 2 * s.buttonPad), h.height()};
}

void TextIconButton::paintEvent(QPaintEvent*)
{
    const Style& s = basesStyle();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool hover = (m_hover || m_menuOpen) && isEnabled();
    QColor fg = m_muted ? s.muted : s.muted;
    QColor iconColor = s.iconColor;
    if (hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_active ? s.activeHover : s.hover);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), s.buttonRadius, s.buttonRadius);
        fg = s.text;
        iconColor = s.iconHover;
    }
    if (m_active) {
        fg = s.iconActive;
        iconColor = s.iconActive;
    }
    if (m_error) {
        fg = s.error;
        iconColor = s.error;
    }
    if (!isEnabled()) {
        fg.setAlphaF(fg.alphaF() * 0.7);
        iconColor.setAlphaF(iconColor.alphaF() * 0.7);
    }
    if (hasFocus() && focusPolicy() != Qt::NoFocus) {
        p.setPen(QPen(s.borderFocus, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), s.buttonRadius, s.buttonRadius);
    }
    int x = s.buttonPad;
    const QRect r = rect();
    const bool showIcon = !m_icon.isEmpty() && !m_muted;
    if (showIcon) {
        drawIcon(&p, QRectF(x, 0, s.iconS, r.height()), m_icon, s.iconS, iconColor);
        x += s.iconS + s.buttonPad;
    }
    int auxWidth = m_aux.isEmpty() ? 0 : s.iconXs + s.buttonPad;
    int badgeWidth = m_badge.isEmpty() ? 0 : int(std::ceil(QFontMetricsF(s.uiSmaller).horizontalAdvance(m_badge))) + s.buttonPad;
    if (m_labelVisible && !m_label.isEmpty()) {
        const QFont f = labelFont();
        p.setFont(f);
        p.setPen(fg);
        const int avail = std::max(0, r.width() - x - s.buttonPad - auxWidth - badgeWidth - 2);
        const QString text = QFontMetricsF(f).elidedText(m_label, Qt::ElideRight, avail);
        p.drawText(QRect(x, 0, avail + 2, r.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += int(std::ceil(QFontMetricsF(f).horizontalAdvance(text))) + 2;
    }
    if (!m_badge.isEmpty()) {
        x += s.buttonPad / 2;
        QColor c = fg;
        c.setAlphaF(c.alphaF() * 0.61);
        p.setFont(s.uiSmaller);
        p.setPen(c);
        p.drawText(QRect(x, 0, badgeWidth, r.height()), Qt::AlignLeft | Qt::AlignVCenter, m_badge);
        x += badgeWidth;
    }
    if (!m_aux.isEmpty()) {
        const int ax = m_fullWidth ? r.width() - s.buttonPad - s.iconXs : x + s.buttonPad / 2;
        drawIcon(&p, QRectF(ax, 0, s.iconXs, r.height()), m_aux, s.iconXs, s.muted);
    }
}

void TextIconButton::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void TextIconButton::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

// ---------------------------------------------------------------------------------------------
// MenuPanel
// ---------------------------------------------------------------------------------------------
namespace {
QList<QPointer<MenuPanel>>& openPanels()
{
    static QList<QPointer<MenuPanel>> panels;
    return panels;
}
} // namespace

MenuPanel::MenuPanel(QWidget* anchor, int width)
    : QWidget(overlayFor(anchor))
    , m_anchor(anchor)
    , m_width(width)
{
    setObjectName(QStringLiteral("ocBasesMenu"));
    setProperty("ocCapturesKeys", true);
    setAttribute(Qt::WA_StyledBackground, false);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    m_holder = new QWidget;
    m_holder->setAutoFillBackground(false);
    m_holderLayout = new QVBoxLayout(m_holder);
    m_holderLayout->setContentsMargins(0, 0, 0, 0);
    m_holderLayout->setSpacing(0);
    m_scroll->setWidget(m_holder);
    outer->addWidget(m_scroll);
    hide();
    qApp->installEventFilter(this);
    if (anchor)
        connect(anchor, &QObject::destroyed, this, &MenuPanel::close);
}

MenuPanel::~MenuPanel()
{
    qApp->removeEventFilter(this);
    openPanels().removeAll(this);
}

MenuPanel* MenuPanel::openPanel(QWidget* window)
{
    for (const QPointer<MenuPanel>& p : openPanels())
        if (p && p->isVisible() && (!window || p->window() == window->window()))
            return p;
    return nullptr;
}

void MenuPanel::push(QWidget* page)
{
    if (QWidget* cur = currentPage())
        cur->hide();
    page->setParent(m_holder);
    m_holderLayout->addWidget(page);
    page->show();
    m_pages << page;
    if (isVisible())
        reposition();
}

void MenuPanel::back()
{
    if (m_pages.size() <= 1) {
        close();
        return;
    }
    QPointer<QWidget> top = m_pages.takeLast();
    if (top) {
        top->hide();
        top->deleteLater();
    }
    if (QWidget* cur = currentPage())
        cur->show();
    reposition();
}

QWidget* MenuPanel::currentPage() const
{
    for (qsizetype i = m_pages.size() - 1; i >= 0; --i)
        if (m_pages[i])
            return m_pages[i];
    return nullptr;
}

void MenuPanel::popup()
{
    for (const QPointer<MenuPanel>& p : openPanels())
        if (p && p != this && p->isVisible())
            p->close();
    openPanels().removeAll(nullptr);
    if (!openPanels().contains(this))
        openPanels() << this;
    reposition();
    show();
    raise();
    if (auto* layer = qobject_cast<OverlayLayer*>(parentWidget()))
        layer->refresh();
    if (auto* b = qobject_cast<TextIconButton*>(m_anchor.data()))
        b->setMenuOpen(true);
}

void MenuPanel::reposition()
{
    QWidget* host = parentWidget();
    if (!host)
        return;
    const Style& s = basesStyle();
    const QRect bounds = host->rect().adjusted(8, 8, -8, -8);
    m_holder->adjustSize();
    const int contentW = m_width > 0 ? m_width : std::max(220, m_holder->sizeHint().width() + 2);
    const int w = std::min(contentW + 2, bounds.width());
    const int contentH = m_holder->heightForWidth(w - 2) > 0 ? m_holder->heightForWidth(w - 2) : m_holder->sizeHint().height();
    QPoint anchorBottom = bounds.topLeft();
    QPoint anchorTop = bounds.topLeft();
    if (m_anchor) {
        anchorBottom = m_anchor->mapTo(host, QPoint(0, m_anchor->height()));
        anchorTop = m_anchor->mapTo(host, QPoint(0, 0));
    }
    const int below = bounds.bottom() - (anchorBottom.y() + kMenuGap);
    const int above = anchorTop.y() - kMenuGap - bounds.top();
    int h = contentH + 2;
    int y;
    if (h <= below || below >= above) {
        h = std::min(h, std::max(60, below));
        y = anchorBottom.y() + kMenuGap;
    } else {
        h = std::min(h, above);
        y = anchorTop.y() - kMenuGap - h;
    }
    const int x = std::clamp(anchorBottom.x(), bounds.left(), std::max(bounds.left(), bounds.right() - w));
    setGeometry(x, y, w, h);
    Q_UNUSED(s);
}

void MenuPanel::close()
{
    if (m_closing)
        return;
    m_closing = true;
    if (auto* b = qobject_cast<TextIconButton*>(m_anchor.data()))
        b->setMenuOpen(false);
    hide();
    if (auto* layer = qobject_cast<OverlayLayer*>(parentWidget()))
        layer->refresh();
    emit closed();
    deleteLater();
}

bool MenuPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (!isVisible() || m_closing)
        return false;
    if (ev->type() == QEvent::MouseButtonPress) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w)
            return false;
        // inside the panel, its button, a menu or popup opened from it
        for (QWidget* p = w; p; p = p->parentWidget()) {
            if (p == this || p == m_anchor.data() || qobject_cast<QMenu*>(p) || p->property("ocBasesPopup").toBool())
                return false;
        }
        if (w->window() != window())
            return false;
        close();
        return false;
    }
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        auto* w = qobject_cast<QWidget*>(obj);
        if (ke->key() == Qt::Key_Escape && w && (w == this || isAncestorOf(w))) {
            if (w->property("ocBasesEscapeHandled").toBool())
                return false;
            back();
            return true;
        }
    }
    return false;
}

void MenuPanel::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        back();
        return;
    }
    QWidget::keyPressEvent(e);
}

void MenuPanel::paintEvent(QPaintEvent*)
{
    const Style& s = basesStyle();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), s.menuRadius, s.menuRadius);
    p.fillPath(path, s.menuBg);
    p.setPen(QPen(s.menuBorder, 1));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------
// MenuRow
// ---------------------------------------------------------------------------------------------
MenuRow::MenuRow(const QString& icon, const QString& text, QWidget* parent)
    : QWidget(parent)
    , m_icon(icon)
    , m_text(text)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MenuRow::setText(const QString& text)
{
    m_text = text;
    update();
}

void MenuRow::setIconName(const QString& icon)
{
    m_icon = icon;
    update();
}

void MenuRow::setActive(bool on)
{
    m_active = on;
    update();
}

void MenuRow::setFaint(bool on)
{
    m_faint = on;
    update();
}

void MenuRow::setDestructive(bool on)
{
    m_destructive = on;
    update();
}

void MenuRow::setCheckable(bool on)
{
    m_checkable = on;
    update();
}

void MenuRow::setChecked(bool on)
{
    m_checked = on;
    update();
}

void MenuRow::setValueText(const QString& text)
{
    m_value = text;
    update();
}

void MenuRow::setChevron(bool on)
{
    m_chevron = on;
    update();
}

void MenuRow::setCheckMark(bool on)
{
    m_checkMark = on;
    update();
}

void MenuRow::setSelected(bool on)
{
    m_selected = on;
    update();
}

QSize MenuRow::sizeHint() const
{
    const Style& s = basesStyle();
    const QFontMetricsF fm(s.uiSmall);
    int w = 2 * s.menuItemPadX + s.iconS + 8 + int(std::ceil(fm.horizontalAdvance(m_text)));
    if (m_checkable)
        w += 16 + 8;
    if (!m_value.isEmpty())
        w += 16 + int(std::ceil(fm.horizontalAdvance(m_value)));
    if (m_chevron || m_checkMark)
        w += 8 + s.iconS;
    return {std::max(w, 120), std::max(28, int(std::ceil(fm.height())) + 2 * 6)};
}

QRect MenuRow::chevronRect() const
{
    const Style& s = basesStyle();
    return QRect(width() - s.menuItemPadX - s.iconS - 4, 0, s.iconS + 8, height());
}

QRect MenuRow::checkboxRect() const
{
    const int size = 14;
    return QRect(basesStyle().menuItemPadX - 2, (height() - size) / 2, size, size);
}

void MenuRow::paintEvent(QPaintEvent*)
{
    const Style& s = basesStyle();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(4, 1, -4, -1);
    if (m_hover || m_selected) {
        p.setPen(Qt::NoPen);
        p.setBrush(s.hover);
        p.drawRoundedRect(r, 4, 4);
    }
    QColor fg = m_destructive ? s.error : m_faint ? s.faint : s.text;
    QColor ic = m_destructive ? s.error : m_faint ? s.faint : (m_iconColor.isValid() ? m_iconColor : (m_active ? s.text : s.iconColor));
    int x = s.menuItemPadX + 2;
    if (m_checkable) {
        drawCheckbox(&p, QRectF(checkboxRect()), m_checked, true);
        x = checkboxRect().right() + 8;
    }
    if (!m_icon.isEmpty()) {
        drawIcon(&p, QRectF(x, 0, s.iconS, height()), m_icon, s.iconS, ic);
        x += s.iconS + 8;
    }
    QFont f = s.uiSmall;
    if (m_active)
        f.setWeight(QFont::Medium);
    p.setFont(f);
    p.setPen(fg);
    int right = width() - s.menuItemPadX;
    if (m_chevron || m_checkMark)
        right -= s.iconS + 8;
    if (!m_value.isEmpty()) {
        const int vw = int(std::ceil(QFontMetricsF(f).horizontalAdvance(m_value)));
        p.setPen(s.muted);
        p.drawText(QRect(right - vw, 0, vw, height()), Qt::AlignRight | Qt::AlignVCenter, m_value);
        right -= vw + 12;
        p.setPen(fg);
    }
    p.drawText(QRect(x, 0, std::max(0, right - x), height()), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetricsF(f).elidedText(m_text, Qt::ElideRight, std::max(0, right - x)));
    if (m_chevron)
        drawIcon(&p, QRectF(chevronRect()), QStringLiteral("chevron-right"), s.iconS, s.muted);
    if (m_checkMark)
        drawIcon(&p, QRectF(chevronRect()), QStringLiteral("check"), s.iconS, s.text);
}

void MenuRow::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        m_pressed = true;
    e->accept();
}

void MenuRow::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    if (!rect().contains(e->pos()))
        return;
    if (m_chevron && chevronRect().contains(e->pos())) {
        emit chevronClicked();
        return;
    }
    if (m_checkable && checkboxRect().adjusted(-4, -4, 4, 4).contains(e->pos())) {
        m_checked = !m_checked;
        update();
        emit checkToggled(m_checked);
        return;
    }
    emit clicked();
}

void MenuRow::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void MenuRow::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

// ---------------------------------------------------------------------------------------------
// SectionHeader
// ---------------------------------------------------------------------------------------------
SectionHeader::SectionHeader(const QString& title, bool collapsible, QWidget* parent)
    : QWidget(parent)
    , m_title(title)
    , m_collapsible(collapsible)
{
    if (collapsible)
        setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SectionHeader::setCollapsed(bool on)
{
    m_collapsed = on;
    update();
}

void SectionHeader::setFlair(const QString& flair)
{
    m_flair = flair;
    update();
}

QSize SectionHeader::sizeHint() const
{
    const Style& s = basesStyle();
    return {200, int(std::ceil(QFontMetricsF(s.uiSmall).height())) + 16};
}

void SectionHeader::paintEvent(QPaintEvent*)
{
    const Style& s = basesStyle();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int x = 12;
    if (m_collapsible) {
        p.save();
        const QPointF c(x + 6, height() / 2.0);
        p.translate(c);
        if (!m_collapsed)
            p.rotate(90);
        QPainterPath tri;
        tri.moveTo(-2.5, -4);
        tri.lineTo(3, 0);
        tri.lineTo(-2.5, 4);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(s.faint);
        p.drawPath(tri);
        p.restore();
        x += 16;
    }
    QFont f = s.uiSmall;
    f.setWeight(QFont::Medium);
    p.setFont(f);
    p.setPen(s.text);
    const int tw = int(std::ceil(QFontMetricsF(f).horizontalAdvance(m_title)));
    p.drawText(QRect(x, 0, tw + 2, height()), Qt::AlignLeft | Qt::AlignVCenter, m_title);
    x += tw + 8;
    if (!m_flair.isEmpty()) {
        const QFont ff = s.uiSmaller;
        const int fw = int(std::ceil(QFontMetricsF(ff).horizontalAdvance(m_flair))) + 10;
        const QRectF pill(x, (height() - 18) / 2.0, fw, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(s.hover);
        p.drawRoundedRect(pill, 4, 4);
        p.setFont(ff);
        p.setPen(s.muted);
        p.drawText(pill, Qt::AlignCenter, m_flair);
    }
}

void SectionHeader::mousePressEvent(QMouseEvent* e)
{
    if (!m_collapsible || e->button() != Qt::LeftButton)
        return;
    m_collapsed = !m_collapsed;
    update();
    emit toggled(m_collapsed);
}

// ---------------------------------------------------------------------------------------------
// PageHeader
// ---------------------------------------------------------------------------------------------
PageHeader::PageHeader(const QString& title, bool back, bool more, bool closeButton, QWidget* parent)
    : QWidget(parent)
{
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(4, 4, 4, 4);
    l->setSpacing(2);
    auto* b = new TextIconButton(back ? QStringLiteral("chevron-left") : QString(), title, this);
    QFont f = basesStyle().uiSmall;
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFullWidth(true);
    b->setEnabled(back);
    connect(b, &QAbstractButton::clicked, this, &PageHeader::backClicked);
    l->addWidget(b, 1);
    if (more) {
        auto* m = new TextIconButton(QStringLiteral("more-vertical"), QString(), this);
        m_more = m;
        connect(m, &QAbstractButton::clicked, this, &PageHeader::moreClicked);
        l->addWidget(m);
    }
    if (closeButton) {
        auto* c = new TextIconButton(QStringLiteral("x"), QString(), this);
        connect(c, &QAbstractButton::clicked, this, &PageHeader::closeClicked);
        l->addWidget(c);
    }
    Q_UNUSED(f);
}

} // namespace oc::basesui
