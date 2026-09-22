#pragma once

#include "core/documents/document.h"

#include <QList>
#include <QObject>

namespace oc {

// Shares one Document per vault path between every view showing it (panes of the same file stay in
// sync), loads it on first use, and saves it when the last user releases it. Released documents stay
// in a small cache so switching back to a big note doesn't re-read and re-parse it; like Obsidian,
// the undo history doesn't survive closing a file.
//
// Vault events are forwarded by the owner: fileModified (external edits only; our own writes are
// suppressed by the vault), fileRenamed/fileDeleted (files or folders — descendants follow), and
// setOnline (a vault on a removable drive went away: documents turn read-only, nothing is lost).
class DocumentManager : public QObject {
    Q_OBJECT
public:
    explicit DocumentManager(DocumentStorage* storage, QObject* parent = nullptr);
    ~DocumentManager() override; // saves dirty documents

    // The shared document for `path`, loaded if needed; nullptr (and *error) if it can't be read.
    // Every successful acquire() must be balanced by release().
    Document* acquire(const QString& path, QString* error = nullptr);
    void release(Document* doc);

    Document* find(const QString& path) const; // open or cached document, nullptr if none; no reference taken
    QList<Document*> documents() const;         // acquired documents
    int refCount(const Document* doc) const;

    // Flush pending autosaves now (quit, window close). Returns false if any save failed.
    bool saveAll(QStringList* failedPaths = nullptr);

    void fileModified(const QString& path);
    void fileRenamed(const QString& oldPath, const QString& newPath);
    void fileDeleted(const QString& path);
    void setOnline(bool online);
    bool isOnline() const { return m_online; }

    bool isDeleted(const Document* doc) const; // its file vanished; kept (read-only) until released

    int cacheCapacity() const { return m_cacheCapacity; }
    void setCacheCapacity(int n);

signals:
    void documentOpened(oc::Document* doc);
    void documentClosed(const QString& path); // last user released it (it may linger in the cache)
    void documentSaved(oc::Document* doc);
    void documentDeleted(oc::Document* doc);
    void documentRenamed(oc::Document* doc, const QString& oldPath);

private:
    struct Entry {
        Document* doc = nullptr;
        int refs = 0;
        bool deleted = false;
        quint64 lastUse = 0;
    };
    Entry* entryFor(const Document* doc);
    const Entry* entryFor(const Document* doc) const;
    QList<Document*> docsUnder(const QString& path) const; // path itself or descendants (not deleted)
    void trimCache();
    void destroy(Document* doc);

    DocumentStorage* m_storage;
    QList<Entry> m_entries; // few (open tabs + cache): linear scans are fine
    int m_cacheCapacity = 4;
    quint64 m_clock = 0;
    bool m_online = true;
};

} // namespace oc
