#include "ui/overlay/modal.h"

#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/widgets/icon_button.h"

#include <QApplication>
#include <QEventLoop>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QVBoxLayout>

#include <cmath>

namespace oc {

namespace {

QColor tok(const char* name, const QColor& fallback = {})
{
    return currentTheme()->color(QString::fromLatin1(name), fallback);
}

double tokPx(const char* name, double fallback)
{
    return currentTheme()->px(QString::fromLatin1(name), -1, fallback);
}

// .modal: --modal-background, --modal-border-width/-color, --modal-radius.
class ModalFrame : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double radius = tokPx("--modal-radius", 12);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, radius, radius);
        p.fillPath(path, tok("--modal-background", tok("--background-primary")));
        QColor border = tok("--modal-border-color");
        if (!border.isValid())
            border = tok("--background-modifier-border-focus");
        p.setPen(QPen(border, tokPx("--modal-border-width", 1)));
        p.drawPath(path);
    }
};

// A paragraph (<p>) laid out with the modal's 1.5 line height; wraps at word boundaries and, like
// .u-break-word, anywhere when a word doesn't fit.
class Paragraph : public QWidget {
public:
    Paragraph(const QString& text, bool warning, QWidget* parent)
        : QWidget(parent)
        , m_text(text)
        , m_warning(warning)
    {
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
    }
    const QString& text() const { return m_text; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return int(std::ceil(layoutLines(nullptr, w))); }
    QSize sizeHint() const override { return {200, heightForWidth(std::max(200, width()))}; }
    QSize minimumSizeHint() const override { return {10, heightForWidth(std::max(200, width()))}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QTextLayout tl;
        layoutLines(&tl, width());
        // p.mod-warning / p.u-muted (property "ocMuted") / plain
        p.setPen(m_warning ? tok("--text-warning") : property("ocMuted").toBool() ? tok("--text-muted") : tok("--text-normal"));
        tl.draw(&p, QPointF(0, 0));
    }

private:
    qreal layoutLines(QTextLayout* out, int width) const
    {
        QTextLayout local;
        QTextLayout& tl = out ? *out : local;
        const QFont font = currentTheme()->interfaceFont(tokPx("--font-ui-medium", 15));
        const qreal lineHeight = std::round(font.pixelSize() > 0 ? font.pixelSize() * 1.5 : QFontMetricsF(font).height() * 1.3);
        tl.setText(m_text);
        tl.setFont(font);
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        tl.setTextOption(opt);
        tl.beginLayout();
        qreal y = 0;
        for (;;) {
            QTextLine line = tl.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(std::max(10, width));
            line.setPosition(QPointF(0, y + (lineHeight - (line.ascent() + line.descent())) / 2));
            y += lineHeight;
        }
        tl.endLayout();
        return y;
    }

    QString m_text;
    bool m_warning;
};

// Obsidian's <button> (.mod-cta / .mod-warning / .mod-destructive): 30 px high, 12 px side padding,
// --button-radius, 13 px text, --input-shadow.
class ModalPushButton : public QPushButton {
public:
    ModalPushButton(const QString& text, Modal::ButtonStyle style, QWidget* parent)
        : QPushButton(text, parent)
        , m_style(style)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFont(currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)));
    }
    Modal::ButtonStyle style() const { return m_style; }
    QSize sizeHint() const override
    {
        const QFontMetricsF fm(font());
        return {int(std::ceil(fm.horizontalAdvance(text()))) + 2 * int(tokPx("--size-4-3", 12)), int(tokPx("--input-height", 30))};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void focusInEvent(QFocusEvent* e) override
    {
        m_focusVisible = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;
        QPushButton::focusInEvent(e);
    }
    void paintEvent(QPaintEvent*) override
    {
        const ThemePtr t = currentTheme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hover = underMouse() || isDown();
        const double radius = tokPx("--button-radius", 5);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath path;
        path.addRoundedRect(r, radius, radius);
        QColor bg, fg = tok("--text-normal");
        switch (m_style) {
        case Modal::ButtonStyle::Normal:
            bg = hover ? tok("--interactive-hover") : tok("--interactive-normal");
            break;
        case Modal::ButtonStyle::Cta:
            bg = hover ? tok("--interactive-accent-hover") : tok("--interactive-accent");
            fg = tok("--text-on-accent", Qt::white);
            break;
        case Modal::ButtonStyle::Warning:
            bg = hover ? tok("--background-modifier-error-hover") : tok("--background-modifier-error");
            fg = tok("--text-on-accent", Qt::white);
            break;
        case Modal::ButtonStyle::Destructive:
            bg = tok("--background-modifier-error");
            bg.setAlphaF(hover ? 0.15 : 0.1);
            fg = tok("--text-error");
            break;
        }
        // --input-shadow: a soft drop shadow plus (dark) a faint top highlight / (light) a hairline.
        QPainterPath shadow;
        shadow.addRoundedRect(r.translated(0, 1), radius, radius);
        p.fillPath(shadow, QColor(0, 0, 0, t->isDark() ? 70 : 22));
        p.fillPath(path, bg);
        if (m_style == Modal::ButtonStyle::Normal) {
            p.setPen(QPen(t->isDark() ? QColor(255, 255, 255, hover ? 40 : 23) : QColor(0, 0, 0, hover ? 43 : 31), 1));
            p.drawPath(path);
        }
        if (hasFocus() && m_focusVisible) {
            p.setPen(QPen(tok("--background-modifier-border-focus"), 3));
            p.drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), radius + 1, radius + 1);
        }
        p.setPen(isEnabled() ? fg : tok("--text-faint"));
        p.setFont(font());
        p.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    Modal::ButtonStyle m_style;
    bool m_focusVisible = false;
};

// input[type=checkbox] inside label.mod-checkbox: --checkbox-size (15 px in a modal), --checkbox-radius,
// 1 px --checkbox-border-color, --checkbox-color fill with a --checkbox-marker-color check when on.
class ModalCheckBox : public QCheckBox {
public:
    ModalCheckBox(const QString& text, QWidget* parent)
        : QCheckBox(text, parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus); // tabindex -1, like Obsidian
        setFont(currentTheme()->interfaceFont(tokPx("--font-ui-medium", 15)));
    }
    QSize sizeHint() const override
    {
        const QFontMetricsF fm(font());
        return {int(std::ceil(box() + gap() + fm.horizontalAdvance(text()))) + 1, int(tokPx("--input-height", 30))};
    }
    QSize minimumSizeHint() const override { return {int(box()) + 1, int(tokPx("--input-height", 30))}; }

protected:
    bool hitButton(const QPoint& pos) const override { return rect().contains(pos); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hover = underMouse();
        const double s = box();
        const QRectF b(0.5, (height() - s) / 2 + 0.5, s - 1, s - 1);
        const double radius = tokPx("--checkbox-radius", 4);
        QPainterPath path;
        path.addRoundedRect(b, radius, radius);
        if (isChecked()) {
            p.fillPath(path, hover ? tok("--checkbox-color-hover") : tok("--checkbox-color"));
            QPen pen(tok("--checkbox-marker-color", tok("--background-primary")), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            const QPointF o = b.topLeft();
            const double k = b.width() / 24.0; // lucide "check": M20 6 9 17l-5-5
            QPainterPath check;
            check.moveTo(o + QPointF(20 * k, 6 * k));
            check.lineTo(o + QPointF(9 * k, 17 * k));
            check.lineTo(o + QPointF(4 * k, 12 * k));
            p.drawPath(check);
        } else {
            p.setPen(QPen(hover ? tok("--checkbox-border-color-hover") : tok("--checkbox-border-color"), 1));
            p.drawPath(path);
        }
        p.setPen(tok("--text-normal"));
        p.setFont(font());
        p.drawText(QRectF(s + gap(), 0, width() - s - gap(), height()), Qt::AlignLeft | Qt::AlignVCenter, text());
    }

private:
    static double box() { return tokPx("--font-ui-medium", 15); }
    static double gap() { return tokPx("--size-4-1", 4); }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Modal
// ---------------------------------------------------------------------------------------------

Modal::Modal(OverlayLayer* layer)
    : QWidget(layer)
    , m_layer(layer)
    , m_width(int(tokPx("--dialog-width", 560)))
{
    setProperty("ocCapturesKeys", true); // global hotkeys don't fire while a modal is open
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(layer->rect());

    m_frame = new ModalFrame(this);
    m_frame->setObjectName(QStringLiteral("ocModal"));
    m_frame->setFocusPolicy(Qt::ClickFocus);
    auto* shadow = new QGraphicsDropShadowEffect(m_frame); // --shadow-l
    shadow->setBlurRadius(60);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, currentTheme()->isDark() ? 90 : 45));
    m_frame->setGraphicsEffect(shadow);

    const int pad = int(tokPx("--size-4-4", 16));
    auto* v = new QVBoxLayout(m_frame);
    v->setContentsMargins(pad, pad, pad, pad);
    v->setSpacing(0);

    // .modal-header > .modal-title (margin-bottom .75em)
    m_title = new QLabel(m_frame);
    QFont tf = currentTheme()->interfaceFont(tokPx("--font-ui-large", 20));
    tf.setWeight(QFont::Weight(int(currentTheme()->number(QStringLiteral("--font-semibold"), 600))));
    m_title->setFont(tf);
    m_title->setWordWrap(true);
    m_title->setMinimumHeight(int(std::ceil(tokPx("--font-ui-large", 20) * 1.3)));
    m_title->hide();
    v->addWidget(m_title);
    v->addSpacing(12);

    // .modal-content (15 px); paragraphs keep their 1em margins inside it
    m_content = new QWidget(m_frame);
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(15);
    v->addWidget(m_content);

    // .modal-button-container (margin-top 1.5em, gap 8 px, right-aligned)
    m_buttonRow = new QWidget(m_frame);
    m_buttonLayout = new QHBoxLayout(m_buttonRow);
    m_buttonLayout->setContentsMargins(0, 22, 0, 0);
    m_buttonLayout->setSpacing(int(tokPx("--size-4-2", 8)));
    m_buttonLayout->addStretch(1);
    v->addWidget(m_buttonRow);

    // .modal-close-button: absolute, top 6 px, 12 px from the right edge, 4 px padding
    m_close = new IconButton(QStringLiteral("x"), tr("Close"), m_frame);
    m_close->setButtonSize({26, 26});
    connect(m_close, &IconButton::clicked, this, [this] { close(-1); });

    layer->installEventFilter(this);
    hide();
}

Modal::~Modal()
{
    if (m_layer && m_open)
        m_layer->setBlocking(this, false);
}

void Modal::setTitle(const QString& title)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    relayout();
}

QString Modal::title() const { return m_title->text(); }

void Modal::addParagraph(const QString& text, bool warning)
{
    auto* p = new Paragraph(text, warning, m_content);
    p->setObjectName(warning ? QStringLiteral("ocModalWarning") : QStringLiteral("ocModalParagraph"));
    // Paragraphs come before any custom content widgets.
    int index = 0;
    for (int i = 0; i < m_contentLayout->count(); ++i)
        if (dynamic_cast<Paragraph*>(m_contentLayout->itemAt(i)->widget()))
            index = i + 1;
    m_contentLayout->insertWidget(index, p);
    m_contentLayout->setContentsMargins(0, 15, 0, 15); // <p> margins (1em)
    relayout();
}

void Modal::setText(const QString& text)
{
    for (int i = m_contentLayout->count() - 1; i >= 0; --i) {
        if (auto* p = dynamic_cast<Paragraph*>(m_contentLayout->itemAt(i)->widget())) {
            m_contentLayout->removeWidget(p);
            delete p;
        }
    }
    const QStringList parts = text.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    for (const QString& part : parts)
        addParagraph(part.trimmed());
    relayout();
}

QStringList Modal::paragraphs() const
{
    QStringList out;
    for (int i = 0; i < m_contentLayout->count(); ++i)
        if (auto* p = dynamic_cast<Paragraph*>(m_contentLayout->itemAt(i)->widget()))
            out << p->text();
    return out;
}

void Modal::addContentWidget(QWidget* widget)
{
    if (!widget)
        return;
    widget->setParent(m_content);
    m_contentLayout->addWidget(widget);
    relayout();
}

int Modal::addButton(const QString& text, ButtonStyle style, bool closes)
{
    auto* b = new ModalPushButton(text, style, m_buttonRow);
    const int index = int(m_buttons.size());
    m_buttons.push_back(b);
    m_closes.push_back(closes);
    m_buttonLayout->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, index] {
        QPointer<Modal> self(this);
        emit buttonClicked(index);
        if (self && m_closes.value(index))
            close(index);
    });
    relayout();
    return index;
}

QPushButton* Modal::button(int index) const { return m_buttons.value(index); }

QPushButton* Modal::createButton(const QString& text, ButtonStyle style, QWidget* parent)
{
    return new ModalPushButton(text, style, parent);
}

void Modal::setDefaultButton(int index) { m_default = index; }

int Modal::defaultButton() const
{
    if (m_default >= 0 && m_default < m_buttons.size())
        return m_default;
    for (qsizetype i = m_buttons.size() - 1; i >= 0; --i) {
        const auto style = static_cast<ModalPushButton*>(m_buttons[i])->style();
        if (style == ButtonStyle::Cta || style == ButtonStyle::Warning)
            return int(i);
    }
    return -1;
}

void Modal::setCheckbox(const QString& text, bool checked)
{
    if (text.isEmpty()) {
        delete m_checkbox;
        m_checkbox = nullptr;
        relayout();
        return;
    }
    if (!m_checkbox) {
        m_checkbox = new ModalCheckBox(text, m_buttonRow);
        m_buttonLayout->insertWidget(0, m_checkbox, 1); // flex-grow: 1, before the buttons
    }
    m_checkbox->setText(text);
    m_checkbox->setChecked(checked);
    relayout();
}

bool Modal::isChecked() const { return m_checkbox && m_checkbox->isChecked(); }

void Modal::setDialogWidth(int px)
{
    m_width = px;
    relayout();
}

void Modal::setSidebarLayout(bool on)
{
    m_sidebarLayout = on;
    auto* v = static_cast<QVBoxLayout*>(m_frame->layout());
    const int pad = on ? 1 : int(tokPx("--size-4-4", 16));
    v->setContentsMargins(pad, pad, pad, pad);
    if (!on)
        m_content->clearMask();
    // The spacer after the title (index 1) collapses with it.
    if (QSpacerItem* s = v->itemAt(1) ? v->itemAt(1)->spacerItem() : nullptr)
        s->changeSize(0, on ? 0 : 12, QSizePolicy::Minimum, QSizePolicy::Fixed);
    v->setStretchFactor(m_content, on ? 1 : 0);
    relayout();
}

void Modal::open()
{
    if (m_open || !m_layer)
        return;
    m_open = true;
    m_result = -1;
    setGeometry(m_layer->rect());
    show();
    raise();
    m_layer->setBlocking(this, true);
    relayout();
    focusInitial();
}

void Modal::close(int result)
{
    if (!m_open)
        return;
    m_open = false;
    m_result = result;
    hide();
    if (m_layer)
        m_layer->setBlocking(this, false);
    emit closed(result);
    deleteLater();
}

int Modal::exec()
{
    int result = -1;
    QEventLoop loop;
    connect(this, &Modal::closed, &loop, [&](int r) {
        result = r;
        loop.quit();
    });
    connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
    open();
    if (m_open)
        loop.exec(QEventLoop::DialogExec);
    return result;
}

QRect Modal::dialogGeometry() const { return m_frame->geometry(); }

void Modal::relayout()
{
    if (!m_layer)
        return;
    const QRect area = rect();
    if (m_sidebarLayout) {
        m_title->hide();
        m_buttonRow->hide();
        const int w = std::min(int(area.width() * 0.9), int(tokPx("--modal-max-width", 1100)));
        const int h = std::min(int(area.height() * 0.85), int(tokPx("--modal-max-height", 1000)));
        m_frame->setGeometry((area.width() - w) / 2, (area.height() - h) / 2, w, h);
        m_frame->layout()->setContentsMargins(1, 1, 1, 1); // inside the 1 px border
        m_frame->layout()->activate();
        // .modal { overflow: hidden }: clip the content to the rounded frame.
        const double radius = std::max(0.0, tokPx("--modal-radius", 12) - 1);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(m_content->rect()), radius, radius);
        m_content->setMask(QRegion(clip.toFillPolygon().toPolygon()));
        m_close->move(w - 12 - m_close->width(), 6);
        m_close->raise();
        return;
    }
    const int w = std::max(200, std::min(m_width, int(area.width() * 0.8)));
    m_buttonRow->setVisible(!m_buttons.isEmpty() || m_checkbox);
    m_frame->layout()->activate();
    int h = m_frame->layout()->hasHeightForWidth() ? m_frame->layout()->heightForWidth(w) : m_frame->sizeHint().height();
    h = std::min(std::max(h, 100), int(area.height() * 0.85)); // min-height 100px, --dialog-max-height 85vh
    m_frame->setGeometry((area.width() - w) / 2, (area.height() - h) / 2, w, h);
    m_close->move(w - 12 - m_close->width(), 6);
    m_close->raise();
}

QList<QWidget*> Modal::focusChain() const
{
    QList<QWidget*> out;
    for (QWidget* w : m_frame->findChildren<QWidget*>()) {
        if (w->isVisible() && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus) && w != m_close)
            out << w;
    }
    // Content first, then the buttons in order.
    const auto buttonIndex = [this](const QWidget* w) {
        for (qsizetype i = 0; i < m_buttons.size(); ++i)
            if (static_cast<const QWidget*>(m_buttons[i]) == w)
                return i;
        return qsizetype(-1);
    };
    std::stable_sort(out.begin(), out.end(), [&](QWidget* a, QWidget* b) {
        const qsizetype ia = buttonIndex(a), ib = buttonIndex(b);
        const bool ab = ia >= 0, bb = ib >= 0;
        if (ab != bb)
            return !ab;
        if (ab)
            return ia < ib;
        return false;
    });
    return out;
}

void Modal::focusInitial()
{
    // Obsidian focuses the button marked as initial focus (the CTA), else the first input.
    const int def = defaultButton();
    if (def >= 0) {
        m_buttons[def]->setFocus(Qt::OtherFocusReason);
        return;
    }
    const QList<QWidget*> chain = focusChain();
    for (QWidget* w : chain) {
        if (!qobject_cast<QPushButton*>(w) || !m_buttons.contains(static_cast<QPushButton*>(w))) {
            w->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
    setFocus(Qt::OtherFocusReason);
}

bool Modal::handleKey(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        close(-1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        QWidget* f = QApplication::focusWidget();
        if (auto* b = qobject_cast<QPushButton*>(f); b && m_buttons.contains(b)) {
            b->click();
            return true;
        }
        // Anything else (e.g. a single-line input in the content): the default button.
        const int def = defaultButton();
        if (def >= 0) {
            m_buttons[def]->click();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool Modal::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_layer && ev->type() == QEvent::Resize) {
        setGeometry(m_layer->rect());
        relayout();
    }
    return QWidget::eventFilter(obj, ev);
}

bool Modal::focusNextPrevChild(bool next)
{
    const QList<QWidget*> chain = focusChain();
    if (chain.isEmpty())
        return true;
    qsizetype i = chain.indexOf(QApplication::focusWidget());
    i = i < 0 ? (next ? 0 : chain.size() - 1) : (i + (next ? 1 : chain.size() - 1)) % chain.size();
    chain[i]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
}

void Modal::keyPressEvent(QKeyEvent* e)
{
    if (!handleKey(e))
        QWidget::keyPressEvent(e);
}

void Modal::mousePressEvent(QMouseEvent* e)
{
    if (!m_frame->geometry().contains(e->position().toPoint()))
        close(-1); // click on the dimmed background
    e->accept();
}

void Modal::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    relayout();
}

// ---------------------------------------------------------------------------------------------
// ConfirmModal
// ---------------------------------------------------------------------------------------------

namespace {
Modal* buildConfirm(OverlayLayer* layer, const QString& title, const QString& text, const QList<ModalButton>& buttons,
                    const QString& checkboxText)
{
    auto* m = new Modal(layer);
    m->setTitle(title);
    m->setText(text);
    if (!checkboxText.isEmpty())
        m->setCheckbox(checkboxText);
    for (const ModalButton& b : buttons)
        m->addButton(b.text, b.style);
    return m;
}
} // namespace

Modal* ConfirmModal::ask(OverlayLayer* layer, const QString& title, const QString& text, const QList<ModalButton>& buttons,
                         const QString& checkboxText, Callback callback)
{
    if (!layer)
        return nullptr;
    Modal* m = buildConfirm(layer, title, text, buttons, checkboxText);
    QObject::connect(m, &Modal::closed, m, [m, callback](int result) {
        if (callback)
            callback(result, m->isChecked());
    });
    m->open();
    return m;
}

int ConfirmModal::exec(OverlayLayer* layer, const QString& title, const QString& text, const QList<ModalButton>& buttons,
                       const QString& checkboxText, bool* checked)
{
    if (!layer)
        return -1;
    Modal* m = buildConfirm(layer, title, text, buttons, checkboxText);
    bool state = false;
    QObject::connect(m, &Modal::closed, m, [m, &state] { state = m->isChecked(); });
    const int r = m->exec();
    if (checked)
        *checked = state;
    return r;
}

} // namespace oc
