#include "core/documents/document_manager.h"

#include <QPointer>

namespace oc {

namespace {
bool isUnder(const QString& path, const QString& root)
{
    return path == root || (path.size() > root.size() && path.startsWith(root) && path.at(root.size()) == u'/');
}
} // namespace

DocumentManager::DocumentManager(DocumentStorage* storage, QObject* parent)
    : QObject(parent)
    , m_storage(storage)
{
}

DocumentManager::~DocumentManager()
{
    saveAll();
    for (const Entry& e : std::as_const(m_entries))
        delete e.doc;
}

DocumentManager::Entry* DocumentManager::entryFor(const Document* doc)
{
    for (Entry& e : m_entries)
        if (e.doc == doc)
            return &e;
    return nullptr;
}

const DocumentManager::Entry* DocumentManager::entryFor(const Document* doc) const
{
    for (const Entry& e : m_entries)
        if (e.doc == doc)
            return &e;
    return nullptr;
}

Document* DocumentManager::find(const QString& path) const
{
    for (const Entry& e : m_entries)
        if (!e.deleted && e.doc->path() == path)
            return e.doc;
    return nullptr;
}

QList<Document*> DocumentManager::documents() const
{
    QList<Document*> out;
    for (const Entry& e : m_entries)
        if (e.refs > 0)
            out.append(e.doc);
    return out;
}

int DocumentManager::refCount(const Document* doc) const
{
    const Entry* e = entryFor(doc);
    return e ? e->refs : 0;
}

bool DocumentManager::isDeleted(const Document* doc) const
{
    const Entry* e = entryFor(doc);
    return e && e->deleted;
}

Document* DocumentManager::acquire(const QString& path, QString* error)
{
    if (Document* doc = find(path)) {
        Entry* e = entryFor(doc);
        if (e->refs == 0)
            doc->history().clear(); // reopened from the cache: a closed file has no undo history
        ++e->refs;
        e->lastUse = ++m_clock;
        return doc;
    }
    auto* doc = new Document(m_storage, path, this);
    if (!doc->load(error)) {
        delete doc;
        return nullptr;
    }
    if (!m_online)
        doc->setReadOnly(true);
    connect(doc, &Document::saved, this, [this, doc] { emit documentSaved(doc); });
    m_entries.append(Entry{doc, 1, false, ++m_clock});
    emit documentOpened(doc);
    return doc;
}

void DocumentManager::release(Document* doc)
{
    Entry* e = entryFor(doc);
    if (!e || e->refs <= 0)
        return;
    if (--e->refs > 0)
        return;
    e->lastUse = ++m_clock;
    const QString path = doc->path();
    const bool deleted = e->deleted;
    if (!deleted && doc->isDirty())
        doc->save(); // Obsidian saves when a file is closed or switched away from
    if (deleted)
        destroy(doc);
    else
        trimCache();
    emit documentClosed(path);
}

void DocumentManager::setCacheCapacity(int n)
{
    m_cacheCapacity = std::max(0, n);
    trimCache();
}

void DocumentManager::trimCache()
{
    for (;;) {
        int cached = 0;
        const Entry* oldest = nullptr;
        for (const Entry& e : std::as_const(m_entries)) {
            // Dirty ones (a save failed, e.g. the drive went away) are kept rather than lose edits.
            if (e.refs > 0 || e.doc->isDirty())
                continue;
            ++cached;
            if (!oldest || e.lastUse < oldest->lastUse)
                oldest = &e;
        }
        if (cached <= m_cacheCapacity || !oldest)
            return;
        destroy(oldest->doc);
    }
}

void DocumentManager::destroy(Document* doc)
{
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].doc == doc) {
            m_entries.removeAt(i);
            break;
        }
    }
    doc->disconnect(this);
    doc->deleteLater(); // may be inside one of its own signal emissions
}

bool DocumentManager::saveAll(QStringList* failedPaths)
{
    bool ok = true;
    const QList<Entry> entries = m_entries;
    for (const Entry& e : entries) {
        if (e.deleted || !e.doc->isDirty())
            continue;
        if (!e.doc->save()) {
            ok = false;
            if (failedPaths)
                failedPaths->append(e.doc->path());
        }
    }
    return ok;
}

QList<Document*> DocumentManager::docsUnder(const QString& path) const
{
    QList<Document*> out;
    for (const Entry& e : m_entries)
        if (!e.deleted && isUnder(e.doc->path(), path))
            out.append(e.doc);
    return out;
}

void DocumentManager::fileModified(const QString& path)
{
    Document* doc = find(path);
    if (!doc)
        return;
    const Entry* e = entryFor(doc);
    if (e->refs == 0 && !doc->isDirty()) {
        destroy(doc); // cached only: just forget it, the next open reads the new content
        return;
    }
    doc->reloadFromDisk();
}

void DocumentManager::fileRenamed(const QString& oldPath, const QString& newPath)
{
    if (oldPath == newPath)
        return;
    for (Document* doc : docsUnder(oldPath)) {
        const QString before = doc->path();
        const QString after = newPath + before.mid(oldPath.size());
        // Something cached at the destination is stale now (renames don't overwrite, but be safe).
        if (Document* other = find(after); other && other != doc) {
            Entry* oe = entryFor(other);
            if (oe->refs == 0) {
                destroy(other);
            } else {
                oe->deleted = true;
                other->setReadOnly(true);
                emit documentDeleted(other);
            }
        }
        doc->setPath(after);
        emit documentRenamed(doc, before);
    }
}

void DocumentManager::fileDeleted(const QString& path)
{
    QList<QPointer<Document>> affected;
    for (Document* doc : docsUnder(path)) {
        Entry* e = entryFor(doc);
        e->deleted = true;
        doc->setReadOnly(true); // no autosave may recreate it
        affected.append(doc);
    }
    for (const QPointer<Document>& doc : std::as_const(affected)) {
        if (!doc)
            continue;
        const Entry* e = entryFor(doc);
        if (!e)
            continue;
        if (e->refs == 0)
            destroy(doc);
        else
            emit documentDeleted(doc); // views close (or offer to keep the text)
    }
}

void DocumentManager::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    const QList<Entry> entries = m_entries;
    for (const Entry& e : entries) {
        if (e.deleted)
            continue;
        e.doc->setReadOnly(!online);
        if (online && e.doc->isDirty())
            e.doc->scheduleSave();
    }
}

} // namespace oc
