#pragma once

#include <QString>
#include <QStringView>

namespace oc {

// Obsidian's word-count plugin rules (status bar "N words  M characters"):
//  - a number ("1,234.5") is one word;
//  - a run of letters, apostrophes and hyphens is one word — so "don't" is one word, "foo_bar" and
//    "abc123" are two, and a lone "-" (a list bullet) counts too;
//  - every CJK ideograph, kana or Tibetan letter is a word of its own (Hangul forms runs);
//  - characters are UTF-16 code units of the counted text, line breaks included.
// For a whole note the YAML frontmatter is excluded; for a selection nothing is.
int countWords(QStringView text);
QStringView withoutFrontmatter(QStringView note);

struct TextCounts {
    int words = 0;
    int characters = 0;
};
TextCounts countNote(QStringView note);          // frontmatter excluded
TextCounts countSelection(QStringView selection);

} // namespace oc
