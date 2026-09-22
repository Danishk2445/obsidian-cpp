#include "text/changeset.h"

#include <QStringList>

#include <algorithm>
#include <stdexcept>

namespace oc {

// ---------------------------------------------------------------------------------------------
// helpers (mirror changeset.ts)
// ---------------------------------------------------------------------------------------------
namespace {

void addSection(QList<qsizetype>& sections, qsizetype len, qsizetype ins, bool forceJoin = false)
{
    if (len == 0 && ins <= 0)
        return;
    const qsizetype last = sections.size() - 2;
    if (last >= 0 && ins <= 0 && ins == sections[last + 1])
        sections[last] += len;
    else if (last >= 0 && len == 0 && sections[last] == 0)
        sections[last + 1] += ins;
    else if (forceJoin && last >= 0) {
        sections[last] += len;
        sections[last + 1] += ins;
    } else {
        sections.push_back(len);
        sections.push_back(ins);
    }
}

void addInsert(QList<QString>& values, const QList<qsizetype>& sections, const QString& value)
{
    if (value.isEmpty())
        return;
    const qsizetype index = (sections.size() - 2) >> 1;
    if (index < values.size()) {
        values.last() += value;
    } else {
        while (values.size() < index)
            values.push_back(QString());
        values.push_back(value);
    }
}

struct SectionIter {
    const QList<qsizetype>& sections;
    const QList<QString>* inserted; // may be null for descs
    qsizetype i = 0;
    qsizetype len = 0;
    qsizetype ins = 0;
    qsizetype off = 0;

    SectionIter(const QList<qsizetype>& s, const QList<QString>* ins_)
        : sections(s)
        , inserted(ins_)
    {
        next();
    }
    void next()
    {
        if (i < sections.size()) {
            len = sections[i++];
            ins = sections[i++];
        } else {
            len = 0;
            ins = -2;
        }
        off = 0;
    }
    bool done() const { return ins == -2; }
    qsizetype len2() const { return ins < 0 ? len : ins; }
    QString text() const
    {
        const qsizetype index = (i - 2) >> 1;
        if (!inserted || index >= inserted->size())
            return {};
        return (*inserted)[index];
    }
    QString textBit(qsizetype l) const
    {
        const qsizetype index = (i - 2) >> 1;
        if (!inserted || index >= inserted->size())
            return {};
        return (*inserted)[index].mid(off, l);
    }
    void forward(qsizetype l)
    {
        if (l == len)
            next();
        else {
            len -= l;
            off += l;
        }
    }
    void forward2(qsizetype l)
    {
        if (ins == -1)
            forward(l);
        else if (l == ins)
            next();
        else {
            ins -= l;
            off += l;
        }
    }
};

[[noreturn]] void mismatch() { throw std::logic_error("Mismatched change set lengths"); }

// Returns sections (+ inserted when mkSet).
void mapSet(const QList<qsizetype>& secA, const QList<QString>* insA, const QList<qsizetype>& secB, bool before, bool mkSet,
            QList<qsizetype>& outSections, QList<QString>& outInserted)
{
    SectionIter a(secA, insA), b(secB, nullptr);
    qsizetype inserted = -1;
    for (;;) {
        if ((a.done() && b.len) || (b.done() && a.len)) {
            mismatch();
        } else if (a.ins == -1 && b.ins == -1) {
            const qsizetype len = std::min(a.len, b.len);
            addSection(outSections, len, -1);
            a.forward(len);
            b.forward(len);
        } else if (b.ins >= 0 && (a.ins < 0 || inserted == a.i || (a.off == 0 && (b.len < a.len || (b.len == a.len && !before))))) {
            qsizetype len = b.len;
            addSection(outSections, b.ins, -1);
            while (len) {
                const qsizetype piece = std::min(a.len, len);
                if (a.ins >= 0 && inserted < a.i && a.len <= piece) {
                    addSection(outSections, 0, a.ins);
                    if (mkSet)
                        addInsert(outInserted, outSections, a.text());
                    inserted = a.i;
                }
                a.forward(piece);
                len -= piece;
            }
            b.next();
        } else if (a.ins >= 0) {
            qsizetype len = 0, left = a.len;
            while (left) {
                if (b.ins == -1) {
                    const qsizetype piece = std::min(left, b.len);
                    len += piece;
                    left -= piece;
                    b.forward(piece);
                } else if (b.ins == 0 && b.len < left) {
                    left -= b.len;
                    b.next();
                } else {
                    break;
                }
            }
            addSection(outSections, len, inserted < a.i ? a.ins : 0);
            if (mkSet && inserted < a.i)
                addInsert(outInserted, outSections, a.text());
            inserted = a.i;
            a.forward(a.len - left);
        } else if (a.done() && b.done()) {
            return;
        } else {
            mismatch();
        }
    }
}

void composeSets(const QList<qsizetype>& secA, const QList<QString>* insA, const QList<qsizetype>& secB, const QList<QString>* insB,
                 bool mkSet, QList<qsizetype>& outSections, QList<QString>& outInserted)
{
    SectionIter a(secA, insA), b(secB, insB);
    bool open = false;
    for (;;) {
        if (a.done() && b.done()) {
            return;
        } else if (a.ins == 0) { // deletion in A
            addSection(outSections, a.len, 0, open);
            a.next();
        } else if (b.len == 0 && !b.done()) { // insertion in B
            addSection(outSections, 0, b.ins, open);
            if (mkSet)
                addInsert(outInserted, outSections, b.text());
            b.next();
        } else if (a.done() || b.done()) {
            mismatch();
        } else {
            const qsizetype len = std::min(a.len2(), b.len);
            const qsizetype sectionLen = outSections.size();
            if (a.ins == -1) {
                const qsizetype insBv = b.ins == -1 ? -1 : b.off ? 0 : b.ins;
                addSection(outSections, len, insBv, open);
                if (mkSet && insBv)
                    addInsert(outInserted, outSections, b.text());
            } else if (b.ins == -1) {
                addSection(outSections, a.off ? 0 : a.len, len, open);
                if (mkSet)
                    addInsert(outInserted, outSections, a.textBit(len));
            } else {
                addSection(outSections, a.off ? 0 : a.len, b.off ? 0 : b.ins, open);
                if (mkSet && !b.off)
                    addInsert(outInserted, outSections, b.text());
            }
            open = (a.ins > len || (b.ins >= 0 && b.len > len)) && (open || outSections.size() > sectionLen);
            a.forward2(len);
            b.forward(len);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// ChangeDesc
// ---------------------------------------------------------------------------------------------
qsizetype ChangeDesc::length() const
{
    qsizetype r = 0;
    for (qsizetype i = 0; i < m_sections.size(); i += 2)
        r += m_sections[i];
    return r;
}

qsizetype ChangeDesc::newLength() const
{
    qsizetype r = 0;
    for (qsizetype i = 0; i < m_sections.size(); i += 2) {
        const qsizetype ins = m_sections[i + 1];
        r += ins < 0 ? m_sections[i] : ins;
    }
    return r;
}

bool ChangeDesc::empty() const
{
    return m_sections.isEmpty() || (m_sections.size() == 2 && m_sections[1] < 0);
}

std::optional<qsizetype> ChangeDesc::mapPosOpt(qsizetype pos, int assoc, MapMode mode) const
{
    qsizetype posA = 0, posB = 0;
    for (qsizetype i = 0; i < m_sections.size();) {
        const qsizetype len = m_sections[i++], ins = m_sections[i++], endA = posA + len;
        if (ins < 0) {
            if (endA > pos)
                return posB + (pos - posA);
            posB += len;
        } else {
            if (mode != MapMode::Simple && endA >= pos
                && ((mode == MapMode::TrackDel && posA < pos && endA > pos) || (mode == MapMode::TrackBefore && posA < pos)
                    || (mode == MapMode::TrackAfter && endA > pos)))
                return std::nullopt;
            if (endA > pos || (endA == pos && assoc < 0 && !len))
                return pos == posA || assoc < 0 ? posB : posB + ins;
            posB += ins;
        }
        posA = endA;
    }
    if (pos > posA)
        throw std::out_of_range("Position out of range for changeset");
    return posB;
}

int ChangeDesc::touchesRange(qsizetype from, qsizetype to) const
{
    for (qsizetype i = 0, pos = 0; i < m_sections.size() && pos <= to;) {
        const qsizetype len = m_sections[i++], ins = m_sections[i++], end = pos + len;
        if (ins >= 0 && pos <= to && end >= from)
            return pos < from && end > to ? 2 : 1;
        pos = end;
    }
    return 0;
}

void ChangeDesc::iterChangedRanges(const std::function<void(qsizetype, qsizetype, qsizetype, qsizetype)>& f, bool individual) const
{
    for (qsizetype posA = 0, posB = 0, i = 0; i < m_sections.size();) {
        qsizetype len = m_sections[i++], ins = m_sections[i++];
        if (ins < 0) {
            posA += len;
            posB += len;
        } else {
            qsizetype endA = posA, endB = posB;
            for (;;) {
                endA += len;
                endB += ins;
                if (individual || i == m_sections.size() || m_sections[i + 1] < 0)
                    break;
                len = m_sections[i++];
                ins = m_sections[i++];
            }
            f(posA, endA, posB, endB);
            posA = endA;
            posB = endB;
        }
    }
}

void ChangeDesc::iterGaps(const std::function<void(qsizetype, qsizetype, qsizetype)>& f) const
{
    for (qsizetype i = 0, posA = 0, posB = 0; i < m_sections.size();) {
        const qsizetype len = m_sections[i++], ins = m_sections[i++];
        if (ins < 0) {
            f(posA, posB, len);
            posB += len;
        } else {
            posB += ins;
        }
        posA += len;
    }
}

ChangeDesc ChangeDesc::invertedDesc() const
{
    QList<qsizetype> s;
    s.reserve(m_sections.size());
    for (qsizetype i = 0; i < m_sections.size();) {
        const qsizetype len = m_sections[i++], ins = m_sections[i++];
        if (ins < 0) {
            s.push_back(len);
            s.push_back(ins);
        } else {
            s.push_back(ins);
            s.push_back(len);
        }
    }
    return ChangeDesc(std::move(s));
}

ChangeDesc ChangeDesc::composeDesc(const ChangeDesc& other) const
{
    if (empty())
        return other;
    if (other.empty())
        return *this;
    QList<qsizetype> sec;
    QList<QString> ins;
    composeSets(m_sections, nullptr, other.m_sections, nullptr, false, sec, ins);
    return ChangeDesc(std::move(sec));
}

ChangeDesc ChangeDesc::mapDesc(const ChangeDesc& other, bool before) const
{
    if (other.empty())
        return *this;
    QList<qsizetype> sec;
    QList<QString> ins;
    mapSet(m_sections, nullptr, other.m_sections, before, false, sec, ins);
    return ChangeDesc(std::move(sec));
}

QString ChangeDesc::debugString() const
{
    QStringList parts;
    for (qsizetype i = 0; i < m_sections.size(); i += 2) {
        const qsizetype len = m_sections[i], ins = m_sections[i + 1];
        parts << (ins < 0 ? QString::number(len) : QStringLiteral("%1:%2").arg(len).arg(ins));
    }
    return parts.join(QLatin1Char(' '));
}

// ---------------------------------------------------------------------------------------------
// ChangeSet
// ---------------------------------------------------------------------------------------------
ChangeSet ChangeSet::emptySet(qsizetype docLength)
{
    return docLength ? ChangeSet({docLength, -1}, {}) : ChangeSet({}, {});
}

ChangeSet ChangeSet::of(const QList<ChangeSpec>& specs, qsizetype length)
{
    QList<qsizetype> sections;
    QList<QString> inserted;
    qsizetype pos = 0;
    std::optional<ChangeSet> total;
    auto flush = [&](bool force) {
        if (!force && sections.isEmpty())
            return;
        if (pos < length)
            addSection(sections, length - pos, -1);
        ChangeSet set(sections, inserted);
        total = total ? total->compose(set.map(*total)) : set;
        sections.clear();
        inserted.clear();
        pos = 0;
    };
    for (const ChangeSpec& spec : specs) {
        const qsizetype from = spec.from, to = std::max(spec.from, spec.to);
        if (from < 0 || to > length)
            throw std::out_of_range("Invalid change range");
        const qsizetype insLen = spec.insert.size();
        if (from == to && insLen == 0)
            continue;
        if (from < pos)
            flush(false);
        if (from > pos)
            addSection(sections, from - pos, -1);
        addSection(sections, to - from, insLen);
        addInsert(inserted, sections, spec.insert);
        pos = to;
    }
    flush(!total.has_value());
    return *total;
}

Text ChangeSet::apply(const Text& doc, Eol newEol) const
{
    if (length() != doc.length())
        throw std::logic_error("Applying change set to a document with the wrong length");
    Text out = doc;
    iterChanges([&](qsizetype fromA, qsizetype toA, qsizetype fromB, qsizetype, const QString& text) {
        out = out.replace(fromB, fromB + (toA - fromA), text, newEol);
    });
    return out;
}

ChangeSet ChangeSet::invert(const Text& doc) const
{
    QList<qsizetype> sections = m_sections;
    QList<QString> inserted;
    for (qsizetype i = 0, pos = 0; i < sections.size(); i += 2) {
        const qsizetype len = sections[i], ins = sections[i + 1];
        if (ins >= 0) {
            sections[i] = ins;
            sections[i + 1] = len;
            const qsizetype index = i >> 1;
            while (inserted.size() < index)
                inserted.push_back(QString());
            inserted.push_back(len ? doc.slice(pos, pos + len) : QString());
        }
        pos += len;
    }
    return ChangeSet(std::move(sections), std::move(inserted));
}

ChangeSet ChangeSet::compose(const ChangeSet& other) const
{
    if (empty())
        return other;
    if (other.empty())
        return *this;
    QList<qsizetype> sec;
    QList<QString> ins;
    composeSets(m_sections, &m_inserted, other.m_sections, &other.m_inserted, true, sec, ins);
    return ChangeSet(std::move(sec), std::move(ins));
}

ChangeSet ChangeSet::map(const ChangeDesc& other, bool before) const
{
    if (other.empty())
        return *this;
    QList<qsizetype> sec;
    QList<QString> ins;
    mapSet(m_sections, &m_inserted, other.sections(), before, true, sec, ins);
    return ChangeSet(std::move(sec), std::move(ins));
}

void ChangeSet::iterChanges(const std::function<void(qsizetype, qsizetype, qsizetype, qsizetype, const QString&)>& f, bool individual) const
{
    for (qsizetype posA = 0, posB = 0, i = 0; i < m_sections.size();) {
        qsizetype len = m_sections[i++], ins = m_sections[i++];
        if (ins < 0) {
            posA += len;
            posB += len;
        } else {
            qsizetype endA = posA, endB = posB;
            QString text;
            for (;;) {
                endA += len;
                endB += ins;
                const qsizetype idx = (i - 2) >> 1;
                if (ins && idx < m_inserted.size())
                    text += m_inserted[idx];
                if (individual || i == m_sections.size() || m_sections[i + 1] < 0)
                    break;
                len = m_sections[i++];
                ins = m_sections[i++];
            }
            f(posA, endA, posB, endB, text);
            posA = endA;
            posB = endB;
        }
    }
}

} // namespace oc
