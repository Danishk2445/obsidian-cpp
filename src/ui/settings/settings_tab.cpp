#include "ui/settings/settings_tab.h"

#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/vault_session.h"
#include "ui/settings/path_suggest.h"
#include "ui/settings/setting_item.h"
#include "ui/settings/settings_modal.h"
#include "ui/shell/main_window.h"
#include "ui/widgets/color_swatch.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/widgets/value_slider.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QVBoxLayout>

namespace oc {

// ---------------------------------------------------------------------------------------------
// SettingDef builders
// ---------------------------------------------------------------------------------------------

namespace {
SettingDef item(const QString& name, const QString& desc)
{
    SettingDef d;
    d.name = name;
    d.desc = desc;
    return d;
}
} // namespace

SettingDef SettingDef::toggle(const QString& name, const QString& desc, const QString& key)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Toggle;
    d.control.key = key;
    return d;
}

SettingDef SettingDef::dropdown(const QString& name, const QString& desc, const QString& key, QList<QPair<QString, QString>> options)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Dropdown;
    d.control.key = key;
    d.control.options = std::move(options);
    return d;
}

SettingDef SettingDef::slider(const QString& name, const QString& desc, const QString& key, double min, double max, double step,
                              double defaultValue)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Slider;
    d.control.key = key;
    d.control.min = min;
    d.control.max = max;
    d.control.step = step;
    d.control.defaultValue = defaultValue;
    return d;
}

SettingDef SettingDef::text(const QString& name, const QString& desc, const QString& key, const QString& placeholder)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Text;
    d.control.key = key;
    d.control.placeholder = placeholder;
    return d;
}

SettingDef SettingDef::number(const QString& name, const QString& desc, const QString& key, double min, double max)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Number;
    d.control.key = key;
    d.control.min = min;
    d.control.max = max;
    return d;
}

SettingDef SettingDef::folder(const QString& name, const QString& desc, const QString& key, const QString& placeholder, bool includeRoot)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::Folder;
    d.control.key = key;
    d.control.placeholder = placeholder;
    d.control.includeRoot = includeRoot;
    return d;
}

SettingDef SettingDef::file(const QString& name, const QString& desc, const QString& key, const QString& placeholder)
{
    SettingDef d = item(name, desc);
    d.control.type = SettingControl::Type::File;
    d.control.key = key;
    d.control.placeholder = placeholder;
    return d;
}

SettingDef SettingDef::custom(const QString& name, const QString& desc, std::function<void(SettingItem&)> render)
{
    SettingDef d = item(name, desc);
    d.render = std::move(render);
    return d;
}

SettingDef SettingDef::group(const QString& heading, QList<SettingDef> items)
{
    SettingDef d;
    d.type = Type::Group;
    d.heading = heading;
    d.items = std::move(items);
    return d;
}

SettingDef SettingDef::list(const QString& heading, QList<SettingDef> items)
{
    SettingDef d;
    d.type = Type::List;
    d.heading = heading;
    d.items = std::move(items);
    return d;
}

SettingDef SettingDef::page(const QString& name, const QString& desc, QList<SettingDef> items)
{
    SettingDef d = item(name, desc);
    d.type = Type::Page;
    d.items = std::move(items);
    return d;
}

// ---------------------------------------------------------------------------------------------
// SettingsTab
// ---------------------------------------------------------------------------------------------

SettingsTab::SettingsTab(SettingsModal* modal, QString id, QString name, QString icon, Section section)
    : QObject(modal)
    , m_modal(modal)
    , m_id(std::move(id))
    , m_name(std::move(name))
    , m_icon(std::move(icon))
    , m_section(section)
{
}

MainWindow* SettingsTab::window() const { return m_modal ? m_modal->window() : nullptr; }
VaultSession* SettingsTab::session() const { return m_modal ? m_modal->session() : nullptr; }
ConfigStore* SettingsTab::config() const { return session() ? session()->config() : nullptr; }
VaultConfig* SettingsTab::vaultConfig() const { return session() ? session()->vaultConfig() : nullptr; }
CommandRegistry* SettingsTab::commands() const { return session() ? session()->commands() : nullptr; }

QJsonValue SettingsTab::controlValue(const QString& key) const
{
    VaultConfig* cfg = vaultConfig();
    return cfg ? cfg->get(key) : QJsonValue();
}

void SettingsTab::setControlValue(const QString& key, const QJsonValue& value)
{
    if (VaultConfig* cfg = vaultConfig())
        cfg->set(key, value);
}

void SettingsTab::update() { emit updateRequested(); }
void SettingsTab::refreshState() { emit refreshRequested(); }

// ---------------------------------------------------------------------------------------------
// PluginSettingsTab
// ---------------------------------------------------------------------------------------------

PluginSettingsTab::PluginSettingsTab(SettingsModal* modal, QString pluginId, QString configFile, QString name, QString icon, Section section)
    : SettingsTab(modal, pluginId, std::move(name), std::move(icon), section)
    , m_pluginId(std::move(pluginId))
    , m_file(std::move(configFile))
{
}

QJsonValue PluginSettingsTab::controlValue(const QString& key) const
{
    ConfigStore* c = config();
    return c ? c->value(m_file, key) : QJsonValue();
}

void PluginSettingsTab::setControlValue(const QString& key, const QJsonValue& value)
{
    // Obsidian's plugin.saveData(options): the key is stored even when it equals the default.
    if (ConfigStore* c = config())
        c->setValue(m_file, key, value);
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void RenderedSettings::refresh()
{
    for (const Row& r : std::as_const(rows)) {
        if (!r.item)
            continue;
        bool visible = true;
        if (r.def.visible)
            visible = r.def.visible();
        r.item->setProperty("ocDefHidden", !visible);
        r.item->setVisible(visible && !r.item->property("ocSearchHidden").toBool());
        if (r.def.disabled)
            r.item->setDisabled(r.def.disabled());
    }
    for (const auto& g : std::as_const(groups))
        if (g)
            g->refresh();
}

SettingItem* RenderedSettings::find(const QString& name) const
{
    for (const Row& r : rows)
        if (r.item && r.def.name == name)
            return r.item;
    return nullptr;
}

namespace {
QJsonValue valueOrDefault(const SettingsTab& tab, const SettingControl& c)
{
    const QJsonValue v = tab.controlValue(c.key);
    return v.isUndefined() || v.isNull() ? c.defaultValue : v;
}

QString numberText(const QJsonValue& v)
{
    if (!v.isDouble())
        return {};
    return QString::number(v.toDouble(), 'g', 12);
}

// Obsidian's control writer: validate, show / clear the error, store, refresh visible()/disabled().
std::function<void(const QJsonValue&)> writer(SettingsTab& tab, const SettingControl& c, SettingItem* row)
{
    QPointer<SettingsTab> t = &tab;
    QPointer<SettingItem> r = row;
    const QString key = c.key;
    const auto validate = c.validate;
    const auto after = c.afterChange;
    return [t, r, key, validate, after](const QJsonValue& v) {
        if (!t)
            return;
        if (validate) {
            const QString err = validate(v);
            if (r)
                r->setErrorMessage(err);
            if (!err.isEmpty())
                return;
        }
        t->setControlValue(key, v);
        if (after)
            after(v);
        if (t)
            t->refreshState();
    };
}

// "Restore default" (rotate-ccw) before sliders / color pickers, aria-disabled at the default value.
IconButton* addRestoreButton(SettingItem& row, std::function<void()> onClick)
{
    IconButton* b = row.addExtraButton(QStringLiteral("rotate-ccw"), QObject::tr("Restore default"), std::move(onClick));
    b->setObjectName(QStringLiteral("ocRestoreDefault"));
    return b;
}

void renderControl(SettingsTab& tab, const SettingControl& c, SettingItem& row)
{
    const QJsonValue value = valueOrDefault(tab, c);
    auto write = writer(tab, c, &row);
    if (c.validate) {
        const QString err = c.validate(value);
        row.setErrorMessage(err);
    }
    switch (c.type) {
    case SettingControl::Type::None:
        break;
    case SettingControl::Type::Toggle:
        row.addToggle(value.toBool(), [write](bool on) { write(on); });
        break;
    case SettingControl::Type::Dropdown: {
        QString v = value.isString() ? value.toString() : value.isBool() ? (value.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                                                          : numberText(value);
        row.addDropdown(c.options, v, [write](const QString& nv) { write(nv); });
        break;
    }
    case SettingControl::Type::Text:
        row.addText(value.toString(), c.placeholder, [write](const QString& nv) { write(nv); });
        break;
    case SettingControl::Type::Number: {
        QLineEdit* e = row.addText(numberText(value), c.placeholder);
        e->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
        QPointer<SettingItem> r = &row;
        auto last = std::make_shared<QString>(numberText(value));
        const SettingControl cc = c;
        auto commit = [e, r, write, cc, last]() -> bool {
            const QString text = e->text().trimmed();
            if (text.isEmpty()) {
                const double d = cc.defaultValue.isDouble() ? cc.defaultValue.toDouble() : 0;
                e->setText(QString::number(d, 'g', 12));
                *last = e->text();
                if (r)
                    r->setErrorMessage({});
                write(d);
                return true;
            }
            bool ok = false;
            const double d = text.toDouble(&ok);
            QString err;
            if (!ok)
                err = QObject::tr("Invalid number");
            else if (!std::isnan(cc.min) && d < cc.min)
                err = QObject::tr("Value must be at least %1").arg(cc.min);
            else if (!std::isnan(cc.max) && d > cc.max)
                err = QObject::tr("Value must be at most %1").arg(cc.max);
            if (r)
                r->setErrorMessage(err);
            if (!err.isEmpty())
                return false;
            *last = text;
            write(d);
            return true;
        };
        QObject::connect(e, &QLineEdit::editingFinished, e, [commit] { commit(); });
        // Escape restores the last value (and must not close the modal).
        struct EscFilter : QObject {
            std::function<void()> onEsc;
            using QObject::QObject;
            bool eventFilter(QObject*, QEvent* ev) override
            {
                if (ev->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
                    onEsc();
                    return true;
                }
                return false;
            }
        };
        auto* f = new EscFilter(e);
        f->onEsc = [e, r, last] {
            e->setText(*last);
            if (r)
                r->setErrorMessage({});
            e->clearFocus();
        };
        e->installEventFilter(f);
        break;
    }
    case SettingControl::Type::Slider: {
        IconButton* restore = c.defaultValue.isDouble() ? addRestoreButton(row, {}) : nullptr;
        const double v = value.toDouble(c.defaultValue.toDouble());
        ValueSlider* slider = row.addSlider(std::isnan(c.min) ? 0 : c.min, std::isnan(c.max) ? 100 : c.max, c.step, v);
        QPointer<ValueSlider> sp = slider;
        QPointer<IconButton> rp = restore;
        const QJsonValue def = c.defaultValue;
        auto sync = [rp, def](double nv) {
            if (rp)
                rp->setEnabled(!def.isDouble() || nv != def.toDouble());
        };
        sync(v);
        QObject::connect(slider, &ValueSlider::valueChanged, slider, [write, sync](double nv) {
            sync(nv);
            write(nv);
        });
        if (restore)
            QObject::connect(restore, &IconButton::clicked, restore, [sp, def, write, sync] {
                if (sp)
                    sp->setValue(def.toDouble());
                sync(def.toDouble());
                write(def);
            });
        break;
    }
    case SettingControl::Type::Folder: {
        QString v = value.toString();
        if (v == u"/")
            v.clear();
        QLineEdit* e = row.addText(v, c.placeholder, [write](const QString& nv) { write(nv); });
        auto* suggest = new FolderSuggest(e, tab.session(), tab.modal() ? tab.modal()->overlay() : nullptr, c.includeRoot);
        QObject::connect(suggest, &FolderSuggest::chosen, e, [write](const QString& path) { write(path); });
        break;
    }
    case SettingControl::Type::File: {
        auto* picker = new FilePicker(tab.session(), tab.modal() ? tab.modal()->overlay() : nullptr);
        if (!c.placeholder.isEmpty())
            picker->setPlaceholder(c.placeholder);
        picker->setFilter(c.filter);
        picker->setValue(value.toString());
        QObject::connect(picker, &FilePicker::valueChanged, picker, [write](const QString& path) { write(path); });
        row.addControl(picker);
        break;
    }
    case SettingControl::Type::Color: {
        IconButton* restore = nullptr;
        if (c.defaultValue.isString())
            restore = addRestoreButton(row, {});
        ColorSwatch* swatch = row.addColorPicker(QColor(value.toString()));
        QPointer<ColorSwatch> sp = swatch;
        QPointer<IconButton> rp = restore;
        const QJsonValue def = c.defaultValue;
        auto sync = [rp, def](const QColor& nv) {
            if (rp)
                rp->setEnabled(!def.isString() || nv != QColor(def.toString()));
        };
        sync(swatch->color());
        QObject::connect(swatch, &ColorSwatch::colorChanged, swatch, [write, sync](const QColor& nv) {
            sync(nv);
            write(nv.name(QColor::HexRgb));
        });
        if (restore)
            QObject::connect(restore, &IconButton::clicked, restore, [sp, def, write, sync] {
                if (sp)
                    sp->setColor(QColor(def.toString()));
                sync(QColor(def.toString()));
                write(def);
            });
        break;
    }
    }
}

void renderRow(SettingsTab& tab, const SettingDef& def, SettingItem& row, const std::function<void(const SettingDef&)>& openPage,
               const SettingDef* list, int index)
{
    row.setName(def.name);
    if (!def.richDesc.isEmpty())
        row.setRichDesc(def.richDesc);
    else
        row.setDesc(def.desc);
    row.setDescWarning(def.descWarning);
    if (list)
        row.setIcon(def.icon);
    if (def.type == SettingDef::Type::Page) {
        row.setDisplayValue(def.displayValue ? def.displayValue() : QString(), def.warning && def.warning());
        const SettingDef page = def;
        row.setClickHandler([openPage, page] { openPage(page); });
        return;
    }
    if (def.action) {
        row.setKind(SettingItem::Kind::Action);
        row.setClickHandler(def.action);
    } else if (def.render) {
        def.render(row);
    } else if (def.control.type != SettingControl::Type::None) {
        renderControl(tab, def.control, row);
    }
    if (list && list->onDelete) {
        const auto del = list->onDelete;
        row.addExtraButton(QStringLiteral("x"), QObject::tr("Delete"), [del, index] { del(index); });
    }
}

bool defaultMatch(const SettingDef& item, const QString& query)
{
    const QString q = query.trimmed().toLower();
    return q.isEmpty() || item.name.toLower().contains(q);
}
} // namespace

void renderSettings(SettingsTab& tab, const QList<SettingDef>& defs, QVBoxLayout* layout, RenderedSettings& out,
                    const std::function<void(const SettingDef& page)>& openPage)
{
    // Consecutive items / pages outside a group form an implicit group (Obsidian's e6()).
    QList<SettingDef> blocks;
    bool implicitOpen = false;
    for (const SettingDef& d : defs) {
        if (d.type == SettingDef::Type::Group || d.type == SettingDef::Type::List) {
            blocks.push_back(d);
            implicitOpen = false;
        } else {
            if (!implicitOpen) {
                blocks.push_back(SettingDef::group(QString(), {}));
                implicitOpen = true;
            }
            blocks.last().items.push_back(d);
        }
    }

    bool first = true;
    for (const SettingDef& block : std::as_const(blocks)) {
        auto* g = new SettingGroup;
        g->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        g->setList(block.type == SettingDef::Type::List);
        g->setHeading(block.heading);
        for (const SettingDef::HeaderButton& hb : block.headerButtons)
            g->addHeaderButton(hb.icon, hb.tooltip, hb.onClick);
        if (block.type == SettingDef::Type::List && block.addItem)
            g->addHeaderButton(QStringLiteral("plus"), block.addItemName, block.addItem);
        QList<RenderedSettings::Row> groupRows;
        for (int i = 0; i < block.items.size(); ++i) {
            const SettingDef& d = block.items[i];
            SettingItem* row = g->addItem();
            renderRow(tab, d, *row, openPage, block.type == SettingDef::Type::List ? &block : nullptr, i);
            groupRows.push_back({d, row, g});
        }
        out.rows += groupRows;
        if (!block.emptyState.isEmpty())
            g->setEmptyState(block.emptyState);
        if (block.type == SettingDef::Type::List)
            g->setAlwaysVisible(true);
        if (block.search && !block.items.isEmpty()) {
            SearchInput* s = g->addSearch(block.search->placeholder);
            const auto match = block.search->match ? block.search->match : defaultMatch;
            QPointer<SettingGroup> gp = g;
            QObject::connect(s, &QLineEdit::textChanged, g, [gp, groupRows, match](const QString& q) {
                for (const RenderedSettings::Row& r : groupRows) {
                    if (!r.item)
                        continue;
                    const bool hide = r.def.searchable && !match(r.def, q); // searchable: false rows always stay
                    r.item->setProperty("ocSearchHidden", hide);
                    r.item->setVisible(!hide && !r.item->property("ocDefHidden").toBool());
                }
                if (gp)
                    gp->refresh();
            });
        }
        if (!first)
            layout->addSpacing(24); // .setting-group + .setting-group { margin-top: var(--size-4-6) }
        layout->addWidget(g);
        out.groups.push_back(g);
        first = false;
    }
    out.refresh();
}

} // namespace oc
