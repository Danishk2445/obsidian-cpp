#pragma once

// Test-only DocumentStorage over a plain directory (the real one is Vault).
#include "core/vault/vault.h"
#include "text/codec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

namespace oc::test {

class DirStorage : public DocumentStorage {
public:
    explicit DirStorage(QString root) : m_root(std::move(root)) {}
    QString abs(const QString& p) const { return m_root + QLatin1Char('/') + p; }

    DiskState stat(const QString& path) override
    {
        struct ::stat st{};
        DiskState s;
        if (::stat(QFile::encodeName(abs(path)).constData(), &st) != 0)
            return s;
        s.size = st.st_size;
        s.mtimeNs = qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        return s;
    }
    std::optional<QByteArray> read(const QString& path, DiskState* state, QString* error) override
    {
        QFile f(abs(path));
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
        ++reads;
        return b;
    }
    WriteResult write(const QString& path, const QByteArray& bytes, const DiskState& expected, DiskState* newState, QString* error) override
    {
        const DiskState cur = stat(path);
        if (expected.exists() != cur.exists() || (cur.exists() && (cur.size != expected.size || cur.mtimeNs != expected.mtimeNs)))
            return WriteResult::Conflict;
        QSaveFile f(abs(path));
        if (!f.open(QIODevice::WriteOnly)) {
            if (error)
                *error = f.errorString();
            return WriteResult::Error;
        }
        f.write(bytes);
        if (!f.commit())
            return WriteResult::Error;
        ++writes;
        if (newState) {
            *newState = stat(path);
            newState->hash = contentHash(bytes);
        }
        return WriteResult::Ok;
    }

    // Simulate an external editor writing the file (bumps mtime to guarantee a visible change).
    void externalWrite(const QString& path, const QByteArray& bytes)
    {
        QFile f(abs(path));
        (void)f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(bytes);
        f.close();
        struct timespec ts[2];
        ::clock_gettime(CLOCK_REALTIME, &ts[0]);
        ts[0].tv_sec += ++m_bump;
        ts[1] = ts[0];
        ::utimensat(AT_FDCWD, QFile::encodeName(abs(path)).constData(), ts, 0);
    }
    QByteArray bytes(const QString& path) const
    {
        QFile f(abs(path));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    int reads = 0;
    int writes = 0;

private:
    QString m_root;
    int m_bump = 0;
};

} // namespace oc::test
