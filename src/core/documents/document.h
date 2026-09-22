#pragma once

#include "core/vault/vault.h"
#include "markdown/syntax_tree.h"
#include "text/history.h"
#include "text/transaction.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace oc {

struct DocUpdate {
    Text before;
    Text after;
    Transaction tr;             // the dispatched transaction (changes + userEvent)
    QObject* origin = nullptr;  // the view that dispatched it (nullptr for external/programmatic)
    md::TreeUpdate syntax;      // how the block tree changed
};

// One open file, shared by every view showing it (like Obsidian keeping panes of the same file in
// sync). Owns the text, the incremental block tree, the undo history and the save state.
//
// Saving is byte-exact: the file is re-encoded from the per-line EOLs/BOM of the text and written
// only when the bytes differ from what's on disk, with a compare-and-swap against the disk state we
// last saw. External changes are diffed in (clean buffer) or 3-way merged (dirty buffer).
class Document : public QObject {
    Q_OBJECT
public:
    Document(DocumentStorage* storage, QString path, QObject* parent = nullptr);
    ~Document() override;

    const QString& path() const { return m_path; }
    void setPath(const QString& path); // after a rename (no IO)

    bool load(QString* error = nullptr);
    bool isLoaded() const { return m_loaded; }

    const Text& text() const { return m_text; }
    quint64 version() const { return m_version; }
    Eol newlineEol() const { return m_eol; }
    bool hasBom() const { return m_bom; }
    bool hadInvalidUtf8() const { return m_invalidUtf8; }

    bool isReadOnly() const { return m_readOnly || m_forcedReadOnly; }
    void setReadOnly(bool ro); // e.g. vault offline

    md::SyntaxTree& syntax() { return m_syntax; }
    const md::SyntaxTree& syntax() const { return m_syntax; }
    // Make sure the tree is exact through `line` (budgeted parses may lag behind).
    void ensureParsedThrough(int line);

    History& history() { return m_history; }
    const History& history() const { return m_history; }

    // Apply a transaction. `selectionBefore` is the origin view's selection before it (recorded
    // for undo). Transactions with userEvent "undo"/"redo" must come from undo()/redo().
    void dispatch(const Transaction& tr, QObject* origin, Selection selectionBefore); // by value: views mutate theirs during dispatch
    // Undo/redo; returns the applied transaction's selection (for the origin view), or nullopt.
    std::optional<Selection> undo(QObject* origin, Selection current);
    std::optional<Selection> redo(QObject* origin, Selection current);
    void noteSelectionChange() { m_history.noteSelectionChange(); }

    bool isDirty() const { return m_version != m_savedVersion; }
    int autosaveDelayMs() const { return m_autosave.interval(); }
    void setAutosaveDelayMs(int ms) { m_autosave.setInterval(ms); }
    void scheduleSave(); // debounced autosave
    bool save(QString* error = nullptr); // now; no-op when bytes equal the disk content

    // The file changed on disk (external). Reloads/merges; returns false on read error.
    bool reloadFromDisk();
    const DiskState& diskState() const { return m_disk; }
    const Text& baseText() const { return m_base; } // last text known to equal the disk content

signals:
    void changed(const oc::DocUpdate& update);
    void syntaxProgress(const oc::md::TreeUpdate& update); // background parse advanced
    void dirtyChanged(bool dirty);
    void saved();
    void saveFailed(const QString& error);
    // External change merged in. conflict=true: overlapping edits; our text was kept and the disk
    // version is available via conflictText().
    void externallyChanged(bool conflict);
    void pathChanged(const QString& oldPath, const QString& newPath);
    void readOnlyChanged(bool readOnly);

public:
    const Text& conflictText() const { return m_conflictText; }

private:
    void applyInternal(const Transaction& tr, QObject* origin);
    void setVersionDirty(bool wasDirty);
    void scheduleResume();

    DocumentStorage* m_storage;
    QString m_path;
    bool m_loaded = false;
    Text m_text;
    Text m_base;
    Text m_conflictText;
    quint64 m_version = 0;
    quint64 m_savedVersion = 0;
    Eol m_eol = Eol::LF;
    bool m_bom = false;
    bool m_invalidUtf8 = false;
    bool m_readOnly = false;
    bool m_forcedReadOnly = false;
    DiskState m_disk;
    md::SyntaxTree m_syntax;
    History m_history;
    QTimer m_autosave;
    QTimer m_resumeTimer;
    QElapsedTimer m_clock;
};

} // namespace oc

Q_DECLARE_METATYPE(oc::DocUpdate)
