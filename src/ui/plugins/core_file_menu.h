#pragma once

namespace oc {

// Obsidian's built-in "file-menu" items for menus other than the file explorer's own (which builds
// the same items itself): "Copy path" (as Obsidian URL / from vault folder / from system root), "Open
// in default app", "Show in system explorer", "Reveal file in navigation", "Move file to...", the
// linked views of a note shown in a leaf ("Open backlinks", "Open outgoing links", "Open outline",
// "Open local graph", "Open file properties"), and for a view's own menus ("more-options",
// "tab-header") FileView's "Rename..." and "Delete file". Installed once per process.
void installCoreFileMenu();

} // namespace oc
