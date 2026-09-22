#pragma once

namespace oc {

class CommandRegistry;
class Workspace;

// Obsidian's editor:* and markdown:* commands (ids, names, icons and default hotkeys as in Obsidian
// 1.13) acting on the active markdown view. Formatting/editing commands are only available while
// editing (not in Reading view). Also installs the registry's editor provider for `workspace`.
void registerMarkdownCommands(CommandRegistry& registry, Workspace* workspace);

} // namespace oc
