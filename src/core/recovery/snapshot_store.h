#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

struct sqlite3;

namespace oc {

// Storage for File recovery snapshots, outside the vault: a SQLite database (in the clone's per-vault
// data dir) holding each snapshot's path, time and the file's exact bytes, zstd-compressed.
class SnapshotStore {
public:
    struct Info {
        qint64 id = 0;
        QString path;
        qint64 ts = 0;      // ms since epoch
        qint64 size = 0;    // uncompressed bytes
        QByteArray hash;    // contentHash() of the bytes
    };

    explicit SnapshotStore(QString dbPath);
    ~SnapshotStore();
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    bool open(QString* error = nullptr); // creates the file and schema if needed
    bool isOpen() const { return m_db != nullptr; }
    const QString& path() const { return m_path; }

    bool add(const QString& path, qint64 tsMs, const QByteArray& bytes);
    std::optional<Info> latest(const QString& path) const;
    QList<Info> history(const QString& path) const; // newest first
    QStringList paths() const;                      // files with snapshots, most recently saved first
    std::optional<QByteArray> bytes(qint64 id) const;
    int removeOlderThan(qint64 tsMs);               // returns how many were deleted
    void clear();
    int count() const;

private:
    QString m_path;
    sqlite3* m_db = nullptr;
};

} // namespace oc
