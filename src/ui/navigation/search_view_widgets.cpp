#include "ui/navigation/search_view_widgets.h"

#include "core/util/owned_menu.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <cmath>

namespace oc {

namespace {
QColor tok(const ThemePtr& t, const char* name, const QColor& fallback = {})
{
    return t->color(QString::fromLatin1(name), fallback);
}

// Word-wrapped lines of `text` at `width` with a fixed line height; returns the height.
qreal layoutLines(QTextLayout& layout, const QString& text, const QFont& font, qreal width, qreal lineHeight)
{
    layout.setText(text);
    layout.setFont(font);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
    layout.beginLayout();
    qreal y = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(std::max<qreal>(10, width));
        line.setPosition(QPointF(0, y + (lineHeight - (line.ascent() + line.descent())) / 2));
        y += lineHeight;
    }
    layout.endLayout();
    return std::max(y, lineHeight);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// ActiveIconButton
// ---------------------------------------------------------------------------------------------

ActiveIconButton::ActiveIconButton(const QString& icon, const QString& tooltip, QWidget* parent)
    : IconButton(icon, tooltip, parent)
{
    setCheckable(true);
    setShowBackgroundWhenChecked(false);
    auto sync = [this] {
        // .clickable-icon.is-active:hover keeps the active background
        setHoverColor(isChecked() && m_activeBg ? currentTheme()->color(QStringLiteral("--background-modifier-active-hover")) : QColor());
    };
    connect(this, &QAbstractButton::toggled, this, sync);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, sync);
}

void ActiveIconButton::paintEvent(QPaintEvent* e)
{
    if (isChecked() && m_activeBg) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), 4, 4);
        p.fillPath(path, currentTheme()->color(QStringLiteral("--background-modifier-active-hover")));
    }
    IconButton::paintEvent(e);
}

// ---------------------------------------------------------------------------------------------
// GlobalSearchInput
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int kClearWidth = 28;  // .search-input-clear-button
constexpr int kIconInset = 4;    // --input-icon-inset
} // namespace

GlobalSearchInput::GlobalSearchInput(QWidget* parent)
    : SearchInput(parent)
    , m_matchCase(new ActiveIconButton(QStringLiteral("case-sensitive"), tr("Match case"), this))
{
    setObjectName(QStringLiteral("ocGlobalSearchInput"));
    // .input-right-decorator.clickable-icon: padding 2px 4px; is-active keeps a transparent background
    m_matchCase->setIconSize(16);
    m_matchCase->setButtonSize(QSize(24, 20));
    m_matchCase->setActiveBackground(false);
    m_matchCase->resize(24, 20);
    // SearchInput resets its margins on text changes first (connected earlier); the decorator's space follows.
    connect(this, &QLineEdit::textChanged, this, &GlobalSearchInput::adjust);
    adjust();
}

void GlobalSearchInput::resizeEvent(QResizeEvent* e)
{
    SearchInput::resizeEvent(e);
    adjust();
}

void GlobalSearchInput::adjust()
{
    const bool hasText = !text().isEmpty();
    // .search-input-container input:not(:placeholder-shown) ~ .input-right-decorator: inset + 28px;
    // .global-search-input-container input:not(:placeholder-shown): padding-inline-end 56px
    const int right = kIconInset + (hasText ? kClearWidth : 0);
    m_matchCase->move(width() - right - m_matchCase->width(), (height() - m_matchCase->height()) / 2);
    QMargins m = textMargins();
    m.setRight(m.right() + (hasText ? kClearWidth : m_matchCase->width() - kIconInset));
    setTextMargins(m);
}

// ---------------------------------------------------------------------------------------------
// ExplainPanel
// ---------------------------------------------------------------------------------------------

namespace {
// .search-info-container: padding 4px 16px 8px, --font-ui-smaller, muted; .search-info-children: 20px + 1px border
constexpr int kExplainPadTop = 4;
constexpr int kExplainPadX = 16;
constexpr int kExplainPadBottom = 8;
constexpr int kExplainIndent = 21;
constexpr qreal kExplainLine = 18; // 12px at the body line-height 1.5
} // namespace

ExplainPanel::ExplainPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocSearchExplain"));
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Fixed);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void ExplainPanel::setText(const QString& text)
{
    if (m_text == text)
        return;
    m_text = text;
    m_lines.clear();
    for (const QString& raw : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        int spaces = 0;
        while (spaces < raw.size() && raw[spaces] == u' ')
            ++spaces;
        m_lines.append({spaces / 4, raw.mid(spaces)});
    }
    updateGeometry();
    update();
}

int ExplainPanel::heightForWidth(int width) const
{
    if (m_lines.isEmpty())
        return 0;
    const QFont f = currentTheme()->interfaceFont(12);
    qreal h = kExplainPadTop + kExplainPadBottom;
    for (const Line& l : m_lines) {
        QTextLayout tl;
        h += layoutLines(tl, l.text, f, width - 2 * kExplainPadX - l.level * kExplainIndent, kExplainLine);
    }
    return int(std::ceil(h));
}

void ExplainPanel::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    const QFont f = t->interfaceFont(12);
    const QColor border = tok(t, "--background-modifier-border");
    p.setPen(tok(t, "--text-muted"));
    qreal y = kExplainPadTop;
    for (const Line& l : m_lines) {
        QTextLayout tl;
        const qreal h = layoutLines(tl, l.text, f, width() - 2 * kExplainPadX - l.level * kExplainIndent, kExplainLine);
        for (int k = 0; k < l.level; ++k)
            p.fillRect(QRectF(kExplainPadX + k * kExplainIndent, y, 1, h), border);
        tl.draw(&p, QPointF(kExplainPadX + l.level * kExplainIndent, y));
        y += h;
    }
}

// ---------------------------------------------------------------------------------------------
// SortDropdown
// ---------------------------------------------------------------------------------------------

namespace {
// .search-results-info .dropdown: 24px, --font-ui-smaller; --dropdown-padding 0 2.4em 0 0.8em; icon 1em at 0.65em
constexpr qreal kDropFont = 12;
constexpr qreal kDropPadStart = 0.8 * kDropFont;
constexpr qreal kDropPadEnd = 2.4 * kDropFont;
constexpr qreal kDropIcon = 1.0 * kDropFont;
constexpr qreal kDropIconInset = 0.65 * kDropFont;
} // namespace

SortDropdown::SortDropdown(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ocSearchSort"));
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover);
    setFixedHeight(24);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void SortDropdown::setOptions(const QStringList& labels)
{
    m_labels = labels;
    updateGeometry();
    update();
}

void SortDropdown::setCurrentIndex(int index)
{
    if (index == m_index)
        return;
    m_index = index;
    setToolTip(currentText());
    updateGeometry();
    update();
}

QSize SortDropdown::sizeHint() const
{
    const QFontMetricsF fm(currentTheme()->interfaceFont(kDropFont));
    return {int(std::ceil(kDropPadStart + fm.horizontalAdvance(currentText()) + kDropPadEnd)), 24};
}

void SortDropdown::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFont f = t->interfaceFont(kDropFont);
    p.setFont(f);
    p.setPen(m_hover ? tok(t, "--interactive-accent") : tok(t, "--text-muted"));
    const QRectF textRect(kDropPadStart, 0, width() - kDropPadStart - kDropPadEnd, height());
    const QString text = QFontMetricsF(f).elidedText(currentText(), Qt::ElideRight, textRect.width());
    p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, text);
    // the up/down chevrons: white at .6 (dark) / black at .9 (light)
    QColor c = t->isDark() ? QColor(255, 255, 255) : QColor(0, 0, 0);
    c.setAlphaF(t->isDark() ? 0.6f : 0.9f);
    const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("chevrons-up-down"), int(kDropIcon), c, 2.0, devicePixelRatioF());
    p.drawPixmap(QPointF(width() - kDropIconInset - kDropIcon, (height() - kDropIcon) / 2), pm);
}

void SortDropdown::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    QMenu& menu = *ownedMenu(this);
    auto* group = new QActionGroup(&menu);
    for (int i = 0; i < m_labels.size(); ++i) {
        QAction* a = menu.addAction(m_labels[i]);
        a->setCheckable(true);
        a->setChecked(i == m_index);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] {
            setCurrentIndex(i);
            emit activated(i);
        });
    }
    menu.exec(mapToGlobal(QPoint(0, height())));
}

void SortDropdown::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void SortDropdown::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

} // namespace oc
