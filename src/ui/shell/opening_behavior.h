#pragma once

class QString;

namespace oc {

class MainWindow;

// Obsidian's runOpeningBehavior(): Files & links → "Default file to open" (app.json "openBehavior"), run once the
// layout is restored unless the launch opens something itself. "" keeps the restored tabs; "new" creates a note in
// the current tab (file-explorer:new-file-in-current-tab) unless the active note is empty; "daily" opens today's
// daily note (command "daily-notes", when the plugin is on); "file:<path>" opens that file in the current tab
// (".md" added when the path has no extension, matched case-insensitively).
void runOpeningBehavior(MainWindow* window, const QString& behavior);

} // namespace oc
