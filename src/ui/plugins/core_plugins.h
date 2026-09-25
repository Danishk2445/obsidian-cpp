#pragma once

#include <QString>
#include <QStringList>

namespace oc {

class MainWindow;

// What a core plugin contributes to the shell, so that switching it off in Settings → Core plugins
// takes it away like Obsidian's plugin unload: its commands become unavailable (palette, hotkeys,
// slash commands), its ribbon action and status bar item hide, and its views close. Switching it
// back on restores them and re-opens its sidebar view (Obsidian's initLeaf / ensureSideLeaf).
// The note plugins (daily notes, templates, unique notes, random note) are handled by the settings,
// which re-register their commands.
struct CorePluginParts {
    QStringList commandPrefixes; // "graph:" matches "graph:open", "graph:open-local", ...
    QStringList ribbonActions;
    QStringList statusItems;
    QStringList viewTypes;
    QString side; // "left" / "right": the sidebar its first view type lives in when enabled again
};
const CorePluginParts* corePluginParts(const QString& id);

// Installs the command filter, applies the current core-plugins state and follows the settings'
// corePluginToggled(). Call after the workspace layout, ribbon and status bar exist.
void installCorePluginSwitches(MainWindow* window);

// Applies one plugin's state to `window` (what corePluginToggled() triggers).
void applyCorePluginState(MainWindow* window, const QString& id, bool enabled);

} // namespace oc
