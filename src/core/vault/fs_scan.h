#pragma once

// Low-level, thread-safe filesystem helpers shared by the vault model (GUI thread) and the watcher
// backends (watcher thread): stat with symlink rules, a fast directory walker, root probing.
// Private to src/core/vault.

#include <QByteArray>
#include <QHash>
#include <QString>

#include <atomic>
#include <functional>
#include <optional>

namespace oc::vaultfs {

struct DevIno {
    quint64 dev = 0;
    quint64 ino = 0;
    friend bool operator==(const DevIno&, const DevIno&) = default;
};
inline size_t qHash(const DevIno& k, size_t seed = 0) { return qHashMulti(seed, k.dev, k.ino); }

struct DiskEntry {
    bool isDir = false;
    bool isSymlink = false; // the entry itself is a symlink; the other fields describe its target
    qint64 size = 0;
    qint64 mtimeNs = 0;
    qint64 btimeMs = -1; // birth time (statx), -1 if the filesystem doesn't report it
    quint32 mode = 0;
    quint32 nlink = 1;
    DevIno id;
};

QByteArray encodeName(const QString& s);
std::optional<QString> decodeName(const char* name); // nullopt if not valid UTF-8

// lstat; a symlink is followed (broken ones -> nullopt). Only regular files and directories are
// returned. *err = errno of the failing call (ENOENT when missing), 0 for unsupported types.
std::optional<DiskEntry> statEntry(const QByteArray& absPath, int* err = nullptr);
std::optional<DiskEntry> statEntryAt(int dirfd, const char* name, int* err = nullptr);

struct WalkOptions {
    const std::atomic<bool>* cancel = nullptr;
    // Consulted for symlinked directories (after the per-walk visited check): false = skip it.
    std::function<bool(const DevIno&)> acceptSymlinkedDir;
    // Called for each directory before its entries are read (including the start directory);
    // returning false skips the directory's contents.
    std::function<bool(const QString& relPath, const QByteArray& absPath, const DiskEntry&)> onDir;
    // Set to true when a subdirectory/entry couldn't be read for a transient reason (EIO, ENOTCONN,
    // ...): the result is then not a complete picture and must not be used to infer deletions.
    bool* incomplete = nullptr;
    // Only directories matter (watch setup): regular files known from readdir's d_type aren't stat'ed
    // nor visited.
    bool dirsOnly = false;
};

// Walks the directory `relStart` ("" = root) below `absRoot` and calls `visit` for every
// non-ignored descendant (parents before children; not for the start directory itself). Real
// directories are walked first; symlinked directories are deferred and followed only if their
// (dev, inode) wasn't seen in this walk (cycle/alias guard) and acceptSymlinkedDir allows it.
// Returns false if the start directory can't be opened (*err set) or the walk was cancelled.
using WalkVisitor = std::function<void(const QString& relPath, const DiskEntry&)>;
bool walkTree(const QByteArray& absRoot, const QString& relStart, const WalkVisitor& visit,
              const WalkOptions& opts = {}, int* err = nullptr);

// Is the root usable? stat + open + read. `entries` counts ALL entries (dot files too).
struct RootProbe {
    bool ok = false;
    int err = 0;
    DiskEntry st;
    int entries = 0;
};
RootProbe probeRoot(const QByteArray& absRoot);

// errno values that mean "can't tell right now" (EIO, ENOTCONN, ESTALE, ...) rather than "not there".
bool isTransientErrno(int e);

// Filesystem magic numbers of network/FUSE filesystems where inotify can miss changes. *known is
// set to false when statfs() fails (e.g. the folder isn't there): the answer is then meaningless.
bool isFuseOrNetworkFs(const QByteArray& absPath, quint64* magic = nullptr, bool* known = nullptr);

qint64 monotonicMs();

} // namespace oc::vaultfs
