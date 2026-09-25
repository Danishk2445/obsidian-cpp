#include "core/metadata/tag_tree.h"

#include <QCollator>

#include <algorithm>
#include <functional>

namespace oc {

QString countSortOrderId(CountSortOrder order)
{
    switch (order) {
    case CountSortOrder::Alphabetical: return QStringLiteral("alphabetical");
    case CountSortOrder::AlphabeticalReverse: return QStringLiteral("alphabeticalReverse");
    case CountSortOrder::Frequency: return QStringLiteral("frequency");
    case CountSortOrder::FrequencyReverse: return QStringLiteral("frequencyReverse");
    }
    return QStringLiteral("frequency");
}

std::optional<CountSortOrder> countSortOrderFromId(const QString& id)
{
    for (CountSortOrder o : {CountSortOrder::Alphabetical, CountSortOrder::AlphabeticalReverse, CountSortOrder::Frequency, CountSortOrder::FrequencyReverse})
        if (countSortOrderId(o) == id)
            return o;
    return std::nullopt;
}

int naturalTagCompare(const QString& a, const QString& b)
{
    static const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    const int c = collator.compare(a, b);
    return c != 0 ? c : QString::compare(a, b);
}

QList<TagNode> buildTagTree(const QHash<QString, int>& counts, bool nested, CountSortOrder order)
{
    // Nodes keyed by lowercase tag; `owner` records where a node hangs (empty = the root).
    struct Entry {
        TagNode node;
        QString parentKey;
        QList<QString> childKeys;
    };
    QHash<QString, Entry> entries;
    auto split = [](const QString& tag, TagNode& n) {
        const QString bare = tag.startsWith(u'#') ? tag.mid(1) : tag;
        const qsizetype slash = bare.lastIndexOf(u'/');
        n.name = slash < 0 ? bare : bare.mid(slash + 1);
        n.parent = slash < 0 ? QString() : bare.left(slash + 1);
    };
    QStringList keys = counts.keys();
    keys.sort();
    for (const QString& tag : std::as_const(keys)) {
        Entry e;
        e.node.tag = tag;
        e.node.count = counts.value(tag);
        split(tag, e.node);
        entries.insert(tag.toLower(), e);
    }
    QList<QString> roots;
    // Hang every tag under its parent (creating missing parents, recursively).
    std::function<void(const QString&)> attach = [&](const QString& key) {
        Entry& e = entries[key];
        const QString tag = e.node.tag;
        const qsizetype slash = tag.lastIndexOf(u'/');
        if (!nested || slash < 0) {
            if (!roots.contains(key))
                roots.push_back(key);
            return;
        }
        const QString parentTag = tag.left(slash);
        const QString parentKey = parentTag.toLower();
        const bool created = !entries.contains(parentKey);
        if (created) {
            Entry p;
            p.node.tag = parentTag;
            p.node.count = 0;
            split(parentTag, p.node);
            entries.insert(parentKey, p);
        }
        Entry& parent = entries[parentKey];
        if (!parent.childKeys.contains(key))
            parent.childKeys.push_back(key);
        entries[key].parentKey = parentKey;
        if (created)
            attach(parentKey);
    };
    for (const QString& tag : std::as_const(keys))
        attach(tag.toLower());

    auto less = [&](const TagNode& a, const TagNode& b) {
        switch (order) {
        case CountSortOrder::Alphabetical: return naturalTagCompare(a.tag, b.tag) < 0;
        case CountSortOrder::AlphabeticalReverse: return naturalTagCompare(a.tag, b.tag) > 0;
        case CountSortOrder::Frequency:
            return a.count != b.count ? a.count > b.count : naturalTagCompare(a.tag, b.tag) < 0;
        case CountSortOrder::FrequencyReverse:
            return a.count != b.count ? a.count < b.count : naturalTagCompare(a.tag, b.tag) < 0;
        }
        return false;
    };
    std::function<TagNode(const QString&)> build = [&](const QString& key) {
        const Entry& e = entries[key];
        TagNode n = e.node;
        for (const QString& c : e.childKeys)
            n.children.push_back(build(c));
        std::stable_sort(n.children.begin(), n.children.end(), less);
        return n;
    };
    QList<TagNode> out;
    for (const QString& r : std::as_const(roots))
        out.push_back(build(r));
    std::stable_sort(out.begin(), out.end(), less);
    return out;
}

} // namespace oc
