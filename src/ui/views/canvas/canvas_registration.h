#pragma once

// Obsidian's "canvas" core plugin: the canvas view for .canvas files and the plugin's commands.

#include "ui/workspace/workspace.h"

namespace oc {

class CommandRegistry;
class MainWindow;
class TFile;
class VaultSession;
class ViewRegistry;

// Registers the "canvas" view type and the "canvas" extension.
void registerCanvasView(ViewRegistry& registry);

// The plugin's commands: canvas:new-file ("Create new canvas", the ribbon's layout-dashboard item),
// canvas:export-as-image, canvas:jump-to-group, canvas:convert-to-file; the file explorer's
// "New canvas" folder menu item; and, per window, the plugin's rename handling: cards showing a
// renamed file (and group backgrounds) are updated in every canvas of the vault.
void registerCanvasCommands(CommandRegistry& registry, MainWindow* window);

// Obsidian's createNewCanvasFile(): an empty "Untitled.canvas" in the canvas plugin's "Default
// location for new canvas files" (vault root / next to `sourcePath` / a folder), opened with
// {"rename": "all"}. Returns the new file.
TFile* createNewCanvas(VaultSession* session, Workspace::NewLeaf mode, const QString& folder = {}, bool useFolder = false);

} // namespace oc
