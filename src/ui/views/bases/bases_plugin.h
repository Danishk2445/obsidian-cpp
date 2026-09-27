#pragma once

// Obsidian's "bases" core plugin for one window:
//  - the "bases" view for .base files (BasesFileView, the "base" extension);
//  - the embeds: "![[x.base]]" / "![[x.base#View]]" and ```base code blocks (EmbedRegistry);
//  - the commands (resources/commands.json): bases:new-file "Create new base" (the ribbon's
//    layout-list item), bases:insert "Insert new base" (a new base embedded at the cursor),
//    bases:copy-table "Copy table to clipboard", bases:change-view "Switch view...",
//    bases:add-view "Add view", bases:add-item "Add item";
//  - "New base" in the file explorer's folder menu and in the editor's context menu.
// The core plugin switch (Settings → Core plugins → Bases) takes the "bases:" commands, the ribbon
// item and the "bases" views away (core_plugins.cpp).

#include "ui/workspace/workspace.h"

namespace oc {

class MainWindow;
class TFile;
class VaultSession;

// The registration call for the application (after the window's command registry exists and before
// its layout is loaded): registers the view type, the embeds and the commands.
void registerBases(MainWindow* window);

// Obsidian's createNewBasesFile(): an empty "Untitled.base" (Untitled 1.base...) in `folder`, or in
// the folder new notes go to from the active file; opened in `mode` for renaming ({rename: "all"}).
TFile* createNewBase(VaultSession* session, Workspace::NewLeaf mode, const QString& folder = {}, bool useFolder = false,
                     bool open = true);

} // namespace oc
