#include "text/history.h"

#include <QRegularExpression>

namespace oc {

namespace {
bool joinableUserEvent(const QString& ev)
{
    static const QRegularExpression re(QStringLiteral("^(input\\.type|delete)($|\\.)"));
    return ev.isEmpty() || re.match(ev).hasMatch();
}

// Are the ranges changed by `b` (inverse of the new event, in its A space = doc after the new change;
// its B space = doc before = after `a`) adjacent to the ranges of `a` (inverse of the last event,
// whose A space is the doc after the last event)?
bool isAdjacent(const ChangeDesc& a, const ChangeDesc& b)
{
    QList<qsizetype> ranges;
    a.iterChangedRanges([&](qsizetype f, qsizetype t, qsizetype, qsizetype) {
        ranges.push_back(f);
        ranges.push_back(t);
    });
    bool adjacent = false;
    b.iterChangedRanges([&](qsizetype, qsizetype, qsizetype f, qsizetype t) {
        for (qsizetype i = 0; i < ranges.size(); i += 2)
            if (t >= ranges[i] && f <= ranges[i + 1])
                adjacent = true;
    });
    return adjacent;
}
} // namespace

void History::pushTo(QList<Event>& branch, Event e)
{
    branch.push_back(std::move(e));
    if (branch.size() > maxDepth)
        branch.remove(0, branch.size() - maxDepth);
}

void History::record(const Transaction& tr, const Text& before, const Selection& selectionBefore, qint64 time)
{
    if (tr.changes.empty())
        return;
    if (!tr.addToHistory) {
        addMapping(tr.changes.desc());
        return;
    }
    Event ev;
    ev.changes = tr.changes.invert(before);
    ev.startSelection = selectionBefore;

    if (!m_done.isEmpty()) {
        Event& last = m_done.last();
        const bool compose = tr.userEvent == u"input.type.compose";
        if (!last.changes.empty() && joinableUserEvent(tr.userEvent)
            && ((!last.selectionMovedAfter && time - m_prevTime < newGroupDelayMs && isAdjacent(last.changes.desc(), ev.changes.desc()))
                || compose)) {
            last.changes = ev.changes.compose(last.changes);
            last.selectionMovedAfter = false;
            m_undone.clear();
            m_prevTime = time;
            m_prevUserEvent = tr.userEvent;
            return;
        }
    }
    pushTo(m_done, std::move(ev));
    m_undone.clear();
    m_prevTime = time;
    m_prevUserEvent = tr.userEvent;
}

void History::noteSelectionChange()
{
    if (!m_done.isEmpty())
        m_done.last().selectionMovedAfter = true;
}

History::Event History::mapEvent(const Event& e, const ChangeDesc& mapping)
{
    Event out;
    out.changes = e.changes.map(mapping);
    const ChangeDesc before = mapping.mapDesc(e.changes.desc(), true);
    out.mapped = e.mapped ? e.mapped->composeDesc(before) : before;
    out.startSelection = e.startSelection.map(before);
    out.selectionMovedAfter = e.selectionMovedAfter;
    return out;
}

QList<History::Event> History::addMappingToBranch(QList<Event> branch, ChangeDesc mapping)
{
    while (!branch.isEmpty()) {
        Event ev = mapEvent(branch.last(), mapping);
        if (!ev.changes.empty()) {
            branch.last() = std::move(ev);
            return branch;
        }
        // Event vanished (its changes were overwritten); drop it and continue mapping older ones.
        mapping = *ev.mapped;
        branch.removeLast();
    }
    return branch;
}

void History::addMapping(const ChangeDesc& mapping)
{
    m_done = addMappingToBranch(std::move(m_done), mapping);
    m_undone = addMappingToBranch(std::move(m_undone), mapping);
}

std::optional<Transaction> History::undo() const
{
    if (m_done.isEmpty())
        return std::nullopt;
    const Event& ev = m_done.last();
    Transaction tr;
    tr.changes = ev.changes;
    tr.selection = ev.startSelection;
    tr.userEvent = QStringLiteral("undo");
    tr.addToHistory = false;
    return tr;
}

std::optional<Transaction> History::redo() const
{
    if (m_undone.isEmpty())
        return std::nullopt;
    const Event& ev = m_undone.last();
    Transaction tr;
    tr.changes = ev.changes;
    tr.selection = ev.startSelection;
    tr.userEvent = QStringLiteral("redo");
    tr.addToHistory = false;
    return tr;
}

void History::commitUndo(const Transaction& applied, const Text& before, const Selection& selectionBefore)
{
    if (m_done.isEmpty())
        return;
    Event ev = m_done.takeLast();
    if (ev.mapped)
        m_done = addMappingToBranch(std::move(m_done), *ev.mapped);
    Event rev;
    rev.changes = applied.changes.invert(before);
    rev.startSelection = selectionBefore;
    pushTo(m_undone, std::move(rev));
    m_prevTime = 0; // never join into a group across undo
}

void History::commitRedo(const Transaction& applied, const Text& before, const Selection& selectionBefore)
{
    if (m_undone.isEmpty())
        return;
    Event ev = m_undone.takeLast();
    if (ev.mapped)
        m_undone = addMappingToBranch(std::move(m_undone), *ev.mapped);
    Event rev;
    rev.changes = applied.changes.invert(before);
    rev.startSelection = selectionBefore;
    pushTo(m_done, std::move(rev));
    m_prevTime = 0;
}

void History::clear()
{
    m_done.clear();
    m_undone.clear();
    m_prevTime = 0;
}

} // namespace oc
