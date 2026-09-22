#pragma once

// Internal helpers of the search module (not part of its API).

#include <QStringView>

#include <algorithm>

namespace oc::search::detail {

// Start of the line containing `pos` (the offset after the previous '\n').
inline int lineStartOf(QStringView text, int pos)
{
    pos = std::clamp(pos, 0, int(text.size()));
    const qsizetype nl = pos > 0 ? text.lastIndexOf(u'\n', pos - 1) : -1;
    return int(nl + 1);
}

// End of the line containing `pos`: the offset of its '\n' (or of a '\r' right before it), or text.size().
inline int lineEndOf(QStringView text, int pos)
{
    pos = std::clamp(pos, 0, int(text.size()));
    qsizetype nl = text.indexOf(u'\n', pos);
    if (nl < 0)
        nl = text.size();
    if (nl > pos && text[nl - 1] == u'\r')
        --nl;
    return int(nl);
}

} // namespace oc::search::detail
