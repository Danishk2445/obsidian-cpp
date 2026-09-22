#include "core/documents/fold_store.h"

#include "core/config/json_file.h"

#include <QJsonArray>
#include <QJsonObject>

namespace oc {

FoldStore::FoldStore(QString file, QObject* parent)
    : QObject(parent)
    , m_file(std::move(file))
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &FoldStore::flush);
}

FoldStore::~FoldStore() { flush(); }

void FoldStore::load() const
{
    if (m_loaded)
        return;
    m_loaded = true;
    const QJsonObject root = JsonFile::read(m_file);
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.lines = o.value(QStringLiteral("lines")).toInt();
        for (const QJsonValue& f : o.value(QStringLiteral("folds")).toArray()) {
            const QJsonObject fo = f.toObject();
            e.folds.append({fo.value(QStringLiteral("from")).toInt(), fo.value(QStringLiteral("to")).toInt()});
        }
        m_entries.insert(it.key(), e);
    }
}

QList<QPair<int, int>> FoldStore::folds(const QString& path, int lineCount) const
{
    load();
    const auto it = m_entries.constFind(path);
    if (it == m_entries.cend() || it->lines != lineCount)
        return {};
    return it->folds;
}

void FoldStore::setFolds(const QString& path, const QList<QPair<int, int>>& folds, int lineCount)
{
    load();
    if (folds.isEmpty()) {
        if (!m_entries.remove(path))
            return;
    } else {
        m_entries.insert(path, Entry{folds, lineCount});
    }
    m_dirty = true;
    m_saveTimer.start();
}

void FoldStore::renamed(const QString& oldPath, const QString& newPath)
{
    load();
    if (auto it = m_entries.find(oldPath); it != m_entries.end()) {
        const Entry e = *it;
        m_entries.erase(it);
        m_entries.insert(newPath, e);
        m_dirty = true;
        m_saveTimer.start();
    }
}

void FoldStore::removed(const QString& path)
{
    load();
    if (m_entries.remove(path)) {
        m_dirty = true;
        m_saveTimer.start();
    }
}

void FoldStore::flush()
{
    m_saveTimer.stop();
    if (!m_dirty)
        return;
    m_dirty = false;
    QJsonObject root;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        QJsonArray arr;
        for (const auto& [from, to] : it->folds)
            arr.append(QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
        root.insert(it.key(), QJsonObject{{QStringLiteral("folds"), arr}, {QStringLiteral("lines"), it->lines}});
    }
    JsonFile::write(m_file, root);
}

} // namespace oc
