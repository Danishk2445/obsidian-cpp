#pragma once

#include "markdown/syntax_tree.h"
#include "text/text.h"

#include <optional>

namespace oc {

// A foldable region: `line` stays visible (with a "…" placeholder), lines (line, lastLine] are hidden.
struct FoldRange {
    int line = 0;
    int lastLine = 0;
    bool operator==(const FoldRange&) const = default;
};

struct FoldOptions {
    bool headings = true; // Obsidian "Fold heading"
    bool indent = true;   // Obsidian "Fold indent"
    int tabSize = 4;
};

// Obsidian's fold services, tried in this order:
//  - heading: an ATX or setext heading folds up to the next heading of the same or a higher level
//    (trailing blank lines included);
//  - indent: a non-blank line folds the following lines indented deeper than it (blank lines in
//    between are skipped, trailing ones stay visible);
//  - frontmatter: the opening "---" (preceded only by blank lines) folds up to the closing "---".
std::optional<FoldRange> foldableAt(const Text& text, const md::SyntaxTree& tree, int line, const FoldOptions& options);
// Cheap check for fold indicators (no scan to the end of the region).
bool isFoldable(const Text& text, const md::SyntaxTree& tree, int line, const FoldOptions& options);

// 1..6 for heading lines (as Obsidian sees them: the line starts with '#'s, or is setext text), else 100.
int foldHeadingLevel(const Text& text, const md::SyntaxTree& tree, int line);
// Leading whitespace in columns (tabs to tab stops), -1 for a blank line.
int indentColumns(QStringView line, int tabSize);

} // namespace oc
