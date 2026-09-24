#pragma once

// Small helpers shared by the quick switcher, the command palette and the search view.

#include "ui/workspace/workspace.h"

#include <QList>
#include <QRegularExpression>
#include <QString>

namespace oc {

class OverlayLayer;
class VaultConfig;
class VaultSession;

// Obsidian's Keymap.isModEvent(): Mod = new tab, Mod+Alt = split right, Mod+Alt+Shift = new window
// (a middle click arrives here with Ctrl added, see SuggestionList).
Workspace::NewLeaf newLeafForModifiers(Qt::KeyboardModifiers mods);

// "Excluded files" (app.json "userIgnoreFilters"): each filter is a path prefix, or a regular expression
// written as /…/. Excluded files are hidden from search and downranked in the quick switcher.
class UserIgnoreFilters {
public:
    UserIgnoreFilters() = default;
    explicit UserIgnoreFilters(const VaultConfig* config);
    bool isEmpty() const { return m_prefixes.isEmpty() && m_regexes.isEmpty(); }
    bool matches(const QString& path) const;

private:
    QStringList m_prefixes;
    QList<QRegularExpression> m_regexes;
};

// "Folder/Note.md" -> "Folder/Note"; other extensions are kept (Obsidian's display path).
QString displayPath(const QString& path);

// The vault path of the file shown in the active root leaf ("" without one): the source path for
// link resolution and new-note placement.
QString activeFilePath(VaultSession* session);

// Shows an Obsidian notice in the window that hosts `layer` (no-op without a MainWindow).
void showNotice(OverlayLayer* layer, const QString& message);

// Keeps key events inside a modal prompt: keys its input doesn't handle (e.g. Ctrl+O in the command palette)
// would otherwise propagate to the overlay layer, where the HotkeyDispatcher no longer sees the modal and runs
// the global command (opening a second prompt). Obsidian's modal scope swallows them.
void containKeyEvents(QWidget* modal);

} // namespace oc
