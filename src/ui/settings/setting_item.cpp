#include "ui/settings/setting_item.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/widgets/color_swatch.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTextDocument>
#include <QVariantAnimation>

#include <cmath>

namespace oc {

namespace {
double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
QColor tok(const char* name, const QColor& fallback = {}) { return currentTheme()->color(QString::fromLatin1(name), fallback); }

void setTextRole(QLabel* label, const char* role)
{
    if (label->property("ocText").toString() == QLatin1String(role))
        return;
    label->setProperty("ocText", role[0] ? QVariant(QString::fromLatin1(role)) : QVariant());
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QString plainFromHtml(const QString& html)
{
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText();
}

// Description / name text as Qt rich text with Obsidian's 1.3 line height (--line-height-tight).
QString wrapLines(const QString& html) { return QStringLiteral("<div style=\"line-height:130%\">") + html + QStringLiteral("</div>"); }

QPixmap iconPixmap(const QString& name, int px, const QColor& color, double stroke, qreal dpr)
{
    return IconProvider::instance().pixmap(name, px, color, stroke, dpr);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// SettingItem
// ---------------------------------------------------------------------------------------------

SettingItem::SettingItem(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_Hover);
    m_layout = new QHBoxLayout(this);
    m_layout->setSpacing(0);

    m_iconLabel = new QLabel(this);
    m_iconLabel->hide();
    m_layout->addWidget(m_iconLabel, 0, Qt::AlignTop);

    m_info = new QWidget(this);
    auto* info = new QVBoxLayout(m_info);
    info->setContentsMargins(0, 0, 0, 0);
    info->setSpacing(0);
    m_name = new QLabel(m_info);
    m_name->setWordWrap(true);
    m_name->setTextFormat(Qt::PlainText);
    m_name->setObjectName(QStringLiteral("ocSettingName"));
    info->addWidget(m_name);
    m_desc = new QLabel(m_info);
    m_desc->setWordWrap(true);
    m_desc->setTextFormat(Qt::RichText);
    m_desc->setOpenExternalLinks(true);
    m_desc->setContentsMargins(0, 4, 0, 0); // padding-top: var(--size-4-1)
    m_desc->setObjectName(QStringLiteral("ocSettingDesc"));
    m_desc->hide();
    info->addWidget(m_desc);
    m_layout->addWidget(m_info, 1);

    m_controlColumn = new QWidget(this);
    auto* col = new QVBoxLayout(m_controlColumn);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    m_control = new QWidget(m_controlColumn);
    m_controlLayout = new QHBoxLayout(m_control);
    m_controlLayout->setContentsMargins(0, 0, 0, 0);
    m_controlLayout->setSpacing(8); // gap: var(--size-4-2)
    m_controlLayout->addStretch(1);
    col->addWidget(m_control);
    m_error = new QLabel(m_controlColumn);
    m_error->setWordWrap(true);
    m_error->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_error->hide();
    col->addWidget(m_error);
    m_layout->addWidget(m_controlColumn, 0);

    applyStyle();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] { applyStyle(); });
}

void SettingItem::setKind(Kind kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    if (kind == Kind::Navigable && !m_chevron) {
        m_value = new QLabel(m_control);
        m_value->setTextFormat(Qt::PlainText);
        m_status = new QLabel(m_control);
        m_status->hide();
        m_chevron = new QLabel(m_control);
        m_controlLayout->addWidget(m_value);
        m_controlLayout->addWidget(m_status);
        m_controlLayout->addWidget(m_chevron);
        m_value->hide();
    }
    if (kind == Kind::Navigable || kind == Kind::Action)
        setFocusPolicy(Qt::StrongFocus);
    applyStyle();
}

void SettingItem::setName(const QString& name)
{
    m_nameText = name;
    m_name->setText(name);
    m_name->setVisible(!name.isEmpty() || m_kind == Kind::Heading);
}

QString SettingItem::name() const { return m_nameText; }

void SettingItem::setNameFamily(const QString& family)
{
    m_nameFamily = family;
    applyStyle();
}

void SettingItem::setDesc(const QString& text)
{
    m_descPlain = text;
    m_descHtml = text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    m_desc->setText(wrapLines(m_descHtml));
    m_desc->setVisible(!text.isEmpty());
}

void SettingItem::setRichDesc(const QString& html)
{
    m_descHtml = html;
    m_descPlain = plainFromHtml(html);
    applyStyle(); // link and warning colours are baked into the HTML
    m_desc->setVisible(!html.isEmpty());
}

QString SettingItem::desc() const { return m_descPlain; }

void SettingItem::setDescWarning(bool warning)
{
    m_descWarning = warning;
    applyStyle();
}

void SettingItem::setIcon(const QString& icon)
{
    m_iconName = icon;
    applyStyle();
}

void SettingItem::setPadding(int x, int top, int bottom)
{
    if (m_padX == x && m_padY == top && m_padBottom == bottom)
        return;
    m_padX = x;
    m_padY = top;
    m_padBottom = bottom;
    applyStyle();
}

void SettingItem::setDisabled(bool disabled)
{
    m_disabled = disabled;
    updateControlState();
    update();
}

void SettingItem::updateControlState()
{
    for (QWidget* w : controls())
        w->setEnabled(!m_disabled);
    // .setting-item.mod-action.is-disabled { opacity: 0.5 }
    if (m_kind == Kind::Action || m_kind == Kind::Navigable) {
        if (m_disabled && !graphicsEffect()) {
            auto* fade = new QGraphicsOpacityEffect(this);
            fade->setOpacity(0.5);
            setGraphicsEffect(fade);
        } else if (!m_disabled && graphicsEffect()) {
            setGraphicsEffect(nullptr);
        }
    }
}

void SettingItem::setErrorMessage(const QString& message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

QString SettingItem::errorMessage() const { return m_error->text(); }

void SettingItem::setDisplayValue(const QString& value, bool warning)
{
    setKind(Kind::Navigable);
    m_value->setText(value);
    m_value->setVisible(!value.isEmpty());
    m_status->setVisible(warning);
    applyStyle();
}

QString SettingItem::displayValue() const { return m_value ? m_value->text() : QString(); }

void SettingItem::activate()
{
    if (m_disabled || !m_onClick)
        return;
    auto fn = m_onClick; // the handler may rebuild (and delete) this row
    fn();
}

QList<QWidget*> SettingItem::controls() const
{
    QList<QWidget*> out;
    for (int i = 0; i < m_controlLayout->count(); ++i) {
        QWidget* w = m_controlLayout->itemAt(i)->widget();
        if (w && w != m_value && w != m_status && w != m_chevron)
            out << w;
    }
    return out;
}

void SettingItem::addControl(QWidget* widget)
{
    widget->setParent(m_control);
    // Controls go before the navigable value/chevron (which are only present on navigable rows).
    int index = m_controlLayout->count();
    if (m_value)
        index = m_controlLayout->indexOf(m_value);
    m_controlLayout->insertWidget(index, widget, 0, Qt::AlignVCenter);
    widget->setEnabled(!m_disabled);
    widget->show();
    if (m_controlColumn->isHidden()) {
        m_controlColumn->show();
        m_info->setContentsMargins(0, 0, 16, 0);
    }
}

void SettingItem::clearControls()
{
    for (QWidget* w : controls()) {
        m_controlLayout->removeWidget(w);
        w->deleteLater();
        w->hide();
    }
    setErrorMessage({});
}

ToggleSwitch* SettingItem::addToggle(bool value, std::function<void(bool)> onChange)
{
    auto* t = new ToggleSwitch;
    t->setChecked(value);
    if (onChange)
        connect(t, &ToggleSwitch::toggled, this, [onChange](bool on) { onChange(on); });
    addControl(t);
    return t;
}

Dropdown* SettingItem::addDropdown(const QList<QPair<QString, QString>>& options, const QString& value,
                                   std::function<void(const QString&)> onChange)
{
    auto* d = new Dropdown;
    d->setOptions(options);
    d->setValue(value);
    if (onChange)
        connect(d, &Dropdown::valueChanged, this, [onChange](const QString& v) { onChange(v); });
    addControl(d);
    return d;
}

QLineEdit* SettingItem::addText(const QString& value, const QString& placeholder, std::function<void(const QString&)> onChange)
{
    auto* e = new QLineEdit;
    e->setText(value);
    e->setPlaceholderText(placeholder);
    e->setFont(currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)));
    e->setFixedHeight(int(tokPx("--input-height", 30)));
    e->setMinimumWidth(170); // a size=20 <input> at 13 px
    e->setAttribute(Qt::WA_MacShowFocusRect, false);
    if (onChange)
        connect(e, &QLineEdit::textEdited, this, [onChange](const QString& v) { onChange(v); });
    addControl(e);
    return e;
}

ValueSlider* SettingItem::addSlider(double min, double max, double step, double value, std::function<void(double)> onChange)
{
    auto* s = new ValueSlider;
    s->setRange(min, max);
    s->setStep(step);
    s->setValue(value);
    s->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    s->setFixedWidth(s->sizeHint().width() + 40);
    if (onChange)
        connect(s, &ValueSlider::valueChanged, this, [onChange](double v) { onChange(v); });
    addControl(s);
    return s;
}

Button* SettingItem::addButton(const QString& text, Button::Style style, std::function<void()> onClick)
{
    auto* b = new Button(text, style);
    if (onClick)
        connect(b, &QPushButton::clicked, this, [onClick] { onClick(); });
    addControl(b);
    return b;
}

IconButton* SettingItem::addExtraButton(const QString& icon, const QString& tooltip, std::function<void()> onClick)
{
    auto* b = new IconButton(icon, tooltip);
    b->setObjectName(QStringLiteral("ocExtraSettingButton"));
    if (onClick)
        connect(b, &IconButton::clicked, this, [onClick] { onClick(); });
    addControl(b);
    return b;
}

ColorSwatch* SettingItem::addColorPicker(const QColor& value, std::function<void(const QColor&)> onChange)
{
    auto* c = new ColorSwatch;
    c->setColor(value);
    if (onChange)
        connect(c, &ColorSwatch::colorChanged, this, [onChange](const QColor& v) { onChange(v); });
    addControl(c);
    return c;
}

void SettingItem::flash()
{
    auto* anim = new QVariantAnimation(this);
    anim->setDuration(1500);
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    anim->setEasingCurve(QEasingCurve::InQuad);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_flash = v.toDouble();
        update();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SettingItem::applyStyle()
{
    const ThemePtr t = currentTheme();
    const double small = tokPx("--font-ui-small", 13);
    const double smaller = tokPx("--font-ui-smaller", 12);

    // Row padding / alignment.
    const bool centered = m_kind == Kind::Navigable || m_kind == Kind::Action || m_kind == Kind::EmptyState
                          || property("ocCentered").toBool();
    if (m_kind == Kind::Heading)
        m_layout->setContentsMargins(16, 0, 16, 0);
    else
        m_layout->setContentsMargins(m_padX, m_padY, m_padX, m_padBottom);
    const Qt::Alignment va = centered || m_kind == Kind::Heading ? Qt::AlignVCenter : Qt::AlignTop;
    m_layout->setAlignment(m_info, va);
    m_layout->setAlignment(m_controlColumn, va);
    m_layout->setAlignment(m_iconLabel, va);
    // .setting-item > *:first-child { margin-inline-end: 16px }
    m_info->setContentsMargins(0, 0, m_controlColumn->isHidden() ? 0 : 16, 0);

    // Name.
    QFont nf = t->interfaceFont(m_kind == Kind::Heading ? tokPx("--setting-group-heading-size", 15) : small);
    if (m_kind == Kind::Heading)
        nf.setWeight(QFont::Weight(int(t->number(QStringLiteral("--setting-group-heading-weight"), 600))));
    if (!m_nameFamily.isEmpty())
        nf.setFamilies({m_nameFamily});
    // (the role first: re-polishing a label resets its font)
    const bool accentName = m_kind == Kind::Action && !property("ocActionPlain").toBool(); // font rows use --text-normal
    setTextRole(m_name, accentName ? "accent" : m_kind == Kind::EmptyState ? "muted" : "");
    m_name->setFont(nf);
    m_name->setVisible(!m_nameText.isEmpty() || m_kind == Kind::Heading);

    // Description (links use --link-color = --text-muted inside descriptions, underlined).
    setTextRole(m_desc, m_descWarning ? "warning" : "muted");
    m_desc->setFont(t->interfaceFont(m_kind == Kind::Heading ? small : smaller));
    if (!m_descHtml.isEmpty()) {
        QString html = m_descHtml;
        const QString link = (m_descWarning ? tok("--text-warning") : tok("--text-muted")).name();
        html.replace(QStringLiteral("<a "), QStringLiteral("<a style=\"color:%1; text-decoration: underline\" ").arg(link));
        html.replace(QStringLiteral("<span class=warning>"), QStringLiteral("<span style=\"color:%1\">").arg(tok("--text-warning").name()));
        html.replace(QStringLiteral("<code>"),
                     QStringLiteral("<code style=\"font-family:'%1'; font-size:%2px\">")
                         .arg(t->monospaceFamily())
                         .arg(int(std::lround(smaller * 0.875))));
        m_desc->setText(wrapLines(html));
    }

    // Icon (list rows).
    if (!m_iconName.isEmpty()) {
        const int px = int(tokPx("--icon-s", 16));
        m_iconLabel->setPixmap(iconPixmap(m_iconName, px, tok("--text-muted"), tokPx("--icon-s-stroke-width", 2), devicePixelRatioF()));
        m_iconLabel->setFixedWidth(px + 16);
        m_iconLabel->show();
    } else {
        m_iconLabel->hide();
    }

    // Error message.
    setTextRole(m_error, "warning");
    m_error->setFont(t->interfaceFont(smaller));

    // Navigable: value (muted 13 px), status icon, chevron (--text-faint, --icon-s).
    if (m_value) {
        setTextRole(m_value, "muted");
        m_value->setFont(t->interfaceFont(small));
        const int px = int(tokPx("--icon-s", 16));
        m_status->setPixmap(iconPixmap(QStringLiteral("alert-triangle"), px, tok("--text-warning"), 2, devicePixelRatioF()));
        m_chevron->setPixmap(iconPixmap(QStringLiteral("chevron-right"), px, tok("--text-faint"), tokPx("--icon-s-stroke-width", 2),
                                        devicePixelRatioF()));
    }
    m_controlColumn->setVisible(m_controlLayout->count() > 1 || m_kind == Kind::Navigable);
    setCursor(Qt::ArrowCursor); // --cursor: default
    update();
}

QSize SettingItem::sizeHint() const { return QWidget::sizeHint(); }

void SettingItem::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool clickable = m_kind == Kind::Action || m_kind == Kind::Navigable;
    const double radius = property("ocRoundTop").toBool() || property("ocRoundBottom").toBool() ? tokPx("--setting-items-radius", 12) : 0;
    auto rowPath = [&]() {
        QPainterPath path;
        const QRectF r = rect();
        const double rt = property("ocRoundTop").toBool() ? radius : 0;
        const double rb = property("ocRoundBottom").toBool() ? radius : 0;
        path.moveTo(r.left() + rt, r.top());
        path.lineTo(r.right() - rt, r.top());
        if (rt > 0)
            path.quadTo(r.right(), r.top(), r.right(), r.top() + rt);
        path.lineTo(r.right(), r.bottom() - rb);
        if (rb > 0)
            path.quadTo(r.right(), r.bottom(), r.right() - rb, r.bottom());
        path.lineTo(r.left() + rb, r.bottom());
        if (rb > 0)
            path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - rb);
        path.lineTo(r.left(), r.top() + rt);
        if (rt > 0)
            path.quadTo(r.left(), r.top(), r.left() + rt, r.top());
        return path;
    };
    if (clickable && !m_disabled && (m_hover || m_pressed))
        p.fillPath(rowPath(), tok("--background-modifier-hover"));
    if (m_flash > 0) {
        QColor c = tok("--text-highlight-bg", QColor(255, 208, 0, 102));
        c.setAlphaF(c.alphaF() * float(m_flash));
        p.fillPath(rowPath(), c);
    }
    if (hasFocus() && clickable) {
        // .setting-item:focus-visible:after: inset 2px ring, --radius-m
        p.setPen(QPen(tok("--background-modifier-border-focus"), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), tokPx("--radius-m", 8), tokPx("--radius-m", 8));
    }
}

void SettingItem::mousePressEvent(QMouseEvent* e)
{
    if ((m_kind == Kind::Action || m_kind == Kind::Navigable) && !m_disabled) {
        m_pressed = true;
        update();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void SettingItem::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_pressed) {
        m_pressed = false;
        update();
        if (rect().contains(e->position().toPoint()))
            activate();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void SettingItem::keyPressEvent(QKeyEvent* e)
{
    if ((m_kind == Kind::Action || m_kind == Kind::Navigable) && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        activate();
        return;
    }
    QWidget::keyPressEvent(e);
}

void SettingItem::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void SettingItem::leaveEvent(QEvent* e)
{
    m_hover = false;
    m_pressed = false;
    update();
    QWidget::leaveEvent(e);
}

// ---------------------------------------------------------------------------------------------
// SettingGroup
// ---------------------------------------------------------------------------------------------

SettingGroup::SettingGroup(QWidget* parent)
    : QWidget(parent)
{
    setMaximumWidth(int(tokPx("--setting-group-max-width", 700)));
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_heading = new SettingItem(this);
    m_heading->setKind(SettingItem::Kind::Heading);
    m_heading->hide();
    m_layout->addWidget(m_heading);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("ocSettingItems"));
    m_cardLayout = new QVBoxLayout(m_card);
    m_cardLayout->setContentsMargins(0, 0, 0, 0);
    m_cardLayout->setSpacing(0);
    m_layout->addWidget(m_card);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            setMaximumWidth(int(tokPx("--setting-group-max-width", 700)));
            update();
        });
}

void SettingGroup::setHeading(const QString& heading)
{
    m_heading->setName(heading);
    m_heading->setVisible(!heading.isEmpty() || !m_heading->controls().isEmpty());
    // .setting-group .setting-item-heading { margin-bottom: var(--size-4-4) }
    m_layout->setSpacing(0);
    m_heading->setContentsMargins(0, 0, 0, 16);
}

QString SettingGroup::heading() const { return m_heading->name(); }

IconButton* SettingGroup::addHeaderButton(const QString& icon, const QString& tooltip, std::function<void()> onClick)
{
    IconButton* b = m_heading->addExtraButton(icon, tooltip, std::move(onClick));
    m_heading->setContentsMargins(0, 0, 0, 16);
    m_heading->show();
    return b;
}

void SettingGroup::setList(bool list)
{
    m_list = list;
    for (SettingItem* item : m_items) {
        item->setProperty("ocCentered", list);
        item->setPadding(list ? 16 : 20, list ? 8 : 20);
    }
    if (m_searchLayout) {
        const int px = list ? 16 : 20;
        m_searchLayout->setContentsMargins(px, 16, px, list ? 8 : 20);
    }
}

void SettingGroup::setPlain(bool plain)
{
    m_plain = plain;
    refresh();
}

SearchInput* SettingGroup::addSearch(const QString& placeholder)
{
    if (!m_search) {
        m_searchRow = new QWidget(m_card);
        m_searchRow->setObjectName(QStringLiteral("ocSettingGroupSearch"));
        m_searchLayout = new QVBoxLayout(m_searchRow);
        const int px = m_list ? 16 : 20;
        m_searchLayout->setContentsMargins(px, 16, px, m_list ? 8 : 20);
        m_searchLayout->setSpacing(8);
        m_search = new SearchInput(m_searchRow);
        m_searchLayout->addWidget(m_search);
        m_cardLayout->insertWidget(0, m_searchRow);
        m_alwaysVisible = true;
    }
    m_search->setPlaceholderText(placeholder);
    return m_search;
}

QWidget* SettingGroup::addSearchControl()
{
    if (!m_searchRow)
        addSearch(QString());
    auto* w = new QWidget(m_searchRow);
    w->setObjectName(QStringLiteral("ocSettingGroupSearchControl"));
    m_searchLayout->addWidget(w);
    return w;
}

SettingItem* SettingGroup::addItem()
{
    auto* item = new SettingItem;
    addItem(item);
    return item;
}

void SettingGroup::addItem(SettingItem* item)
{
    item->setParent(m_card);
    item->setProperty("ocCentered", m_list);
    item->setPadding(m_list ? 16 : 20, m_list ? 8 : 20);
    item->installEventFilter(this);
    m_itemObjects.insert(item);
    int index = m_cardLayout->count();
    if (m_empty)
        index = m_cardLayout->indexOf(m_empty);
    m_cardLayout->insertWidget(index, item);
    m_items.push_back(item);
    item->show();
    if (m_plain)
        refresh(); // first / last row paddings
}

void SettingGroup::clearItems()
{
    for (SettingItem* item : std::as_const(m_items)) {
        m_cardLayout->removeWidget(item);
        item->removeEventFilter(this);
        m_itemObjects.remove(item);
        item->hide();
        item->deleteLater();
    }
    m_items.clear();
    refresh();
}

void SettingGroup::setEmptyState(const QString& text)
{
    if (text.isEmpty()) {
        delete m_empty;
        m_empty = nullptr;
        refresh();
        return;
    }
    if (!m_empty) {
        m_empty = new SettingItem(m_card);
        m_empty->setKind(SettingItem::Kind::EmptyState);
        m_cardLayout->addWidget(m_empty);
    }
    // .setting-item.mod-empty-state { --setting-items-padding-y: var(--size-4-4) }
    m_empty->setPadding(m_list ? 16 : 20, 16);
    m_empty->setName(text);
    refresh();
}

void SettingGroup::refresh()
{
    SettingItem* first = nullptr;
    SettingItem* last = nullptr;
    int visible = 0;
    for (SettingItem* item : std::as_const(m_items)) {
        if (item->isHidden())
            continue;
        if (!first)
            first = item;
        last = item;
        ++visible;
    }
    if (m_empty) {
        m_empty->setVisible(visible == 0);
        if (visible == 0)
            first = last = m_empty;
    }
    for (SettingItem* item : std::as_const(m_items)) {
        item->setProperty("ocRoundTop", item == first && !m_searchRow);
        item->setProperty("ocRoundBottom", item == last);
    }
    if (m_empty) {
        m_empty->setProperty("ocRoundTop", m_empty == first && !m_searchRow);
        m_empty->setProperty("ocRoundBottom", m_empty == last);
    }
    if (m_plain) {
        // padding: 16px 0; the first row has no top padding, the last none at the bottom
        for (SettingItem* item : std::as_const(m_items)) {
            item->setPadding(0, item == first ? 0 : 16, item == last ? 0 : 16);
            item->setProperty("ocRoundTop", false);
            item->setProperty("ocRoundBottom", false);
        }
    }
    const bool hasRows = visible > 0 || m_empty || m_searchRow;
    m_card->setVisible(hasRows);
    setVisible(m_alwaysVisible || visible > 0 || (m_empty && m_items.isEmpty()));
    update();
}

bool SettingGroup::eventFilter(QObject* obj, QEvent* ev)
{
    if ((ev->type() == QEvent::ShowToParent || ev->type() == QEvent::HideToParent) && m_itemObjects.contains(obj))
        update();
    return QWidget::eventFilter(obj, ev);
}

QSize SettingGroup::sizeHint() const { return QWidget::sizeHint(); }

QRect SettingGroup::cardRect() const { return m_card->geometry(); }

void SettingGroup::paintEvent(QPaintEvent*)
{
    if (!m_card->isVisible())
        return;
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF card = cardRect();
    const double radius = tokPx("--setting-items-radius", 12);
    QPainterPath path;
    path.addRoundedRect(card, radius, radius);
    if (!m_plain)
        p.fillPath(path, t->color(QStringLiteral("--setting-items-background")));
    const double bw = m_plain ? 0 : tokPx("--setting-items-border-width", 0);
    if (bw > 0) {
        p.setPen(QPen(t->color(QStringLiteral("--setting-items-border-color")), bw));
        p.drawRoundedRect(card.adjusted(bw / 2, bw / 2, -bw / 2, -bw / 2), radius, radius);
    }
    // Dividers: .setting-item::before, 1px --background-modifier-border inset by the padding, on every
    // visible row but the first.
    const double divider = tokPx("--setting-items-divider-width", 1);
    if (divider <= 0)
        return;
    const int inset = m_plain ? 0 : m_list ? 16 : 20;
    bool firstSeen = false;
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor line = t->color(QStringLiteral("--background-modifier-border"));
    for (int i = 0; i < m_cardLayout->count(); ++i) {
        QWidget* w = m_cardLayout->itemAt(i)->widget();
        if (!w || w->isHidden() || w == m_searchRow)
            continue;
        if (firstSeen) {
            const QRect g = w->geometry().translated(m_card->pos());
            p.fillRect(QRect(g.left() + inset, g.top(), g.width() - 2 * inset, int(std::max(1.0, divider))), line);
        }
        firstSeen = true;
    }
}

} // namespace oc
