#include "ui/views/canvas/canvas_controls.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/views/canvas/canvas_render.h"

#include <QColorDialog>
#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>

#include <cmath>

namespace oc::canvasui {

using namespace Qt::StringLiterals;

namespace {
constexpr int kDragThreshold = 4;

double tokenPx(const char* name, double fallback)
{
    const ThemePtr t = currentTheme();
    return t ? t->px(QString::fromLatin1(name), -1, fallback) : fallback;
}

QColor token(const char* name, QColor fallback)
{
    const ThemePtr t = currentTheme();
    return t ? t->color(QString::fromLatin1(name), fallback) : fallback;
}

// Obsidian's --input-shadow / menu shadows, approximated by a few translucent rounded rects.
void paintShadow(QPainter& p, const QRectF& r, double radius, double strength)
{
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i) {
        QColor c(0, 0, 0, int(std::lround(strength * 255 / (i + 1))));
        p.setBrush(c);
        p.drawRoundedRect(r.adjusted(-i * 0.5, -i * 0.3, i * 0.5, i * 1.0), radius + i * 0.5, radius + i * 0.5);
    }
    p.restore();
}
} // namespace

// ---------------------------------------------------------------------------------------------
// CanvasButton
// ---------------------------------------------------------------------------------------------

CanvasButton::CanvasButton(Variant variant, const QString& icon, const QString& tooltip, QWidget* parent)
    : QWidget(parent)
    , m_variant(variant)
    , m_icon(icon)
{
    setToolTip(tooltip);
    setAttribute(Qt::WA_Hover);
    setCursor(variant == Variant::CardMenu ? Qt::OpenHandCursor : Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
    resize(sizeHint());
}

void CanvasButton::setIcon(const QString& icon)
{
    m_icon = icon;
    update();
}

void CanvasButton::setDisabledLook(bool disabled)
{
    m_disabled = disabled;
    update();
}

void CanvasButton::setActive(bool active)
{
    m_active = active;
    update();
}

QSize CanvasButton::sizeHint() const
{
    switch (m_variant) {
    case Variant::Control: return QSize(32, 32);   // padding 8 + --icon-s
    case Variant::CardMenu: return QSize(48, 48);  // padding 8 + --icon-xl
    case Variant::Menu: return QSize(34, 30);      // .clickable-icon: 6px 8px + 18px
    }
    return QSize(32, 32);
}

void CanvasButton::paintEvent(QPaintEvent*)
{
    const Palette& pal = Palette::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor fg = pal.textMuted;
    int iconPx = 18;
    double stroke = tokenPx("--icon-m-stroke-width", 1.75);
    switch (m_variant) {
    case Variant::Control:
        iconPx = int(tokenPx("--canvas-controls-icon-size", 16));
        stroke = tokenPx("--canvas-controls-icon-stroke", tokenPx("--icon-s-stroke-width", 2));
        p.fillRect(rect(), m_hover ? pal.interactiveHover : pal.card);
        if (m_hover)
            fg = pal.textNormal;
        if (m_active)
            fg = pal.accent;
        if (m_disabled)
            fg = pal.textFaint;
        break;
    case Variant::CardMenu:
        iconPx = int(tokenPx("--icon-xl", 32));
        stroke = tokenPx("--icon-xl-stroke-width", 1.25);
        if (m_hover)
            fg = pal.accent;
        break;
    case Variant::Menu: {
        if (m_hover || m_active) {
            p.setPen(Qt::NoPen);
            p.setBrush(token("--background-modifier-hover", withAlpha(pal.textNormal, 0.075)));
            p.drawRoundedRect(QRectF(rect()), 4, 4);
        }
        fg = m_hover || m_active ? token("--icon-color-hover", pal.textNormal) : token("--icon-color", pal.textMuted);
        break;
    }
    }
    const QPixmap pm = IconProvider::instance().pixmap(m_icon, iconPx, fg, stroke, devicePixelRatioF());
    const QRectF target(QPointF((width() - iconPx) / 2.0, (height() - iconPx) / 2.0), QSizeF(iconPx, iconPx));
    double lift = 0;
    if (m_variant == Variant::CardMenu && m_hover && !m_dragging)
        lift = -6; // translateY(-6px) on hover
    p.drawPixmap(target.translated(0, lift), pm, QRectF(pm.rect()));
}

void CanvasButton::enterEvent(QEnterEvent*)
{
    m_hover = true;
    update();
}

void CanvasButton::leaveEvent(QEvent*)
{
    m_hover = false;
    update();
}

void CanvasButton::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_pressed = true;
    m_dragging = false;
    m_pressPos = e->pos();
    emit pressed(e->globalPosition().toPoint());
}

void CanvasButton::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_pressed || m_variant != Variant::CardMenu)
        return;
    if (!m_dragging && (e->pos() - m_pressPos).manhattanLength() > kDragThreshold) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (m_dragging)
        emit dragMoved(e->globalPosition().toPoint());
}

void CanvasButton::mouseReleaseEvent(QMouseEvent* e)
{
    if (!m_pressed || e->button() != Qt::LeftButton)
        return;
    m_pressed = false;
    const bool dragged = m_dragging;
    m_dragging = false;
    if (m_variant == Variant::CardMenu)
        setCursor(Qt::OpenHandCursor);
    if (dragged)
        emit dragFinished(e->globalPosition().toPoint(), true);
    else if (rect().contains(e->pos()))
        emit clicked();
    update();
}

// ---------------------------------------------------------------------------------------------
// CanvasControls
// ---------------------------------------------------------------------------------------------

CanvasControls::CanvasControls(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(u"ocCanvasControls"_s);
    setAttribute(Qt::WA_TranslucentBackground);
    auto add = [&](QList<CanvasButton*>& group, const QString& name, const QString& icon, const QString& tip) {
        auto* b = new CanvasButton(CanvasButton::Variant::Control, icon, tip, this);
        connect(b, &CanvasButton::clicked, this, [this, name, b] { emit triggered(name, b->mapToGlobal(b->rect().bottomLeft())); });
        m_buttons.insert(name, b);
        group << b;
    };
    QList<CanvasButton*> g1, g2, g3, g4;
    add(g1, u"settings"_s, u"settings"_s, tr("Canvas settings"));
    add(g2, u"zoom-in"_s, u"plus"_s, tr("Zoom in"));
    add(g2, u"reset-zoom"_s, u"rotate-cw"_s, tr("Reset zoom"));
    add(g2, u"zoom-to-fit"_s, u"maximize"_s, tr("Zoom to fit\n(Shift+1)"));
    add(g2, u"zoom-out"_s, u"minus"_s, tr("Zoom out"));
    add(g3, u"undo"_s, u"undo-2"_s, tr("Undo"));
    add(g3, u"redo"_s, u"redo-2"_s, tr("Redo"));
    add(g4, u"help"_s, u"circle-help"_s, tr("Canvas help"));
    m_groups = {g1, g2, g3, g4};
    relayout();
}

CanvasButton* CanvasControls::button(const QString& name) const { return m_buttons.value(name); }

void CanvasControls::setReadOnly(bool readOnly)
{
    CanvasButton* b = m_buttons.value(u"settings"_s);
    b->setIcon(readOnly ? u"lock"_s : u"settings"_s);
    b->setToolTip(readOnly ? tr("Disable read-only") : tr("Canvas settings"));
}

void CanvasControls::setHistory(bool canUndo, bool canRedo)
{
    m_buttons.value(u"undo"_s)->setDisabledLook(!canUndo);
    m_buttons.value(u"redo"_s)->setDisabledLook(!canRedo);
}

QSize CanvasControls::sizeHint() const
{
    int h = 0;
    for (const auto& g : m_groups)
        h += int(g.size()) * 33 + 1; // items 32 + 1px separators, 1px borders
    h += (int(m_groups.size()) - 1) * 8 + 4; // gaps + shadow room
    return QSize(34 + 4, h);
}

void CanvasControls::relayout()
{
    int y = 0;
    for (const auto& g : m_groups) {
        int iy = y + 1;
        for (CanvasButton* b : g) {
            b->setGeometry(1, iy, 32, 32);
            iy += 33;
        }
        y = iy + 8;
    }
    resize(sizeHint());
}

void CanvasControls::paintEvent(QPaintEvent*)
{
    const Palette& pal = Palette::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double radius = tokenPx("--canvas-controls-radius", 4);
    for (const auto& g : m_groups) {
        if (g.isEmpty())
            continue;
        const QRectF box(0.5, g.first()->y() - 0.5, 33, g.size() * 33.0);
        paintShadow(p, box, radius, pal.dark ? 0.12 : 0.05);
        QPainterPath path;
        path.addRoundedRect(box, radius, radius);
        p.fillPath(path, pal.interactive);
        p.setPen(QPen(pal.border, 1));
        // Separators between items (border-bottom of all but the last).
        for (qsizetype i = 0; i + 1 < g.size(); ++i) {
            const double y = g[i]->geometry().bottom() + 1;
            p.drawLine(QPointF(1, y), QPointF(33, y));
        }
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
}

// ---------------------------------------------------------------------------------------------
// CardMenu
// ---------------------------------------------------------------------------------------------

CardMenu::CardMenu(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(u"ocCanvasCardMenu"_s);
    setAttribute(Qt::WA_TranslucentBackground);
    const struct {
        const char* kind;
        const char* icon;
        QString tip;
    } items[] = {{"card", "sticky-note", tr("Drag to add card")},
                 {"note", "file-text", tr("Drag to add note from vault")},
                 {"media", "file-image", tr("Drag to add media from vault")}};
    int x = 2;
    for (const auto& it : items) {
        auto* b = new CanvasButton(CanvasButton::Variant::CardMenu, QString::fromLatin1(it.icon), it.tip, this);
        const QString kind = QString::fromLatin1(it.kind);
        b->setGeometry(x, 2, 48, 48);
        x += 48;
        connect(b, &CanvasButton::clicked, this, [this, kind] { emit clicked(kind); });
        connect(b, &CanvasButton::dragMoved, this, [this, kind](const QPoint& g) { emit dragMoved(kind, g); });
        connect(b, &CanvasButton::dragFinished, this, [this, kind](const QPoint& g, bool) { emit dragFinished(kind, g); });
        m_buttons << b;
        m_kinds << kind;
    }
    resize(sizeHint());
}

CanvasButton* CardMenu::button(const QString& kind) const
{
    const qsizetype i = m_kinds.indexOf(kind);
    return i < 0 ? nullptr : m_buttons[i];
}

QSize CardMenu::sizeHint() const { return QSize(int(m_buttons.size()) * 48 + 4, 48 + 5); }

void CardMenu::paintEvent(QPaintEvent*)
{
    const Palette& pal = Palette::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box(2, 2, m_buttons.size() * 48.0, 48);
    const double radius = tokenPx("--radius-s", 4);
    paintShadow(p, box, radius, pal.dark ? 0.12 : 0.05);
    QPainterPath path;
    path.addRoundedRect(box, radius, radius);
    p.fillPath(path, pal.dark ? pal.cardDark : pal.card);
    p.setPen(QPen(pal.dark ? QColor(255, 255, 255, 23) : QColor(0, 0, 0, 31), 1));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------
// SelectionMenu
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int kPad = 2;          // .canvas-menu padding: var(--size-2-1)
constexpr int kSubmenuGap = 5;   // top: calc(100% + 5px)
constexpr int kSwatch = 24, kSwatchMargin = 2, kSubmenuPad = 8;

class ColorPicker : public QWidget {
public:
    ColorPicker(QWidget* parent, QString current, std::function<void(const QString&)> pick)
        : QWidget(parent)
        , m_current(std::move(current))
        , m_pick(std::move(pick))
    {
        setObjectName(u"ocCanvasColorPicker"_s);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        resize(sizeHint());
    }
    static QStringList values() { return {QString(), u"1"_s, u"2"_s, u"3"_s, u"4"_s, u"5"_s, u"6"_s, u"custom"_s}; }
    QSize sizeHint() const override
    {
        const int n = int(values().size());
        return QSize(n * (kSwatch + 2 * kSwatchMargin) + (n - 1) + 2 * kSubmenuPad + 2, kSwatch + 2 * kSwatchMargin + 2 * kSubmenuPad + 2);
    }
    QRectF swatchRect(int i) const
    {
        return QRectF(1 + kSubmenuPad + i * (kSwatch + 2 * kSwatchMargin + 1) + kSwatchMargin, 1 + kSubmenuPad + kSwatchMargin, kSwatch, kSwatch);
    }
    int swatchAt(const QPoint& pos) const
    {
        for (int i = 0; i < values().size(); ++i)
            if (swatchRect(i).adjusted(-2, -2, 2, 2).contains(pos))
                return i;
        return -1;
    }
    void choose(int i)
    {
        const QString v = values().value(i);
        if (v == u"custom") {
            auto* dlg = new QColorDialog(parseCssColor(m_current).isValid() ? parseCssColor(m_current) : QColor(Qt::black), window());
            dlg->setOption(QColorDialog::DontUseNativeDialog);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            auto pick = m_pick;
            QObject::connect(dlg, &QColorDialog::colorSelected, dlg, [pick](const QColor& c) {
                if (pick)
                    pick(c.name(QColor::HexRgb));
            });
            dlg->open();
            return;
        }
        if (m_pick)
            m_pick(v);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const Palette& pal = Palette::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(pal.border, 1));
        p.setBrush(pal.card);
        p.drawRoundedRect(box, 4, 4);
        const QStringList vals = values();
        const bool customCurrent = !m_current.isEmpty() && !vals.mid(1, 6).contains(m_current);
        for (int i = 0; i < vals.size(); ++i) {
            const QRectF r = swatchRect(i);
            const bool custom = vals[i] == u"custom";
            const bool active = custom ? customCurrent : vals[i] == m_current;
            QColor c = custom ? (customCurrent ? resolveColor(m_current, pal).color : QColor()) : resolveColor(vals[i], pal).color;
            if (active || m_hover == i) {
                p.setPen(Qt::NoPen);
                p.setBrush(custom && !active ? pal.borderHover : c);
                p.drawEllipse(r.adjusted(-2, -2, 2, 2)); // box-shadow: 0 0 0 2px color
            }
            if (custom && !customCurrent) {
                QConicalGradient g(r.center(), 90);
                const QColor stops[] = {pal.presets[1], pal.presets[3], pal.presets[4], token("--color-blue", QColor(0x086ddd)), pal.presets[6],
                                        pal.presets[1]};
                for (int k = 0; k < 6; ++k)
                    g.setColorAt(1.0 - k / 5.0, stops[k]);
                p.setBrush(g);
            } else {
                p.setBrush(c);
            }
            p.setPen(QPen(pal.card, 2)); // border: 2px solid var(--background-primary)
            p.drawEllipse(r.adjusted(1, 1, -1, -1));
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int h = swatchAt(e->pos());
        if (h != m_hover) {
            m_hover = h;
            update();
        }
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
            if (const int i = swatchAt(e->pos()); i >= 0)
                choose(i);
    }

private:
    QString m_current;
    std::function<void(const QString&)> m_pick;
    int m_hover = -1;
};
} // namespace

SelectionMenu::SelectionMenu(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(u"ocCanvasSelectionMenu"_s);
    setAttribute(Qt::WA_TranslucentBackground);
}

void SelectionMenu::setItems(const QList<Item>& items)
{
    if (m_colorPicker) {
        delete m_colorPicker;
        m_colorPicker = nullptr;
    }
    qDeleteAll(m_buttons);
    m_buttons.clear();
    m_items = items;
    for (const Item& it : m_items) {
        auto* b = new CanvasButton(CanvasButton::Variant::Menu, it.icon, it.tooltip, this);
        b->setObjectName(it.name);
        auto run = it.run;
        connect(b, &CanvasButton::clicked, this, [b, run] {
            if (run)
                run(b);
        });
        b->show();
        m_buttons << b;
    }
    relayout();
}

CanvasButton* SelectionMenu::button(const QString& name) const
{
    for (qsizetype i = 0; i < m_items.size(); ++i)
        if (m_items[i].name == name)
            return m_buttons[i];
    return nullptr;
}

QRect SelectionMenu::toolbarRect() const
{
    const int w = int(m_buttons.size()) * 34 + std::max(0, int(m_buttons.size()) - 1) + 2 * kPad + 2;
    const int h = 30 + 2 * kPad + 2;
    const int total = std::max(w, m_colorPicker ? m_colorPicker->width() : 0);
    return QRect((total - w) / 2, 0, w, h);
}

QSize SelectionMenu::sizeHint() const
{
    const QRect tb = toolbarRect();
    int w = tb.width(), h = tb.height();
    if (m_colorPicker) {
        w = std::max(w, m_colorPicker->width());
        h += kSubmenuGap + m_colorPicker->height();
    }
    return QSize(w, h + 4); // shadow room
}

void SelectionMenu::relayout()
{
    const QRect tb = toolbarRect();
    int x = tb.x() + 1 + kPad;
    for (CanvasButton* b : std::as_const(m_buttons)) {
        b->setGeometry(x, 1 + kPad, 34, 30);
        x += 35;
    }
    if (m_colorPicker) {
        const QSize s = m_colorPicker->sizeHint();
        const int total = std::max(tb.width(), s.width());
        m_colorPicker->setGeometry((total - s.width()) / 2, tb.height() + kSubmenuGap, s.width(), s.height());
    }
    resize(sizeHint());
    update();
}

void SelectionMenu::toggleColorPicker(const QString& current, std::function<void(const QString&)> pick)
{
    CanvasButton* colorButton = button(u"color"_s);
    if (m_colorPicker) {
        delete m_colorPicker;
        m_colorPicker = nullptr;
        if (colorButton)
            colorButton->setActive(false);
        relayout();
        return;
    }
    QPointer<SelectionMenu> self(this);
    m_colorPicker = new ColorPicker(this, current, [self, pick](const QString& v) {
        if (pick)
            pick(v);
        if (self && self->m_colorPicker) {
            self->m_colorPicker->deleteLater();
            self->m_colorPicker = nullptr;
            if (CanvasButton* b = self->button(u"color"_s))
                b->setActive(false);
            self->relayout();
        }
    });
    m_colorPicker->show();
    if (colorButton)
        colorButton->setActive(true);
    relayout();
}

void SelectionMenu::paintEvent(QPaintEvent*)
{
    if (m_buttons.isEmpty())
        return;
    const Palette& pal = Palette::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(toolbarRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    paintShadow(p, box, 4, 0.04);
    p.setPen(QPen(pal.border, 1));
    p.setBrush(pal.card);
    p.drawRoundedRect(box, 4, 4);
    if (m_colorPicker)
        paintShadow(p, QRectF(m_colorPicker->geometry()), 4, 0.04);
}

} // namespace oc::canvasui
