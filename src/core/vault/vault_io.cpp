#include "core/vault/vault_io.h"

#include <QRandomGenerator>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oc::vaultfs {

// ---------------------------------------------------------------------------------------------
// SelfWriteRegistry

SelfWriteRegistry::Entry& SelfWriteRegistry::put(const QString& path, Kind kind)
{
    if (m_entries.size() >= 1024)
        expire(); // normally done by every watcher pass; this bounds it without a watcher too
    Entry& e = m_entries[path];
    e = Entry{};
    e.kind = kind;
    e.expiresMs = monotonicMs() + m_ttlMs;
    return e;
}

void SelfWriteRegistry::recordWrite(const QString& path, qint64 size, qint64 mtimeNs, const QByteArray& hash, DevIno id)
{
    Entry& e = put(path, Kind::Write);
    e.size = size;
    e.mtimeNs = mtimeNs;
    e.hash = hash;
    e.id = id;
}

void SelfWriteRegistry::recordRemoved(const QString& path, DevIno id) { put(path, Kind::Removed).id = id; }

void SelfWriteRegistry::recordRename(const QString& from, const QString& to, DevIno id)
{
    put(from, Kind::RenamedAway).id = id;
    put(to, Kind::RenamedHere).id = id;
}

const SelfWriteRegistry::Entry* SelfWriteRegistry::find(const QString& path) const
{
    const auto it = m_entries.constFind(path);
    if (it == m_entries.cend() || it->expiresMs < monotonicMs())
        return nullptr;
    return &*it;
}

bool SelfWriteRegistry::isOwnWrite(const QString& path, qint64 size, qint64 mtimeNs,
                                   const std::function<QByteArray()>& diskHash) const
{
    const Entry* e = find(path);
    if (!e || e->kind != Kind::Write || e->size != size)
        return false;
    if (e->mtimeNs == mtimeNs)
        return true;
    return !e->hash.isEmpty() && diskHash && diskHash() == e->hash;
}

void SelfWriteRegistry::expire()
{
    const qint64 now = monotonicMs();
    m_entries.removeIf([now](QHash<QString, Entry>::iterator it) { return it->expiresMs < now; });
}

// ---------------------------------------------------------------------------------------------

QString errnoString(int e) { return QString::fromStdString(std::generic_category().message(e)); }

QByteArray randomHex(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; i += 4) {
        const quint32 v = QRandomGenerator::global()->generate();
        for (int k = 0; k < 4 && i + k < bytes; ++k)
            raw[i + k] = char((v >> (8 * k)) & 0xFF);
    }
    return raw.toHex();
}

namespace {

int writeAll(int fd, const QByteArray& bytes)
{
    const char* p = bytes.constData();
    qint64 left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, size_t(left));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        p += n;
        left -= n;
    }
    return 0;
}

int syncAndClose(int fd)
{
    int e = 0;
    if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS && errno != ENOTSUP)
        e = errno;
    if (::close(fd) != 0 && e == 0 && errno != EINTR)
        e = errno;
    return e;
}

void fsyncDir(const QByteArray& dir)
{
    const int fd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)::fsync(fd); // not supported everywhere (EINVAL on some FUSE filesystems): best effort
    ::close(fd);
}

bool isFallbackErrno(int e)
{
    return e == EXDEV || e == EPERM || e == EACCES || e == EBUSY || e == ENOTSUP || e == EOPNOTSUPP || e == ETXTBSY
           || e == ENAMETOOLONG;
}

} // namespace

bool readWholeFile(const QByteArray& abs, QByteArray* bytes, DiskEntry* st, int* err)
{
    for (int attempt = 0;; ++attempt) {
        const int fd = ::open(abs.constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            *err = errno;
            return false;
        }
        struct stat a {};
        if (::fstat(fd, &a) != 0) {
            *err = errno;
            ::close(fd);
            return false;
        }
        if (S_ISDIR(a.st_mode)) {
            ::close(fd);
            *err = EISDIR;
            return false;
        }
        QByteArray buf;
        buf.resize(qsizetype(a.st_size) + 1); // +1: EOF is detected without growing
        qsizetype got = 0;
        int readErr = 0;
        while (true) {
            if (got == buf.size())
                buf.resize(buf.size() * 2); // the file grew while we read it
            const ssize_t n = ::read(fd, buf.data() + got, size_t(buf.size() - got));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                readErr = errno;
                break;
            }
            if (n == 0)
                break;
            got += n;
        }
        struct stat b {};
        const bool statOk = ::fstat(fd, &b) == 0;
        ::close(fd);
        if (readErr != 0 || !statOk) {
            *err = readErr ? readErr : EIO;
            return false;
        }
        buf.resize(got);
        const qint64 mtimeA = qint64(a.st_mtim.tv_sec) * 1'000'000'000 + a.st_mtim.tv_nsec;
        const qint64 mtimeB = qint64(b.st_mtim.tv_sec) * 1'000'000'000 + b.st_mtim.tv_nsec;
        const bool stable = a.st_size == b.st_size && mtimeA == mtimeB && got == qsizetype(b.st_size);
        if (stable || attempt >= 2) {
            *bytes = std::move(buf);
            st->isDir = false;
            st->isSymlink = false;
            st->size = got;
            st->mtimeNs = mtimeB;
            st->btimeMs = -1;
            st->mode = b.st_mode;
            st->nlink = quint32(b.st_nlink);
            st->id = {quint64(b.st_dev), quint64(b.st_ino)};
            *err = 0;
            return true;
        }
    }
}

int renameNoReplace(const QByteArray& from, const QByteArray& to)
{
    if (::renameat2(AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    const int e = errno;
    if (e != EINVAL && e != ENOSYS && e != ENOTSUP && e != EOPNOTSUPP)
        return e;
    // The filesystem can't do RENAME_NOREPLACE (e.g. FUSE daemons without rename2): check, then rename.
    struct stat st {};
    if (::lstat(to.constData(), &st) == 0)
        return EEXIST;
    return ::rename(from.constData(), to.constData()) == 0 ? 0 : errno;
}

IoResult writeFileAtomic(const QByteArray& absIn, const QByteArray& bytes, WriteMode mode, QString* error,
                         mode_t createMode, bool* usedFallback)
{
    auto fail = [&](const char* what, int e) {
        if (error)
            *error = QString::fromLatin1(what) + QStringLiteral(": ") + errnoString(e);
        return IoResult::Error;
    };
    if (usedFallback)
        *usedFallback = false;

    QByteArray target = absIn;
    bool targetExists = false;
    struct stat cur {};
    struct stat lst {};
    if (::lstat(absIn.constData(), &lst) == 0) {
        if (mode == WriteMode::CreateNew)
            return IoResult::Exists;
        if (S_ISLNK(lst.st_mode)) {
            // Write through the link: replace its target, keep the link itself.
            char* real = ::realpath(absIn.constData(), nullptr);
            if (!real)
                return fail("cannot resolve symlink", errno);
            target = QByteArray(real);
            ::free(real);
        }
        if (::stat(target.constData(), &cur) == 0) {
            if (S_ISDIR(cur.st_mode))
                return fail("cannot write", EISDIR);
            targetExists = true;
        } else if (errno != ENOENT) {
            return fail("stat failed", errno);
        }
    } else if (errno != ENOENT) {
        return fail("stat failed", errno);
    }

    const qsizetype slash = target.lastIndexOf('/');
    const QByteArray dir = slash > 0 ? target.left(slash) : QByteArray("/");
    const QByteArray name = target.mid(slash + 1);
    const QByteArray pid = QByteArray::number(qint64(::getpid()));
    QByteArray tmpName = '.' + name + ".oc~" + pid + '.' + randomHex(4);
    if (tmpName.size() > 250) // NAME_MAX
        tmpName = ".oc~" + pid + '.' + randomHex(8);
    const QByteArray tmp = dir + '/' + tmpName;

    // Hard-linked files are written in place so every link sees the new content.
    const bool inPlaceOnly = targetExists && cur.st_nlink > 1;
    if (!inPlaceOnly) {
        const mode_t perm = targetExists ? mode_t(cur.st_mode & 07777) : createMode;
        const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, targetExists ? 0600 : perm);
        if (fd >= 0) {
            int e = writeAll(fd, bytes);
            if (e == 0 && targetExists) {
                (void)::fchmod(fd, perm); // no-ops/failures on ntfs-3g & co. are fine
                if (cur.st_uid != ::geteuid() || cur.st_gid != ::getegid())
                    (void)::fchown(fd, cur.st_uid, cur.st_gid);
            }
            const int ce = syncAndClose(fd);
            if (e == 0)
                e = ce;
            if (e != 0) {
                ::unlink(tmp.constData());
                return fail("write failed", e);
            }
            int re = 0;
            if (mode == WriteMode::CreateNew)
                re = renameNoReplace(tmp, target);
            else if (::rename(tmp.constData(), target.constData()) != 0)
                re = errno;
            if (re == 0) {
                fsyncDir(dir);
                return IoResult::Ok;
            }
            ::unlink(tmp.constData());
            if (re == EEXIST && mode == WriteMode::CreateNew)
                return IoResult::Exists;
            if (!isFallbackErrno(re))
                return fail("rename failed", re);
        } else if (!isFallbackErrno(errno)) {
            return fail("cannot create temporary file", errno);
        }
    }

    // In-place fallback.
    if (usedFallback)
        *usedFallback = true;
    const int flags = O_WRONLY | O_CLOEXEC | O_CREAT | (mode == WriteMode::CreateNew ? O_EXCL : O_TRUNC);
    const int fd = ::open(target.constData(), flags, createMode);
    if (fd < 0) {
        if (errno == EEXIST)
            return IoResult::Exists;
        return fail("cannot open for writing", errno);
    }
    int e = writeAll(fd, bytes);
    const int ce = syncAndClose(fd);
    if (e == 0)
        e = ce;
    if (e != 0)
        return fail("write failed", e);
    if (!targetExists)
        fsyncDir(dir);
    return IoResult::Ok;
}

namespace {

int removeTreeAt(int parentFd, const char* name)
{
    struct stat st {};
    if (::fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(st.st_mode)) {
        if (::unlinkat(parentFd, name, 0) == 0 || errno == ENOENT)
            return 0;
        return errno;
    }
    const int fd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno;
    DIR* d = ::fdopendir(fd);
    if (!d) {
        const int e = errno;
        ::close(fd);
        return e;
    }
    std::vector<QByteArray> names;
    while (const dirent* de = ::readdir(d)) {
        if (qstrcmp(de->d_name, ".") == 0 || qstrcmp(de->d_name, "..") == 0)
            continue;
        names.emplace_back(de->d_name);
    }
    int firstErr = 0;
    for (const QByteArray& n : names) {
        const int e = removeTreeAt(::dirfd(d), n.constData());
        if (e != 0 && firstErr == 0)
            firstErr = e;
    }
    ::closedir(d);
    if (firstErr != 0)
        return firstErr;
    if (::unlinkat(parentFd, name, AT_REMOVEDIR) == 0 || errno == ENOENT)
        return 0;
    return errno;
}

} // namespace

bool removeRecursively(const QByteArray& abs, QString* error)
{
    const qsizetype slash = abs.lastIndexOf('/');
    const QByteArray dir = slash > 0 ? abs.left(slash) : QByteArray("/");
    const QByteArray name = abs.mid(slash + 1);
    const int dfd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        if (error)
            *error = QStringLiteral("cannot open folder: ") + errnoString(errno);
        return false;
    }
    const int e = removeTreeAt(dfd, name.constData());
    ::close(dfd);
    if (e != 0) {
        if (error)
            *error = QStringLiteral("delete failed: ") + errnoString(e);
        return false;
    }
    return true;
}

} // namespace oc::vaultfs
