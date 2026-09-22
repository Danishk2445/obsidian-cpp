#pragma once

#include "text/transaction.h"

#include <QList>

#include <optional>

namespace oc {

// Undo/redo history with CodeMirror 6 semantics (@codemirror/commands history.ts, MIT):
//  - an event stores the INVERTED changes (applying them undoes the event) and the selection before;
//  - consecutive "input.type*"/"delete*" events that are adjacent and < 500 ms apart join one group,
//    unless the selection moved in between; "input.type.compose" always joins;
//  - changes made outside history (external reload merges) are mapped into the stored events;
//  - undo/redo return a Transaction to dispatch; dispatching it with the matching userEvent
//    ("undo"/"redo") records the reverse event on the other branch.
class History {
public:
    struct Event {
        ChangeSet changes;             // inverted changes
        std::optional<ChangeDesc> mapped; // mapping still to apply to older events when this one is popped
        Selection startSelection;      // selection before the event
        bool selectionMovedAfter = false;
    };

    int newGroupDelayMs = 500;
    int maxDepth = 1000;

    // Record a document-changing transaction. `before` is the document before tr.changes.
    void record(const Transaction& tr, const Text& before, const Selection& selectionBefore, qint64 timeMs);
    // A selection-only change (prevents joining the next change into the current group).
    void noteSelectionChange();
    // Changes that aren't part of history (applied to the current document).
    void addMapping(const ChangeDesc& mapping);

    bool canUndo() const { return !m_done.isEmpty(); }
    bool canRedo() const { return !m_undone.isEmpty(); }
    int undoDepth() const { return int(m_done.size()); }
    int redoDepth() const { return int(m_undone.size()); }

    // Build the transaction that undoes/redoes the last event (nullopt if nothing to do).
    // After dispatching the returned transaction, call commitUndo()/commitRedo() with the document
    // state before it was applied.
    std::optional<Transaction> undo() const;
    std::optional<Transaction> redo() const;
    void commitUndo(const Transaction& applied, const Text& before, const Selection& selectionBefore);
    void commitRedo(const Transaction& applied, const Text& before, const Selection& selectionBefore);

    void clear();

private:
    static QList<Event> addMappingToBranch(QList<Event> branch, ChangeDesc mapping);
    static Event mapEvent(const Event& e, const ChangeDesc& mapping);
    void pushTo(QList<Event>& branch, Event e);

    QList<Event> m_done;
    QList<Event> m_undone;
    qint64 m_prevTime = 0;
    QString m_prevUserEvent;
};

} // namespace oc
