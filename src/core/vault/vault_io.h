#pragma once

// File IO primitives for the vault (absolute paths): whole-file reads with a consistent stat,
// atomic writes with fallbacks, a symlink-safe recursive delete, and the registry of our own recent
// writes/renames/removals used to recognise their echo in watcher events. Private to src/core/vault.

#include "core/vault/fs_scan.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <functional>

#include <sys/types.h>

namespace oc::vaultfs {

class SelfWriteRegistry {
public:
    enum class Kind { Write, Removed, RenamedAway, RenamedHere };
    struct Entry {
        Kind kind = Kind::Write;
        qint64 size = -1;
        qint64 mtimeNs = 0;
        QByteArray hash;
        DevIno id;
        qint64 expiresMs = 0;
    };

    explicit SelfWriteRegistry(qint64 ttlMs = 3000) : m_ttlMs(ttlMs) {}
    void setTtl(qint64 ms) { m_ttlMs = ms; }

    void recordWrite(const QString& path, qint64 size, qint64 mtimeNs, const QByteArray& hash, DevIno id);
    void recordRemoved(const QString& path, DevIno id);
    void recordRename(const QString& from, const QString& to, DevIno id);

    const Entry* find(const QString& path) const; // live (non-expired) entry or nullptr
    // Is the file now on disk (size, mtime, inode; content hash computed lazily) the one we wrote?
    // Same size + mtime + inode is trusted; otherwise (attribute caching, coarse timestamps, a
    // same-tick replacement) the content hash decides.
    bool isOwnWrite(const QString& path, qint64 size, qint64 mtimeNs, const DevIno& id,
                    const std::function<QByteArray()>& diskHash) const;
    void expire();
    int count() const { return int(m_entries.size()); }

private:
    Entry& put(const QString& path, Kind kind);
    QHash<QString, Entry> m_entries;
    qint64 m_ttlMs;
};

QString errnoString(int e);

// Reads the whole file; *st describes the opened file (retries if it changes while being read).
bool readWholeFile(const QByteArray& abs, QByteArray* bytes, DiskEntry* st, int* err);

enum class WriteMode { Replace, CreateNew };
enum class IoResult { Ok, Exists, Error };

// Atomic write: ".<name>.oc~<pid>.<rand>" in the target's directory, write + fsync, rename over the
// target (CreateNew: never replaces an existing file), fsync the directory. Existing files keep
// their permission bits (and owner when permitted); symlinks are resolved so the link survives.
// Falls back to an in-place write when the temp file can't be created or the rename fails with
// EXDEV/EPERM/EACCES/EBUSY/ENOTSUP, and when the target has several hard links.
IoResult writeFileAtomic(const QByteArray& abs, const QByteArray& bytes, WriteMode mode, QString* error,
                         mode_t createMode = 0666, bool* usedFallback = nullptr);

// Rename that refuses to replace an existing target (renameat2 RENAME_NOREPLACE, with a
// check-then-rename fallback on filesystems without it). Returns 0 or an errno.
int renameNoReplace(const QByteArray& from, const QByteArray& to);

// Recursive delete that never follows symlinks (a symlinked folder loses only the link).
bool removeRecursively(const QByteArray& abs, QString* error);

QByteArray randomHex(int bytes);

} // namespace oc::vaultfs
