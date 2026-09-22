#include "core/documents/local_storage.h"

#include "text/codec.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oc {

LocalFileStorage::LocalFileStorage(QString rootDir)
    : m_root(QDir::cleanPath(std::move(rootDir)))
{
}

QString LocalFileStorage::absolutePath(const QString& path) const
{
    return QFileInfo(path).isAbsolute() ? path : m_root + QLatin1Char('/') + path;
}

DiskState LocalFileStorage::stat(const QString& path)
{
    DiskState s;
    struct ::stat st{};
    if (::stat(QFile::encodeName(absolutePath(path)).constData(), &st) != 0)
        return s;
    s.size = st.st_size;
    s.mtimeNs = qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
    return s;
}

std::optional<QByteArray> LocalFileStorage::read(const QString& path, DiskState* state, QString* error)
{
    QFile f(absolutePath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    QByteArray b = f.readAll();
    if (state) {
        *state = stat(path);
        state->hash = contentHash(b);
    }
    return b;
}

DocumentStorage::WriteResult LocalFileStorage::write(const QString& path, const QByteArray& bytes, const DiskState& expected,
                                                     DiskState* newState, QString* error)
{
    const QString abs = absolutePath(path);
    const DiskState cur = stat(path);
    if (expected.exists() != cur.exists() || (cur.exists() && (cur.size != expected.size || cur.mtimeNs != expected.mtimeNs)))
        return WriteResult::Conflict;
    const QFileInfo fi(abs);
    QDir().mkpath(fi.absolutePath());
    const QString tmp = fi.absolutePath() + QStringLiteral("/.") + fi.fileName() + QStringLiteral(".oc~%1.%2")
                                                                             .arg(QCoreApplication::applicationPid())
                                                                             .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    const QByteArray tmpPath = QFile::encodeName(tmp);
    const int fd = ::open(tmpPath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    bool ok = fd >= 0;
    if (ok) {
        qsizetype off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.constData() + off, size_t(bytes.size() - off));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            off += n;
        }
        ok = ok && ::fsync(fd) == 0;
        ::close(fd);
        if (ok && cur.exists()) {
            struct ::stat st{};
            if (::stat(QFile::encodeName(abs).constData(), &st) == 0)
                (void)::chmod(tmpPath.constData(), st.st_mode & 07777);
        }
        ok = ok && ::rename(tmpPath.constData(), QFile::encodeName(abs).constData()) == 0;
        if (!ok)
            ::unlink(tmpPath.constData());
    }
    if (!ok) {
        // Fall back to an in-place write (e.g. filesystems that refuse renames).
        QFile f(abs);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size()) {
            if (error)
                *error = f.errorString();
            return WriteResult::Error;
        }
        f.flush();
        ::fsync(f.handle());
    }
    if (newState) {
        *newState = stat(path);
        newState->hash = contentHash(bytes);
    }
    return WriteResult::Ok;
}

} // namespace oc
