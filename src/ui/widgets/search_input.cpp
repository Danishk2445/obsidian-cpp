#include "ui/widgets/search_input.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace oc {

namespace {
constexpr int kHeight = 30;     // --input-height
constexpr int kIcon = 16;       // --search-icon-size
constexpr int kClearWidth = 28; // .search-input-clear-button
} // namespace

SearchInput::SearchInput(QWidget* parent)
    : QLineEdit(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(this, &QLineEdit::textChanged, this, [this] {
        updateMargins();
        update();
    });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] { updateStyle(); });
    updateStyle();
    updateMargins();
}

QSize SearchInput::sizeHint() const { return {QLineEdit::sizeHint().width(), kHeight}; }

void SearchInput::setClearButtonEnabled(bool on)
{
    m_clear = on;
    updateMargins();
    update();
}

void SearchInput::setDecorator(const QString& text)
{
    if (m_decorator == text)
        return;
    m_decorator = text;
    updateMargins();
    update();
}

void SearchInput::setNoMatch(bool noMatch)
{
    if (m_noMatch == noMatch)
        return;
    m_noMatch = noMatch;
    updateStyle();
}

void SearchInput::setShowIcon(bool on)
{
    m_icon = on;
    updateMargins();
    update();
}

void SearchInput::setRightReserve(int px)
{
    m_rightReserve = std::max(0, px);
    updateMargins();
    update();
}

void SearchInput::updateStyle()
{
    // .document-search-input.mod-no-match: color-mix(--background-modifier-error 20%, transparent)
    setFixedHeight(kHeight);
    if (m_noMatch) {
        QColor c = currentTheme()->color(QStringLiteral("--background-modifier-error"), QColor(0xfb, 0x46, 0x4c));
        c.setAlphaF(0.2f);
        setStyleSheet(QStringLiteral("QLineEdit { background: rgba(%1,%2,%3,%4); }").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF()));
    } else {
        setStyleSheet(QString());
    }
}

void SearchInput::updateMargins()
{
    // padding-inline-start: icon*2 - 2px; end: 28px with the clear button (text present)
    const int left = m_icon ? kIcon * 2 - 2 - 8 : 0; // QSS already pads 8px
    int right = m_rightReserve;
    if (m_clear && !text().isEmpty())
        right += kClearWidth - 8;
    if (!m_decorator.isEmpty()) {
        const QFont f = currentTheme()->interfaceFont(12);
        right += int(QFontMetricsF(f).horizontalAdvance(m_decorator)) + 8;
    }
    setTextMargins(left, 0, std::max(0, right), 0);
}

QRect SearchInput::clearRect() const { return QRect(width() - 2 - kClearWidth - m_rightReserve, 0, kClearWidth, height()); }

void SearchInput::resizeEvent(QResizeEvent* e)
{
    QLineEdit::resizeEvent(e);
    updateMargins();
}

void SearchInput::paintEvent(QPaintEvent* e)
{
    QLineEdit::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePtr t = currentTheme();
    if (m_icon) {
        const QColor c = t->color(QStringLiteral("--search-icon-color"), t->color(QStringLiteral("--text-muted")));
        const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("search"), kIcon, c, 2.0, devicePixelRatioF());
        p.drawPixmap(QPointF(8, (height() - kIcon) / 2.0), pm);
    }
    int right = width() - m_rightReserve;
    if (m_clear && !text().isEmpty()) {
        const QRect r = clearRect();
        right = r.left();
        const qreal d = 13; // --search-clear-button-size
        const QRectF c(r.center().x() - d / 2 + 0.5, r.center().y() - d / 2 + 0.5, d, d);
        QPainterPath circle;
        circle.addEllipse(c);
        QPainterPathStroker st;
        const qreal k = d / 12.0;
        st.setWidth(1.1 * k);
        QPainterPath lines;
        lines.moveTo(c.left() + 3.5 * k, c.top() + 3.5 * k);
        lines.lineTo(c.left() + 8.5 * k, c.top() + 8.5 * k);
        lines.moveTo(c.left() + 8.5 * k, c.top() + 3.5 * k);
        lines.lineTo(c.left() + 3.5 * k, c.top() + 8.5 * k);
        const bool hover = r.contains(mapFromGlobal(QCursor::pos()));
        const QColor col = hover ? t->color(QStringLiteral("--text-normal")) : t->color(QStringLiteral("--search-clear-button-color"), t->color(QStringLiteral("--text-muted")));
        p.fillPath(circle.subtracted(st.createStroke(lines)), col);
    }
    if (!m_decorator.isEmpty()) {
        // .document-search-count: --font-ui-smaller, muted, tabular numbers, 8px from the right
        QFont f = t->interfaceFont(12);
        f.setFeature(QFont::Tag("tnum"), 1);
        p.setFont(f);
        p.setPen(t->color(QStringLiteral("--text-muted")));
        const int w = int(QFontMetricsF(f).horizontalAdvance(m_decorator)) + 1;
        p.drawText(QRect(right - 8 - w, 0, w, height()), Qt::AlignVCenter | Qt::AlignRight, m_decorator);
    }
}

void SearchInput::mousePressEvent(QMouseEvent* e)
{
    if (m_clear && !text().isEmpty() && clearRect().contains(e->position().toPoint()) && e->button() == Qt::LeftButton) {
        clear();
        setFocus();
        emit cleared();
        e->accept();
        return;
    }
    QLineEdit::mousePressEvent(e);
}

void SearchInput::mouseMoveEvent(QMouseEvent* e)
{
    const bool overClear = m_clear && !text().isEmpty() && clearRect().contains(e->position().toPoint());
    setCursor(overClear ? Qt::ArrowCursor : Qt::IBeamCursor);
    update(clearRect());
    QLineEdit::mouseMoveEvent(e);
}

} // namespace oc
