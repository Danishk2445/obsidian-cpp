#include "core/vault/fs_scan.h"

#include "core/vault/ignore_rules.h"

#include <QByteArrayView>
#include <QFile>
#include <QSet>

#include <cerrno>
#include <chrono>
#include <deque>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace oc::vaultfs {

namespace {

constexpr unsigned kStatxMask =
    STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_MTIME | STATX_BTIME;

std::atomic<int> g_statxState{0}; // 0 = works/unknown, -1 = kernel without statx (ENOSYS)

bool doStat(int dirfd, const char* name, bool follow, DiskEntry* out, int* err)
{
    const int nofollow = follow ? 0 : AT_SYMLINK_NOFOLLOW;
    if (g_statxState.load(std::memory_order_relaxed) == 0) {
        struct statx stx {};
        if (::statx(dirfd, name, nofollow | AT_NO_AUTOMOUNT | AT_STATX_SYNC_AS_STAT, kStatxMask, &stx) == 0) {
            out->mode = stx.stx_mode;
            out->nlink = stx.stx_nlink;
            out->size = qint64(stx.stx_size);
            out->mtimeNs = qint64(stx.stx_mtime.tv_sec) * 1'000'000'000 + stx.stx_mtime.tv_nsec;
            out->btimeMs = (stx.stx_mask & STATX_BTIME)
                               ? qint64(stx.stx_btime.tv_sec) * 1000 + stx.stx_btime.tv_nsec / 1'000'000
                               : -1;
            out->id = {quint64(makedev(stx.stx_dev_major, stx.stx_dev_minor)), quint64(stx.stx_ino)};
            return true;
        }
        if (errno != ENOSYS) {
            *err = errno;
            return false;
        }
        g_statxState.store(-1, std::memory_order_relaxed);
    }
    struct stat st {};
    if (::fstatat(dirfd, name, &st, nofollow) != 0) {
        *err = errno;
        return false;
    }
    out->mode = st.st_mode;
    out->nlink = quint32(st.st_nlink);
    out->size = qint64(st.st_size);
    out->mtimeNs = qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
    out->btimeMs = -1;
    out->id = {quint64(st.st_dev), quint64(st.st_ino)};
    return true;
}

} // namespace

bool isTransientErrno(int e)
{
    return e != 0 && e != ENOENT && e != ENOTDIR && e != EACCES && e != EPERM && e != ELOOP && e != ENAMETOOLONG;
}

QByteArray encodeName(const QString& s) { return QFile::encodeName(s); }

std::optional<QString> decodeName(const char* name)
{
    const QByteArrayView v(name);
    if (!v.isValidUtf8())
        return std::nullopt;
    return QString::fromUtf8(v);
}

std::optional<DiskEntry> statEntryAt(int dirfd, const char* name, int* err)
{
    int e = 0;
    DiskEntry d;
    if (!doStat(dirfd, name, false, &d, &e)) {
        if (err)
            *err = e;
        return std::nullopt;
    }
    if (S_ISLNK(d.mode)) {
        if (!doStat(dirfd, name, true, &d, &e)) {
            if (err)
                *err = (e == ENOENT || e == ELOOP) ? ENOENT : e; // broken or looping symlink
            return std::nullopt;
        }
        d.isSymlink = true;
    }
    if (S_ISDIR(d.mode)) {
        d.isDir = true;
    } else if (!S_ISREG(d.mode)) {
        if (err)
            *err = 0; // fifo/socket/device: not something the vault tracks
        return std::nullopt;
    }
    if (err)
        *err = 0;
    return d;
}

std::optional<DiskEntry> statEntry(const QByteArray& absPath, int* err)
{
    return statEntryAt(AT_FDCWD, absPath.constData(), err);
}

bool walkTree(const QByteArray& absRoot, const QString& relStart, const WalkVisitor& visit,
              const WalkOptions& opts, int* err)
{
    struct Dir {
        QString rel;
        QByteArray abs;
    };
    auto cancelled = [&] { return opts.cancel && opts.cancel->load(std::memory_order_relaxed); };

    const QByteArray startAbs = relStart.isEmpty() ? absRoot : QByteArray(absRoot + '/' + encodeName(relStart));
    int e = 0;
    const std::optional<DiskEntry> startSt = statEntry(startAbs, &e);
    if (!startSt || !startSt->isDir) {
        if (err)
            *err = startSt ? ENOTDIR : (e ? e : ENOENT);
        return false;
    }

    QSet<DevIno> visited;
    visited.insert(startSt->id);
    std::vector<Dir> stack;
    std::deque<std::pair<Dir, DiskEntry>> deferred; // symlinked directories
    if (opts.onDir && !opts.onDir(relStart, startAbs, *startSt)) {
        if (err)
            *err = 0;
        return true;
    }
    stack.push_back({relStart, startAbs});
    bool isStart = true;

    while (true) {
        if (cancelled()) {
            if (err)
                *err = ECANCELED;
            return false;
        }
        Dir cur;
        if (!stack.empty()) {
            cur = std::move(stack.back());
            stack.pop_back();
        } else if (!deferred.empty()) {
            auto [dir, st] = std::move(deferred.front());
            deferred.pop_front();
            if (visited.contains(st.id))
                continue; // cycle, or an alias of a directory we already have
            if (opts.acceptSymlinkedDir && !opts.acceptSymlinkedDir(st.id))
                continue;
            visited.insert(st.id);
            visit(dir.rel, st);
            if (opts.onDir && !opts.onDir(dir.rel, dir.abs, st))
                continue;
            cur = std::move(dir);
        } else {
            break;
        }

        const int fd = ::open(cur.abs.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            const int oe = errno;
            if (isStart) {
                if (err)
                    *err = oe;
                return false;
            }
            if (isTransientErrno(oe) && opts.incomplete)
                *opts.incomplete = true;
            continue;
        }
        DIR* d = ::fdopendir(fd);
        if (!d) {
            const int oe = errno;
            ::close(fd);
            if (isStart) {
                if (err)
                    *err = oe;
                return false;
            }
            if (opts.incomplete)
                *opts.incomplete = true;
            continue;
        }
        const int dfd = ::dirfd(d);
        errno = 0;
        while (const dirent* de = ::readdir(d)) {
            const char* name = de->d_name;
            if (name[0] == '.')
                continue; // ".", "..", and every dot entry is ignored
            if (opts.dirsOnly && de->d_type != DT_DIR && de->d_type != DT_LNK && de->d_type != DT_UNKNOWN)
                continue;
            std::optional<QString> qname = decodeName(name);
            if (!qname || isIgnoredName(*qname)) {
                errno = 0;
                continue;
            }
            int se = 0;
            std::optional<DiskEntry> st = statEntryAt(dfd, name, &se);
            if (!st) {
                if (isTransientErrno(se) && opts.incomplete)
                    *opts.incomplete = true;
                errno = 0;
                continue;
            }
            QString rel = joinPath(cur.rel, *qname);
            if (st->isDir) {
                QByteArray childAbs = cur.abs + '/' + name;
                if (st->isSymlink) {
                    deferred.push_back({Dir{std::move(rel), std::move(childAbs)}, *st});
                } else if (!visited.contains(st->id)) { // (a bind mount of an ancestor would loop)
                    visited.insert(st->id);
                    visit(rel, *st);
                    if (!opts.onDir || opts.onDir(rel, childAbs, *st))
                        stack.push_back({std::move(rel), std::move(childAbs)});
                }
            } else if (!opts.dirsOnly) {
                visit(rel, *st);
            }
            errno = 0;
        }
        const int re = errno;
        ::closedir(d);
        if (re != 0) {
            if (isStart) {
                if (err)
                    *err = re;
                return false;
            }
            if (opts.incomplete)
                *opts.incomplete = true;
        }
        isStart = false;
    }
    if (err)
        *err = 0;
    return true;
}

RootProbe probeRoot(const QByteArray& absRoot)
{
    RootProbe p;
    int e = 0;
    const std::optional<DiskEntry> st = statEntry(absRoot, &e);
    if (!st || !st->isDir) {
        p.err = st ? ENOTDIR : (e ? e : ENOENT);
        return p;
    }
    p.st = *st;
    DIR* d = ::opendir(absRoot.constData());
    if (!d) {
        p.err = errno;
        return p;
    }
    errno = 0;
    while (const dirent* de = ::readdir(d)) {
        if (qstrcmp(de->d_name, ".") == 0 || qstrcmp(de->d_name, "..") == 0)
            continue;
        ++p.entries;
        break; // "non-empty" is all we need to know
    }
    const int re = errno;
    ::closedir(d);
    if (re != 0) {
        p.err = re;
        return p;
    }
    p.ok = true;
    return p;
}

bool isFuseOrNetworkFs(const QByteArray& absPath, quint64* magicOut)
{
    struct statfs sf {};
    if (::statfs(absPath.constData(), &sf) != 0)
        return false;
    const quint32 magic = quint32(sf.f_type);
    if (magicOut)
        *magicOut = magic;
    switch (magic) {
    case 0x65735546u: // FUSE (ntfs-3g, sshfs, rclone, ...)
    case 0x00006969u: // NFS
    case 0x0000517Bu: // SMB
    case 0xFF534D42u: // CIFS
    case 0xFE534D42u: // SMB2
    case 0x01021997u: // 9P (e.g. WSL/VM shares)
    case 0x00C36400u: // Ceph
    case 0x5346414Fu: // AFS
        return true;
    default:
        return false;
    }
}

qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace oc::vaultfs
