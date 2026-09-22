#pragma once

namespace oc {

// A [from, to) range of UTF-16 indices, for highlighting matches (fuzzy matcher, simple search, global search).
struct MatchRange {
    int from = 0;
    int to = 0;
    bool operator==(const MatchRange&) const = default;
};

} // namespace oc
