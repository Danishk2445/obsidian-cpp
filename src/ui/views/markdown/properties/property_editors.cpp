#include "ui/views/markdown/properties/property_editors.h"

#include "core/properties/property_types.h"
#include "core/properties/property_yaml.h"
#include "markdown/inline_rules.h"
#include "render/theme/icons.h"
#include "ui/views/markdown/properties/properties_style.h"
#include "ui/views/markdown/properties/property_suggest.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

#include <cmath>

namespace oc {

namespace {

const QString kEmpty = QObject::tr("Empty");

void drawIcon(QPainter& p, const QString& name, const QPointF& center, int size, const QColor& color, double stroke, qreal dpr)
{
    const QPixmap pm = IconProvider::instance().pixmap(name, size, color, stroke, dpr);
    p.drawPixmap(QPointF(center.x() - size / 2.0, center.y() - size / 2.0), pm);
}

// Line box of the metadata inputs: the editor's line height (--line-height-normal) at the input size.
int inputLineHeight()
{
    const propui::Style& s = propui::style();
    const double lh = currentTheme()->number(QStringLiteral("--line-height-normal"), 1.5);
    return int(std::round(s.inputFont.pixelSize() > 0 ? s.inputFont.pixelSize() * lh : QFontMetricsF(s.inputFont).height() * 1.25));
}

void styleFlatInput(QWidget* w)
{
    const propui::Style& s = propui::style();
    w->setFont(s.inputFont);
    QPalette pal = w->palette();
    pal.setColor(QPalette::Text, s.inputText);
    pal.setColor(QPalette::PlaceholderText, s.placeholder);
    pal.setColor(QPalette::Base, Qt::transparent);
    w->setPalette(pal);
    w->setAttribute(Qt::WA_TranslucentBackground);
}

QString localeDateFormat()
{
    QString f = QLocale().dateFormat(QLocale::ShortFormat);
    if (!f.contains(QLatin1String("yyyy")))
        f.replace(QLatin1String("yy"), QLatin1String("yyyy"));
    static const QRegularExpression singleM(QStringLiteral("(?<!M)M(?!M)")), singleD(QStringLiteral("(?<!d)d(?!d)"));
    f.replace(singleM, QStringLiteral("MM"));
    f.replace(singleD, QStringLiteral("dd"));
    return f;
}

QString localeTimeFormat()
{
    QString f = QLocale().timeFormat(QLocale::ShortFormat);
    static const QRegularExpression singleH(QStringLiteral("(?<![hH])([hH])(?![hH])"));
    f.replace(singleH, QStringLiteral("\\1\\1"));
    return f;
}

} // namespace

// -------------------------------------------------------------------------------------------------
// helpers
// -------------------------------------------------------------------------------------------------
std::optional<PropertyLink> parsePropertyLink(const QString& v)
{
    if (v.size() > 4 && v.startsWith(QLatin1String("[[")) && v.endsWith(QLatin1String("]]"))) {
        const QString inner = v.mid(2, v.size() - 4);
        if (inner.contains(QLatin1String("[[")) || inner.contains(QLatin1String("]]")))
            return std::nullopt;
        const qsizetype bar = inner.indexOf(u'|');
        PropertyLink l;
        l.internal = true;
        l.href = (bar < 0 ? inner : inner.left(bar)).trimmed();
        l.title = bar < 0 ? l.href : inner.mid(bar + 1);
        if (l.href.isEmpty() && bar < 0)
            return std::nullopt;
        return l;
    }
    static const QRegularExpression md(QStringLiteral("^\\[([^\\]]*)\\]\\((.+)\\)$"));
    if (const auto m = md.match(v); m.hasMatch()) {
        QString url = m.captured(2).trimmed();
        if (url.startsWith(u'<') && url.endsWith(u'>'))
            url = url.mid(1, url.size() - 2).trimmed();
        static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
        PropertyLink l;
        l.title = m.captured(1);
        l.internal = !scheme.match(url).hasMatch();
        l.href = l.internal ? QUrl::fromPercentEncoding(url.toUtf8()) : url;
        return l;
    }
    static const QRegularExpression url(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://\\S+$"));
    if (url.match(v).hasMatch())
        return PropertyLink{false, v, v};
    static const QRegularExpression email(QStringLiteral("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$"));
    if (email.match(v).hasMatch())
        return PropertyLink{false, QStringLiteral("mailto:") + v, v};
    return std::nullopt;
}

bool isValidTagValue(const QString& tag)
{
    const QString t = tag.startsWith(u'#') ? tag : u'#' + tag;
    return md::rules::scanTag(t, 0) == t.size();
}

// -------------------------------------------------------------------------------------------------
// PropertyValueEditor
// -------------------------------------------------------------------------------------------------
PropertyValueEditor::PropertyValueEditor(const PropertyEditorContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setAttribute(Qt::WA_TranslucentBackground);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

bool PropertyValueEditor::hasFocusWithin() const
{
    QWidget* f = QApplication::focusWidget();
    return f && (f == this || isAncestorOf(f));
}

int PropertyValueEditor::heightForWidth(int) const { return propui::style().inputHeight; }

QSize PropertyValueEditor::sizeHint() const { return {200, heightForWidth(width() > 0 ? width() : 400)}; }

PropertyValueEditor* createPropertyEditor(const QString& type, const QVariant& value, const PropertyEditorContext& ctx, QWidget* parent)
{
    const QString t = props::propertyType(type).id;
    if (t == u"multitext")
        return new MultiSelectPropertyEditor(ctx, value, MultiSelectPropertyEditor::Kind::List, parent);
    if (t == u"tags")
        return new MultiSelectPropertyEditor(ctx, value, MultiSelectPropertyEditor::Kind::Tags, parent);
    if (t == u"aliases")
        return new MultiSelectPropertyEditor(ctx, value, MultiSelectPropertyEditor::Kind::Aliases, parent);
    if (t == u"number")
        return new NumberPropertyEditor(ctx, value, parent);
    if (t == u"checkbox")
        return new CheckboxPropertyEditor(ctx, value, parent);
    if (t == u"date")
        return new DatePropertyEditor(ctx, value, false, parent);
    if (t == u"datetime")
        return new DatePropertyEditor(ctx, value, true, parent);
    if (t == u"unknown")
        return new UnknownPropertyEditor(ctx, value, parent);
    return new TextPropertyEditor(ctx, value, parent); // text, file, folder, property
}

// -------------------------------------------------------------------------------------------------
// LongTextEdit
// -------------------------------------------------------------------------------------------------
LongTextEdit::LongTextEdit(QWidget* parent)
    : QTextEdit(parent)
{
    setObjectName(QStringLiteral("ocPropertyText"));
    setFrameShape(QFrame::NoFrame);
    setAcceptRichText(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setTabChangesFocus(true);
    viewport()->setAutoFillBackground(false);
    document()->setDocumentMargin(0);
    styleFlatInput(this);
    setStyleSheet(QStringLiteral("QTextEdit#ocPropertyText { background: transparent; border: none; padding: 0; }"));
    const propui::Style& s = propui::style();
    setViewportMargins(s.padX, s.padY, s.padX, s.padY);
    setPlaceholderText(kEmpty);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { updateGeometry(); });
}

int LongTextEdit::heightForWidth(int w) const
{
    const propui::Style& s = propui::style();
    const int line = inputLineHeight();
    QTextDocument* doc = document()->clone();
    doc->setDefaultFont(font());
    doc->setTextWidth(std::max(10, w - 2 * s.padX));
    QTextCursor c(doc);
    c.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setLineHeight(line, QTextBlockFormat::FixedHeight);
    c.mergeBlockFormat(bf);
    int h = int(std::ceil(doc->size().height()));
    delete doc;
    h = std::max(h, line);
    if (!hasFocus())
        h = std::min(h, 3 * line); // --metadata-input-longtext-lines
    else
        h = std::min(h, 300);
    return std::max(h + 2 * s.padY, s.inputHeight);
}

QSize LongTextEdit::sizeHint() const { return {200, heightForWidth(width() > 0 ? width() : 300)}; }
QSize LongTextEdit::minimumSizeHint() const { return {40, propui::style().inputHeight}; }

void LongTextEdit::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (e->modifiers() & Qt::ShiftModifier) {
            insertPlainText(QStringLiteral("\n"));
            return;
        }
        emit commitRequested();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit revertRequested();
        return;
    }
    QTextEdit::keyPressEvent(e);
}

void LongTextEdit::focusInEvent(QFocusEvent* e)
{
    QTextEdit::focusInEvent(e);
    updateGeometry();
}

void LongTextEdit::focusOutEvent(QFocusEvent* e)
{
    QTextEdit::focusOutEvent(e);
    updateGeometry();
    if (e->reason() != Qt::PopupFocusReason)
        emit focusLost();
}

void LongTextEdit::paintEvent(QPaintEvent* e)
{
    // line-height 1.5 like the editor's lines
    const int line = inputLineHeight();
    QTextCursor c(document());
    c.select(QTextCursor::Document);
    if (c.blockFormat().lineHeight() != line || c.blockFormat().lineHeightType() != QTextBlockFormat::FixedHeight) {
        QTextBlockFormat bf;
        bf.setLineHeight(line, QTextBlockFormat::FixedHeight);
        const bool mod = document()->isModified();
        c.mergeBlockFormat(bf);
        document()->setModified(mod);
    }
    QTextEdit::paintEvent(e);
}

void LongTextEdit::insertFromMimeData(const QMimeData* source)
{
    insertPlainText(source->text()); // plain text only (Obsidian: contenteditable paste as text)
}

// -------------------------------------------------------------------------------------------------
// text
// -------------------------------------------------------------------------------------------------
TextPropertyEditor::TextPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
    , m_input(new LongTextEdit(this))
{
    setMouseTracking(true);
    connect(m_input, &LongTextEdit::commitRequested, this, [this] {
        commitText(m_input->toPlainText());
        emit blurRequested();
    });
    connect(m_input, &LongTextEdit::revertRequested, this, [this] {
        setValue(m_value);
        emit blurRequested();
    });
    connect(m_input, &LongTextEdit::focusLost, this, [this] {
        if (m_saving || m_readOnly)
            return;
        QString t = m_input->toPlainText();
        while (!t.isEmpty() && t.back().isSpace())
            t.chop(1);
        commitText(t);
    });
    connect(m_input, &QTextEdit::textChanged, this, [this] { emit heightChanged(); });
    PropertySuggest::Source src{m_ctx.session, m_ctx.sourcePath, [this] { return m_ctx.key; }, {}};
    m_suggest = new PropertySuggest(m_input, PropertySuggest::Mode::Values, src, this);
    connect(m_suggest, &PropertySuggest::accepted, this, [this](const QString& text, bool commit, int cursor) {
        if (commit) {
            commitText(text);
            emit blurRequested();
            return;
        }
        m_saving = true;
        m_input->setPlainText(text);
        QTextCursor c = m_input->textCursor();
        c.setPosition(std::clamp(cursor, 0, int(text.size())));
        m_input->setTextCursor(c);
        m_saving = false;
        m_suggest->refresh();
    });
    setValue(value);
}

void TextPropertyEditor::setValue(const QVariant& value)
{
    m_saving = true;
    m_value = value.typeId() == QMetaType::QString ? value.toString() : props::propertyValueText(value);
    if (m_input->toPlainText() != m_value)
        m_input->setPlainText(m_value);
    m_saving = false;
    render();
}

void TextPropertyEditor::commitText(const QString& text)
{
    const QString old = m_value;
    setValue(text);
    if (text != old || text.isEmpty())
        emit changed(text);
}

void TextPropertyEditor::render()
{
    m_link = parsePropertyLink(m_value);
    m_linkShown = m_link.has_value() && !m_input->hasFocus();
    m_input->setVisible(!m_linkShown);
    updateGeometry();
    update();
    emit heightChanged();
}

void TextPropertyEditor::setReadOnly(bool ro)
{
    PropertyValueEditor::setReadOnly(ro);
    m_input->setReadOnly(ro);
    m_input->setTextInteractionFlags(ro ? Qt::TextSelectableByMouse : Qt::TextEditorInteraction);
    m_suggest->setEnabled(!ro);
}

void TextPropertyEditor::focusEditor(const QString& where)
{
    m_linkShown = false;
    m_input->show();
    m_input->setFocus(Qt::OtherFocusReason);
    QTextCursor c = m_input->textCursor();
    if (where == u"start")
        c.movePosition(QTextCursor::Start);
    else if (where == u"end")
        c.movePosition(QTextCursor::End);
    else
        c.select(QTextCursor::Document);
    m_input->setTextCursor(c);
    updateGeometry();
    emit heightChanged();
}

int TextPropertyEditor::heightForWidth(int w) const
{
    if (m_linkShown)
        return std::max(propui::style().inputHeight, inputLineHeight() + 2 * propui::style().padY);
    return m_input->heightForWidth(w);
}

void TextPropertyEditor::resizeEvent(QResizeEvent* e)
{
    PropertyValueEditor::resizeEvent(e);
    m_input->setGeometry(rect());
}

QRect TextPropertyEditor::linkRect() const
{
    if (!m_link)
        return {};
    const propui::Style& s = propui::style();
    QFontMetrics fm(s.inputFont);
    const int avail = width() - 2 * s.padX - s.iconXs - 8;
    const int w = std::min(fm.horizontalAdvance(m_link->title), std::max(10, avail));
    const int h = inputLineHeight();
    return QRect(s.padX, (height() - h) / 2, w, h);
}

void TextPropertyEditor::paintEvent(QPaintEvent*)
{
    if (!m_linkShown || !m_link)
        return;
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r = linkRect();
    QFont f = s.inputFont;
    f.setUnderline(s.linkUnderline);
    p.setFont(f);
    QColor c = m_hoverLink ? s.linkHover : s.link;
    if (m_link->internal && m_ctx.isResolved && !m_ctx.isResolved(m_link->href)) {
        c = s.linkUnresolved;
        if (!m_hoverLink)
            c.setAlphaF(c.alphaF() * s.linkUnresolvedOpacity);
    }
    p.setPen(c);
    const QString shown = QFontMetrics(f).elidedText(m_link->title, Qt::ElideRight, r.width());
    p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, shown);
    // .metadata-link-flair: a pencil on hover (not over the link itself)
    if (m_hover && !m_hoverLink && !m_readOnly) {
        QColor ic = s.iconColor;
        ic.setAlphaF(ic.alphaF() * currentTheme()->number(QStringLiteral("--icon-opacity"), 0.85));
        drawIcon(p, QStringLiteral("pencil"), QPointF(width() - s.padX - s.iconXs / 2.0, height() / 2.0), s.iconXs, ic, 2, devicePixelRatioF());
    }
}

void TextPropertyEditor::mousePressEvent(QMouseEvent* e)
{
    if (!m_linkShown || !m_link || e->button() == Qt::RightButton) {
        PropertyValueEditor::mousePressEvent(e);
        return;
    }
    if (linkRect().contains(e->pos()) && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
        const bool newLeaf = e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ControlModifier);
        if (m_ctx.openLink)
            m_ctx.openLink(m_link->href, m_link->internal ? 0 : 3, newLeaf);
        return;
    }
    if (!m_readOnly)
        focusEditor(QStringLiteral("end"));
}

void TextPropertyEditor::mouseMoveEvent(QMouseEvent* e)
{
    const bool wasHover = m_hover, wasLink = m_hoverLink;
    m_hover = true;
    m_hoverLink = m_linkShown && linkRect().contains(e->pos());
    setCursor(m_hoverLink ? Qt::PointingHandCursor : (m_linkShown ? Qt::IBeamCursor : Qt::IBeamCursor));
    if (m_hoverLink != wasLink && m_link && m_link->internal && m_ctx.hoverLink) {
        if (m_hoverLink)
            m_ctx.hoverLink(m_link->href, QRect(mapToGlobal(linkRect().topLeft()), linkRect().size()), this);
        else
            m_ctx.hoverLink(QString(), {}, this);
    }
    if (wasHover != m_hover || wasLink != m_hoverLink)
        update();
    PropertyValueEditor::mouseMoveEvent(e);
}

void TextPropertyEditor::leaveEvent(QEvent* e)
{
    if (m_hoverLink && m_ctx.hoverLink)
        m_ctx.hoverLink(QString(), {}, this);
    m_hover = m_hoverLink = false;
    update();
    PropertyValueEditor::leaveEvent(e);
}

// -------------------------------------------------------------------------------------------------
// number
// -------------------------------------------------------------------------------------------------
NumberPropertyEditor::NumberPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
    , m_input(new QLineEdit(this))
{
    m_input->setObjectName(QStringLiteral("ocPropertyNumber"));
    m_input->setProperty("ocFlat", true);
    styleFlatInput(m_input);
    m_input->setStyleSheet(QStringLiteral("QLineEdit#ocPropertyNumber { background: transparent; border: none; padding: 0; }"));
    const propui::Style& s = propui::style();
    m_input->setTextMargins(s.padX, 0, s.padX, 0);
    m_input->setPlaceholderText(kEmpty);
    m_input->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[-+]?[0-9]*[.]?[0-9]*([eE][-+]?[0-9]*)?")), m_input));
    m_input->installEventFilter(this);
    setValue(value);
}

void NumberPropertyEditor::setValue(const QVariant& value)
{
    m_value = value;
    if (props::isNullProperty(value)) {
        m_input->clear();
        return;
    }
    const double d = value.toDouble();
    m_input->setText(std::isnan(d) ? QString() : props::propertyValueText(value));
}

void NumberPropertyEditor::setReadOnly(bool ro)
{
    PropertyValueEditor::setReadOnly(ro);
    m_input->setReadOnly(ro);
}

void NumberPropertyEditor::focusEditor(const QString& where)
{
    m_input->setFocus(Qt::OtherFocusReason);
    if (where == u"start")
        m_input->setCursorPosition(0);
    else if (where == u"end")
        m_input->end(false);
    else
        m_input->selectAll();
}

bool NumberPropertyEditor::commit()
{
    const QString t = m_input->text().trimmed();
    if (t.isEmpty()) {
        if (!props::isNullProperty(m_value)) {
            m_value = QVariant();
            emit changed(QVariant());
        }
        return true;
    }
    bool ok = false;
    const double d = t.toDouble(&ok);
    if (!ok || !std::isfinite(d)) {
        if (m_ctx.notice)
            m_ctx.notice(tr("Invalid number"));
        QToolTip::showText(m_input->mapToGlobal(QPoint(0, m_input->height())), tr("Invalid number"), m_input);
        return false;
    }
    if (props::isNullProperty(m_value) || m_value.toDouble() != d) {
        m_value = d;
        emit changed(d);
    }
    m_input->setText(props::propertyValueText(d));
    return true;
}

bool NumberPropertyEditor::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_input && !m_readOnly) {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (commit())
                    emit blurRequested();
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                setValue(m_value);
                emit blurRequested();
                return true;
            }
        } else if (ev->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(ev)->reason() != Qt::PopupFocusReason) {
            if (!commit())
                setValue(m_value);
        }
    }
    return PropertyValueEditor::eventFilter(obj, ev);
}

void NumberPropertyEditor::resizeEvent(QResizeEvent* e)
{
    PropertyValueEditor::resizeEvent(e);
    m_input->setGeometry(0, 0, width(), propui::style().inputHeight);
}

// -------------------------------------------------------------------------------------------------
// checkbox
// -------------------------------------------------------------------------------------------------
CheckboxPropertyEditor::CheckboxPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setValue(value);
}

void CheckboxPropertyEditor::setValue(const QVariant& value)
{
    m_indeterminate = props::isNullProperty(value);
    m_checked = !m_indeterminate && value.toBool();
    update();
}

void CheckboxPropertyEditor::focusEditor(const QString&) { setFocus(Qt::OtherFocusReason); }

void CheckboxPropertyEditor::toggle()
{
    if (m_readOnly)
        return;
    m_checked = !m_checked;
    m_indeterminate = false;
    update();
    emit changed(m_checked);
}

QRect CheckboxPropertyEditor::boxRect() const
{
    const propui::Style& s = propui::style();
    return QRect(s.padX, (std::max(height(), s.inputHeight) - s.checkboxSize) / 2, s.checkboxSize, s.checkboxSize);
}

void CheckboxPropertyEditor::paintEvent(QPaintEvent*)
{
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(boxRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (hasFocus()) {
        p.setPen(QPen(s.ringFocus, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(-1.5, -1.5, 1.5, 1.5), s.checkboxRadius + 1.5, s.checkboxRadius + 1.5);
    }
    if (m_checked) {
        const QColor fill = m_hover ? s.checkboxFillHover : s.checkboxFill;
        p.setPen(QPen(fill, 1));
        p.setBrush(fill);
        p.drawRoundedRect(box, s.checkboxRadius, s.checkboxRadius);
        // the check mark (a 65% mask of a 12x8 tick)
        const QRectF b = box;
        QPainterPath tick;
        tick.moveTo(b.left() + b.width() * 0.27, b.top() + b.height() * 0.52);
        tick.lineTo(b.left() + b.width() * 0.43, b.top() + b.height() * 0.68);
        tick.lineTo(b.left() + b.width() * 0.74, b.top() + b.height() * 0.35);
        p.setPen(QPen(s.checkboxMarker, std::max(1.6, b.width() * 0.12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(tick);
        return;
    }
    p.setPen(QPen(m_hover || hasFocus() ? s.checkboxBorderHover : s.checkboxBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box, s.checkboxRadius, s.checkboxRadius);
    if (m_indeterminate) {
        const double w = box.width() - 6;
        p.setPen(Qt::NoPen);
        p.setBrush(s.text);
        p.drawRoundedRect(QRectF(box.center().x() - w / 2, box.top() + box.height() / 2 - 1.5, w, 2), 1, 1);
    }
}

void CheckboxPropertyEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && boxRect().adjusted(-2, -2, 2, 2).contains(e->pos())) {
        setFocus(Qt::MouseFocusReason);
        toggle();
        return;
    }
    PropertyValueEditor::mousePressEvent(e);
}

void CheckboxPropertyEditor::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Space:
        toggle();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
        emit blurRequested();
        return;
    default:
        PropertyValueEditor::keyPressEvent(e);
    }
}

void CheckboxPropertyEditor::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    PropertyValueEditor::enterEvent(e);
}

void CheckboxPropertyEditor::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    PropertyValueEditor::leaveEvent(e);
}

// -------------------------------------------------------------------------------------------------
// date / date & time
// -------------------------------------------------------------------------------------------------
DateInput::DateInput(bool withTime, QWidget* parent)
    : QDateTimeEdit(parent)
    , m_withTime(withTime)
{
    setObjectName(QStringLiteral("ocPropertyDate"));
    setCalendarPopup(true);
    setFrame(false);
    setButtonSymbols(QAbstractSpinBox::NoButtons);
    setDisplayFormat(withTime ? localeDateFormat() + u' ' + localeTimeFormat() : localeDateFormat());
    setMinimumDate(QDate(1, 1, 1));
    setMaximumDate(QDate(9999, 12, 31));
    styleFlatInput(this);
    const propui::Style& s = propui::style();
    setStyleSheet(QStringLiteral("QDateTimeEdit#ocPropertyDate { background: transparent; border: none; padding: 0 0 0 %1px; }"
                                 "QDateTimeEdit#ocPropertyDate::drop-down { width: 0; border: none; }")
                      .arg(s.padX));
    setDateTime(QDateTime(QDate::currentDate(), QTime(0, 0)));
    connect(this, &QDateTimeEdit::dateTimeChanged, this, [this] {
        m_dirty = true;
        m_empty = false;
        update();
    });
}

void DateInput::setEmpty(bool empty)
{
    m_empty = empty;
    update();
}

void DateInput::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        emit commitRequested();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit revertRequested();
        return;
    }
    if ((e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) && e->modifiers() == Qt::NoModifier
        && (lineEdit()->selectedText().size() >= lineEdit()->text().size() - 1 || m_empty)) {
        m_empty = true; // cleared (like emptying a date input)
        m_dirty = true;
        update();
        return;
    }
    if (m_empty && e->text().size() == 1 && e->text()[0].isDigit())
        m_empty = false;
    QDateTimeEdit::keyPressEvent(e);
}

void DateInput::focusOutEvent(QFocusEvent* e)
{
    QDateTimeEdit::focusOutEvent(e);
    if (e->reason() != Qt::PopupFocusReason)
        emit focusLost();
}

void DateInput::paintEvent(QPaintEvent* e)
{
    if (!m_empty) {
        QDateTimeEdit::paintEvent(e);
        return;
    }
    // empty: the format as a placeholder ("mm/dd/yyyy")
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setFont(s.inputFont);
    p.setPen(hasFocus() ? s.inputText : s.placeholder);
    p.drawText(rect().adjusted(s.padX, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, displayFormat().toLower());
}

DatePropertyEditor::DatePropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, bool withTime, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
    , m_withTime(withTime)
    , m_input(new DateInput(withTime, this))
{
    connect(m_input, &DateInput::commitRequested, this, [this] {
        commit();
        emit blurRequested();
    });
    connect(m_input, &DateInput::revertRequested, this, [this] {
        setValue(m_value);
        emit blurRequested();
    });
    connect(m_input, &DateInput::focusLost, this, [this] { commit(); });
    setValue(value);
}

void DatePropertyEditor::setValue(const QVariant& value)
{
    m_value = props::isNullProperty(value) ? QString() : props::propertyValueText(value);
    const QSignalBlocker block(m_input);
    if (m_withTime) {
        const auto dt = props::parseLenientDateTime(m_value);
        m_input->setEmpty(!dt);
        if (dt)
            m_input->setDateTime(*dt);
    } else {
        const auto d = props::parseLenientDate(m_value);
        m_input->setEmpty(!d);
        if (d)
            m_input->setDateTime(QDateTime(*d, QTime(0, 0)));
    }
    m_input->setDirty(false);
}

QString DatePropertyEditor::formatted() const
{
    if (m_input->isEmpty())
        return QString();
    return m_withTime ? m_input->dateTime().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"))
                      : m_input->date().toString(QStringLiteral("yyyy-MM-dd"));
}

void DatePropertyEditor::commit()
{
    if (!m_input->isDirty() || m_readOnly)
        return;
    m_input->setDirty(false);
    const QString v = formatted();
    if (v == m_value)
        return;
    m_value = v;
    emit changed(v);
}

void DatePropertyEditor::focusEditor(const QString&)
{
    m_input->setFocus(Qt::OtherFocusReason);
}

void DatePropertyEditor::setReadOnly(bool ro)
{
    PropertyValueEditor::setReadOnly(ro);
    m_input->setReadOnly(ro);
}

void DatePropertyEditor::resizeEvent(QResizeEvent* e)
{
    PropertyValueEditor::resizeEvent(e);
    QFontMetrics fm(propui::style().inputFont);
    const int w = std::min(width(), fm.horizontalAdvance(m_input->displayFormat() + QStringLiteral("0000")) + 2 * propui::style().padX);
    m_input->setGeometry(0, 0, w, propui::style().inputHeight);
}

// -------------------------------------------------------------------------------------------------
// pills
// -------------------------------------------------------------------------------------------------
namespace {
int pillLineHeight(bool tag)
{
    const propui::Style& s = propui::style();
    const double px = s.inputFont.pixelSize() > 0 ? s.inputFont.pixelSize() : 14;
    if (tag)
        return int(std::round(px + 2 * s.tagPadY)); // line-height: 1 + --tag-padding-y
    return int(std::round(px * currentTheme()->number(QStringLiteral("--line-height-tight"), 1.3)));
}
} // namespace

PropertyPill::PropertyPill(MultiSelectPropertyEditor* owner, int index, const QString& value)
    : QWidget(owner)
    , m_owner(owner)
    , m_index(index)
    , m_value(value)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
    if (owner->kind() == MultiSelectPropertyEditor::Kind::List)
        m_link = parsePropertyLink(value);
    if (owner->kind() == MultiSelectPropertyEditor::Kind::Tags)
        m_invalid = !isValidTagValue(value);
    if (owner->kind() == MultiSelectPropertyEditor::Kind::Aliases)
        m_invalid = value.trimmed().isEmpty();
    if (m_invalid && owner->kind() == MultiSelectPropertyEditor::Kind::Tags)
        setToolTip(tr("Invalid tag name"));
}

QString PropertyPill::displayText() const
{
    if (m_owner->kind() == MultiSelectPropertyEditor::Kind::Tags)
        return m_value.startsWith(u'#') ? m_value.mid(1) : m_value;
    if (m_link)
        return m_link->title;
    return m_value;
}

QSize PropertyPill::sizeHint() const
{
    const propui::Style& s = propui::style();
    const bool tag = m_owner->kind() == MultiSelectPropertyEditor::Kind::Tags;
    QFontMetrics fm(s.inputFont);
    int w = fm.horizontalAdvance(displayText()) + 2 + s.iconXs; // content, gap (--size-2-1), remove button
    if (tag)
        w += int(std::round(s.tagPadX)) + std::min(6, int(std::round(s.tagPadX)));
    return {w, pillLineHeight(tag)};
}

QRect PropertyPill::removeRect() const
{
    const propui::Style& s = propui::style();
    const bool tag = m_owner->kind() == MultiSelectPropertyEditor::Kind::Tags;
    const int right = tag ? std::min(6, int(std::round(s.tagPadX))) : 0;
    return QRect(width() - right - s.iconXs, (height() - s.iconXs) / 2, s.iconXs, s.iconXs);
}

void PropertyPill::paintEvent(QPaintEvent*)
{
    const propui::Style& s = propui::style();
    const bool tag = m_owner->kind() == MultiSelectPropertyEditor::Kind::Tags;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = tag ? std::min(s.tagRadius, r.height() / 2) : 4;
    if (tag && !m_invalid) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_hover ? s.tagBgHover : s.tagBg);
        p.drawRoundedRect(r, radius, radius);
    }
    if (hasFocus()) {
        p.setPen(QPen(s.ringFocus, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    QFont f = s.inputFont;
    QColor c = s.inputText;
    if (tag) {
        c = s.tagText;
    } else if (m_link) {
        c = m_hover && !m_hoverRemove ? s.linkHover : s.link;
        f.setUnderline(s.linkUnderline);
        if (m_link->internal && m_owner->m_ctx.isResolved && !m_owner->m_ctx.isResolved(m_link->href)) {
            c = s.linkUnresolved;
            c.setAlphaF(c.alphaF() * s.linkUnresolvedOpacity);
        }
    }
    if (m_invalid) {
        c = s.error;
        f.setStrikeOut(true);
    }
    p.setFont(f);
    p.setPen(c);
    const int left = tag ? int(std::round(s.tagPadX)) : 0;
    const QRect textRect(left, 0, removeRect().left() - 2 - left, height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(displayText(), Qt::ElideRight, textRect.width()));
    if (!m_owner->isReadOnly()) {
        const QColor rc = m_invalid ? s.error : (tag ? (m_hoverRemove ? s.tagText : s.tagText) : (m_hoverRemove ? s.pillRemoveHover : s.pillRemove));
        drawIcon(p, QStringLiteral("x"), QRectF(removeRect()).center(), s.iconXs, rc, 2, devicePixelRatioF());
    }
}

void PropertyPill::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && !m_owner->isReadOnly() && removeRect().adjusted(-2, -2, 2, 2).contains(e->pos())) {
        const int i = m_index;
        MultiSelectPropertyEditor* owner = m_owner;
        QTimer::singleShot(0, owner, [owner, i] {
            owner->removeValue(i);
            owner->input()->setFocus(Qt::OtherFocusReason);
        });
        return;
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
        setFocus(Qt::MouseFocusReason);
    QWidget::mousePressEvent(e);
}

void PropertyPill::mouseReleaseEvent(QMouseEvent* e)
{
    if ((e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) && !removeRect().contains(e->pos()) && rect().contains(e->pos()))
        m_owner->activatePill(m_index, e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ControlModifier));
    QWidget::mouseReleaseEvent(e);
}

void PropertyPill::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (!m_owner->isReadOnly()) {
        m_owner->editValue(m_index);
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void PropertyPill::mouseMoveEvent(QMouseEvent* e)
{
    const bool overRemove = removeRect().adjusted(-2, -2, 2, 2).contains(e->pos());
    if (overRemove != m_hoverRemove) {
        m_hoverRemove = overRemove;
        update();
    }
    const bool clickable = m_link || m_owner->kind() == MultiSelectPropertyEditor::Kind::Tags;
    setCursor(overRemove || clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void PropertyPill::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    if (m_link && m_link->internal && m_owner->m_ctx.hoverLink)
        m_owner->m_ctx.hoverLink(m_link->href, QRect(mapToGlobal(QPoint(0, 0)), size()), this);
    QWidget::enterEvent(e);
}

void PropertyPill::leaveEvent(QEvent* e)
{
    m_hover = m_hoverRemove = false;
    update();
    if (m_link && m_link->internal && m_owner->m_ctx.hoverLink)
        m_owner->m_ctx.hoverLink(QString(), {}, this);
    QWidget::leaveEvent(e);
}

void PropertyPill::keyPressEvent(QKeyEvent* e)
{
    if (e->matches(QKeySequence::Copy)) {
        QGuiApplication::clipboard()->setText(m_value);
        return;
    }
    const bool ro = m_owner->isReadOnly();
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!ro)
            m_owner->editValue(m_index);
        return;
    case Qt::Key_Escape:
        m_owner->input()->setFocus(Qt::OtherFocusReason);
        return;
    case Qt::Key_Backspace:
        if (!ro)
            m_owner->removeValue(m_index, true);
        return;
    case Qt::Key_Up:
        m_owner->focusPill(0);
        return;
    case Qt::Key_Down:
        m_owner->input()->setFocus(Qt::OtherFocusReason);
        return;
    case Qt::Key_Left:
        m_owner->focusPill(m_index - 1);
        return;
    case Qt::Key_Right:
        m_owner->focusPill(m_index + 1);
        return;
    default:
        QWidget::keyPressEvent(e);
    }
}

void PropertyPill::contextMenuEvent(QContextMenuEvent* e)
{
    m_owner->showPillMenu(m_index, e->globalPos());
}

// -------------------------------------------------------------------------------------------------
// multi-select
// -------------------------------------------------------------------------------------------------
MultiSelectPropertyEditor::MultiSelectPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, Kind kind, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
    , m_kind(kind)
    , m_input(new QLineEdit(this))
{
    setCursor(Qt::IBeamCursor);
    m_input->setObjectName(QStringLiteral("ocPropertyListInput"));
    m_input->setProperty("ocFlat", true);
    styleFlatInput(m_input);
    m_input->setStyleSheet(QStringLiteral("QLineEdit#ocPropertyListInput { background: transparent; border: none; padding: 0; }"));
    m_input->setFrame(false);
    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, [this] { relayout(); });
    if (kind != Kind::Aliases) {
        PropertySuggest::Source src{m_ctx.session, m_ctx.sourcePath, [this] { return m_ctx.key; },
                                    [this](const QString& v) {
                                        if (m_kind == Kind::Tags)
                                            return findDuplicate(v, m_values) >= 0;
                                        return m_values.contains(v);
                                    }};
        m_suggest = new PropertySuggest(m_input, kind == Kind::Tags ? PropertySuggest::Mode::Tags : PropertySuggest::Mode::Values, src, this);
        connect(m_suggest, &PropertySuggest::accepted, this, [this](const QString& text, bool commit, int cursor) {
            if (commit) {
                if (addValue(text))
                    m_input->clear();
                return;
            }
            m_input->setText(text);
            m_input->setCursorPosition(std::clamp(cursor, 0, int(text.size())));
            m_suggest->refresh();
        });
    }
    setValue(value);
}

QString MultiSelectPropertyEditor::type() const
{
    switch (m_kind) {
    case Kind::Tags: return QStringLiteral("tags");
    case Kind::Aliases: return QStringLiteral("aliases");
    case Kind::List: break;
    }
    return QStringLiteral("multitext");
}

void MultiSelectPropertyEditor::setValue(const QVariant& value)
{
    const QStringList v = props::propertyValueList(value);
    if (v == m_values && m_pills.size() == v.size())
        return;
    m_values = v;
    rebuild();
}

void MultiSelectPropertyEditor::setReadOnly(bool ro)
{
    PropertyValueEditor::setReadOnly(ro);
    m_input->setReadOnly(ro);
    m_input->setVisible(!ro || m_values.isEmpty());
    if (m_suggest)
        m_suggest->setEnabled(!ro);
    for (PropertyPill* p : std::as_const(m_pills))
        p->update();
    relayout();
}

void MultiSelectPropertyEditor::rebuild()
{
    const bool hadFocus = hasFocusWithin() && !m_input->hasFocus();
    qDeleteAll(m_pills);
    m_pills.clear();
    for (int i = 0; i < m_values.size(); ++i) {
        auto* pill = new PropertyPill(this, i, m_values[i]);
        if (m_kind != Kind::List) {
            const int dup = findDuplicate(m_values[i], m_values.mid(0, i));
            if (dup >= 0 && dup < i)
                pill->m_invalid = true;
        }
        pill->show();
        m_pills << pill;
    }
    m_input->setPlaceholderText(m_values.isEmpty() ? kEmpty : QString());
    m_input->setVisible(!m_readOnly || m_values.isEmpty());
    Q_UNUSED(hadFocus);
    relayout();
}

int MultiSelectPropertyEditor::layoutPills(int width, bool apply) const
{
    const propui::Style& s = propui::style();
    const int inner = std::max(20, width - 2 * s.padX);
    QFontMetrics fm(s.inputFont);
    const int lineH = inputLineHeight();
    struct Item {
        QWidget* w;
        QSize size;
    };
    QList<Item> items;
    for (PropertyPill* p : m_pills) {
        if (!p->isVisibleTo(this) && p->index() != m_editIndex)
            continue;
        if (p->index() == m_editIndex && m_editInput) {
            const int w = std::max(fm.horizontalAdvance(m_editInput->text()) + 12, 40);
            items.append({m_editInput, QSize(std::min(w, inner), lineH)});
            continue;
        }
        QSize sz = p->sizeHint();
        sz.setWidth(std::min(sz.width(), inner - s.pillGap - fm.horizontalAdvance(u'0')));
        items.append({p, sz});
    }
    if (m_input->isVisibleTo(this) || !m_readOnly) {
        const QString t = m_input->text().isEmpty() ? m_input->placeholderText() : m_input->text();
        const int w = std::max(fm.horizontalAdvance(t) + 6, fm.horizontalAdvance(u'0'));
        items.append({m_input, QSize(std::min(w, inner), lineH)});
    }
    // flex-wrap with align-items: center
    int y = s.padY, x = 0, rowH = 0;
    QList<QPair<Item, QPoint>> placed;
    QList<int> rowStart{0};
    QList<int> rowHeights;
    for (const Item& it : std::as_const(items)) {
        if (x > 0 && x + it.size.width() > inner) {
            rowHeights << rowH;
            y += rowH + s.pillGap;
            x = 0;
            rowH = 0;
            rowStart << int(placed.size());
        }
        placed.append({it, QPoint(x, y)});
        x += it.size.width() + s.pillGap;
        rowH = std::max(rowH, it.size.height());
    }
    rowHeights << rowH;
    const int total = std::max(s.inputHeight, y + rowH + s.padY);
    if (apply) {
        // single row: center within the input height
        int row = 0;
        for (int i = 0; i < placed.size(); ++i) {
            while (row + 1 < rowStart.size() && i >= rowStart[row + 1])
                ++row;
            const auto& [it, pos] = placed[i];
            int rh = rowHeights.value(row);
            int top = pos.y();
            if (rowHeights.size() == 1)
                top = (total - rh) / 2;
            it.w->setGeometry(s.padX + pos.x(), top + (rh - it.size.height()) / 2, it.size.width(), it.size.height());
        }
    }
    return total;
}

int MultiSelectPropertyEditor::heightForWidth(int w) const { return layoutPills(w, false); }

void MultiSelectPropertyEditor::relayout()
{
    layoutPills(width(), true);
    updateGeometry();
    emit heightChanged();
}

void MultiSelectPropertyEditor::resizeEvent(QResizeEvent* e)
{
    PropertyValueEditor::resizeEvent(e);
    layoutPills(width(), true);
}

void MultiSelectPropertyEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && !m_readOnly) // click on the container: the input
        m_input->setFocus(Qt::MouseFocusReason);
    PropertyValueEditor::mousePressEvent(e);
}

void MultiSelectPropertyEditor::focusEditor(const QString&)
{
    if (m_readOnly && !m_pills.isEmpty()) {
        m_pills.first()->setFocus(Qt::OtherFocusReason);
        return;
    }
    m_input->setFocus(Qt::OtherFocusReason);
}

std::optional<QString> MultiSelectPropertyEditor::createOption(const QString& text)
{
    switch (m_kind) {
    case Kind::List:
        if (text.isEmpty())
            return std::nullopt;
        return text;
    case Kind::Aliases: {
        const QString t = text.trimmed();
        if (t.isEmpty())
            return std::nullopt;
        return t;
    }
    case Kind::Tags: {
        if (text.trimmed().isEmpty())
            return std::nullopt;
        if (!isValidTagValue(text.trimmed())) {
            QToolTip::showText(m_input->mapToGlobal(QPoint(0, m_input->height())), tr("Invalid tag name"), m_input);
            if (m_ctx.notice)
                m_ctx.notice(tr("Invalid tag name"));
            return std::nullopt;
        }
        return text.trimmed();
    }
    }
    return std::nullopt;
}

int MultiSelectPropertyEditor::findDuplicate(const QString& value, const QStringList& in) const
{
    switch (m_kind) {
    case Kind::Tags: {
        auto strip = [](const QString& t) { return t.startsWith(u'#') ? t.mid(1) : t; };
        const QString v = strip(value);
        for (int i = 0; i < in.size(); ++i)
            if (strip(in[i]) == v)
                return i;
        return -1;
    }
    case Kind::Aliases:
        return int(in.indexOf(value));
    case Kind::List:
        break;
    }
    return -1; // lists allow duplicates
}

bool MultiSelectPropertyEditor::addValue(const QString& text)
{
    if (m_readOnly)
        return false;
    const std::optional<QString> opt = createOption(text);
    if (!opt)
        return false;
    const int dup = findDuplicate(*opt, m_values);
    if (dup >= 0) {
        if (PropertyPill* p = m_pills.value(dup)) {
            p->m_hover = true; // .multi-select-duplicate flash
            p->update();
            QPointer<PropertyPill> guard = p;
            QTimer::singleShot(2000, p, [guard] {
                if (guard) {
                    guard->m_hover = false;
                    guard->update();
                }
            });
        }
        return false;
    }
    m_values << *opt;
    rebuild();
    triggerChange();
    return true;
}

void MultiSelectPropertyEditor::removeValue(int index, bool focusPrevious)
{
    if (m_readOnly || index < 0 || index >= m_values.size())
        return;
    m_values.removeAt(index);
    rebuild();
    if (focusPrevious)
        focusPill(index - 1);
    triggerChange();
}

void MultiSelectPropertyEditor::focusPill(int index)
{
    const int i = std::max(0, index);
    if (PropertyPill* p = m_pills.value(i))
        p->setFocus(Qt::OtherFocusReason);
    else
        m_input->setFocus(Qt::OtherFocusReason);
}

void MultiSelectPropertyEditor::editValue(int index)
{
    if (m_readOnly || index < 0 || index >= m_values.size())
        return;
    delete m_editInput;
    m_editIndex = index;
    m_editInput = new QLineEdit(this);
    m_editInput->setObjectName(QStringLiteral("ocPropertyListInput"));
    m_editInput->setProperty("ocFlat", true);
    styleFlatInput(m_editInput);
    m_editInput->setStyleSheet(m_input->styleSheet());
    m_editInput->setText(m_values[index]);
    m_editInput->installEventFilter(this);
    connect(m_editInput, &QLineEdit::textChanged, this, [this] { relayout(); });
    m_pills[index]->hide();
    m_editInput->show();
    relayout();
    m_editInput->setFocus(Qt::OtherFocusReason);
    m_editInput->selectAll();
}

void MultiSelectPropertyEditor::triggerChange() { emit changed(QVariant(m_values)); }

void MultiSelectPropertyEditor::commitInput()
{
    const QString t = m_input->text();
    if (t.isEmpty())
        return;
    if (addValue(t))
        m_input->clear();
}

void MultiSelectPropertyEditor::activatePill(int index, bool newLeaf)
{
    PropertyPill* p = m_pills.value(index);
    if (!p)
        return;
    if (m_kind == Kind::Tags && !p->isInvalid()) {
        if (m_ctx.openTag)
            m_ctx.openTag(u'#' + (p->value().startsWith(u'#') ? p->value().mid(1) : p->value()));
        return;
    }
    if (p->m_link && m_ctx.openLink)
        m_ctx.openLink(p->m_link->href, p->m_link->internal ? 0 : 3, newLeaf);
}

void MultiSelectPropertyEditor::showPillMenu(int index, const QPoint& globalPos)
{
    if (index < 0 || index >= m_values.size())
        return;
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QString value = m_values[index];
    if (!m_readOnly)
        menu->addAction(IconProvider::instance().icon(QStringLiteral("edit-3"), 16), tr("Edit"), this, [this, index] { editValue(index); });
    menu->addAction(IconProvider::instance().icon(QStringLiteral("copy"), 16), tr("Copy"), this, [value] { QGuiApplication::clipboard()->setText(value); });
    if (!m_readOnly) {
        QAction* remove = menu->addAction(IconProvider::instance().icon(QStringLiteral("trash-2"), 16), tr("Remove from list"), this,
                                          [this, index] { removeValue(index); });
        remove->setProperty("ocWarning", true);
    }
    menu->popup(globalPos);
}

bool MultiSelectPropertyEditor::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_input) {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (!m_input->text().isEmpty()) {
                    commitInput();
                    return true;
                }
                break;
            case Qt::Key_Backspace:
                if (m_input->text().isEmpty() && !m_pills.isEmpty()) {
                    m_pills.last()->setFocus(Qt::OtherFocusReason);
                    return true;
                }
                break;
            case Qt::Key_Left:
                if (m_input->text().isEmpty() && m_input->cursorPosition() == 0 && !m_pills.isEmpty()) {
                    focusPill(int(m_pills.size()) - 1);
                    return true;
                }
                break;
            case Qt::Key_Escape:
                emit blurRequested();
                return true;
            default:
                break;
            }
        } else if (ev->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(ev)->reason() != Qt::PopupFocusReason) {
            commitInput();
        }
    } else if (m_editInput && obj == m_editInput) {
        auto finish = [this](bool accept) {
            if (!m_editInput)
                return;
            const int i = m_editIndex;
            const QString text = m_editInput->text();
            QLineEdit* ed = m_editInput;
            m_editInput = nullptr;
            m_editIndex = -1;
            ed->deleteLater();
            bool changedValue = false;
            if (accept && !text.isEmpty()) {
                if (const auto opt = createOption(text)) {
                    QStringList others = m_values;
                    others.removeAt(i);
                    const int dup = findDuplicate(*opt, m_values);
                    if ((dup < 0 || dup == i) && m_values.value(i) != *opt) {
                        m_values[i] = *opt;
                        changedValue = true;
                    }
                }
            }
            rebuild();
            focusPill(i);
            if (changedValue)
                triggerChange();
        };
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !m_editInput->text().isEmpty()) {
                finish(true);
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                finish(false);
                return true;
            }
        } else if (ev->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(ev)->reason() != Qt::PopupFocusReason) {
            QTimer::singleShot(0, this, [finish] { finish(true); });
        }
    }
    return PropertyValueEditor::eventFilter(obj, ev);
}

// -------------------------------------------------------------------------------------------------
// unknown
// -------------------------------------------------------------------------------------------------
UnknownPropertyEditor::UnknownPropertyEditor(const PropertyEditorContext& ctx, const QVariant& value, QWidget* parent)
    : PropertyValueEditor(ctx, parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setValue(value);
}

void UnknownPropertyEditor::setValue(const QVariant& value)
{
    m_text = props::jsonText(value);
    setToolTip(m_text);
    update();
}

void UnknownPropertyEditor::focusEditor(const QString&) { setFocus(Qt::OtherFocusReason); }

void UnknownPropertyEditor::paintEvent(QPaintEvent*)
{
    const propui::Style& s = propui::style();
    QPainter p(this);
    p.setFont(s.inputFont);
    p.setPen(s.warning);
    const QRect r = rect().adjusted(s.padX, 0, -s.padX, 0);
    p.drawText(QRect(r.left(), 0, r.width(), std::max(height(), s.inputHeight)), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(s.inputFont).elidedText(m_text, Qt::ElideRight, r.width()));
}

} // namespace oc
