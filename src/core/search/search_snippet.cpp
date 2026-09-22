#include "core/search/search_snippet.h"

#include "core/search/search_internal.h"

#include <algorithm>

namespace oc::search {
namespace {

constexpr int kWordSnapSlack = 12; // how far a cut may move back to keep a word whole

bool hasNonSpace(QStringView s, int from, int to)
{
    for (int i = from; i < to; ++i)
        if (!s[i].isSpace())
            return true;
    return false;
}

} // namespace

QList<Snippet> buildSnippets(QStringView content, const QList<ContentMatch>& matches, const SnippetOptions& options)
{
    QList<Snippet> out;
    const int size = int(content.size());
    const int ctx = std::max(0, options.contextChars);
    const int extraLines = std::max(0, options.contextLines);

    // Window [start, end) around one match, before merging and snapping.
    const auto windowStart = [&](int from, int* linesBack) {
        int start = detail::lineStartOf(content, from);
        if (extraLines == 0)
            return std::max(start, from - ctx);
        for (int k = 0; k < extraLines && start > 0; ++k) {
            start = detail::lineStartOf(content, start - 1);
            ++*linesBack;
        }
        return start;
    };
    const auto windowEnd = [&](int from, int to) {
        const int lastLineEnd = std::max(to, detail::lineEndOf(content, to > from ? to - 1 : from));
        if (extraLines == 0)
            return std::max(to, std::min(lastLineEnd, to + ctx));
        int end = lastLineEnd;
        for (int k = 0; k < extraLines; ++k) {
            const qsizetype nl = content.indexOf(u'\n', end);
            if (nl < 0)
                break;
            end = detail::lineEndOf(content, int(nl + 1));
        }
        return end;
    };

    qsizetype i = 0;
    while (i < matches.size()) {
        if (options.maxSnippets > 0 && out.size() >= options.maxSnippets)
            break;
        const int firstFrom = std::clamp(matches[i].from, 0, size);
        const int firstTo = std::clamp(matches[i].to, firstFrom, size);
        int linesBack = 0;
        int start = windowStart(firstFrom, &linesBack);
        int end = windowEnd(firstFrom, firstTo);
        int lastTo = firstTo;
        qsizetype j = i + 1;
        while (j < matches.size()) {
            const int from = std::clamp(matches[j].from, 0, size);
            const int to = std::clamp(matches[j].to, from, size);
            int unused = 0;
            if (windowStart(from, &unused) > end)
                break;
            end = std::max(end, windowEnd(from, to));
            lastTo = std::max(lastTo, to);
            ++j;
        }

        Snippet s;
        s.line = matches[i].line - linesBack;
        if (extraLines == 0) {
            const int lineStart = detail::lineStartOf(content, firstFrom);
            const int lineEnd = detail::lineEndOf(content, std::max(lastTo - 1, firstFrom));
            // Keep words whole: extend a cut start back to its word (a little), otherwise skip the partial word.
            if (start > lineStart && start < size && !content[start - 1].isSpace() && !content[start].isSpace()) {
                int back = start;
                while (back > lineStart && start - back < kWordSnapSlack && !content[back - 1].isSpace())
                    --back;
                if (back == lineStart || content[back - 1].isSpace()) {
                    start = back;
                } else {
                    int fwd = start;
                    while (fwd < firstFrom && !content[fwd].isSpace())
                        ++fwd;
                    if (fwd < firstFrom)
                        start = fwd;
                }
            }
            if (end < lineEnd && end > 0 && !content[end - 1].isSpace() && !content[end].isSpace()) {
                int fwd = end;
                while (fwd < lineEnd && fwd - end < kWordSnapSlack && !content[fwd].isSpace())
                    ++fwd;
                if (fwd == lineEnd || content[fwd].isSpace()) {
                    end = fwd;
                } else {
                    int back = end;
                    while (back > lastTo && !content[back - 1].isSpace())
                        --back;
                    if (back > lastTo)
                        end = back;
                }
            }
            while (start < firstFrom && content[start].isSpace())
                ++start;
            while (end > lastTo && content[end - 1].isSpace())
                --end;
            // Never split a surrogate pair.
            if (start > 0 && start < size && content[start].isLowSurrogate() && content[start - 1].isHighSurrogate())
                --start;
            if (end > 0 && end < size && content[end - 1].isHighSurrogate() && content[end].isLowSurrogate())
                ++end;
            s.truncatedStart = hasNonSpace(content, lineStart, start);
            s.truncatedEnd = hasNonSpace(content, end, detail::lineEndOf(content, end));
        }
        s.from = start;
        s.to = end;
        s.text = content.mid(start, end - start).toString();
        for (qsizetype k = i; k < j; ++k) {
            const int from = std::clamp(matches[k].from, start, end);
            const int to = std::clamp(matches[k].to, from, end);
            if (to > from)
                s.matches.push_back({from - start, to - start});
        }
        out.push_back(std::move(s));
        i = j;
    }
    return out;
}

} // namespace oc::search
