#include "editor/search/doc_searcher.h"

namespace oc {

namespace {
bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c.isMark();
}
} // namespace

DocSearcher::DocSearcher(SearchQuery query)
    : m_q(std::move(query))
{
    if (m_q.regex && !m_q.text.isEmpty()) {
        QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
        if (!m_q.caseSensitive)
            opts |= QRegularExpression::CaseInsensitiveOption;
        m_re = QRegularExpression(m_q.text, opts);
    }
}

bool DocSearcher::isValid() const
{
    if (m_q.text.isEmpty())
        return false;
    return !m_q.regex || m_re.isValid();
}

bool DocSearcher::wordBoundaryOk(QStringView line, int from, int to) const
{
    if (!m_q.wholeWord)
        return true;
    if (from > 0 && isWordChar(line.at(from - 1)) && isWordChar(line.at(from)))
        return false;
    if (to < line.size() && isWordChar(line.at(to)) && isWordChar(line.at(to - 1)))
        return false;
    return true;
}

std::optional<std::pair<int, int>> DocSearcher::matchInLine(QStringView line, int from) const
{
    if (!isValid())
        return std::nullopt;
    const Qt::CaseSensitivity cs = m_q.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    int pos = std::max(0, from);
    while (pos <= line.size()) {
        int s = -1, e = -1;
        if (m_q.regex) {
            const QRegularExpressionMatch m = m_re.matchView(line, pos);
            if (!m.hasMatch())
                return std::nullopt;
            s = int(m.capturedStart());
            e = int(m.capturedEnd());
            if (e == s) { // never report empty matches
                pos = s + 1;
                continue;
            }
        } else {
            const qsizetype i = line.indexOf(QStringView(m_q.text), pos, cs);
            if (i < 0)
                return std::nullopt;
            s = int(i);
            e = s + int(m_q.text.size());
        }
        if (wordBoundaryOk(line, s, e))
            return std::pair{s, e};
        pos = s + 1;
    }
    return std::nullopt;
}

void DocSearcher::forEachInLine(QStringView line, const std::function<void(int, int)>& fn) const
{
    int pos = 0;
    while (auto m = matchInLine(line, pos)) {
        fn(m->first, m->second);
        pos = m->second;
    }
}

std::optional<SearchMatch> DocSearcher::next(const Text& text, qsizetype pos, bool wrap) const
{
    if (!isValid())
        return std::nullopt;
    pos = std::clamp<qsizetype>(pos, 0, text.length());
    const int startLine = text.lineAt(pos);
    for (int line = startLine; line < text.lineCount(); ++line) {
        const qsizetype ls = text.lineStart(line);
        if (auto m = matchInLine(text.lineView(line), line == startLine ? int(pos - ls) : 0))
            return SearchMatch{ls + m->first, ls + m->second};
    }
    if (!wrap)
        return std::nullopt;
    // Wrap around: from the top down to the start line (whose matches at >= pos were seen above).
    for (int line = 0; line <= startLine; ++line) {
        const qsizetype ls = text.lineStart(line);
        if (auto m = matchInLine(text.lineView(line), 0))
            return SearchMatch{ls + m->first, ls + m->second};
    }
    return std::nullopt;
}

std::optional<SearchMatch> DocSearcher::previous(const Text& text, qsizetype pos, bool wrap) const
{
    if (!isValid())
        return std::nullopt;
    pos = std::clamp<qsizetype>(pos, 0, text.length());
    const auto lastIn = [&](int line, qsizetype limit) -> std::optional<SearchMatch> {
        const qsizetype ls = text.lineStart(line);
        std::optional<SearchMatch> best;
        forEachInLine(text.lineView(line), [&](int f, int t) {
            if (ls + t <= limit)
                best = SearchMatch{ls + f, ls + t};
        });
        return best;
    };
    const int startLine = text.lineAt(pos);
    for (int line = startLine; line >= 0; --line)
        if (auto m = lastIn(line, pos))
            return m;
    if (!wrap)
        return std::nullopt;
    // Wrap: the last match of the document (necessarily at or after pos).
    for (int line = text.lineCount() - 1; line >= startLine; --line)
        if (auto m = lastIn(line, text.length()))
            return m;
    return std::nullopt;
}

QList<SearchMatch> DocSearcher::all(const Text& text, int limit) const
{
    QList<SearchMatch> out;
    if (!isValid())
        return out;
    const int n = text.lineCount();
    for (int line = 0; line < n; ++line) {
        const qsizetype ls = text.lineStart(line);
        int pos = 0;
        const QStringView view = text.lineView(line);
        while (auto m = matchInLine(view, pos)) {
            out.append(SearchMatch{ls + m->first, ls + m->second});
            if (limit >= 0 && out.size() >= limit)
                return out;
            pos = m->second;
        }
    }
    return out;
}

std::pair<int, int> DocSearcher::indexAndCount(const Text& text, const std::optional<SearchMatch>& current, int cap) const
{
    if (!isValid())
        return {0, 0};
    int index = 0, count = 0;
    const int n = text.lineCount();
    for (int line = 0; line < n && count < cap; ++line) {
        const qsizetype ls = text.lineStart(line);
        int pos = 0;
        const QStringView view = text.lineView(line);
        while (count < cap) {
            const auto m = matchInLine(view, pos);
            if (!m)
                break;
            ++count;
            if (current && ls + m->first <= current->from)
                ++index;
            pos = m->second;
        }
    }
    return {current ? index : 0, count};
}

} // namespace oc
