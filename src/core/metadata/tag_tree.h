#pragma once

// Obsidian's Tags view model (core plugin "tag-pane"): the vault's tags with their counts, flat or as
// a tree of nested tags ("#a/b" under "#a"), in one of four sort orders.

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace oc {

// View state "sortOrder" of the tag pane (and of the All properties view).
enum class CountSortOrder { Alphabetical, AlphabeticalReverse, Frequency, FrequencyReverse };
QString countSortOrderId(CountSortOrder order); // "alphabetical", "frequencyReverse", ...
std::optional<CountSortOrder> countSortOrderFromId(const QString& id);

struct TagNode {
    QString tag;    // "#parent/child", spelled as in the counts
    QString name;   // last segment: "child"
    QString parent; // what precedes it without '#': "parent/" (shown faint in the flat list)
    int count = 0;
    QList<TagNode> children;
};

// `counts` as returned by vaultTags() (every parent of a nested tag counted too). Nested: children
// hang under their parent tag (looked up case-insensitively; a parent missing from the counts is
// created with count 0). Siblings are sorted by `order` (Obsidian: natural order of the whole tag,
// frequency ties broken by name).
QList<TagNode> buildTagTree(const QHash<QString, int>& counts, bool nested, CountSortOrder order);

// Natural, case-insensitive comparison (Obsidian's Intl.Collator with numeric collation).
int naturalTagCompare(const QString& a, const QString& b);

} // namespace oc
