#pragma once

#include "core/recovery/snapshot_store.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <optional>

namespace oc {

// Obsidian's File recovery core plugin: when a note (md, canvas, base) is created, opened or
// modified, snapshot it unless its last snapshot is younger than the interval (then it is retried
// every minute); drop snapshots older than `keepDays` every hour. Options live in the clone config
// ("file-recovery.json": {"intervalMinutes": 5, "keepDays": 7}).
class FileRecovery : public QObject {
    Q_OBJECT
public:
    struct Options {
        int intervalMinutes = 5;
        int keepDays = 7;
    };
    using Reader = std::function<std::optional<QByteArray>(const QString& path)>;
    using Clock = std::function<qint64()>; // ms since epoch

    FileRecovery(SnapshotStore* store, Reader reader, QObject* parent = nullptr);

    Options options() const { return m_options; }
    void setOptions(const Options& o) { m_options = o; }
    void setClock(Clock clock) { m_clock = std::move(clock); } // tests
    void startTimers();                                          // resave every minute, cleanup hourly

    static bool isTracked(const QString& path); // .md, .canvas, .base

    // A tracked file was created, opened or modified.
    void fileChanged(const QString& path);
    // Snapshot these bytes now (before bulk or destructive operations).
    void forceAdd(const QString& path, const QByteArray& bytes);
    void resave();  // retry files that changed within the interval
    void cleanup(); // drop snapshots older than keepDays
    void clearAll();

    SnapshotStore* store() const { return m_store; }
    int pendingCount() const { return int(m_pending.size()); }

signals:
    void snapshotAdded(const QString& path);

private:
    qint64 now() const { return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch(); }

    SnapshotStore* m_store;
    Reader m_reader;
    Clock m_clock;
    Options m_options;
    QHash<QString, qint64> m_lastTs; // path -> ts of its latest snapshot (0 = none)
    QSet<QString> m_pending;
    QTimer m_resaveTimer;
    QTimer m_cleanupTimer;
};

} // namespace oc
