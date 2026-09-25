#pragma once

namespace oc {

class CommandRegistry;

// Obsidian's property commands, acting on the active markdown view (ids, names, icons and default
// hotkeys as in Obsidian 1.13):
//   markdown:add-metadata-property   "Add file property" (Mod+;)
//   markdown:add-alias               "Add alias"
//   markdown:clear-metadata-properties "Clear file properties"
//   editor:toggle-fold-properties    "Toggle fold properties in current file"
// Needs the registry's editor provider (registerMarkdownCommands installs it).
void registerPropertyCommands(CommandRegistry& registry);

} // namespace oc
