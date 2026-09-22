#pragma once

// Change sets with CodeMirror 6 semantics (port of @codemirror/state changeset.ts, MIT licensed,
// © Marijn Haverbeke and others). Positions are offsets into the '\n'-joined text (see Text).

#include "text/text.h"

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace oc {

enum class MapMode { Simple, TrackDel, TrackBefore, TrackAfter };

struct ChangeSpec {
    qsizetype from = 0;
    qsizetype to = 0; // == from for pure insertion
    QString insert;   // '\n' separated
};

// Describes changed/unchanged spans without the inserted text. `sections` holds pairs
// (len, ins): ins == -1 marks len unchanged chars; otherwise len chars were replaced by ins chars.
class ChangeDesc {
public:
    ChangeDesc() = default;
    explicit ChangeDesc(QList<qsizetype> sections) : m_sections(std::move(sections)) {}

    qsizetype length() const;     // length of the document before the change
    qsizetype newLength() const;  // after
    bool empty() const;           // no actual change
    const QList<qsizetype>& sections() const { return m_sections; }

    // Map a position through the change. assoc < 0 keeps positions at an insertion point before the
    // inserted text, > 0 after it. Returns nullopt only for tracking modes when the position was deleted.
    std::optional<qsizetype> mapPosOpt(qsizetype pos, int assoc = -1, MapMode mode = MapMode::Simple) const;
    qsizetype mapPos(qsizetype pos, int assoc = -1) const { return *mapPosOpt(pos, assoc, MapMode::Simple); }

    // 0 = no, 1 = touches, 2 = the change covers the whole range
    int touchesRange(qsizetype from, qsizetype to) const;

    // (fromA, toA, fromB, toB) for every changed range; adjacent changes merged unless individual.
    void iterChangedRanges(const std::function<void(qsizetype, qsizetype, qsizetype, qsizetype)>& f, bool individual = false) const;
    // Unchanged ranges: (posA, posB, length)
    void iterGaps(const std::function<void(qsizetype, qsizetype, qsizetype)>& f) const;

    ChangeDesc invertedDesc() const;
    ChangeDesc composeDesc(const ChangeDesc& other) const; // this, then other
    ChangeDesc mapDesc(const ChangeDesc& other, bool before = false) const; // this rebased over other

    QString debugString() const;

protected:
    QList<qsizetype> m_sections;
};

class ChangeSet : public ChangeDesc {
public:
    ChangeSet() = default;
    ChangeSet(QList<qsizetype> sections, QList<QString> inserted)
        : ChangeDesc(std::move(sections)), m_inserted(std::move(inserted)) {}

    // Specs refer to positions in the ORIGINAL document (like CM6's ChangeSet.of); order doesn't matter.
    static ChangeSet of(const QList<ChangeSpec>& specs, qsizetype docLength);
    static ChangeSet of(const ChangeSpec& spec, qsizetype docLength) { return of(QList<ChangeSpec>{spec}, docLength); }
    static ChangeSet emptySet(qsizetype docLength);

    Text apply(const Text& doc, Eol newEol = Eol::LF) const;
    ChangeSet invert(const Text& before) const;
    ChangeSet compose(const ChangeSet& other) const; // this, then other
    ChangeSet map(const ChangeDesc& other, bool before = false) const;
    ChangeDesc desc() const { return ChangeDesc(m_sections); }

    // (fromA, toA, fromB, toB, insertedText)
    void iterChanges(const std::function<void(qsizetype, qsizetype, qsizetype, qsizetype, const QString&)>& f, bool individual = false) const;

    const QList<QString>& inserted() const { return m_inserted; }

private:
    friend struct ChangeSetInternals;
    QList<QString> m_inserted; // indexed by section index/2 (empty for unchanged sections)
};

} // namespace oc
