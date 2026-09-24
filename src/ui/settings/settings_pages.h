#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace oc {

class ConfigStore;
class SettingsModal;
class ThemeManager;

// Adds Obsidian 1.13's settings tabs to `modal`, in its order:
//   Options: General, Appearance, Interface, Editor, Files and links, Hotkeys, Keychain, Core plugins,
//            Community plugins;
//   Core plugins (listed while the plugin is enabled): Backlinks, Command palette, Daily notes, File
//            recovery, Page preview, Quick switcher, Templates, Unique note creator;
//   Community plugins: Greentext Guard (the user's plugin, built in).
void addSettingsTabs(SettingsModal& modal);

// Obsidian 1.13's core plugins: id, name, description and whether it's on by default.
struct CorePluginInfo {
    QString id;
    QString name;
    QString description;
    bool defaultOn = true;
};
const QList<CorePluginInfo>& corePluginInfos();

// CSS snippets: *.css files in the clone's config dir (~/.config/obsidian-cpp/snippets), enabled per
// vault in appearance.json "enabledCssSnippets" (Obsidian's key). The clone's ThemeManager only takes
// CSS custom properties, so an enabled snippet contributes the `--var: value` declarations of its
// `body` / `:root` / `.theme-light` / `.theme-dark` rules (later snippets win); other rules are ignored.
namespace css_snippets {
QString folder();
QStringList list(); // snippet names (file names without ".css"), sorted
QStringList enabled(const ConfigStore* config);
void setEnabled(ConfigStore* config, const QString& name, bool enabled);
struct Overrides {
    QHash<QString, QString> body, light, dark;
};
Overrides parse(const QString& css);
Overrides collect(const ConfigStore* config); // the enabled snippets' declarations, in order
} // namespace css_snippets

// Appearance font values are comma-separated lists ("Inter, Arial"); the first installed one applies.
QString resolveFontList(const QString& list);

} // namespace oc
