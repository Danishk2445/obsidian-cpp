#pragma once

// Navigation core plugins — Quick switcher (switcher), Command palette (command-palette) and Search
// (global-search) — and how the application wires them. In Application::populateWindow():
//
//   registerCoreViews(*registry);
//   registerMarkdownView(*registry);
//   registerNavigationViews(*registry);                          // after registerCoreViews: replaces "search"
//   ...
//   registerAppCommands(*commands, w);
//   registerMarkdownCommands(*commands, workspace);
//   registerNavigationCommands(*commands, session, w->overlay()); // switcher:open, command-palette:open, ...
//   MarkdownView::setTagHandler([](const QString& tag, MarkdownView* from) {
//       openGlobalSearchForTag(from->session(), tag);
//   });

#include "ui/navigation/command_palette.h"
#include "ui/navigation/quick_switcher.h"
#include "ui/navigation/search_view.h"

namespace oc {

class CommandRegistry;
class OverlayLayer;
class VaultSession;
class ViewRegistry;

// The "search" view (SearchView).
void registerNavigationViews(ViewRegistry& registry);

// Commands, with Obsidian's ids, names (core plugin commands carry the plugin name as prefix), icons and hotkeys:
//   switcher:open          "Quick switcher: Open quick switcher"   Mod+O
//   command-palette:open   "Command palette: Open command palette" Mod+P
//   global-search:open     "Search: Search in all files"            Mod+Shift+F (a one-line editor selection becomes
//                                                                    the query)
// Modals open in `layer` (MainWindow::overlay()), or in the focused pop-out window of its window
// (OverlayLayer::focused()). Also makes the recent commands persistent (see
// persistRecentCommands()).
void registerNavigationCommands(CommandRegistry& registry, VaultSession* session, OverlayLayer* layer);

// Tag clicks: the search view with "tag:#tag" (the tag with or without its leading '#').
SearchView* openGlobalSearchForTag(VaultSession* session, const QString& tag);

} // namespace oc
