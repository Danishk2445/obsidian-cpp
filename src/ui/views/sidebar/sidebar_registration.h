#pragma once

// The sidebar views of Obsidian's core plugins "backlink", "outgoing-link", "tag-pane", "outline",
// "bookmarks" and "properties", and their commands.
//
// Wiring (Application::populateWindow):
//   registerCoreViews(*registry);
//   registerSidebarViews(*registry);          // after registerCoreViews: replaces its placeholders
//   ...
//   registerSidebarCommands(*commands, w);    // needs the window's workspace at run time only
// (Tag / property / search-bookmark clicks reach the search view through ui/navigation's
// openGlobalSearch(); globalSearchHooks() can override that.)
// Default layout (Obsidian's): left sidedock file-explorer, search, bookmarks; right sidedock backlink,
// outgoing-link, tag, outline, all-properties.

namespace oc {

class CommandRegistry;
class MainWindow;
class ViewRegistry;

// View types "backlink", "outgoing-link", "tag", "outline", "bookmarks", "all-properties".
void registerSidebarViews(ViewRegistry& registry);

// backlink:open, backlink:open-backlinks, backlink:toggle-backlinks-in-document (the active note's
// embedded backlinks), outgoing-links:open, outgoing-links:open-for-current, tag-pane:open,
// outline:open, outline:open-for-current, bookmarks:open, bookmarks:bookmark-current-view,
// bookmarks:bookmark-current-search, bookmarks:unbookmark-current-view,
// bookmarks:bookmark-current-section, bookmarks:bookmark-current-heading, bookmarks:bookmark-all-tabs,
// properties:open — with Obsidian 1.13's names ("Backlinks: Show backlinks": core plugin commands carry
// the plugin name) and icons; none has a default hotkey.
void registerSidebarCommands(CommandRegistry& commands, MainWindow* window);

// The plugins' file-menu / editor-menu items (FileMenuHooks / EditorMenuHooks, once per process;
// registerSidebarCommands() installs them): "Bookmark..." / "Edit bookmark" for files and folders,
// "Open backlinks" / "Open outgoing links" / "Open outline" for a leaf's note (section "view.linked"),
// "Bookmark this heading..." on a heading line in the editor.
void installSidebarMenus();

} // namespace oc
