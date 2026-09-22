#pragma once

namespace oc {

class CommandRegistry;
class MainWindow;

// Obsidian's core plugins Daily notes, Templates, Unique note creator ("zk-prefixer") and Random note:
// their commands (ids, names, icons as in Obsidian 1.13), reading the options from the vault's
// "daily-notes", "templates" and "zk-prefixer" config. Unique note and Random note are off by default
// (Obsidian's defaults) and only register when enabled in "core-plugins".
void registerNotePluginCommands(CommandRegistry& registry, MainWindow* window);

// Whether a core plugin is enabled ("core-plugins" config {id: bool}, else Obsidian's default).
bool isCorePluginEnabled(MainWindow* window, const char* id);

} // namespace oc
