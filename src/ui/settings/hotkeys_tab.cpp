#include "ui/settings/hotkeys_tab.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/fuzzy/fuzzy.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/settings/setting_item.h"
#include "ui/settings/settings_modal.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <cmath>

namespace oc {

namespace {
double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
QColor tok(const char* name, const QColor& fallback = {}) { return currentTheme()->color(QString::fromLatin1(name), fallback); }

// .setting-group-filter: a small pill (12 px text, 4 × 8 px padding, --pill-radius, 1 px border);
// .is-active is accent-filled; .mod-error (Conflicts) is tinted red, with a count badge.
class FilterPill : public QWidget {
public:
    FilterPill(const QString& text, bool error, QWidget* parent)
        : QWidget(parent)
        , m_text(text)
        , m_error(error)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::TabFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    std::function<void()> onClick;
    void setActive(bool on)
    {
        m_active = on;
        update();
    }
    void setBadge(const QString& badge)
    {
        m_badge = badge;
        updateGeometry();
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override
    {
        const QFontMetricsF fm(font());
        double w = fm.horizontalAdvance(m_text) + 16 + 2;
        if (!m_badge.isEmpty())
            w += 4 + badgeWidth();
        return {int(std::ceil(w)), int(std::ceil(fm.height())) + 8 + 2};
    }

protected:
    QFont font() const { return currentTheme()->interfaceFont(tokPx("--font-ui-smaller", 12)); }
    double badgeWidth() const
    {
        const QFontMetricsF bf(currentTheme()->interfaceFont(tokPx("--font-small", 13) * 0.9));
        return bf.horizontalAdvance(m_badge) + 0.5 * 2 * 12;
    }
    void paintEvent(QPaintEvent*) override
    {
        const ThemePtr t = currentTheme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const double radius = r.height() / 2;
        QColor bg = Qt::transparent, border = t->color(QStringLiteral("--background-modifier-border"));
        QColor fg = t->color(underMouse() ? QStringLiteral("--text-normal") : QStringLiteral("--text-muted"));
        if (underMouse())
            border = t->color(QStringLiteral("--background-modifier-border-hover"));
        const QColor err = t->color(QStringLiteral("--background-modifier-error"));
        if (m_error) {
            border = err;
            border.setAlphaF(0.15f);
            bg = err;
            bg.setAlphaF(0.10f);
            fg = t->color(QStringLiteral("--text-error"));
        }
        if (m_active) {
            bg = border = m_error ? err : t->color(QStringLiteral("--interactive-accent"));
            fg = t->color(QStringLiteral("--text-on-accent"), Qt::white);
        }
        p.setPen(QPen(border, 1));
        p.setBrush(bg);
        p.drawRoundedRect(r, radius, radius);
        p.setFont(font());
        p.setPen(fg);
        const QFontMetricsF fm(font());
        p.drawText(QRectF(9, 0, fm.horizontalAdvance(m_text) + 1, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text);
        if (!m_badge.isEmpty()) {
            const double bw = badgeWidth();
            const QFont bf = currentTheme()->interfaceFont(tokPx("--font-small", 13) * 0.9);
            const double bh = QFontMetricsF(bf).height() * 0.9 + 2;
            const QRectF br(9 + fm.horizontalAdvance(m_text) + 4, (height() - bh) / 2, bw, bh);
            QColor bbg = m_active ? t->color(QStringLiteral("--text-on-accent"), Qt::white) : err;
            if (!m_active)
                bbg.setAlphaF(0.15f);
            p.setPen(Qt::NoPen);
            p.setBrush(bbg);
            p.drawRoundedRect(br, bh / 2, bh / 2);
            p.setFont(bf);
            p.setPen(m_active ? err : fg);
            p.drawText(br, Qt::AlignCenter, m_badge);
        }
        if (hasFocus()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border-focus")), 2));
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (rect().contains(e->position().toPoint()) && onClick)
            onClick();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) && onClick)
            onClick();
        else
            QWidget::keyPressEvent(e);
    }

private:
    QString m_text;
    QString m_badge;
    bool m_error;
    bool m_active = false;
};

QString chordText(const Hotkey& h) { return h.toString(); }
} // namespace

// ---------------------------------------------------------------------------------------------
// HotkeyChip
// ---------------------------------------------------------------------------------------------

HotkeyChip::HotkeyChip(const QString& text, State state, bool deletable, QWidget* parent)
    : QWidget(parent)
    , m_text(text)
    , m_state(state)
    , m_deletable(deletable && state != State::Empty && state != State::Active)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

QFont HotkeyChip::chipFont() const
{
    // font-family: -apple-system, BlinkMacSystemFont, var(--font-monospace) -> the monospace font on Linux
    return currentTheme()->monospaceFont(tokPx("--font-ui-smaller", 12));
}

QSize HotkeyChip::sizeHint() const
{
    const QFontMetricsF fm(chipFont());
    // padding: 2px 4px 2px 8px (mod-empty: 8px at the end); gap 4px before the 16 px ×
    double w = 8 + fm.horizontalAdvance(QLatin1Char(' ') + m_text + QLatin1Char(' ')) + (m_deletable ? 4 + 16 + 4 : 8);
    const double h = std::max(fm.height() * 1.25, 16.0) + 4;
    return {int(std::ceil(w)), int(std::ceil(h))};
}

QRectF HotkeyChip::deleteRect() const { return {width() - 4.0 - 16, (height() - 16) / 2.0, 16, 16}; }

void HotkeyChip::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = t->color(QStringLiteral("--background-modifier-hover"));
    QColor fg = t->color(QStringLiteral("--text-normal"));
    if (m_state == State::Conflict) {
        bg = t->color(QStringLiteral("--background-modifier-error"));
        fg = t->color(QStringLiteral("--text-on-accent"), Qt::white);
    } else if (m_state == State::Active) {
        bg = t->color(QStringLiteral("--interactive-accent"));
        fg = t->color(QStringLiteral("--text-on-accent"), Qt::white);
    }
    const double radius = tokPx("--radius-m", 8);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), radius, radius);
    p.fillPath(path, bg);
    p.setFont(chipFont());
    p.setPen(fg);
    const double textRight = m_deletable ? deleteRect().left() - 4 : width() - 8;
    p.drawText(QRectF(8, 0, textRight - 8, height()), Qt::AlignLeft | Qt::AlignVCenter, QLatin1Char(' ') + m_text + QLatin1Char(' '));
    if (m_deletable) {
        const QRectF dr = deleteRect();
        QColor ic = fg;
        if (m_deleteHover) {
            // .setting-delete-hotkey:hover: red circle, white icon at full opacity
            p.setPen(Qt::NoPen);
            p.setBrush(t->color(QStringLiteral("--background-modifier-error")));
            p.drawEllipse(dr);
            ic = t->color(QStringLiteral("--text-on-accent"), Qt::white);
        } else {
            ic.setAlphaF(0.6f);
        }
        p.drawPixmap(dr.topLeft(), IconProvider::instance().pixmap(QStringLiteral("x"), 16, ic, 2.0, devicePixelRatioF()));
    }
}

void HotkeyChip::mouseMoveEvent(QMouseEvent* e)
{
    const bool hover = m_deletable && deleteRect().contains(e->position());
    if (hover != m_deleteHover) {
        m_deleteHover = hover;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

void HotkeyChip::leaveEvent(QEvent*)
{
    m_deleteHover = false;
    update();
}

void HotkeyChip::mouseReleaseEvent(QMouseEvent* e)
{
    if (!rect().contains(e->position().toPoint()))
        return;
    if (m_deletable && deleteRect().contains(e->position()))
        emit deleteClicked();
    else
        emit clicked();
}

// ---------------------------------------------------------------------------------------------
// HotkeysTab
// ---------------------------------------------------------------------------------------------

HotkeysTab::HotkeysTab(SettingsModal* modal)
    : SettingsTab(modal, QStringLiteral("hotkeys"), tr("Hotkeys"), QStringLiteral("command"))
{
    if (CommandRegistry* r = commands()) {
        connect(r, &CommandRegistry::commandsChanged, this, [this] {
            if (m_content)
                renderRows();
        });
    }
}

HotkeysTab::~HotkeysTab() { grabKeys(false); }

bool HotkeysTab::isValidRecordedHotkey(const Hotkey& h)
{
    if (!h.isValid())
        return false;
    const bool anyMod = h.mod || h.alt || h.meta || h.shift;
    static const QRegularExpression fkey(QStringLiteral("^F[0-9]+$"), QRegularExpression::CaseInsensitiveOption);
    if (!anyMod && !fkey.match(h.key).hasMatch())
        return false;
    const bool onlyShift = h.shift && !h.mod && !h.alt && !h.meta;
    if (onlyShift && h.key.size() == 1 && h.key[0].isLetter())
        return false;
    return true;
}

QWidget* HotkeysTab::createContent()
{
    auto* content = new QWidget;
    m_content = content;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* g = new SettingGroup(content);
    g->setObjectName(QStringLiteral("ocHotkeysGroup")); // .setting-group.mod-list.mod-hotkeys
    g->setList(true);
    g->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_group = g;
    m_search = g->addSearch(tr("Filter..."));
    m_search->setObjectName(QStringLiteral("ocHotkeysFilter"));
    connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilters(); });
    // The keyboard button at the end of the input: filter by a pressed chord.
    m_keyboardButton = new IconButton(QStringLiteral("keyboard"), tr("Filter by hotkey"), m_search);
    m_keyboardButton->setButtonSize({22, 22});
    m_keyboardButton->setIconSize(16);
    connect(m_keyboardButton, &IconButton::clicked, this, [this] {
        if (m_hotkeyFilter.isValid())
            setHotkeyFilter({});
        else
            startHotkeyFilter();
    });
    m_search->installEventFilter(this);

    QWidget* filters = g->addSearchControl();
    auto* fh = new QHBoxLayout(filters);
    fh->setContentsMargins(0, 4, 0, 4);
    fh->setSpacing(6); // gap: var(--size-2-3)
    const QList<QPair<StatusFilter, QString>> pills = {{StatusFilter::All, tr("All")},
                                                       {StatusFilter::Conflicts, tr("Conflicts")},
                                                       {StatusFilter::Assigned, tr("Assigned")},
                                                       {StatusFilter::AssignedByMe, tr("Assigned by me")},
                                                       {StatusFilter::Unassigned, tr("Unassigned")}};
    for (const auto& [filter, label] : pills) {
        auto* pill = new FilterPill(label, filter == StatusFilter::Conflicts, filters);
        pill->setObjectName(QStringLiteral("ocHotkeyFilterPill"));
        const StatusFilter f = filter;
        pill->onClick = [this, f] { setStatusFilter(f); };
        fh->addWidget(pill);
        m_pills.insert(filter, pill);
    }
    fh->addStretch(1);
    v->addWidget(g);
    m_status = StatusFilter::All;
    m_hotkeyFilter = {};
    renderRows();
    return content;
}

void HotkeysTab::hidden()
{
    cancelCustomizing();
    m_filteringByHotkey = false;
    grabKeys(false);
}

void HotkeysTab::save()
{
    CommandRegistry* r = commands();
    if (r && config())
        config()->setObject(QStringLiteral("hotkeys"), r->saveHotkeys());
}

void HotkeysTab::recomputeConflicts()
{
    m_hotkeyToCommands.clear();
    m_conflicts.clear();
    CommandRegistry* r = commands();
    if (!r)
        return;
    const QList<const Command*> all = r->all();
    for (const Command* c : all)
        for (const Hotkey& h : r->hotkeys(c->id))
            if (h.isValid())
                m_hotkeyToCommands[chordText(h)].append(c->name);
    for (const Command* c : all) {
        for (const Hotkey& h : r->hotkeys(c->id)) {
            if (m_hotkeyToCommands.value(chordText(h)).size() > 1) {
                m_conflicts.insert(c->name);
                break;
            }
        }
    }
    if (m_conflicts.isEmpty() && m_status == StatusFilter::Conflicts)
        m_status = StatusFilter::All;
}

void HotkeysTab::renderRows()
{
    if (!m_group)
        return;
    CommandRegistry* r = commands();
    recomputeConflicts();
    m_group->clearItems();
    m_rows.clear();
    m_order.clear();
    delete m_emptyRow;
    if (!r)
        return;
    for (const Command* c : r->all()) {
        SettingItem* row = m_group->addItem();
        row->setName(c->name);
        row->setProperty("ocCommandId", c->id);
        const QString id = c->id;
        const QString name = c->name;
        const QList<Hotkey> custom = r->hasCustomHotkeys(id) ? r->hotkeys(id) : QList<Hotkey>{};
        const bool isCustom = r->hasCustomHotkeys(id);
        const QList<Hotkey> defaults = c->defaultHotkeys;
        const QList<Hotkey> effective = r->hotkeys(id);

        auto* chips = new QWidget;
        chips->setObjectName(QStringLiteral("ocCommandHotkeys")); // .setting-command-hotkeys
        auto* ch = new QHBoxLayout(chips);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->setSpacing(4);
        for (const Hotkey& h : effective) {
            const QStringList others = [&] {
                QStringList o = m_hotkeyToCommands.value(chordText(h));
                o.removeAll(name);
                return o;
            }();
            auto* chip = new HotkeyChip(chordText(h), others.isEmpty() ? HotkeyChip::State::Normal : HotkeyChip::State::Conflict, true, chips);
            if (!others.isEmpty()) {
                chip->setToolTip(others.size() == 1 ? tr("This hotkey conflicts with “%1”").arg(others.first())
                                                    : tr("This hotkey conflicts with %1 other commands").arg(others.size()));
                connect(chip, &HotkeyChip::clicked, this, [this, h] {
                    m_search->clear();
                    setHotkeyFilter(h);
                });
            }
            chip->setToolTip(chip->toolTip());
            connect(chip, &HotkeyChip::deleteClicked, this, [this, id, h] { removeHotkey(id, h); });
            ch->addWidget(chip);
        }
        if (m_customizing == id)
            ch->addWidget(new HotkeyChip(tr("Press hotkey..."), HotkeyChip::State::Active, false, chips));
        else if ((isCustom && custom.isEmpty()) || (!isCustom && defaults.isEmpty()))
            ch->addWidget(new HotkeyChip(tr("Blank"), HotkeyChip::State::Empty, false, chips));
        row->addControl(chips);
        if (isCustom) {
            IconButton* restore = row->addExtraButton(QStringLiteral("rotate-ccw"), tr("Restore default"), [this, id] { restoreDefault(id); });
            restore->setObjectName(QStringLiteral("ocRestoreHotkey"));
        }
        IconButton* add = row->addExtraButton(QStringLiteral("plus-circle"), tr("Customize this command"), [this, id] { startCustomizing(id); });
        add->setObjectName(QStringLiteral("ocAddHotkey"));
        add->setCheckable(true);
        add->setChecked(m_customizing == id);
        m_rows.insert(id, row);
        m_order << id;
    }
    m_emptyRow = new SettingItem;
    m_emptyRow->setKind(SettingItem::Kind::EmptyState);
    m_emptyRow->setName(tr("No results found."));
    m_group->addItem(m_emptyRow);
    updatePills();
    applyFilters();
}

void HotkeysTab::applyFilters()
{
    CommandRegistry* r = commands();
    if (!m_group || !r)
        return;
    const QString q = m_search ? m_search->text() : QString();
    const SimpleSearch search(q);
    int shown = 0;
    for (const QString& id : std::as_const(m_order)) {
        SettingItem* row = m_rows.value(id);
        const Command* c = r->find(id);
        if (!row || !c)
            continue;
        bool show = search.isEmpty() || search.match(c->name) || search.match(id);
        const QList<Hotkey> effective = r->hotkeys(id);
        const bool userAssigned = r->hasCustomHotkeys(id);
        const bool hasDefault = !c->defaultHotkeys.isEmpty();
        if (show && m_hotkeyFilter.isValid())
            show = effective.contains(m_hotkeyFilter);
        switch (m_status) {
        case StatusFilter::All:
            break;
        case StatusFilter::Conflicts:
            show = show && m_conflicts.contains(c->name);
            break;
        case StatusFilter::Assigned:
            show = show && (hasDefault || userAssigned);
            break;
        case StatusFilter::AssignedByMe:
            show = show && userAssigned;
            break;
        case StatusFilter::Unassigned:
            show = show && !(userAssigned || hasDefault);
            break;
        }
        row->setVisible(show);
        shown += show ? 1 : 0;
    }
    if (m_emptyRow)
        m_emptyRow->setVisible(shown == 0);
    m_group->refresh();
}

void HotkeysTab::updatePills()
{
    for (auto it = m_pills.begin(); it != m_pills.end(); ++it) {
        auto* pill = static_cast<FilterPill*>(it.value().data());
        if (!pill)
            continue;
        pill->setActive(it.key() == m_status);
        if (it.key() == StatusFilter::Conflicts) {
            pill->setVisible(!m_conflicts.isEmpty());
            pill->setBadge(QString::number(m_conflicts.size()));
        }
    }
}

void HotkeysTab::updateDecorator()
{
    if (!m_search || !m_keyboardButton)
        return;
    if (m_filteringByHotkey)
        m_search->setDecorator(tr("Press hotkey..."));
    else if (m_hotkeyFilter.isValid())
        m_search->setDecorator(chordText(m_hotkeyFilter));
    else
        m_search->setDecorator(QString());
    m_keyboardButton->setIconName(m_hotkeyFilter.isValid() ? QStringLiteral("x") : QStringLiteral("keyboard"));
    m_keyboardButton->setChecked(m_filteringByHotkey);
}

void HotkeysTab::setQuery(const QString& query)
{
    if (m_search) {
        m_search->setText(query);
        m_search->setFocus(Qt::OtherFocusReason);
    }
    applyFilters();
}

QString HotkeysTab::query() const { return m_search ? m_search->text() : QString(); }

void HotkeysTab::setStatusFilter(StatusFilter filter)
{
    if (m_status == filter)
        return;
    m_status = filter;
    updatePills();
    applyFilters();
}

void HotkeysTab::setHotkeyFilter(const Hotkey& hotkey)
{
    m_hotkeyFilter = hotkey;
    m_filteringByHotkey = false;
    grabKeys(!m_customizing.isEmpty());
    updateDecorator();
    applyFilters();
}

void HotkeysTab::startHotkeyFilter()
{
    m_filteringByHotkey = true;
    grabKeys(true);
    updateDecorator();
}

void HotkeysTab::startCustomizing(const QString& commandId)
{
    m_customizing = commandId;
    grabKeys(true);
    renderRows();
}

void HotkeysTab::cancelCustomizing()
{
    if (m_customizing.isEmpty())
        return;
    m_customizing.clear();
    grabKeys(m_filteringByHotkey);
    renderRows();
}

bool HotkeysTab::handleChord(const Hotkey& hotkey, bool escape)
{
    if (m_filteringByHotkey) {
        if (escape) {
            m_filteringByHotkey = false;
            setHotkeyFilter({});
        } else {
            setHotkeyFilter(hotkey);
        }
        return true;
    }
    if (m_customizing.isEmpty())
        return false;
    if (escape) {
        cancelCustomizing();
        return true;
    }
    if (!isValidRecordedHotkey(hotkey))
        return false;
    CommandRegistry* r = commands();
    if (!r)
        return false;
    const QString id = m_customizing;
    QList<Hotkey> keys = r->hotkeys(id);
    if (!keys.contains(hotkey)) {
        keys.append(hotkey);
        r->setCustomHotkeys(id, keys);
        save();
    }
    m_customizing.clear();
    grabKeys(false);
    renderRows();
    return true;
}

void HotkeysTab::removeHotkey(const QString& commandId, const Hotkey& hotkey)
{
    CommandRegistry* r = commands();
    const Command* c = r ? r->find(commandId) : nullptr;
    if (!c)
        return;
    QList<Hotkey> keys = r->hotkeys(commandId);
    keys.removeAll(hotkey);
    // Obsidian: removing the last custom key of a command without defaults drops the entry.
    if (keys.isEmpty() && r->hasCustomHotkeys(commandId) && c->defaultHotkeys.isEmpty())
        r->resetHotkeys(commandId);
    else
        r->setCustomHotkeys(commandId, keys);
    save();
    renderRows();
}

void HotkeysTab::restoreDefault(const QString& commandId)
{
    if (CommandRegistry* r = commands()) {
        r->resetHotkeys(commandId);
        save();
    }
    renderRows();
}

QStringList HotkeysTab::visibleCommands() const
{
    QStringList out;
    for (const QString& id : m_order)
        if (SettingItem* row = m_rows.value(id); row && !row->isHidden())
            out << id;
    return out;
}

SettingItem* HotkeysTab::rowFor(const QString& commandId) const { return m_rows.value(commandId); }

void HotkeysTab::grabKeys(bool on)
{
    if (on == m_grabbing)
        return;
    m_grabbing = on;
    if (on)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

bool HotkeysTab::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_search && ev->type() == QEvent::Resize && m_keyboardButton) {
        m_keyboardButton->move(m_search->width() - m_keyboardButton->width() - 4, (m_search->height() - m_keyboardButton->height()) / 2);
        return false;
    }
    if (!m_grabbing || (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride))
        return false;
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || !modal() || w->window() != modal()->window())
        return false;
    auto* ke = static_cast<QKeyEvent*>(ev);
    if (ev->type() == QEvent::ShortcutOverride) {
        ke->accept();
        return true;
    }
    switch (ke->key()) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_AltGr:
        return true; // wait for the key
    default:
        break;
    }
    const bool escape = ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier;
    handleChord(Hotkey::fromEvent(ke), escape);
    return true;
}

} // namespace oc
