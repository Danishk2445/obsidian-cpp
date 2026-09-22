#include "text/text.h"

#include <algorithm>
#include <array>

namespace oc {

void Text::Chunk::finalize()
{
    starts.resize(lines.size() + 1);
    qsizetype pos = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        starts[i] = pos;
        pos += lines[i].size() + 1;
    }
    starts[lines.size()] = pos;
}

void Text::Index::finalize()
{
    lineBase.resize(chunks.size() + 1);
    charBase.resize(chunks.size() + 1);
    int l = 0;
    qsizetype c = 0;
    for (size_t k = 0; k < chunks.size(); ++k) {
        lineBase[k] = l;
        charBase[k] = c;
        l += int(chunks[k]->lines.size());
        c += chunks[k]->span();
    }
    lineBase[chunks.size()] = l;
    charBase[chunks.size()] = c;
}

std::shared_ptr<const Text::Index> Text::build(std::vector<QString>&& lines, std::vector<Eol>&& eols)
{
    auto idx = std::make_shared<Index>();
    const size_t n = lines.size();
    for (size_t i = 0; i < n; i += kChunkLines) {
        auto ch = std::make_shared<Chunk>();
        const size_t end = std::min(n, i + kChunkLines);
        ch->lines.assign(std::make_move_iterator(lines.begin() + i), std::make_move_iterator(lines.begin() + end));
        ch->eols.assign(eols.begin() + i, eols.begin() + end);
        ch->finalize();
        idx->chunks.push_back(std::move(ch));
    }
    idx->finalize();
    return idx;
}

Text::Text()
{
    std::vector<QString> lines(1);
    std::vector<Eol> eols(1, Eol::None);
    m_index = build(std::move(lines), std::move(eols));
}

Text::Text(std::shared_ptr<const Index> index)
    : m_index(std::move(index))
{
}

Text Text::fromString(QStringView s)
{
    std::vector<QString> lines;
    std::vector<Eol> eols;
    qsizetype start = 0;
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = s[i];
        if (c == u'\n' || c == u'\r') {
            lines.emplace_back(s.mid(start, i - start).toString());
            if (c == u'\r' && i + 1 < n && s[i + 1] == u'\n') {
                eols.push_back(Eol::CRLF);
                ++i;
            } else {
                eols.push_back(c == u'\n' ? Eol::LF : Eol::CR);
            }
            start = i + 1;
        }
    }
    lines.emplace_back(s.mid(start).toString());
    eols.push_back(Eol::None);
    return Text(build(std::move(lines), std::move(eols)));
}

Text Text::fromLines(const QList<QString>& l, const QList<Eol>& e)
{
    std::vector<QString> lines(l.begin(), l.end());
    std::vector<Eol> eols(e.begin(), e.end());
    if (lines.empty()) {
        lines.emplace_back();
        eols.assign(1, Eol::None);
    }
    eols.resize(lines.size(), Eol::LF);
    return Text(build(std::move(lines), std::move(eols)));
}

qsizetype Text::length() const { return m_index->span() - 1; }
int Text::lineCount() const { return m_index->lines(); }

int Text::chunkOfLine(int line) const
{
    const auto& lb = m_index->lineBase;
    // last k with lineBase[k] <= line
    auto it = std::upper_bound(lb.begin(), lb.end() - 1, line);
    return int(it - lb.begin()) - 1;
}

int Text::chunkOfPos(qsizetype pos) const
{
    const auto& cb = m_index->charBase;
    auto it = std::upper_bound(cb.begin(), cb.end() - 1, pos);
    int k = int(it - cb.begin()) - 1;
    return std::clamp(k, 0, int(m_index->chunks.size()) - 1);
}

QString Text::line(int i) const
{
    const int k = chunkOfLine(i);
    return m_index->chunks[k]->lines[i - m_index->lineBase[k]];
}

QStringView Text::lineView(int i) const
{
    const int k = chunkOfLine(i);
    return m_index->chunks[k]->lines[i - m_index->lineBase[k]];
}

int Text::lineLength(int i) const { return int(lineView(i).size()); }

Eol Text::lineEol(int i) const
{
    const int k = chunkOfLine(i);
    return m_index->chunks[k]->eols[i - m_index->lineBase[k]];
}

qsizetype Text::lineStart(int i) const
{
    const int k = chunkOfLine(i);
    return m_index->charBase[k] + m_index->chunks[k]->starts[i - m_index->lineBase[k]];
}

qsizetype Text::lineEnd(int i) const { return lineStart(i) + lineLength(i); }

int Text::lineAt(qsizetype pos) const
{
    pos = std::clamp<qsizetype>(pos, 0, length());
    const int k = chunkOfPos(pos);
    const Chunk& c = *m_index->chunks[k];
    const qsizetype local = pos - m_index->charBase[k];
    // last i with starts[i] <= local, within [0, n)
    auto it = std::upper_bound(c.starts.begin(), c.starts.end() - 1, local);
    const int i = std::max(0, int(it - c.starts.begin()) - 1);
    return m_index->lineBase[k] + i;
}

QChar Text::charAt(qsizetype pos) const
{
    if (pos < 0 || pos >= length())
        return {};
    const int l = lineAt(pos);
    const qsizetype off = pos - lineStart(l);
    const QStringView v = lineView(l);
    return off < v.size() ? v[off] : QChar(u'\n');
}

QString Text::slice(qsizetype from, qsizetype to) const
{
    from = std::clamp<qsizetype>(from, 0, length());
    to = std::clamp<qsizetype>(to, from, length());
    if (from == to)
        return {};
    const int la = lineAt(from), lb = lineAt(to);
    const qsizetype sa = lineStart(la);
    if (la == lb)
        return lineView(la).mid(from - sa, to - from).toString();
    QString out;
    out.reserve(to - from);
    out += lineView(la).mid(from - sa);
    for (int l = la + 1; l <= lb; ++l) {
        out += QLatin1Char('\n');
        if (l == lb)
            out += lineView(l).left(to - lineStart(l));
        else
            out += lineView(l);
    }
    return out;
}

QString Text::toString() const { return slice(0, length()); }

void Text::forEachLine(int from, int to, const std::function<void(int, qsizetype, QStringView)>& fn) const
{
    from = std::max(0, from);
    to = std::min(to, lineCount());
    if (from >= to)
        return;
    int k = chunkOfLine(from);
    int line = from;
    while (line < to && k < int(m_index->chunks.size())) {
        const Chunk& c = *m_index->chunks[k];
        const int base = m_index->lineBase[k];
        for (int i = line - base; i < int(c.lines.size()) && line < to; ++i, ++line)
            fn(line, m_index->charBase[k] + c.starts[i], c.lines[i]);
        ++k;
    }
}

Text Text::replace(qsizetype from, qsizetype to, QStringView insert, Eol newEol) const
{
    from = std::clamp<qsizetype>(from, 0, length());
    to = std::clamp<qsizetype>(to, from, length());
    const int la = lineAt(from), lb = lineAt(to);
    const QStringView lineA = lineView(la), lineB = lineView(lb);
    const qsizetype offA = from - lineStart(la), offB = to - lineStart(lb);
    const Eol tailEol = lineEol(lb);

    // New lines for the replaced region [la, lb].
    std::vector<QString> fresh;
    std::vector<Eol> freshEols;
    {
        QString first = lineA.left(offA).toString();
        qsizetype s = 0;
        bool firstDone = false;
        for (qsizetype i = 0; i <= insert.size(); ++i) {
            if (i == insert.size() || insert[i] == u'\n') {
                const QStringView piece = insert.mid(s, i - s);
                if (!firstDone) {
                    first += piece;
                    fresh.push_back(std::move(first));
                    firstDone = true;
                } else {
                    fresh.emplace_back(piece.toString());
                }
                freshEols.push_back(newEol);
                s = i + 1;
            }
        }
        fresh.back() += lineB.mid(offB);
        freshEols.back() = tailEol;
    }

    // Rebuild the chunks that overlap [la, lb]; merge a small tail with the next chunk.
    const Index& old = *m_index;
    const int ka = chunkOfLine(la), kb0 = chunkOfLine(lb);
    int kb = kb0;
    std::vector<QString> lines;
    std::vector<Eol> eols;
    const Chunk& ca = *old.chunks[ka];
    const int baseA = old.lineBase[ka];
    lines.reserve(size_t(la - baseA) + fresh.size() + Text::kChunkLines);
    for (int i = 0; i < la - baseA; ++i) {
        lines.push_back(ca.lines[i]);
        eols.push_back(ca.eols[i]);
    }
    for (size_t i = 0; i < fresh.size(); ++i) {
        lines.push_back(std::move(fresh[i]));
        eols.push_back(freshEols[i]);
    }
    {
        const Chunk& cb = *old.chunks[kb];
        const int baseB = old.lineBase[kb];
        for (size_t i = size_t(lb - baseB + 1); i < cb.lines.size(); ++i) {
            lines.push_back(cb.lines[i]);
            eols.push_back(cb.eols[i]);
        }
    }
    if (lines.size() < size_t(Text::kChunkLines / 4) && kb + 1 < int(old.chunks.size())) {
        ++kb;
        const Chunk& cn = *old.chunks[kb];
        for (size_t i = 0; i < cn.lines.size(); ++i) {
            lines.push_back(cn.lines[i]);
            eols.push_back(cn.eols[i]);
        }
    }

    auto idx = std::make_shared<Index>();
    idx->chunks.reserve(old.chunks.size() + 2);
    for (int k = 0; k < ka; ++k)
        idx->chunks.push_back(old.chunks[k]);
    const size_t n = lines.size();
    const size_t pieces = std::max<size_t>(1, (n + Text::kChunkLines - 1) / Text::kChunkLines);
    const size_t per = (n + pieces - 1) / pieces;
    for (size_t i = 0; i < n; i += per) {
        auto ch = std::make_shared<Chunk>();
        const size_t end = std::min(n, i + per);
        ch->lines.assign(std::make_move_iterator(lines.begin() + i), std::make_move_iterator(lines.begin() + end));
        ch->eols.assign(eols.begin() + i, eols.begin() + end);
        ch->finalize();
        idx->chunks.push_back(std::move(ch));
    }
    for (size_t k = size_t(kb) + 1; k < old.chunks.size(); ++k)
        idx->chunks.push_back(old.chunks[k]);
    idx->finalize();
    return Text(std::move(idx));
}

Eol Text::dominantEol() const
{
    std::array<qsizetype, 4> counts{};
    for (const auto& ch : m_index->chunks)
        for (Eol e : ch->eols)
            ++counts[size_t(e)];
    Eol best = Eol::LF;
    qsizetype bestCount = 0;
    for (Eol e : {Eol::LF, Eol::CRLF, Eol::CR}) {
        if (counts[size_t(e)] > bestCount) {
            best = e;
            bestCount = counts[size_t(e)];
        }
    }
    return best;
}

bool Text::contentEquals(const Text& other) const
{
    if (m_index == other.m_index)
        return true;
    if (lineCount() != other.lineCount() || length() != other.length())
        return false;
    for (int i = 0; i < lineCount(); ++i) {
        if (lineEol(i) != other.lineEol(i) || lineView(i) != other.lineView(i))
            return false;
    }
    return true;
}

} // namespace oc
