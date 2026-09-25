#pragma once

// Live evaluation of one base view (Obsidian's query controller loop): the first scan goes over
// every vault file in time slices on the GUI thread (like Obsidian's batched queue; the UI stays
// responsive and a scan can be superseded), then only changed files are re-evaluated — metadata
// changes, created / modified non-markdown files, renames, deletions — and the view data is
// republished after a short debounce (50 ms, Obsidian's requestNotifyView).
//
// Changing the view's sort, grouping, columns or limit only republishes; changing the filters or
// formulas (or the "this" file) rescans. Filter evaluation errors are collected and reported once
// per publish (Obsidian shows them as notices).

#include "core/bases/base_query.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>

namespace oc {
class VaultSession;
}

namespace oc::bases {

class QueryRunner : public QObject {
    Q_OBJECT
public:
    explicit QueryRunner(std::shared_ptr<DataSource> ds, QObject* parent = nullptr);
    ~QueryRunner() override;

    // Follow the session's vault and metadata events (optional; tests may call the file*() methods).
    void followSession(VaultSession* session);
    // The vault's property names by lowercase key (column names), e.g. the property type manager's.
    void setPropertyNames(std::function<QString(const QString& lowerKey)> fn) { m_propertyName = std::move(fn); }
    // Tests: scans and updates complete synchronously.
    void setSynchronous(bool sync) { m_sync = sync; }
    void setDebounceMs(int ms) { m_notifyTimer.setInterval(ms); }

    // The base and the view to evaluate ("" = the first view).
    void setQuery(const BaseFile& base, const QString& viewName);
    void setThisFile(const QString& path);
    void setSearch(const QString& text);

    const BaseFile& base() const { return m_base; }
    const BaseView* view() const { return m_base.view(m_viewName); }
    QString viewName() const { return m_viewName; }
    QString thisFile() const { return m_thisPath; }
    QString search() const { return m_search; }
    const DataSource* dataSource() const { return m_ds.get(); }
    std::shared_ptr<DataSource> dataSourcePtr() const { return m_ds; }

    bool isScanning() const { return m_scanning; }
    bool isReady() const { return m_ready; } // data() reflects a complete scan
    const ViewData& data() const { return m_data; }
    int resultCount() const { return int(m_results.size()); }
    QList<EntryPtr> results() const { return m_results.values(); }
    EntryPtr entry(const QString& path) const { return m_results.value(path); }
    std::shared_ptr<QueryContext> context() const { return m_ctx; }

    // Vault events.
    void fileChanged(const QString& path);
    void fileRemoved(const QString& path);
    void fileRenamed(const QString& oldPath, const QString& newPath);
    void rescan();
    // Republish now (e.g. the property types changed).
    void republish();

    // Scan budget per event loop turn.
    static constexpr int kSliceMs = 12;

signals:
    void updated();                                 // data() changed
    void errorsReported(const QStringList& messages); // filter errors since the last publish

private:
    void startScan();
    void scanSlice();
    void evaluate(const QString& path);
    void scheduleNotify();
    void publish();
    void holdBusy(bool hold);
    QString queryState() const;

    std::shared_ptr<DataSource> m_ds;
    std::function<QString(const QString&)> m_propertyName;
    BaseFile m_base;
    QString m_viewName;
    QString m_thisPath;
    QString m_search;
    QString m_state; // filters + formulas the current results were computed with
    std::shared_ptr<QueryContext> m_ctx;
    QHash<QString, EntryPtr> m_results;
    QStringList m_queue;
    QSet<QString> m_queued;
    QStringList m_errors;
    ViewData m_data;
    QTimer m_scanTimer;
    QTimer m_notifyTimer;
    bool m_sync = false;
    bool m_scanning = false;
    bool m_ready = false;
    bool m_busy = false;
    bool m_localSeen = false;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace oc::bases
