#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QTimer>

namespace oc {

// Remembers the folds of each note (like Obsidian's fold manager): per vault path, the folded
// regions as (first line, last line) pairs plus the note's line count when they were saved — folds
// are only restored if the note still has that many lines. Stored as JSON in the clone's per-vault
// data dir (never in the vault); writes are debounced.
class FoldStore : public QObject {
    Q_OBJECT
public:
    explicit FoldStore(QString file, QObject* parent = nullptr);
    ~FoldStore() override; // flushes

    QList<QPair<int, int>> folds(const QString& path, int lineCount) const;
    void setFolds(const QString& path, const QList<QPair<int, int>>& folds, int lineCount);
    void renamed(const QString& oldPath, const QString& newPath);
    void removed(const QString& path);
    void flush();

private:
    void load() const;
    struct Entry {
        QList<QPair<int, int>> folds;
        int lines = 0;
    };
    QString m_file;
    mutable bool m_loaded = false;
    mutable QHash<QString, Entry> m_entries;
    QTimer m_saveTimer;
    bool m_dirty = false;
};

} // namespace oc
