#pragma once

namespace oc {

class CommandRegistry;
class MainWindow;

// Obsidian's app:* and workspace:* commands for a window: navigation history, sidebars, tabs
// (new/close/undo close/next/previous/goto N/pin), splits, rename/copy path of the active file.
void registerAppCommands(CommandRegistry& registry, MainWindow* window);

} // namespace oc
