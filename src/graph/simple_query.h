#pragma once

#include "graph/graph_data.h"

#include <QString>

namespace oc::graph {

// Stand-in for the search engine until it is wired in (tests, headless demos): whitespace
// separated terms are ANDed, "OR" separates alternatives, "quoted phrases" and a leading '-'
// (negation) are supported, as are the path:, file: and tag: operators. Plain terms match the
// node id or label, case-insensitively; tag:#x matches tag nodes and files linking to them.
// An empty query yields an empty predicate (no filter).
NodePredicate compileSimpleQuery(const QString& query);

} // namespace oc::graph
