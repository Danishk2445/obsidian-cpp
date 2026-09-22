#include "core/documents/document.h"

#include "core/util/busy.h"
#include "text/codec.h"
#include "text/diff.h"

#include <QDateTime>

namespace oc {

Document::Document(DocumentStorage* storage, QString path, QObject* parent)
    : QObject(parent)
    , m_storage(storage)
    , m_path(std::move(path))
{
    m_clock.start();
    m_autosave.setSingleShot(true);
    m_autosave.setInterval(2000); // Obsidian saves ~2 s after the last change
    connect(&m_autosave, &QTimer::timeout, this, [this] { save(); });
    m_resumeTimer.setSingleShot(true);
    m_resumeTimer.setInterval(0);
    connect(&m_resumeTimer, &QTimer::timeout, this, [this] {
        md::TreeUpdate u;
        const bool more = m_syntax.resume(m_text, 4'000'000, &u);
        emit syntaxProgress(u);
        if (more)
            m_resumeTimer.start();
        else
            Busy::end();
    });
}

Document::~Document()
{
    if (m_resumeTimer.isActive())
        Busy::end();
}

void Document::setPath(const QString& path)
{
    if (path == m_path)
        return;
    const QString old = m_path;
    m_path = path;
    emit pathChanged(old, path);
}

bool Document::load(QString* error)
{
    DiskState st;
    const std::optional<QByteArray> bytes = m_storage->read(m_path, &st, error);
    if (!bytes)
        return false;
    DecodedFile dec = decodeFile(*bytes);
    m_text = dec.text;
    m_base = dec.text;
    m_bom = dec.bom;
    m_invalidUtf8 = dec.hadInvalidUtf8;
    m_readOnly = dec.hasNul;
    m_eol = m_text.dominantEol();
    if (st.hash.isEmpty())
        st.hash = contentHash(*bytes);
    m_disk = st;
    m_syntax.parse(m_text);
    m_history.clear();
    ++m_version;
    m_savedVersion = m_version;
    m_loaded = true;
    return true;
}

void Document::setReadOnly(bool ro)
{
    if (m_forcedReadOnly == ro)
        return;
    const bool before = isReadOnly();
    m_forcedReadOnly = ro;
    if (before != isReadOnly())
        emit readOnlyChanged(isReadOnly());
}

void Document::ensureParsedThrough(int line)
{
    if (m_syntax.validUpTo() > line)
        return;
    md::TreeUpdate u;
    while (m_syntax.validUpTo() <= line && m_syntax.resume(m_text, 50'000'000, &u)) {
    }
    emit syntaxProgress(u);
}

void Document::scheduleResume()
{
    if (!m_resumeTimer.isActive()) {
        Busy::begin();
        m_resumeTimer.start();
    }
}

void Document::applyInternal(const Transaction& tr, QObject* origin)
{
    const bool wasDirty = isDirty();
    DocUpdate u;
    u.before = m_text;
    u.tr = tr;
    u.origin = origin;
    m_text = tr.changes.apply(m_text, m_eol);
    u.after = m_text;
    ++m_version;
    md::ParseBudget budget;
    u.syntax = m_syntax.update(m_text, tr.changes.desc(), budget);
    if (!u.syntax.complete)
        scheduleResume();
    setVersionDirty(wasDirty);
    emit changed(u);
}

void Document::setVersionDirty(bool wasDirty)
{
    if (wasDirty != isDirty())
        emit dirtyChanged(isDirty());
}

void Document::dispatch(const Transaction& tr, QObject* origin, Selection selectionBefore)
{
    if (!tr.docChanged())
        return;
    const Text before = m_text;
    if (tr.addToHistory)
        m_history.record(tr, before, selectionBefore, m_clock.elapsed());
    else
        m_history.addMapping(tr.changes.desc());
    applyInternal(tr, origin);
    if (tr.userEvent != u"external")
        scheduleSave();
}

std::optional<Selection> Document::undo(QObject* origin, Selection current)
{
    auto tr = m_history.undo();
    if (!tr)
        return std::nullopt;
    const Text before = m_text;
    applyInternal(*tr, origin);
    m_history.commitUndo(*tr, before, current);
    scheduleSave();
    return tr->selection;
}

std::optional<Selection> Document::redo(QObject* origin, Selection current)
{
    auto tr = m_history.redo();
    if (!tr)
        return std::nullopt;
    const Text before = m_text;
    applyInternal(*tr, origin);
    m_history.commitRedo(*tr, before, current);
    scheduleSave();
    return tr->selection;
}

void Document::scheduleSave()
{
    if (!isReadOnly())
        m_autosave.start();
}

bool Document::save(QString* error)
{
    m_autosave.stop();
    if (!m_loaded || isReadOnly())
        return false;
    const QByteArray bytes = encodeFile(m_text, m_bom);
    const QByteArray hash = contentHash(bytes);
    const bool wasDirty = isDirty();
    if (hash == m_disk.hash) {
        // Same bytes as on disk (e.g. undone back to the saved state): nothing to write.
        m_savedVersion = m_version;
        m_base = m_text;
        setVersionDirty(wasDirty);
        return true;
    }
    DiskState newState;
    QString err;
    DocumentStorage::WriteResult r = m_storage->write(m_path, bytes, m_disk, &newState, &err);
    if (r == DocumentStorage::WriteResult::Conflict) {
        // Someone changed the file since we last saw it: merge their version first, then retry once.
        reloadFromDisk();
        const QByteArray again = encodeFile(m_text, m_bom);
        if (contentHash(again) == m_disk.hash) {
            m_savedVersion = m_version;
            m_base = m_text;
            setVersionDirty(wasDirty);
            return true;
        }
        r = m_storage->write(m_path, again, m_disk, &newState, &err);
    }
    if (r != DocumentStorage::WriteResult::Ok) {
        if (error)
            *error = err;
        emit saveFailed(err);
        return false;
    }
    if (newState.hash.isEmpty())
        newState.hash = contentHash(encodeFile(m_text, m_bom));
    m_disk = newState;
    m_base = m_text;
    m_savedVersion = m_version;
    setVersionDirty(wasDirty);
    emit saved();
    return true;
}

bool Document::reloadFromDisk()
{
    if (!m_loaded)
        return load();
    DiskState st;
    QString err;
    const std::optional<QByteArray> bytes = m_storage->read(m_path, &st, &err);
    if (!bytes)
        return false;
    if (st.hash.isEmpty())
        st.hash = contentHash(*bytes);
    if (st.hash == m_disk.hash) {
        m_disk = st; // same content; refresh mtime
        return true;
    }
    const DecodedFile dec = decodeFile(*bytes);
    const bool wasDirty = isDirty();
    bool conflict = false;
    if (!wasDirty) {
        const ChangeSet changes = diffTexts(m_text, dec.text);
        Transaction tr;
        tr.changes = changes;
        tr.userEvent = QStringLiteral("external");
        tr.addToHistory = false;
        tr.scrollIntoView = false;
        if (!changes.empty()) {
            m_history.addMapping(changes.desc());
            applyInternal(tr, nullptr);
        }
        // Adopt the exact disk text (EOLs/BOM may differ even when the '\n'-joined text is equal).
        m_text = dec.text;
        m_savedVersion = m_version;
    } else {
        std::optional<ChangeSet> merged = merge3(m_base, m_text, dec.text);
        if (merged) {
            Transaction tr;
            tr.changes = *merged;
            tr.userEvent = QStringLiteral("external");
            tr.addToHistory = false;
            tr.scrollIntoView = false;
            if (!merged->empty()) {
                m_history.addMapping(merged->desc());
                applyInternal(tr, nullptr);
            }
        } else {
            conflict = true;
            m_conflictText = dec.text;
        }
        scheduleSave();
    }
    m_base = dec.text;
    m_bom = dec.bom;
    m_disk = st;
    setVersionDirty(wasDirty);
    emit externallyChanged(conflict);
    return true;
}

} // namespace oc
