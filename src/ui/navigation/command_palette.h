#pragma once

#include "core/util/match_range.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace oc {

class CommandRegistry;
class OverlayLayer;
class SuggestModal;
class VaultSession;

// Obsidian's command-palette.json: {"pinned": ["command-id", ...]} (in the clone's per-vault config).
struct CommandPaletteOptions {
    QStringList pinned;

    static CommandPaletteOptions fromJson(const QJsonObject& json);
    QJsonObject toJson() const;
};

struct PaletteItem {
    QString id;
    QString name;              // "Prefix: Name" for plugin commands (Obsidian prefixes them with the plugin name)
    QList<MatchRange> matches; // fuzzy highlights in `name`
    double score = 0;
    bool pinned = false;
};

// Obsidian's command palette suggestions over the commands that are available right now (isAvailable) and
// have a name: with an empty query the pinned commands (in pinned order), then the recent ones (most recent
// first), then all others alphabetically; otherwise fuzzy matches of the name, best first (ties alphabetical).
QList<PaletteItem> commandPaletteItems(const QString& query, const CommandRegistry& registry,
                                       const CommandPaletteOptions& options = {});

// Opens the command palette (command-palette:open, Mod+P) in `layer`: "Select a command..." with hotkey chips and
// the pin flair; the chosen command runs through CommandRegistry::execute after the palette closed and focus
// returned to where it was. Returns the already open palette when there is one.
SuggestModal* openCommandPalette(VaultSession* session, OverlayLayer* layer);

// Recently used commands survive restarts: loads them into the registry and saves them (debounced) when they
// change, in the clone's per-vault data dir (<data>/<vault id>/recent-commands.json), never in the vault.
void persistRecentCommands(CommandRegistry& registry, VaultSession* session);

} // namespace oc
