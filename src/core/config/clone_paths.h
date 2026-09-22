#pragma once

#include <QString>

namespace oc {

// Where the clone keeps its own state. Never inside a vault (user decision: config fully separate from
// <vault>/.obsidian and nothing new written into the vault). All roots honour XDG_* env vars, which tests
// point into a scratch directory.
class ClonePaths {
public:
    // ~/.config/obsidian-cpp
    static QString configRoot();
    // ~/.local/share/obsidian-cpp
    static QString dataRoot();
    // ~/.cache/obsidian-cpp
    static QString cacheRoot();

    // ~/.config/obsidian-cpp/obsidian-cpp.json (vault registry + global options)
    static QString registryFile();
    // ~/.config/obsidian-cpp/vaults/<id>
    static QString vaultConfigDir(const QString& vaultId);
    // ~/.local/share/obsidian-cpp/<id>
    static QString vaultDataDir(const QString& vaultId);
    // ~/.cache/obsidian-cpp/<id>
    static QString vaultCacheDir(const QString& vaultId);

    // Real Obsidian's registry, read-only (vault picker suggestions).
    static QString obsidianRegistryFile();

    // Creates dir (and parents) if missing; returns path.
    static QString ensureDir(const QString& path);
};

} // namespace oc
