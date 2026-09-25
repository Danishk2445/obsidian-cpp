#include "ui/views/markdown/properties/properties_widget.h"

#include "core/properties/property_types.h"
#include "core/properties/property_yaml.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "ui/overlay/modal.h"
#include "ui/shell/main_window.h"
#include "ui/workspace/workspace.h"
#include "ui/views/markdown/properties/properties_style.h"
#include "ui/views/markdown/properties/property_suggest.h"

#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>

#include <cmath>

namespace oc {

namespace {

const char* kPropertiesMime = "obsidian/properties";

void drawIcon(QPainter& p, const QString& name, const QPointF& center, int size, const QColor& color, double stroke, qreal dpr)
{
    const QPixmap pm = IconProvider::instance().pixmap(name, size, color, stroke, dpr);
    p.drawPixmap(QPointF(center.x() - size / 2.0, center.y() - size / 2.0), pm);
}

QIcon menuIcon(const QString& name) { return IconProvider::instance().icon(name, 16); }

bool isPlainKey(QKeyEvent* e, Qt::KeyboardModifiers allowed = Qt::NoModifier)
{
    return (e->modifiers() & ~(allowed | Qt::KeypadModifier)) == Qt::NoModifier;
}

bool modPressed(QKeyEvent* e) { return e->modifiers() & Qt::ControlModifier; }

// The heading line: "Properties" with the collapse indicator in the gutter to its left.
class PropertiesHeading : public QWidget {
public:
    explicit PropertiesHeading(PropertiesWidget* owner)
        : QWidget(owner)
        , m_owner(owner)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }
    static constexpr int kGutter = 22; // .collapse-indicator: inset-inline-start -22px
    QSize sizeHint() const override
    {
        const propui::Style& s = propui::style();
        QFontMetrics fm(s.titleFont);
        const int lineH = int(std::round(s.titleFont.pixelSize() * 1.2));
        return {kGutter + 2 * s.headingPad + fm.horizontalAdvance(tr("Properties")), lineH + 2 * s.headingPad};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const propui::Style& s = propui::style();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect box = rect().adjusted(kGutter, 0, 0, 0);
        if (hasFocus()) {
            p.setPen(QPen(s.ringFocus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(box).adjusted(1, 1, -1, -1), s.radius, s.radius);
        }
        p.setFont(s.titleFont);
        p.setPen(s.text);
        p.drawText(box.adjusted(s.headingPad, s.headingPad, -s.headingPad, -s.headingPad), Qt::AlignLeft | Qt::AlignVCenter, tr("Properties"));
        if (underMouse() || m_owner->isCollapsed()) {
            // right-triangle, 10px, stroke 4 (24-unit box), rotated -90° when collapsed
            const QPointF c(6 + 5, height() / 2.0);
            const qreal k = 10.0 / 24.0;
            QPainterPath path;
            path.moveTo(-9 * k, -4 * k);
            path.lineTo(0, 5 * k);
            path.lineTo(9 * k, -4 * k);
            QTransform tr;
            tr.translate(c.x(), c.y());
            if (m_owner->isCollapsed())
                tr.rotate(-90);
            p.setPen(QPen(s.collapseIcon, 4 * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(tr.map(path));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_owner->setCollapsed(!m_owner->isCollapsed());
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        m_owner->populateHeadingMenu(menu);
        menu->popup(e->globalPos());
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier)) {
            QWidget::keyPressEvent(e);
            return;
        }
        switch (e->key()) {
        case Qt::Key_Left:
            m_owner->setCollapsed(true);
            return;
        case Qt::Key_Right:
            m_owner->setCollapsed(false);
            return;
        case Qt::Key_Up:
            emit m_owner->focusBeforeRequested();
            return;
        case Qt::Key_Down:
        case Qt::Key_J:
            if (m_owner->isCollapsed())
                emit m_owner->focusAfterRequested();
            else
                m_owner->focusPropertyAt(0);
            return;
        default:
            QWidget::keyPressEvent(e);
        }
    }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        return QWidget::event(e);
    }

private:
    PropertiesWidget* m_owner;
};

// .metadata-add-button.text-icon-button: "+ Add property".
class AddPropertyButton : public QWidget {
public:
    explicit AddPropertyButton(PropertiesWidget* owner)
        : QWidget(owner)
        , m_owner(owner)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override
    {
        const propui::Style& s = propui::style();
        QFontMetrics fm(s.buttonFont);
        const int h = std::max(s.iconSize, fm.height()) + 8;
        return {6 + s.iconSize + 4 + fm.horizontalAdvance(tr("Add property")) + 2 + 4, h};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const propui::Style& s = propui::style();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hot = underMouse();
        const double radius = currentTheme()->px(QStringLiteral("--button-radius"), -1, 5);
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(s.hoverBg);
            p.drawRoundedRect(QRectF(rect()), radius, radius);
        }
        if (hasFocus()) {
            p.setPen(QPen(s.ringFocus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);
        }
        const QColor fg = hot ? s.text : s.muted;
        drawIcon(p, QStringLiteral("plus"), QPointF(6 + s.iconSize / 2.0, height() / 2.0), s.iconSize,
                 hot ? currentTheme()->color(QStringLiteral("--icon-color-hover"), s.muted) : s.muted, 1.75, devicePixelRatioF());
        p.setFont(s.buttonFont);
        p.setPen(fg);
        p.drawText(QRect(6 + s.iconSize + 4, 0, width(), height()), Qt::AlignLeft | Qt::AlignVCenter, tr("Add property"));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_owner->addProperty();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        if ((e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !shift) {
            m_owner->addProperty();
            return;
        }
        if (e->key() == Qt::Key_Up || (e->key() == Qt::Key_K && !shift) || e->key() == Qt::Key_Backtab) {
            m_owner->focusPropertyAt(-1);
            return;
        }
        if (e->key() == Qt::Key_Down || (e->key() == Qt::Key_J && !shift) || e->key() == Qt::Key_Tab) {
            emit m_owner->focusAfterRequested();
            return;
        }
        if (!m_owner->handleKey(e, nullptr))
            QWidget::keyPressEvent(e);
    }
    bool focusNextPrevChild(bool) override { return false; }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        if (e->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(e);
            if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
                keyPressEvent(ke);
                return true;
            }
        }
        return QWidget::event(e);
    }

private:
    PropertiesWidget* m_owner;
};

// .metadata-error-container: "Invalid properties" + "Show source".
class PropertiesErrorBox : public QWidget {
public:
    explicit PropertiesErrorBox(PropertiesWidget* owner)
        : QWidget(owner)
        , m_owner(owner)
    {
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
    }
    int titleHeight() const { return int(std::round(propui::style().titleFont.pixelSize() * 1.2)); }
    QRect buttonRect() const
    {
        const propui::Style& s = propui::style();
        QFontMetrics fm(s.buttonFont);
        return QRect(s.padX - 4 + 6 - 6, s.padY + titleHeight() + 7, fm.horizontalAdvance(tr("Show source")) + 12, fm.height() + 8);
    }
    QSize sizeHint() const override
    {
        const propui::Style& s = propui::style();
        return {300, buttonRect().bottom() + 1 + s.padY};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const propui::Style& s = propui::style();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double radius = currentTheme()->px(QStringLiteral("--callout-radius"), -1, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(s.errorBg);
        p.drawRoundedRect(QRectF(rect()), radius, radius);
        p.setFont(s.titleFont);
        p.setPen(s.error);
        p.drawText(QRect(s.padX, s.padY, width() - 2 * s.padX, titleHeight()), Qt::AlignLeft | Qt::AlignVCenter, tr("Invalid properties"));
        const QRect b = buttonRect();
        const bool hot = b.contains(mapFromGlobal(QCursor::pos())) && underMouse();
        if (hot) {
            p.setBrush(s.hoverBg);
            p.drawRoundedRect(QRectF(b), 5, 5);
        }
        p.setFont(s.buttonFont);
        p.setPen(hot ? s.text : s.muted);
        p.drawText(b, Qt::AlignCenter, tr("Show source"));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && buttonRect().contains(e->pos())) {
            emit m_owner->showSourceRequested();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        setCursor(buttonRect().contains(e->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
        QWidget::mouseMoveEvent(e);
    }

private:
    PropertiesWidget* m_owner;
};

bool sameValue(const QVariant& a, const QVariant& b)
{
    if (props::isNullProperty(a) || props::isNullProperty(b))
        return props::isNullProperty(a) == props::isNullProperty(b);
    const bool la = a.typeId() == QMetaType::QVariantList || a.typeId() == QMetaType::QStringList;
    const bool lb = b.typeId() == QMetaType::QVariantList || b.typeId() == QMetaType::QStringList;
    if (la || lb)
        return la && lb && a.toList() == b.toList();
    if (a.typeId() == QMetaType::QString || b.typeId() == QMetaType::QString)
        return a.typeId() == b.typeId() && a.toString() == b.toString();
    return a == b;
}

} // namespace

// =================================================================================================
// PropertyRow
// =================================================================================================
PropertyRow::PropertyRow(PropertiesWidget* owner, const props::Property& entry)
    : QWidget(owner)
    , m_owner(owner)
    , m_entry(entry)
    , m_keyInput(new QLineEdit(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    const propui::Style& s = propui::style();
    m_keyInput->setObjectName(QStringLiteral("ocPropertyKey"));
    m_keyInput->setProperty("ocFlat", true);
    m_keyInput->setFrame(false);
    m_keyInput->setFont(s.labelFont);
    QPalette pal = m_keyInput->palette();
    pal.setColor(QPalette::Text, s.labelText);
    pal.setColor(QPalette::PlaceholderText, s.placeholder);
    pal.setColor(QPalette::Base, Qt::transparent);
    m_keyInput->setPalette(pal);
    m_keyInput->setStyleSheet(QStringLiteral("QLineEdit#ocPropertyKey { background: transparent; border: none; padding: 0; }"));
    m_keyInput->setTextMargins(s.padX - 2, 0, s.padX - 2, 0); // QLineEdit adds 2px of its own
    m_keyInput->setText(entry.key);
    m_keyInput->setCursorPosition(0);
    m_keyInput->setToolTip(entry.key);
    m_keyInput->installEventFilter(this);
    connect(m_keyInput, &QLineEdit::textEdited, this, [this] { update(); });
    PropertySuggest::Source src;
    src.session = [this] { return m_owner->session(); };
    src.sourcePath = [this] { return m_owner->context().sourcePath ? m_owner->context().sourcePath() : QString(); };
    src.exclude = [this](const QString& name) {
        const QString lower = name.toLower();
        for (PropertyRow* r : std::as_const(m_owner->m_rows))
            if (r != this && r->key().toLower() == lower)
                return true;
        return false;
    };
    m_keySuggest = new PropertySuggest(m_keyInput, PropertySuggest::Mode::Keys, src, this);
    connect(m_keySuggest, &PropertySuggest::accepted, this, [this](const QString& text, bool, int) {
        m_keyInput->setText(text.trimmed());
        if (commitKey(text.trimmed())) {
            render(m_entry, true);
            focusValue();
        }
    });
    render(entry, true);
}

PropertyEditorContext PropertyRow::editorContext() const
{
    PropertyEditorContext ctx = m_owner->context();
    ctx.key = m_entry.key;
    return ctx;
}

QString PropertyRow::widgetType() const { return m_editor ? m_editor->type() : QString(); }

void PropertyRow::createEditor(const QString& type)
{
    if (m_editor) {
        m_editor->hide();
        m_editor->deleteLater();
    }
    m_editor = createPropertyEditor(type, m_entry.value, editorContext(), this);
    m_editor->setReadOnly(m_readOnly);
    connect(m_editor, &PropertyValueEditor::changed, this, &PropertyRow::updateValue);
    connect(m_editor, &PropertyValueEditor::blurRequested, this, [this] { focusProperty(); });
    connect(m_editor, &PropertyValueEditor::heightChanged, this, [this] {
        layoutChildren();
        emit heightChanged();
    });
    m_editor->show();
    m_shown = m_editor->type();
    layoutChildren();
}

void PropertyRow::render(const props::Property& entry, bool force, bool useExpected)
{
    const QString prevInferred = m_inferred;
    const QVariant prevValue = m_entry.value;
    m_entry = entry;
    if (m_keyInput->text() != entry.key && !m_keyInput->hasFocus()) {
        m_keyInput->setText(entry.key);
        m_keyInput->setCursorPosition(0);
    }
    m_keyInput->setToolTip(entry.key);
    PropertyTypeManager* types = PropertyTypeManager::of(m_owner->session());
    const PropertyTypeManager::TypeInfo info = types ? types->typeInfo(entry.key, entry.value)
                                                     : PropertyTypeManager::TypeInfo{inferPropertyType(entry.value), inferPropertyType(entry.value)};
    m_expected = info.expected;
    m_inferred = info.inferred;
    const QString show = useExpected ? m_expected : m_inferred;
    if (!force && m_rendered
        && ((m_editor && m_editor->hasFocusWithin()) || (m_editor && sameValue(prevValue, entry.value) && prevInferred == m_inferred))) {
        update();
        return;
    }
    m_rendered = true;
    m_warning = !useExpected && props::propertyType(m_expected).id != props::propertyType(m_inferred).id;
    if (m_editor && m_editor->type() == props::propertyType(show).id && !force)
        m_editor->setValue(entry.value);
    else
        createEditor(props::propertyType(show).id);
    if (m_editor && !m_editor->hasFocusWithin())
        m_editor->setValue(entry.value);
    update();
    emit heightChanged();
}

void PropertyRow::setReadOnly(bool ro)
{
    m_readOnly = ro;
    m_keyInput->setReadOnly(ro);
    m_keySuggest->setEnabled(!ro);
    if (m_editor)
        m_editor->setReadOnly(ro);
    update();
}

QRect PropertyRow::iconRect() const
{
    const propui::Style& s = propui::style();
    return QRect(4, (s.inputHeight - s.iconSize) / 2, s.iconSize, s.iconSize);
}

QRect PropertyRow::warningRect() const
{
    const propui::Style& s = propui::style();
    const int size = 16 + 8;
    return QRect(width() - 2 - size, 2, size, std::min(height(), s.inputHeight) - 4);
}

int PropertyRow::heightForWidth(int w) const
{
    const propui::Style& s = propui::style();
    const int valueW = std::max(40, w - s.labelWidth);
    return std::max(s.inputHeight, m_editor ? m_editor->heightForWidth(valueW) : s.inputHeight);
}

QSize PropertyRow::sizeHint() const { return {400, heightForWidth(width() > 0 ? width() : 600)}; }

void PropertyRow::layoutChildren()
{
    const propui::Style& s = propui::style();
    const int keyX = 4 + s.iconSize;
    m_keyInput->setGeometry(keyX, 0, std::max(10, s.labelWidth - keyX), s.inputHeight);
    if (m_editor) {
        const int w = std::max(40, width() - s.labelWidth);
        m_editor->setGeometry(s.labelWidth, 0, w, std::max(s.inputHeight, m_editor->heightForWidth(w)));
    }
}

void PropertyRow::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutChildren();
}

bool PropertyRow::hasFocusWithin() const
{
    QWidget* f = QApplication::focusWidget();
    return f && (f == this || isAncestorOf(f));
}

void PropertyRow::setSelected(bool on)
{
    if (m_selected == on)
        return;
    m_selected = on;
    update();
}

void PropertyRow::paintEvent(QPaintEvent*)
{
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()), s.radius, s.radius);
    p.setClipPath(clip);
    if (m_selected && m_owner->hasFocusWithin())
        p.fillRect(rect(), s.selectedBg);
    if (m_keyInput->hasFocus())
        p.fillRect(QRect(0, 0, s.labelWidth, height()), s.hoverBg); // --metadata-label-background-active
    if (m_editor && m_editor->hasFocusWithin())
        p.fillRect(QRect(s.labelWidth, 0, width() - s.labelWidth, height()), s.hoverBg); // --metadata-input-background-active
    p.setClipping(false);
    // type icon (.metadata-property-icon; aria-disabled while the key is empty)
    const bool disabled = m_entry.key.isEmpty() && m_keyInput->text().trimmed().isEmpty();
    QColor ic = m_hoverIcon && !disabled ? s.iconHover : s.iconColor;
    if (disabled) {
        ic = s.muted;
        ic.setAlphaF(ic.alphaF() * 0.4);
    }
    drawIcon(p, props::propertyType(m_expected).icon, QRectF(iconRect()).center(), s.iconSize, ic,
             currentTheme()->number(QStringLiteral("--icon-stroke"), 1.75), devicePixelRatioF());
    if (m_warning) {
        const QRect w = warningRect();
        if (m_hoverWarning) {
            QColor bg = s.warning;
            bg.setAlphaF(0.15);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(QRectF(w), 4, 4);
        }
        drawIcon(p, QStringLiteral("alert-triangle"), QRectF(w).center(), 16, s.warning, 2, devicePixelRatioF());
    }
}

void PropertyRow::enterEvent(QEnterEvent* e)
{
    m_owner->update();
    QWidget::enterEvent(e);
}

void PropertyRow::leaveEvent(QEvent* e)
{
    m_hoverIcon = m_hoverWarning = false;
    update();
    m_owner->update();
    QWidget::leaveEvent(e);
}

void PropertyRow::mouseMoveEvent(QMouseEvent* e)
{
    const bool icon = iconRect().adjusted(-4, -4, 4, 4).contains(e->pos());
    const bool warn = m_warning && warningRect().contains(e->pos());
    if (icon != m_hoverIcon || warn != m_hoverWarning) {
        m_hoverIcon = icon;
        m_hoverWarning = warn;
        update();
    }
    if (warn) {
        QToolTip::showText(e->globalPosition().toPoint(), tr("Type mismatch, expected %1").arg(props::propertyType(m_expected).name), this);
    }
    setCursor(icon || warn ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void PropertyRow::mousePressEvent(QMouseEvent* e)
{
    if (m_warning && warningRect().contains(e->pos()) && e->button() == Qt::LeftButton) {
        showDisplayAsPrompt();
        return;
    }
    if (iconRect().adjusted(-4, -4, 4, 4).contains(e->pos()) && e->button() == Qt::LeftButton) {
        if (m_entry.key.isEmpty())
            return;
        // Alt+click toggles the selection, Shift+click extends it; a plain click opens the menu
        if ((e->modifiers() & Qt::AltModifier) && !(e->modifiers() & Qt::ShiftModifier)) {
            focusProperty();
            setSelected(!m_selected);
            return;
        }
        if (e->modifiers() & Qt::ShiftModifier) {
            PropertyRow* anchor = m_owner->focusedRow();
            focusProperty();
            if (anchor && anchor != this) {
                int a = m_owner->indexOf(anchor), b = m_owner->indexOf(this);
                if (a > b)
                    std::swap(a, b);
                const bool on = !(anchor->isSelected() && isSelected());
                for (int i = a; i <= b; ++i)
                    m_owner->rowAt(i)->setSelected(on);
            } else {
                setSelected(true);
            }
            return;
        }
        m_owner->clearSelection();
        focusProperty();
        showMenu(mapToGlobal(iconRect().bottomLeft()));
        return;
    }
    if (e->button() == Qt::LeftButton && e->pos().x() >= propui::style().labelWidth && m_editor && !m_readOnly) {
        // a click on the empty part of the value cell edits the value
        focusValue(QStringLiteral("end"));
        return;
    }
    QWidget::mousePressEvent(e);
}

void PropertyRow::contextMenuEvent(QContextMenuEvent* e)
{
    if (m_entry.key.isEmpty() || e->pos().x() >= propui::style().labelWidth)
        return QWidget::contextMenuEvent(e);
    focusProperty();
    showMenu(e->globalPos());
}

bool PropertyRow::focusNextPrevChild(bool next)
{
    // Tab / Shift+Tab inside a property: key -> value -> next property (Obsidian's DOM focus order).
    QWidget* f = QApplication::focusWidget();
    if (f == m_keyInput) {
        if (!next) {
            focusProperty();
        } else if (m_readOnly || commitKey(m_keyInput->text().trimmed())) {
            render(m_entry, !m_readOnly);
            focusValue();
        }
        return true;
    }
    if (m_editor && f && (f == m_editor || m_editor->isAncestorOf(f))) {
        if (next)
            m_owner->focusPropertyAt(m_owner->indexOf(this) + 1);
        else
            focusKey();
        return true;
    }
    QKeyEvent ev(QEvent::KeyPress, next ? Qt::Key_Tab : Qt::Key_Backtab, next ? Qt::NoModifier : Qt::ShiftModifier);
    return m_owner->handleKey(&ev, this);
}

void PropertyRow::focusInEvent(QFocusEvent* e)
{
    QWidget::focusInEvent(e);
    m_owner->update();
}

void PropertyRow::focusProperty()
{
    if (m_owner->isCollapsed())
        m_owner->setCollapsed(false);
    setFocus(Qt::OtherFocusReason);
}

void PropertyRow::focusKey()
{
    if (m_owner->isCollapsed())
        m_owner->setCollapsed(false);
    m_keyInput->setFocus(Qt::OtherFocusReason);
    if (!m_readOnly)
        m_keyInput->selectAll();
}

void PropertyRow::focusValue(const QString& where)
{
    if (m_owner->isCollapsed())
        m_owner->setCollapsed(false);
    if (m_editor)
        m_editor->focusEditor(where);
}

void PropertyRow::keyPressEvent(QKeyEvent* e)
{
    if (m_owner->selectedRows().size() > 1) {
        if (!m_owner->handleKey(e, this))
            QWidget::keyPressEvent(e);
        return;
    }
    const QString t = e->text();
    const bool noMods = isPlainKey(e);
    if (noMods && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space)) {
        focusValue(QStringLiteral("both"));
        return;
    }
    if (t == u"A" && isPlainKey(e, Qt::ShiftModifier)) {
        focusValue(QStringLiteral("end"));
        return;
    }
    if (t == u"i" && noMods) {
        focusValue(QStringLiteral("start"));
        return;
    }
    if (noMods && (e->key() == Qt::Key_Left || t == u"h")) {
        focusKey();
        return;
    }
    if (noMods && (e->key() == Qt::Key_Right || t == u"l")) {
        focusValue(QStringLiteral("both"));
        return;
    }
    if (!m_owner->handleKey(e, this))
        QWidget::keyPressEvent(e);
}

bool PropertyRow::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_keyInput)
        return QWidget::eventFilter(obj, ev);
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const int k = ke->key();
        if ((k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Tab) && isPlainKey(ke)) {
            if (m_readOnly)
                return false;
            if (commitKey(m_keyInput->text().trimmed())) {
                render(m_entry, true);
                focusValue();
            }
            return true;
        }
        if (k == Qt::Key_Backtab) {
            focusProperty();
            return true;
        }
        if (k == Qt::Key_Escape) {
            m_keyInput->setText(m_entry.key);
            if (!m_entry.key.isEmpty())
                focusProperty();
            else
                m_owner->removeProperties({this});
            return true;
        }
    } else if (ev->type() == QEvent::FocusOut) {
        const auto reason = static_cast<QFocusEvent*>(ev)->reason();
        update();
        if (reason == Qt::PopupFocusReason || m_readOnly)
            return false;
        const QString v = m_keyInput->text().trimmed();
        if (v.isEmpty() && m_entry.key.isEmpty()) {
            QPointer<PropertyRow> self = this;
            QTimer::singleShot(0, m_owner, [self, owner = QPointer<PropertiesWidget>(m_owner)] {
                if (self && owner && self->key().isEmpty() && !self->hasFocusWithin())
                    owner->removeProperties({self.data()}, false);
            });
        } else if (!commitKey(v)) {
            m_keyInput->setText(m_entry.key);
        }
        m_keyInput->setCursorPosition(0);
    } else if (ev->type() == QEvent::FocusIn) {
        update();
    }
    return QWidget::eventFilter(obj, ev);
}

bool PropertyRow::commitKey(const QString& raw)
{
    const QString k = raw.trimmed();
    if (k.isEmpty()) {
        if (m_keyInput->hasFocus())
            QToolTip::showText(m_keyInput->mapToGlobal(QPoint(0, m_keyInput->height())), tr("Property name cannot be empty."), m_keyInput);
        return false;
    }
    if (k == m_entry.key)
        return true;
    for (PropertyRow* other : std::as_const(m_owner->m_rows)) {
        if (other != this && other->key().compare(k, Qt::CaseInsensitive) == 0) {
            QToolTip::showText(m_keyInput->mapToGlobal(QPoint(0, m_keyInput->height())), tr("Property already exists"), m_keyInput);
            return false;
        }
    }
    const QString old = m_entry.key;
    const QVariant value = m_entry.value;
    m_entry.key = k;
    m_keyInput->setToolTip(k);
    const bool ok = m_owner->edit([&](props::FrontmatterEditor& ed) {
        if (old.isEmpty() || !ed.has(old))
            ed.add(k, value);
        else
            ed.rename(old, k);
    });
    if (!ok) {
        m_entry.key = old;
        return false;
    }
    update();
    return true;
}

void PropertyRow::updateValue(const QVariant& raw)
{
    const QVariant value = props::normalizePropertyValue(raw);
    if (sameValue(value, m_entry.value) && value.typeId() == m_entry.value.typeId())
        return;
    m_entry.value = value;
    if (m_entry.key.isEmpty())
        return; // saved with its key
    const QString key = m_entry.key;
    m_owner->edit([&](props::FrontmatterEditor& ed) { ed.set(key, value); });
}

void PropertyRow::changeType(const QString& type)
{
    PropertyTypeManager* types = PropertyTypeManager::of(m_owner->session());
    if (!types || m_entry.key.isEmpty())
        return;
    if (type.isEmpty())
        types->unsetType(m_entry.key);
    else
        types->setType(m_entry.key, type);
    render(m_entry, true);
    if (!type.isEmpty() && !props::isNullProperty(m_entry.value) && !props::validatePropertyValue(type, m_entry.value))
        showDisplayAsPrompt();
}

void PropertyRow::showDisplayAsPrompt()
{
    auto* w = qobject_cast<MainWindow*>(window());
    const QString expected = props::propertyType(m_expected).name;
    const QString inferred = props::propertyType(m_inferred).name;
    if (!w || !w->overlay()) {
        render(m_entry, true, true);
        return;
    }
    QPointer<PropertyRow> self = this;
    ConfirmModal::ask(w->overlay(), tr("Display as %1?").arg(expected),
                      tr("Your %1 data is not compatible. It will be adapted to fit the new format.").arg(inferred.toLower()),
                      {{tr("Update"), Modal::ButtonStyle::Cta}, {tr("Cancel"), Modal::ButtonStyle::Normal}}, QString(),
                      [self](int button, bool) {
                          if (!self || button != 0)
                              return;
                          self->render(self->entry(), true, true);
                          self->focusValue();
                      });
}

void PropertyRow::populateMenu(QMenu* menu)
{
    PropertyTypeManager* types = PropertyTypeManager::of(m_owner->session());
    const bool reserved = PropertyTypeManager::isReserved(m_entry.key);
    if (!m_readOnly && types) {
        QMenu* sub = menu->addMenu(menuIcon(QStringLiteral("info")), tr("Property type"));
        const QString assigned = types->assignedType(m_entry.key);
        if (!reserved) {
            const QString automatic = assigned.isEmpty() ? tr("Automatic (%1)").arg(props::propertyType(m_inferred).name) : tr("Automatic");
            QAction* a = sub->addAction(menuIcon(QStringLiteral("loader")), automatic, this, [this] { changeType(QString()); });
            a->setCheckable(true);
            a->setChecked(assigned.isEmpty());
        }
        for (const props::PropertyType& t : props::menuPropertyTypes(m_entry.key)) {
            QAction* a = sub->addAction(menuIcon(t.icon), t.name, this, [this, id = t.id] { changeType(id); });
            a->setCheckable(true);
            a->setChecked(t.id == assigned);
            a->setEnabled(!reserved);
        }
        menu->addSeparator();
        menu->addAction(menuIcon(QStringLiteral("scissors")), tr("Cut"), this, [this] { m_owner->copyProperties({this}, true); });
    }
    menu->addAction(menuIcon(QStringLiteral("copy")), tr("Copy"), this, [this] {
        QList<PropertyRow*> rows = m_owner->selectedRows();
        if (!rows.contains(this))
            rows = {this};
        m_owner->copyProperties(rows, false);
    });
    if (m_readOnly)
        return;
    menu->addAction(menuIcon(QStringLiteral("clipboard-check")), tr("Paste"), this, [this] { m_owner->pasteProperties(); });
    menu->addSeparator();
    QAction* remove = menu->addAction(menuIcon(QStringLiteral("trash-2")), tr("Remove"), this, [this] { m_owner->removeProperties({this}); });
    remove->setProperty("ocWarning", true);
}

void PropertyRow::showMenu(const QPoint& globalPos)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    populateMenu(menu);
    menu->popup(globalPos);
}

// =================================================================================================
// PropertiesWidget
// =================================================================================================
PropertiesWidget::PropertiesWidget(QWidget* parent)
    : QWidget(parent)
    , m_heading(new PropertiesHeading(this))
    , m_addButton(new AddPropertyButton(this))
    , m_errorBox(new PropertiesErrorBox(this))
{
    setObjectName(QStringLiteral("ocProperties"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    m_errorBox->hide();
    m_focusConn = connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
        if ((old && isAncestorOf(old)) || (now && isAncestorOf(now))) {
            update();
            for (PropertyRow* r : std::as_const(m_rows))
                r->update();
        }
    });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            for (PropertyRow* r : std::as_const(m_rows))
                r->render(r->entry(), true);
            relayout();
        });
    relayout();
}

PropertiesWidget::~PropertiesWidget()
{
    disconnect(m_typesConn);
    disconnect(m_focusConn); // the rows are deleted after this destructor (and move the focus)
    const QList<PropertyRow*> rows = m_rows;
    m_rows.clear();
    qDeleteAll(rows);
}

void PropertiesWidget::setContext(const PropertyEditorContext& ctx)
{
    m_ctx = ctx;
    disconnect(m_typesConn);
    m_typesOwner = nullptr;
    connectTypes();
}

void PropertiesWidget::connectTypes()
{
    // the session may only be known once a file is loaded
    PropertyTypeManager* types = PropertyTypeManager::of(session());
    if (!types || m_typesOwner == types)
        return;
    disconnect(m_typesConn);
    m_typesOwner = types;
    m_typesConn = connect(types, &PropertyTypeManager::changed, this, &PropertiesWidget::rowTypeChanged);
}

void PropertiesWidget::rowTypeChanged(const QString& lowerKey)
{
    for (PropertyRow* r : std::as_const(m_rows))
        if (r->key().toLower() == lowerKey && !r->hasFocusWithin())
            r->render(r->entry(), true);
}

void PropertiesWidget::synchronize(props::FrontmatterEditor::State state, const QList<props::Property>& properties)
{
    connectTypes();
    m_state = state;
    QList<PropertyRow*> rows;
    if (state != props::FrontmatterEditor::State::Invalid) {
        for (const props::Property& p : properties) {
            PropertyRow* r = nullptr;
            for (PropertyRow* existing : std::as_const(m_rows)) {
                if (existing->key() == p.key && !rows.contains(existing)) {
                    r = existing;
                    break;
                }
            }
            if (!r) {
                r = new PropertyRow(this, p);
                r->setReadOnly(m_readOnly);
                connect(r, &PropertyRow::heightChanged, this, &PropertiesWidget::relayout);
                r->show();
            } else {
                r->render(p);
            }
            rows << r;
        }
        if (PropertyRow* pending = pendingRow(); pending && !rows.contains(pending))
            rows << pending; // a property being added (no key yet)
    }
    for (PropertyRow* r : std::as_const(m_rows)) {
        if (!rows.contains(r)) {
            r->hide();
            r->deleteLater();
        }
    }
    m_rows = rows;
    relayout();
}

PropertyRow* PropertiesWidget::pendingRow() const
{
    for (PropertyRow* r : m_rows)
        if (r->key().isEmpty())
            return r;
    return nullptr;
}

void PropertiesWidget::setDisplay(Display display)
{
    m_display = display;
    relayout();
}

void PropertiesWidget::setReadOnly(bool ro)
{
    if (m_readOnly == ro)
        return;
    m_readOnly = ro;
    for (PropertyRow* r : std::as_const(m_rows))
        r->setReadOnly(ro);
    if (ro)
        if (PropertyRow* pending = pendingRow())
            removeProperties({pending}, false);
    relayout();
}

void PropertiesWidget::setLeftInset(int px)
{
    m_inset = px;
    relayout();
}

bool PropertiesWidget::shouldShow() const
{
    if (isInvalid())
        return true;
    return m_display == Display::Normal && !m_rows.isEmpty();
}

QStringList PropertiesWidget::keys() const
{
    QStringList out;
    for (PropertyRow* r : m_rows)
        out << r->key();
    return out;
}

PropertyRow* PropertiesWidget::row(const QString& key) const
{
    for (PropertyRow* r : m_rows)
        if (r->key() == key)
            return r;
    return nullptr;
}

PropertyRow* PropertiesWidget::focusedRow() const
{
    QWidget* f = QApplication::focusWidget();
    for (PropertyRow* r : m_rows)
        if (f && (f == r || r->isAncestorOf(f)))
            return r;
    return nullptr;
}

bool PropertiesWidget::hasFocusWithin() const
{
    QWidget* f = QApplication::focusWidget();
    return f && (f == this || isAncestorOf(f));
}

// ---- layout ----------------------------------------------------------------------------------------

int PropertiesWidget::layoutContent(int width, bool apply) const
{
    const propui::Style& s = propui::style();
    const int x = m_inset;
    const int w = std::max(100, width - m_inset);
    int y = s.containerPadY;
    const bool invalid = isInvalid();
    if (apply) {
        m_errorBox->setVisible(invalid);
        m_heading->setVisible(!invalid);
    }
    if (invalid) {
        const int h = m_errorBox->sizeHint().height();
        if (apply)
            m_errorBox->setGeometry(x, y, w, h);
        y += h;
        if (apply) {
            m_addButton->hide();
            for (PropertyRow* r : m_rows)
                r->hide();
        }
        return y + s.containerPadY;
    }
    const QSize hs = m_heading->sizeHint();
    if (apply)
        m_heading->setGeometry(x - PropertiesHeading::kGutter, y, hs.width(), hs.height());
    y += hs.height() + s.headingGapBelow;
    bool first = true;
    for (PropertyRow* r : m_rows) {
        if (m_collapsed) {
            if (apply)
                r->hide();
            continue;
        }
        if (!first)
            y += s.gap;
        first = false;
        const int h = r->heightForWidth(w);
        if (apply) {
            r->setGeometry(x, y, w, h);
            r->show();
        }
        y += h;
    }
    const bool button = !m_collapsed && !m_readOnly;
    if (apply)
        m_addButton->setVisible(button);
    if (button) {
        y += int(std::round(s.buttonFont.pixelSize() * 0.5)); // margin-top: 0.5em
        const QSize bs = m_addButton->sizeHint();
        if (apply)
            m_addButton->setGeometry(x, y, bs.width(), bs.height());
        y += bs.height();
    }
    return y + s.containerPadY;
}

int PropertiesWidget::heightForWidth(int w) const { return layoutContent(w, false); }

QSize PropertiesWidget::sizeHint() const { return {600, heightForWidth(width() > 0 ? width() : 700)}; }

void PropertiesWidget::relayout()
{
    const int h = layoutContent(width(), true);
    update();
    updateGeometry();
    if (h != m_lastHeight) {
        m_lastHeight = h;
        emit heightChanged();
    }
}

void PropertiesWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutContent(width(), true);
    const int h = layoutContent(width(), false);
    if (h != m_lastHeight) {
        m_lastHeight = h;
        emit heightChanged();
    }
}

void PropertiesWidget::paintEvent(QPaintEvent*)
{
    // .metadata-property rings: 1px --background-modifier-border-hover on hover, 2px
    // --background-modifier-border-focus while the property has the focus (box-shadows).
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (PropertyRow* r : std::as_const(m_rows)) {
        if (!r->isVisible())
            continue;
        const QRectF g = r->geometry();
        if (r->hasFocusWithin()) {
            p.setPen(QPen(s.ringFocus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(g.adjusted(-1, -1, 1, 1), s.radius + 1, s.radius + 1);
        } else if (r->underMouse()) {
            p.setPen(QPen(s.ringHover, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(g.adjusted(-0.5, -0.5, 0.5, 0.5), s.radius + 0.5, s.radius + 0.5);
        }
    }
}

bool PropertiesWidget::event(QEvent* e)
{
    if (e->type() == QEvent::LayoutRequest)
        relayout();
    return QWidget::event(e);
}

// ---- operations ------------------------------------------------------------------------------------

bool PropertiesWidget::edit(const Edit& fn)
{
    if (!m_edit || m_readOnly)
        return false;
    return m_edit(fn);
}

void PropertiesWidget::notify(const QString& message)
{
    if (m_ctx.notice)
        m_ctx.notice(message);
}

void PropertiesWidget::addProperty(const QString& key)
{
    if (m_readOnly)
        return;
    if (isInvalid()) {
        notify(tr("Failed to read properties."));
        return;
    }
    setCollapsed(false);
    if (!key.isEmpty()) {
        if (!row(key)) {
            if (!edit([&](props::FrontmatterEditor& ed) { ed.add(key, QVariant()); }))
                return;
        }
        focusValue(key, QStringLiteral("both"));
        return;
    }
    PropertyRow* pending = pendingRow();
    if (!pending) {
        pending = new PropertyRow(this, props::Property{QString(), QVariant()});
        pending->setReadOnly(m_readOnly);
        connect(pending, &PropertyRow::heightChanged, this, &PropertiesWidget::relayout);
        m_rows << pending;
        pending->show();
        relayout();
    }
    pending->focusKey();
}

bool PropertiesWidget::focus(bool bottom)
{
    if (!shouldShow() || isInvalid())
        return false;
    if (m_collapsed || !bottom) {
        m_heading->setFocus(Qt::OtherFocusReason);
        return true;
    }
    if (m_addButton->isVisible())
        m_addButton->setFocus(Qt::OtherFocusReason);
    else
        focusPropertyAt(-1);
    return true;
}

void PropertiesWidget::focusPropertyAt(int index)
{
    const int n = int(m_rows.size());
    const int i = index < 0 ? n + index : index;
    if (i >= 0 && i < n) {
        m_rows[i]->focusProperty();
        return;
    }
    if (m_addButton->isVisible())
        m_addButton->setFocus(Qt::OtherFocusReason);
    else if (i >= n)
        emit focusAfterRequested();
}

void PropertiesWidget::focusProperty(const QString& key)
{
    if (PropertyRow* r = row(key))
        r->focusProperty();
}

void PropertiesWidget::focusKey(const QString& key)
{
    if (PropertyRow* r = row(key))
        r->focusKey();
}

void PropertiesWidget::focusValue(const QString& key, const QString& where)
{
    if (PropertyRow* r = row(key))
        r->focusValue(where);
}

void PropertiesWidget::removeProperties(QList<PropertyRow*> rows, bool refocus)
{
    PropertyRow* focused = focusedRow();
    int index = focused ? indexOf(focused) : -1;
    int before = 0;
    QStringList keys;
    for (PropertyRow* r : std::as_const(rows)) {
        if (!r || !m_rows.contains(r))
            continue;
        if (indexOf(r) <= index)
            ++before;
        if (!r->key().isEmpty())
            keys << r->key();
    }
    index -= before;
    // rows without a key are not in the note: drop them here
    for (PropertyRow* r : std::as_const(rows)) {
        if (r && m_rows.contains(r) && r->key().isEmpty()) {
            m_rows.removeAll(r);
            r->hide();
            r->deleteLater();
        }
    }
    if (!keys.isEmpty())
        edit([&](props::FrontmatterEditor& ed) {
            for (const QString& k : std::as_const(keys))
                ed.remove(k);
        });
    relayout();
    if (!refocus)
        return;
    if (!m_rows.isEmpty())
        focusPropertyAt(std::clamp(index, 0, int(m_rows.size()) - 1));
    else
        emit focusAfterRequested();
}

void PropertiesWidget::sortProperties(bool reverse)
{
    QStringList keys = this->keys();
    keys.removeAll(QString());
    std::sort(keys.begin(), keys.end(), [reverse](const QString& a, const QString& b) {
        const int c = QString::localeAwareCompare(a, b);
        return reverse ? c > 0 : c < 0;
    });
    edit([&](props::FrontmatterEditor& ed) { ed.reorder(keys); });
}

void PropertiesWidget::clearProperties()
{
    edit([](props::FrontmatterEditor& ed) { ed.clear(); });
}

void PropertiesWidget::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    m_heading->update();
    relayout();
}

void PropertiesWidget::selectAll()
{
    for (PropertyRow* r : std::as_const(m_rows))
        r->setSelected(true);
}

void PropertiesWidget::clearSelection()
{
    for (PropertyRow* r : std::as_const(m_rows))
        r->setSelected(false);
}

QList<PropertyRow*> PropertiesWidget::selectedRows() const
{
    QList<PropertyRow*> out;
    for (PropertyRow* r : m_rows)
        if (r->isSelected())
            out << r;
    return out;
}

void PropertiesWidget::copyProperties(const QList<PropertyRow*>& rows, bool cut)
{
    QList<QPair<QString, QVariant>> entries;
    QJsonObject json;
    for (PropertyRow* r : rows) {
        if (!r || r->key().isEmpty())
            continue;
        entries.append({r->key(), r->entry().value});
        json.insert(r->key(), QJsonValue::fromVariant(r->entry().value));
    }
    if (entries.isEmpty())
        return;
    auto* mime = new QMimeData;
    mime->setText(props::yamlDocument(entries) + u'\n');
    mime->setData(QString::fromLatin1(kPropertiesMime), QJsonDocument(json).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
    if (cut)
        removeProperties(rows);
}

bool PropertiesWidget::pasteProperties()
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(QString::fromLatin1(kPropertiesMime)) || m_readOnly)
        return false;
    const QJsonObject o = QJsonDocument::fromJson(mime->data(QString::fromLatin1(kPropertiesMime))).object();
    if (o.isEmpty())
        return false;
    return edit([&](props::FrontmatterEditor& ed) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            QVariant v = it.value().toVariant();
            if (v.typeId() == QMetaType::Nullptr)
                v = QVariant();
            ed.merge(it.key(), v);
        }
    });
}

void PropertiesWidget::populateHeadingMenu(QMenu* menu)
{
    if (m_readOnly)
        return;
    menu->addAction(tr("Add file property"), this, [this] { addProperty(); });
    QMenu* sort = menu->addMenu(menuIcon(QStringLiteral("sort-asc")), tr("Sort"));
    sort->addAction(tr("Name (A to Z)"), this, [this] { sortProperties(false); });
    sort->addAction(tr("Name (Z to A)"), this, [this] { sortProperties(true); });
    menu->addSeparator();
    QAction* clear = menu->addAction(tr("Clear file properties"), this, [this] { clearProperties(); });
    clear->setProperty("ocWarning", true);
}

bool PropertiesWidget::handleKey(QKeyEvent* e, PropertyRow* from)
{
    const int k = e->key();
    const QString t = e->text();
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool alt = e->modifiers() & Qt::AltModifier;
    const bool mod = modPressed(e);
    PropertyRow* focused = from ? from : focusedRow();
    const int index = focused ? indexOf(focused) : -1;
    if (t == u"o" && !mod && !alt) {
        addProperty();
        return true;
    }
    if (k == Qt::Key_Home || (k == Qt::Key_Down && alt)) {
        emit focusAfterRequested();
        return true;
    }
    if ((k == Qt::Key_Up || k == Qt::Key_Down) && shift && !mod) {
        if (!focused)
            return true;
        PropertyRow* next = m_rows.value(index + (k == Qt::Key_Down ? 1 : -1));
        if (next) {
            const bool both = focused->isSelected() && next->isSelected();
            if (both) {
                focused->setSelected(false);
            } else {
                focused->setSelected(true);
                next->setSelected(true);
            }
            next->focusProperty();
        }
        return true;
    }
    if ((k == Qt::Key_Up || (t == u"k" && !mod) || k == Qt::Key_Backtab) && !mod) {
        clearSelection();
        if (PropertyRow* prev = m_rows.value(index - 1))
            prev->focusProperty();
        else
            m_heading->setFocus(Qt::OtherFocusReason);
        return true;
    }
    if ((k == Qt::Key_Down || (t == u"j" && !mod) || (k == Qt::Key_Tab && !shift)) && !mod) {
        clearSelection();
        if (PropertyRow* next = m_rows.value(index + 1))
            next->focusProperty();
        else if (m_addButton->isVisible())
            m_addButton->setFocus(Qt::OtherFocusReason);
        else
            emit focusAfterRequested();
        return true;
    }
    if (k == Qt::Key_A && mod && !shift) {
        selectAll();
        return true;
    }
    if (k == Qt::Key_Z && mod) {
        if (shift)
            emit redoRequested();
        else
            emit undoRequested();
        return true;
    }
    if (k == Qt::Key_Y && mod) {
        emit redoRequested();
        return true;
    }
    if (k == Qt::Key_Escape && !selectedRows().isEmpty()) {
        clearSelection();
        return true;
    }
    if (k == Qt::Key_Delete && !m_readOnly) {
        QList<PropertyRow*> rows = selectedRows();
        if (rows.isEmpty() && focused)
            rows << focused;
        removeProperties(rows);
        clearSelection();
        return true;
    }
    if (mod && (k == Qt::Key_C || k == Qt::Key_X)) {
        QList<PropertyRow*> rows = selectedRows();
        if (rows.isEmpty() && focused)
            rows << focused;
        copyProperties(rows, k == Qt::Key_X && !m_readOnly);
        return true;
    }
    if (mod && k == Qt::Key_V) {
        pasteProperties();
        return true;
    }
    return false;
}

} // namespace oc
