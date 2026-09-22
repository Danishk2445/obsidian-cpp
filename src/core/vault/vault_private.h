#pragma once

// Tunables and test hooks for oc::Vault (not part of the frozen interface). Production code only
// needs vault.h; tests and diagnostics may include this to force a watcher backend, shorten timers
// or inspect the watcher state.

#include <QString>

namespace oc {

class Vault;

struct VaultTuning {
    // "" = automatic: HybridWatcher on FUSE/network filesystems, InotifyWatcher otherwise (decided
    // again when a vault that was unavailable at load() comes back, e.g. a drive plugged in later).
    // "inotify" | "hybrid" | "poll" | "none". Environment: OC_VAULT_WATCHER.
    QString watcher;
    // Tests: what automatic mode concludes for the vault folder's filesystem whenever it can be
    // examined ("inotify" | "hybrid" | "poll"), instead of asking statfs(). "" = really detect.
    QString simulatedFsBackend;
    int pollIntervalMs = 3000;  // PollingWatcher sweeps (and inotify -> polling fallback). OC_VAULT_POLL_MS
    int hybridSweepMs = 15000;  // HybridWatcher verification sweeps. OC_VAULT_SWEEP_MS
    int offlinePollMs = 2000;   // how often an offline vault checks whether its folder is back
    int coalesceMs = 75;        // quiet time before a burst of watcher events is processed
    int coalesceMaxMs = 300;    // ... but never later than this after the first pending event
    int selfWriteTtlMs = 3000;  // how long our own writes/renames/removals are remembered
    double massDeleteFraction = 0.2; // mass-delete guard: more than this share of the files ...
    int massDeleteMinFiles = 20;     // ... and more than this many files

    static VaultTuning fromEnvironment();
};

struct VaultImplAccess {
    // Must be called before Vault::load() for watcher settings to take effect.
    static VaultTuning& tuning(Vault& v);
    // "inotify", "hybrid", "poll" or "none"; "+poll" is appended after an inotify -> polling fallback.
    static QString watcherBackend(const Vault& v);
    // Number of processed watcher passes (coalesced event batches / sweeps).
    static int watcherPasses(const Vault& v);
    // Drop incoming watcher events (not sweep snapshots) to simulate missed notifications.
    static void setDropWatcherEvents(Vault& v, bool drop);
    // Entries currently remembered by the self-write registry.
    static int selfWriteEntries(const Vault& v);
};

} // namespace oc
