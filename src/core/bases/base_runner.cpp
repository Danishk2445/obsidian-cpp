#include "core/bases/base_runner.h"

#include "core/metadata/metadata_cache.h"
#include "core/util/busy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"

namespace oc::bases {

QueryRunner::QueryRunner(std::shared_ptr<DataSource> ds, QObject* parent)
    : QObject(parent)
    , m_ds(std::move(ds))
{
    m_scanTimer.setSingleShot(true);
    m_scanTimer.setInterval(0);
    connect(&m_scanTimer, &QTimer::timeout, this, &QueryRunner::scanSlice);
    m_notifyTimer.setSingleShot(true);
    m_notifyTimer.setInterval(50);
    connect(&m_notifyTimer, &QTimer::timeout, this, &QueryRunner::publish);
}

QueryRunner::~QueryRunner()
{
    for (const QMetaObject::Connection& c : std::as_const(m_connections))
        disconnect(c);
    holdBusy(false);
}

void QueryRunner::holdBusy(bool hold)
{
    if (hold == m_busy)
        return;
    m_busy = hold;
    if (hold)
        Busy::begin();
    else
        Busy::end();
}

void QueryRunner::followSession(VaultSession* session)
{
    for (const QMetaObject::Connection& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    if (!session)
        return;
    if (MetadataCache* mc = session->metadata()) {
        m_connections << connect(mc, &MetadataCache::changed, this, [this](const QString& path) { fileChanged(path); });
        m_connections << connect(mc, &MetadataCache::deleted, this, [this](const QString& path) {
            if (m_ds && !m_ds->stat(path))
                fileRemoved(path);
        });
    }
    if (Vault* v = session->vault()) {
        auto nonMarkdown = [](TAbstractFile* item) { return item && !item->isFolder() && !static_cast<TFile*>(item)->isMarkdown(); };
        m_connections << connect(v, &Vault::created, this, [this, nonMarkdown](TAbstractFile* item, bool) {
            if (nonMarkdown(item))
                fileChanged(item->path());
        });
        m_connections << connect(v, &Vault::modified, this, [this, nonMarkdown](TFile* f, bool) {
            if (nonMarkdown(f))
                fileChanged(f->path());
        });
        m_connections << connect(v, &Vault::renamed, this, [this](TAbstractFile* item, const QString& oldPath, bool) {
            if (item && !item->isFolder())
                fileRenamed(oldPath, item->path());
        });
        m_connections << connect(v, &Vault::deleted, this, [this](TAbstractFile* item, bool) {
            if (item && !item->isFolder())
                fileRemoved(item->path());
        });
    }
}

QString QueryRunner::queryState() const
{
    // Obsidian: JSON.stringify({filter, formulas}); here the serialized filter + formula texts.
    const BaseView* v = view();
    const std::optional<Filter> f = Filter::both(m_base.filters, v ? v->filters : std::nullopt);
    QString s = f ? dumpYaml(YamlNode::map({{QStringLiteral("f"), f->serialize()}})) : QString();
    for (const auto& [n, formula] : m_base.formulas)
        s += u'\n' + n + u'=' + formula->text();
    return s;
}

void QueryRunner::setQuery(const BaseFile& base, const QString& viewName)
{
    m_base = base;
    m_viewName = viewName;
    if (!m_base.view(m_viewName))
        m_viewName.clear();
    if (m_viewName.isEmpty() && !m_base.views.isEmpty())
        m_viewName = m_base.views.first().name;
    const QString state = queryState();
    if (state != m_state || !m_ctx) {
        m_state = state;
        startScan();
    } else {
        // the context keeps its formulas: sorting / grouping / columns only
        scheduleNotify();
    }
}

void QueryRunner::setThisFile(const QString& path)
{
    if (path == m_thisPath)
        return;
    m_thisPath = path;
    if (m_ctx)
        startScan();
}

void QueryRunner::setSearch(const QString& text)
{
    if (text == m_search)
        return;
    m_search = text;
    scheduleNotify();
}

void QueryRunner::startScan()
{
    m_ctx = QueryContext::create(m_ds.get(), m_base, view(), m_thisPath);
    m_results.clear();
    m_queue = m_ds ? m_ds->files() : QStringList();
    m_queued = QSet<QString>(m_queue.cbegin(), m_queue.cend());
    m_errors.clear();
    m_scanning = true;
    m_ready = false;
    m_localSeen = false;
    holdBusy(true);
    if (m_sync) {
        while (m_scanning)
            scanSlice();
        return;
    }
    m_scanTimer.start();
}

void QueryRunner::evaluate(const QString& path)
{
    m_results.remove(path);
    if (!m_ds || !m_ds->stat(path) || m_ds->isIgnored(path))
        return;
    auto entry = std::make_shared<Entry>(m_ctx, path);
    QString error;
    if (evaluateFile(m_ctx, entry, &error))
        m_results.insert(path, entry);
    else if (!error.isEmpty() && !m_errors.contains(error))
        m_errors << error;
}

void QueryRunner::scanSlice()
{
    QElapsedTimer clock;
    clock.start();
    while (!m_queue.isEmpty()) {
        const QString path = m_queue.takeFirst();
        m_queued.remove(path);
        // the "this" file changed after the scan saw it: its entry must be rebuilt (Obsidian's
        // regenerateLocal) when the results depend on it
        if (!m_thisPath.isEmpty() && path == m_thisPath) {
            if (m_localSeen && m_ctx && m_ctx->localUsed() && !m_scanning) {
                startScan();
                return;
            }
            m_localSeen = true;
        }
        evaluate(path);
        if (!m_sync && clock.elapsed() >= kSliceMs)
            break;
    }
    if (!m_queue.isEmpty()) {
        if (!m_sync)
            m_scanTimer.start();
        return;
    }
    if (m_scanning) {
        m_scanning = false;
        publish(); // the initial scan is complete
        return;
    }
    scheduleNotify();
}

void QueryRunner::fileChanged(const QString& path)
{
    if (!m_ctx)
        return;
    if (!m_queued.contains(path)) {
        m_queue << path;
        m_queued.insert(path);
    }
    if (m_scanning)
        return; // the scan picks it up
    holdBusy(true);
    if (m_sync) {
        scanSlice();
        return;
    }
    if (!m_scanTimer.isActive())
        m_scanTimer.start();
}

void QueryRunner::fileRemoved(const QString& path)
{
    m_queue.removeAll(path);
    m_queued.remove(path);
    if (m_results.remove(path) > 0 && !m_scanning)
        scheduleNotify();
}

void QueryRunner::fileRenamed(const QString& oldPath, const QString& newPath)
{
    fileRemoved(oldPath);
    if (m_thisPath == oldPath)
        m_thisPath = newPath;
    fileChanged(newPath);
}

void QueryRunner::rescan()
{
    if (m_ctx)
        startScan();
}

void QueryRunner::republish()
{
    if (!m_scanning)
        scheduleNotify();
}

void QueryRunner::scheduleNotify()
{
    if (m_scanning)
        return;
    holdBusy(true);
    if (m_sync) {
        publish();
        return;
    }
    m_notifyTimer.start();
}

void QueryRunner::publish()
{
    m_notifyTimer.stop();
    const BaseView* v = view();
    if (v) {
        m_data = buildViewData(m_base, *v, m_results.values(), m_search, m_propertyName);
    } else {
        m_data = ViewData();
    }
    m_ready = true;
    if (!m_scanning && m_queue.isEmpty())
        holdBusy(false);
    if (!m_errors.isEmpty()) {
        const QStringList errors = m_errors;
        m_errors.clear();
        emit errorsReported(errors);
    }
    emit updated();
}

} // namespace oc::bases
