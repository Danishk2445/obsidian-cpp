#pragma once

// The canvas core plugin's settings tab (Obsidian's canvas options, stored in the clone's per-vault
// canvas.json): new file location and folder, wheel and Ctrl+Drag behavior, card names, snapping, and
// the zoom threshold for hiding card content. Open canvases follow changes (ConfigStore::changed).

namespace oc {

class PluginSettingsTab;
class SettingsModal;

PluginSettingsTab* createCanvasSettingsTab(SettingsModal& modal);

} // namespace oc
