#pragma once

#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>

#include <cmath>
#include <functional>
#include <optional>

class QVBoxLayout;

namespace oc {

class ConfigStore;
class CommandRegistry;
class MainWindow;
class SettingGroup;
class SettingItem;
class SettingsModal;
class VaultConfig;
class VaultSession;

// The control of a declarative setting (Obsidian 1.13's `control: {type, key, ...}`): the renderer
// builds the widget, reads the value through the tab's controlValue(key) and writes every change back
// through setControlValue(key, value) — immediately, like Obsidian (the stores debounce the writes).
struct SettingControl {
    enum class Type { None, Toggle, Dropdown, Text, Number, Slider, Folder, File, Color };
    Type type = Type::None;
    QString key;
    QList<QPair<QString, QString>> options;       // dropdown: (value, label)
    QString placeholder;                          // text / number / folder / file
    double min = NAN, max = NAN, step = 1;        // number / slider (NaN = unbounded)
    QJsonValue defaultValue;                      // slider / color: shows "Restore default"; number: empty input
    bool includeRoot = false;                     // folder: offer "/" too
    std::function<bool(const QString& path)> filter;          // file: which files to offer (default: all)
    std::function<QString(const QJsonValue& value)> validate; // an error message blocks the write
    std::function<void(const QJsonValue& value)> afterChange; // live effects once the value is stored
};

// A declarative setting definition, mirroring Obsidian 1.13's setting definitions:
//  - Item: a row (name, description) with a `control`, a custom `render` or an `action` (.mod-action);
//  - Group: a card of rows under an optional heading, with an optional search (`search`);
//  - List: a .mod-list group (rows with icons, an empty state, an "add" header button, deletable rows);
//  - Page: a navigable row (display value + chevron) opening a sub-page of `items` (or a custom `page`).
// Consecutive top-level items are wrapped into an implicit group, like Obsidian.
struct SettingDef {
    enum class Type { Item, Group, List, Page };
    Type type = Type::Item;

    QString name;
    QString desc;       // plain text; also what the settings search matches
    QString richDesc;   // optional Qt rich text shown instead of desc
    bool descWarning = false;
    QStringList aliases; // extra search terms
    QString icon;        // list rows
    bool searchable = true;
    std::function<bool()> visible;  // evaluated on every refresh (default: visible)
    std::function<bool()> disabled; // evaluated on every refresh (default: enabled)

    SettingControl control;
    std::function<void(SettingItem& item)> render; // custom controls (Obsidian's render(setting))
    std::function<void()> action;                  // the row is a button

    // Group / List / Page
    QString heading;
    QList<SettingDef> items;
    struct Search {
        QString placeholder;
        std::function<bool(const SettingDef& item, const QString& query)> match; // default: name contains
    };
    std::optional<Search> search;
    QString emptyState;
    struct HeaderButton {
        QString icon, tooltip;
        std::function<void()> onClick;
    };
    QList<HeaderButton> headerButtons;
    QString addItemName;          // list: the "+" header button's tooltip
    std::function<void()> addItem;
    std::function<void(int index)> onDelete; // list: an "×" button on every row

    // Page
    std::function<QString()> displayValue;
    std::function<bool()> warning;                              // a warning status icon on the row
    std::function<QWidget*(SettingsModal& modal)> customPage;   // a hand-built page instead of `items`

    // Builders for the common cases.
    static SettingDef toggle(const QString& name, const QString& desc, const QString& key);
    static SettingDef dropdown(const QString& name, const QString& desc, const QString& key, QList<QPair<QString, QString>> options);
    static SettingDef slider(const QString& name, const QString& desc, const QString& key, double min, double max, double step,
                             double defaultValue);
    static SettingDef text(const QString& name, const QString& desc, const QString& key, const QString& placeholder = {});
    static SettingDef number(const QString& name, const QString& desc, const QString& key, double min = NAN, double max = NAN);
    static SettingDef folder(const QString& name, const QString& desc, const QString& key, const QString& placeholder, bool includeRoot);
    static SettingDef file(const QString& name, const QString& desc, const QString& key, const QString& placeholder = {});
    static SettingDef custom(const QString& name, const QString& desc, std::function<void(SettingItem&)> render);
    static SettingDef group(const QString& heading, QList<SettingDef> items);
    static SettingDef list(const QString& heading, QList<SettingDef> items);
    static SettingDef page(const QString& name, const QString& desc, QList<SettingDef> items);
};

// A settings tab (Obsidian's SettingTab): an entry of the modal's navigation with a name, a Lucide
// icon and definitions. Values are read and written through controlValue()/setControlValue(): the
// vault's app.json / appearance.json options (VaultConfig) by default; PluginSettingsTab uses a
// plugin's JSON file. Tabs with a hand-built UI (Hotkeys) override createContent().
class SettingsTab : public QObject {
    Q_OBJECT
public:
    enum class Section { Options, CorePlugins, CommunityPlugins };

    SettingsTab(SettingsModal* modal, QString id, QString name, QString icon, Section section = Section::Options);

    const QString& id() const { return m_id; }
    const QString& name() const { return m_name; }
    const QString& icon() const { return m_icon; }
    Section section() const { return m_section; }
    SettingsModal* modal() const { return m_modal; }

    MainWindow* window() const;
    VaultSession* session() const;
    ConfigStore* config() const;
    VaultConfig* vaultConfig() const;
    CommandRegistry* commands() const;

    virtual QList<SettingDef> definitions() { return {}; }
    // A hand-built tab body (nullptr: render definitions()).
    virtual QWidget* createContent() { return nullptr; }
    // The tab stopped being shown (closing, another tab opened).
    virtual void hidden() {}

    virtual QJsonValue controlValue(const QString& key) const;
    virtual void setControlValue(const QString& key, const QJsonValue& value);

    // Definitions changed: the modal re-renders the tab if it is shown (keeping the scroll position).
    void update();
    // Values changed: re-evaluates visible()/disabled() of the rendered rows.
    void refreshState();

signals:
    void updateRequested();
    void refreshRequested();

private:
    SettingsModal* m_modal;
    QString m_id, m_name, m_icon;
    Section m_section;
};

// A core plugin's options tab, storing its values in the plugin's config file ("daily-notes",
// "templates", ...). Values equal to `defaults` are still written (Obsidian saves the whole object).
class PluginSettingsTab : public SettingsTab {
    Q_OBJECT
public:
    PluginSettingsTab(SettingsModal* modal, QString pluginId, QString configFile, QString name, QString icon,
                      Section section = Section::CorePlugins);
    const QString& pluginId() const { return m_pluginId; }
    const QString& configFile() const { return m_file; }

    QJsonValue controlValue(const QString& key) const override;
    void setControlValue(const QString& key, const QJsonValue& value) override;

    // Filled in by the plugin tab factories.
    std::function<QList<SettingDef>(PluginSettingsTab&)> build;
    QList<SettingDef> definitions() override { return build ? build(*this) : QList<SettingDef>{}; }

private:
    QString m_pluginId;
    QString m_file;
};

// Rendered definitions of one tab or page: the widgets and the definition each came from, for
// refreshing visibility / disabled state and for scrolling to a search result.
class RenderedSettings {
public:
    struct Row {
        SettingDef def;
        QPointer<SettingItem> item;
        QPointer<SettingGroup> group;
    };
    QList<Row> rows;
    QList<QPointer<SettingGroup>> groups;

    void refresh(); // visible() / disabled()
    SettingItem* find(const QString& name) const;
};

// Renders `defs` into `layout` (a tab body or page), with `tab` providing the values. `openPage` is
// called when a navigable (page) row is clicked.
void renderSettings(SettingsTab& tab, const QList<SettingDef>& defs, QVBoxLayout* layout, RenderedSettings& out,
                    const std::function<void(const SettingDef& page)>& openPage);

// Folder path normalization used by folder controls (Obsidian's normalizePath, "" for the root).
QString normalizeSettingPath(const QString& path);

} // namespace oc
