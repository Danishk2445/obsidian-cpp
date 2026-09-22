#include "core/recovery/file_recovery.h"

#include "text/codec.h"

#include <QDateTime>

namespace oc {

FileRecovery::FileRecovery(SnapshotStore* store, Reader reader, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_reader(std::move(reader))
{
    m_resaveTimer.setInterval(60'000);
    connect(&m_resaveTimer, &QTimer::timeout, this, &FileRecovery::resave);
    m_cleanupTimer.setInterval(3'600'000);
    connect(&m_cleanupTimer, &QTimer::timeout, this, &FileRecovery::cleanup);
}

void FileRecovery::startTimers()
{
    m_resaveTimer.start();
    m_cleanupTimer.start();
    QTimer::singleShot(60'000, this, &FileRecovery::cleanup);
}

bool FileRecovery::isTracked(const QString& path)
{
    return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".canvas"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".base"), Qt::CaseInsensitive);
}

void FileRecovery::fileChanged(const QString& path)
{
    if (!isTracked(path) || !m_store || !m_store->isOpen())
        return;
    std::optional<SnapshotStore::Info> last;
    auto it = m_lastTs.constFind(path);
    qint64 lastTs = 0;
    if (it != m_lastTs.cend()) {
        lastTs = *it;
    } else {
        last = m_store->latest(path);
        lastTs = last ? last->ts : 0;
        m_lastTs.insert(path, lastTs);
    }
    const qint64 t = now();
    const int interval = m_options.intervalMinutes < 0 ? 5 : m_options.intervalMinutes;
    if (t - lastTs < qint64(interval) * 60'000) {
        m_pending.insert(path);
        return;
    }
    const std::optional<QByteArray> bytes = m_reader ? m_reader(path) : std::nullopt;
    if (!bytes)
        return;
    if (!last)
        last = m_store->latest(path);
    if (last && last->hash == contentHash(*bytes))
        return; // unchanged since the last snapshot
    if (m_store->add(path, t, *bytes)) {
        m_lastTs.insert(path, t);
        emit snapshotAdded(path);
    }
}

void FileRecovery::forceAdd(const QString& path, const QByteArray& bytes)
{
    if (!m_store || !m_store->isOpen())
        return;
    const qint64 t = now();
    if (m_store->add(path, t, bytes)) {
        m_lastTs.insert(path, t);
        emit snapshotAdded(path);
    }
}

void FileRecovery::resave()
{
    const QSet<QString> pending = std::exchange(m_pending, {});
    for (const QString& p : pending)
        fileChanged(p);
}

void FileRecovery::cleanup()
{
    if (!m_store || !m_store->isOpen())
        return;
    const int days = m_options.keepDays < 1 ? 7 : m_options.keepDays;
    if (m_store->removeOlderThan(now() - qint64(days) * 24 * 3600 * 1000) > 0)
        m_lastTs.clear(); // cached times may point at deleted snapshots
}

void FileRecovery::clearAll()
{
    if (m_store)
        m_store->clear();
    m_lastTs.clear();
    m_pending.clear();
}

} // namespace oc
