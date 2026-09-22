#pragma once

#include "text/changeset.h"

#include <optional>
#include <vector>

namespace oc {

// Edit script between two int sequences (Myers O((N+M)·D), with a cap on D; beyond the cap the
// middle is replaced wholesale). Returns matched index pairs (i in a, j in b) in increasing order.
std::vector<std::pair<int, int>> diffSequences(const std::vector<int>& a, const std::vector<int>& b, int maxD = 4000);

// ChangeSet turning `a` into `b` (line diff refined to characters inside small hunks).
// Positions follow Text's '\n'-joined coordinates; EOL-only differences produce no changes.
ChangeSet diffTexts(const Text& a, const Text& b);

// Three-way merge. Given the common ancestor `base`, the local version `ours` and the external
// version `theirs`, returns changes to apply to `ours` that incorporate `theirs` — or nullopt when
// their edits overlap ours (conflict).
std::optional<ChangeSet> merge3(const Text& base, const Text& ours, const Text& theirs);

} // namespace oc
