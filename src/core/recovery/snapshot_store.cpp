#include "core/recovery/snapshot_store.h"

#include "text/codec.h"

#include <QDir>
#include <QFileInfo>

#include <sqlite3.h>
#include <zstd.h>

namespace oc {

namespace {
// RAII prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql)
    {
        if (db && sqlite3_prepare_v2(db, sql, -1, &m_s, nullptr) != SQLITE_OK)
            m_s = nullptr;
    }
    ~Stmt() { sqlite3_finalize(m_s); }
    explicit operator bool() const { return m_s != nullptr; }
    sqlite3_stmt* get() const { return m_s; }
    void bind(int i, const QString& s)
    {
        const QByteArray u = s.toUtf8();
        sqlite3_bind_text(m_s, i, u.constData(), int(u.size()), SQLITE_TRANSIENT);
    }
    void bind(int i, const QByteArray& b) { sqlite3_bind_blob(m_s, i, b.constData(), int(b.size()), SQLITE_TRANSIENT); }
    void bind(int i, qint64 v) { sqlite3_bind_int64(m_s, i, v); }
    bool step() { return sqlite3_step(m_s) == SQLITE_ROW; }
    bool exec() { return sqlite3_step(m_s) == SQLITE_DONE; }
    qint64 i64(int c) const { return sqlite3_column_int64(m_s, c); }
    QString text(int c) const
    {
        return QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(m_s, c)), sqlite3_column_bytes(m_s, c));
    }
    QByteArray blob(int c) const
    {
        return QByteArray(static_cast<const char*>(sqlite3_column_blob(m_s, c)), sqlite3_column_bytes(m_s, c));
    }

private:
    sqlite3_stmt* m_s = nullptr;
};

QByteArray compress(const QByteArray& in)
{
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(in.size()))), Qt::Uninitialized);
    const size_t n = ZSTD_compress(out.data(), size_t(out.size()), in.constData(), size_t(in.size()), 3);
    if (ZSTD_isError(n))
        return {};
    out.resize(qsizetype(n));
    return out;
}

std::optional<QByteArray> decompress(const QByteArray& in, qint64 size)
{
    QByteArray out(qsizetype(size), Qt::Uninitialized);
    const size_t n = ZSTD_decompress(out.data(), size_t(out.size()), in.constData(), size_t(in.size()));
    if (ZSTD_isError(n) || qint64(n) != size)
        return std::nullopt;
    return out;
}

SnapshotStore::Info readInfo(const Stmt& s)
{
    SnapshotStore::Info i;
    i.id = s.i64(0);
    i.path = s.text(1);
    i.ts = s.i64(2);
    i.size = s.i64(3);
    i.hash = s.blob(4);
    return i;
}
} // namespace

SnapshotStore::SnapshotStore(QString dbPath)
    : m_path(std::move(dbPath))
{
}

SnapshotStore::~SnapshotStore()
{
    if (m_db)
        sqlite3_close(m_db);
}

bool SnapshotStore::open(QString* error)
{
    if (m_db)
        return true;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    if (sqlite3_open_v2(QFile::encodeName(m_path).constData(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(m_db, 2000);
    const char* schema = "PRAGMA journal_mode=WAL;"
                         "PRAGMA synchronous=NORMAL;"
                         "CREATE TABLE IF NOT EXISTS snapshots("
                         " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         " path TEXT NOT NULL, ts INTEGER NOT NULL, size INTEGER NOT NULL,"
                         " hash BLOB NOT NULL, data BLOB NOT NULL);"
                         "CREATE INDEX IF NOT EXISTS snapshots_path ON snapshots(path, ts);"
                         "CREATE INDEX IF NOT EXISTS snapshots_ts ON snapshots(ts);";
    char* msg = nullptr;
    if (sqlite3_exec(m_db, schema, nullptr, nullptr, &msg) != SQLITE_OK) {
        if (error)
            *error = QString::fromUtf8(msg ? msg : "schema error");
        sqlite3_free(msg);
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    return true;
}

bool SnapshotStore::add(const QString& path, qint64 tsMs, const QByteArray& bytes)
{
    Stmt s(m_db, "INSERT INTO snapshots(path, ts, size, hash, data) VALUES(?,?,?,?,?)");
    if (!s)
        return false;
    const QByteArray packed = compress(bytes);
    if (packed.isEmpty() && !bytes.isEmpty())
        return false;
    s.bind(1, path);
    s.bind(2, tsMs);
    s.bind(3, qint64(bytes.size()));
    s.bind(4, contentHash(bytes));
    s.bind(5, packed);
    return s.exec();
}

std::optional<SnapshotStore::Info> SnapshotStore::latest(const QString& path) const
{
    Stmt s(m_db, "SELECT id, path, ts, size, hash FROM snapshots WHERE path = ? ORDER BY ts DESC, id DESC LIMIT 1");
    if (!s)
        return std::nullopt;
    s.bind(1, path);
    if (!s.step())
        return std::nullopt;
    return readInfo(s);
}

QList<SnapshotStore::Info> SnapshotStore::history(const QString& path) const
{
    QList<Info> out;
    Stmt s(m_db, "SELECT id, path, ts, size, hash FROM snapshots WHERE path = ? ORDER BY ts DESC, id DESC");
    if (!s)
        return out;
    s.bind(1, path);
    while (s.step())
        out.append(readInfo(s));
    return out;
}

QStringList SnapshotStore::paths() const
{
    QStringList out;
    Stmt s(m_db, "SELECT path, MAX(ts) AS last FROM snapshots GROUP BY path ORDER BY last DESC");
    if (!s)
        return out;
    while (s.step())
        out.append(s.text(0));
    return out;
}

std::optional<QByteArray> SnapshotStore::bytes(qint64 id) const
{
    Stmt s(m_db, "SELECT size, data FROM snapshots WHERE id = ?");
    if (!s)
        return std::nullopt;
    s.bind(1, id);
    if (!s.step())
        return std::nullopt;
    const qint64 size = s.i64(0);
    if (size == 0)
        return QByteArray();
    return decompress(s.blob(1), size);
}

int SnapshotStore::removeOlderThan(qint64 tsMs)
{
    Stmt s(m_db, "DELETE FROM snapshots WHERE ts < ?");
    if (!s)
        return 0;
    s.bind(1, tsMs);
    return s.exec() ? sqlite3_changes(m_db) : 0;
}

void SnapshotStore::clear()
{
    if (m_db)
        sqlite3_exec(m_db, "DELETE FROM snapshots", nullptr, nullptr, nullptr);
}

int SnapshotStore::count() const
{
    Stmt s(m_db, "SELECT COUNT(*) FROM snapshots");
    return s && s.step() ? int(s.i64(0)) : 0;
}

} // namespace oc
