// Pure logic of the sidebar views: mentions and snippets (backlinks / outgoing links), the tag tree,
// the outline, bookmarks.json and the all-properties counts — on hand-built metadata.
#include "core/bookmarks/bookmarks.h"
#include "core/metadata/mentions.h"
#include "core/metadata/outline.h"
#include "core/metadata/property_index.h"
#include "core/metadata/tag_tree.h"
#include "markdown/metadata.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <gtest/gtest.h>

#include <ostream>

using namespace oc;

inline void PrintTo(const QString& s, std::ostream* os) { *os << '"' << s.toStdString() << '"'; }
namespace oc {
inline void PrintTo(const MatchRange& r, std::ostream* os) { *os << '[' << r.from << ',' << r.to << ')'; }
}

namespace {

// Positions of `needle` (n-th occurrence) in `text`, as metadata ranges (line/col not needed here).
md::Range rangeOf(const QString& text, const QString& needle, int nth = 0)
{
    qsizetype at = -1;
    for (int i = 0; i <= nth; ++i)
        at = text.indexOf(needle, at + 1);
    md::Range r;
    r.start.offset = at;
    r.end.offset = at + needle.size();
    r.start.line = int(text.left(at).count(u'\n'));
    r.end.line = int(text.left(at + needle.size()).count(u'\n'));
    r.start.col = int(at - (text.lastIndexOf(u'\n', at - 1) + 1));
    return r;
}

md::LinkCache link(const QString& text, const QString& original, const QString& target, int nth = 0)
{
    md::LinkCache l;
    l.link = target;
    l.original = original;
    l.displayText = target;
    l.position = rangeOf(text, original, nth);
    return l;
}

md::Range lines(const QString& text, int from, int to)
{
    const QStringList ls = text.split(u'\n');
    md::Range r;
    qsizetype off = 0;
    for (int i = 0; i < from; ++i)
        off += ls[i].size() + 1;
    r.start = {from, 0, off};
    qsizetype end = off;
    for (int i = from; i <= to; ++i)
        end += ls[i].size() + (i < to ? 1 : 0);
    r.end = {to, int(ls[to].size()), end};
    return r;
}

md::HeadingCache heading(const QString& text, int level, int line)
{
    md::HeadingCache h;
    h.level = level;
    h.heading = text.split(u'\n')[line].mid(level + 1);
    h.position = lines(text, line, line);
    return h;
}

} // namespace

// ---- mentions --------------------------------------------------------------------------------------

TEST(Mentions, WordCharacters)
{
    for (QChar c : QStringLiteral("aZ5éßДж"))
        EXPECT_TRUE(isMentionWordChar(c)) << QString(c).toStdString();
    for (QChar c : QStringLiteral(" -_.,!#[]中あ한"))
        EXPECT_FALSE(isMentionWordChar(c)) << QString(c).toStdString();
}

TEST(Mentions, WholeWordCaseInsensitive)
{
    const QString s = QStringLiteral("Note, note! Notebook anote NOTE");
    EXPECT_EQ(findMentions(s, QStringLiteral("Note")), (QList<MatchRange>{{0, 4}, {6, 10}, {27, 31}}));
    // a name that starts / ends with a non-word character needs no boundary on that side
    EXPECT_EQ(findMentions(QStringLiteral("use C++x now"), QStringLiteral("C++")), (QList<MatchRange>{{4, 7}}));
    EXPECT_TRUE(findMentions(QStringLiteral("aC++"), QStringLiteral("C++")).isEmpty());
    // CJK names match inside words (Obsidian's word class leaves them out)
    EXPECT_EQ(findMentions(QStringLiteral("这是中文字"), QStringLiteral("中文")), (QList<MatchRange>{{2, 4}}));
    // multi-word names and an empty name
    EXPECT_EQ(findMentions(QStringLiteral("my big note."), QStringLiteral("Big Note")), (QList<MatchRange>{{3, 11}}));
    EXPECT_TRUE(findMentions(QStringLiteral("abc"), QString()).isEmpty());
}

TEST(Mentions, LinkedRanges)
{
    const QString s = QStringLiteral("---\nup: \"[[Target]]\"\n---\nSee [[Target#Head|t]] and [[Other]] and ![[target]].\n[x](Target.md)\n");
    md::CachedMetadata meta;
    meta.frontmatterLinks << link(s, QStringLiteral("[[Target]]"), QStringLiteral("Target"));
    meta.links << link(s, QStringLiteral("[[Target#Head|t]]"), QStringLiteral("Target#Head")) << link(s, QStringLiteral("[[Other]]"), QStringLiteral("Other"));
    meta.embeds << link(s, QStringLiteral("![[target]]"), QStringLiteral("target"));
    md::LinkCache mdLink = link(s, QStringLiteral("[x](Target.md)"), QStringLiteral("Target.md"));
    mdLink.markdown = true;
    meta.links << mdLink;
    QStringList asked;
    const QList<MatchRange> r = linkedMentionRanges(meta, [&](const QString& linkpath) {
        asked << linkpath;
        return linkpath.compare(QStringLiteral("target"), Qt::CaseInsensitive) == 0 || linkpath == u"Target.md";
    });
    ASSERT_EQ(r.size(), 4);
    EXPECT_EQ(s.mid(r[0].from, r[0].to - r[0].from), QStringLiteral("[[Target]]"));
    EXPECT_EQ(s.mid(r[1].from, r[1].to - r[1].from), QStringLiteral("[[Target#Head|t]]"));
    EXPECT_EQ(s.mid(r[2].from, r[2].to - r[2].from), QStringLiteral("![[target]]"));
    EXPECT_EQ(s.mid(r[3].from, r[3].to - r[3].from), QStringLiteral("[x](Target.md)"));
    EXPECT_TRUE(asked.contains(QStringLiteral("Target"))); // the subpath is cut off before resolving
}

TEST(Mentions, UnlinkedRangesSkipLinksAndFrontmatter)
{
    const QString s = QStringLiteral("---\ntitle: Target\n---\nSee Target, [[Target]] and ![[Target|x]].\nTGT and target.\n");
    md::CachedMetadata meta;
    md::Range fm;
    fm.start = {0, 0, 0};
    fm.end = {2, 3, s.indexOf(QStringLiteral("---\nSee")) + 3};
    meta.frontmatterPosition = fm;
    meta.links << link(s, QStringLiteral("[[Target]]"), QStringLiteral("Target"));
    meta.embeds << link(s, QStringLiteral("![[Target|x]]"), QStringLiteral("Target"));
    const QList<MatchRange> r = unlinkedMentionRanges(s, {QStringLiteral("Target"), QStringLiteral("tgt"), QStringLiteral("TARGET")}, &meta);
    QStringList found;
    for (const MatchRange& m : r)
        found << s.mid(m.from, m.to - m.from);
    EXPECT_EQ(found, (QStringList{QStringLiteral("Target"), QStringLiteral("TGT"), QStringLiteral("target")}));
    // without metadata every whole-word occurrence counts
    EXPECT_EQ(unlinkedMentionRanges(s, {QStringLiteral("Target")}, nullptr).size(), 5);
}

TEST(Mentions, OutgoingCandidatesAndMentions)
{
    const QList<MentionCandidate> c = mentionCandidates({{QStringLiteral("A/Note.md"), {}},
                                                         {QStringLiteral("B/note.md"), {QStringLiteral("Memo")}},
                                                         {QStringLiteral("Self.md"), {QStringLiteral("me")}},
                                                         {QStringLiteral("Item 10.md"), {}},
                                                         {QStringLiteral("Item 9.md"), {}}},
                                                        QStringLiteral("Self.md"));
    QStringList texts;
    for (const MentionCandidate& m : c)
        texts << m.text;
    EXPECT_EQ(texts, (QStringList{QStringLiteral("item 9"), QStringLiteral("item 10"), QStringLiteral("memo"), QStringLiteral("note")}));
    EXPECT_EQ(c.last().targets, (QStringList{QStringLiteral("A/Note.md"), QStringLiteral("B/note.md")}));

    const QString s = QStringLiteral("---\nnote: note\n---\nA note, a Memo, [[Note]], notes and item 9.\n");
    md::CachedMetadata meta;
    md::Range fm;
    fm.start = {0, 0, 0};
    fm.end = {2, 3, s.indexOf(QStringLiteral("---\nA")) + 3};
    meta.frontmatterPosition = fm;
    meta.links << link(s, QStringLiteral("[[Note]]"), QStringLiteral("Note"));
    const QList<OutgoingMention> m = outgoingUnlinkedMentions(s, &meta, c);
    QStringList found;
    for (const OutgoingMention& om : m)
        found << s.mid(om.range.from, om.range.to - om.range.from) + QLatin1Char('>') + om.targets.join(QLatin1Char(','));
    EXPECT_EQ(found, (QStringList{QStringLiteral("note>A/Note.md,B/note.md"), QStringLiteral("Memo>B/note.md"), QStringLiteral("item 9>Item 9.md")}));
}

TEST(Mentions, LineSnippets)
{
    const QString longLine = QString(150, u'x') + QStringLiteral(" KEY ") + QString(150, u'y');
    const QString s = QStringLiteral("first\n") + longLine + QStringLiteral("\nlast");
    const int at = int(s.indexOf(QStringLiteral("KEY")));
    const MatchSnippet sn = lineSnippet(s, {at, at + 3});
    EXPECT_EQ(sn.from, at - 100);
    EXPECT_EQ(sn.to, at + 3 + 100);
    EXPECT_TRUE(sn.ellipsisBefore);
    EXPECT_TRUE(sn.ellipsisAfter);
    const MatchSnippet shortOne = lineSnippet(s, {0, 5});
    EXPECT_EQ(shortOne.from, 0);
    EXPECT_EQ(shortOne.to, 5);
    EXPECT_FALSE(shortOne.ellipsisBefore);
    EXPECT_FALSE(shortOne.ellipsisAfter);
}

TEST(Mentions, SnippetGrouping)
{
    const QString s = QStringLiteral("alpha beta gamma\ndelta beta\n");
    const QList<MatchRange> matches{{17, 22}, {0, 5}, {6, 10}, {23, 27}};
    const QList<MatchSnippet> merged = buildMatchSnippets(s, matches, false, nullptr);
    ASSERT_EQ(merged.size(), 2); // one per line; matches sorted
    EXPECT_EQ(merged[0].matches, (QList<MatchRange>{{0, 5}, {6, 10}}));
    EXPECT_EQ(s.mid(merged[0].from, merged[0].to - merged[0].from), QStringLiteral("alpha beta gamma"));
    EXPECT_EQ(merged[1].matches, (QList<MatchRange>{{17, 22}, {23, 27}}));
    EXPECT_EQ(buildMatchSnippets(s, matches, false, nullptr, true).size(), 4); // unlinked mentions: one per match
}

TEST(Mentions, ExtraContext)
{
    const QString s = QStringLiteral("# H\n\npara one\npara two\n\n- a\n  - b KEY\n  - c\n- d\n\nlast\n");
    md::CachedMetadata meta;
    auto item = [&](int from, int to, int parent) {
        md::ListItemCache li;
        li.position = lines(s, from, to);
        li.parent = parent;
        return li;
    };
    meta.listItems << item(5, 7, -5) << item(6, 6, 5) << item(7, 7, 5) << item(8, 8, -5);
    md::SectionCache para;
    para.type = QStringLiteral("paragraph");
    para.position = lines(s, 2, 3);
    meta.sections << para;
    const int key = int(s.indexOf(QStringLiteral("KEY")));
    const MatchSnippet li = extraContextSnippet(s, {key, key + 3}, &meta);
    EXPECT_EQ(s.mid(li.from, li.to - li.from), QStringLiteral("  - b KEY"));
    const int two = int(s.indexOf(QStringLiteral("two")));
    const MatchSnippet sec = extraContextSnippet(s, {two, two + 3}, &meta);
    EXPECT_EQ(s.mid(sec.from, sec.to - sec.from), QStringLiteral("para one\npara two"));
    // no metadata: the list block / the paragraph around the match
    const int dash = int(s.indexOf(QStringLiteral("- a")));
    const MatchSnippet list = extraContextSnippet(s, {dash + 2, dash + 3}, nullptr);
    EXPECT_EQ(s.mid(list.from, list.to - list.from), QStringLiteral("- a\n  - b KEY\n  - c"));
    const MatchSnippet p = extraContextSnippet(s, {two, two + 3}, nullptr);
    EXPECT_EQ(s.mid(p.from, p.to - p.from), QStringLiteral("para one\npara two"));
}

TEST(Mentions, MoreContextSteps)
{
    const QString s = QStringLiteral("one\ntwo\nthree KEY\nfour\nfive");
    const int key = int(s.indexOf(QStringLiteral("KEY")));
    const MatchSnippet sn = lineSnippet(s, {key, key + 3});
    const int before = moreContextBefore(s, nullptr, sn.from);
    EXPECT_EQ(s.mid(before, sn.from - before), QStringLiteral("two\n"));
    const int after = moreContextAfter(s, nullptr, sn.to);
    EXPECT_EQ(s.mid(sn.to, after - sn.to), QStringLiteral("\nfour"));
    EXPECT_EQ(moreContextBefore(s, nullptr, 0), 0);
    EXPECT_EQ(moreContextAfter(s, nullptr, int(s.size())), int(s.size()));
}

TEST(Mentions, LinkTextInTables)
{
    EXPECT_EQ(mentionLinkText(QStringLiteral("[[Target|target]]"), true), QStringLiteral("[[Target\\|target]]"));
    EXPECT_EQ(mentionLinkText(QStringLiteral("[[Target|target]]"), false), QStringLiteral("[[Target|target]]"));
    const QString s = QStringLiteral("| a | target |\n|---|---|\n");
    md::CachedMetadata meta;
    md::SectionCache t;
    t.type = QStringLiteral("table");
    t.position = lines(s, 0, 1);
    meta.sections << t;
    EXPECT_TRUE(isInTable(&meta, {6, 12}));
    EXPECT_FALSE(isInTable(nullptr, {6, 12}));
}

// ---- tag tree ---------------------------------------------------------------------------------------

namespace {
QStringList flatten(const QList<TagNode>& nodes, int depth = 0)
{
    QStringList out;
    for (const TagNode& n : nodes) {
        out << QString(depth * 2, u' ') + n.parent + n.name + QLatin1Char(' ') + QString::number(n.count);
        out << flatten(n.children, depth + 1);
    }
    return out;
}
} // namespace

TEST(TagTree, NestedAndFlat)
{
    const QHash<QString, int> counts{{QStringLiteral("#a"), 3},     {QStringLiteral("#a/b"), 2}, {QStringLiteral("#a/b/c"), 1},
                                     {QStringLiteral("#z"), 5},     {QStringLiteral("#m"), 3}};
    EXPECT_EQ(flatten(buildTagTree(counts, true, CountSortOrder::Frequency)),
              (QStringList{"z 5", "a 3", "  a/b 2", "    a/b/c 1", "m 3"}));
    EXPECT_EQ(flatten(buildTagTree(counts, false, CountSortOrder::Frequency)), (QStringList{"z 5", "a 3", "m 3", "a/b 2", "a/b/c 1"}));
    EXPECT_EQ(flatten(buildTagTree(counts, false, CountSortOrder::FrequencyReverse)), (QStringList{"a/b/c 1", "a/b 2", "a 3", "m 3", "z 5"}));
    EXPECT_EQ(flatten(buildTagTree(counts, false, CountSortOrder::Alphabetical)), (QStringList{"a 3", "a/b 2", "a/b/c 1", "m 3", "z 5"}));
    EXPECT_EQ(flatten(buildTagTree(counts, true, CountSortOrder::AlphabeticalReverse)),
              (QStringList{"z 5", "m 3", "a 3", "  a/b 2", "    a/b/c 1"}));
    const QList<TagNode> nested = buildTagTree(counts, true, CountSortOrder::Frequency);
    EXPECT_EQ(nested[1].children[0].tag, QStringLiteral("#a/b"));
    EXPECT_EQ(nested[1].children[0].name, QStringLiteral("b"));
}

TEST(TagTree, MissingParentsAndCase)
{
    EXPECT_EQ(flatten(buildTagTree({{QStringLiteral("#x/y"), 1}}, true, CountSortOrder::Frequency)), (QStringList{"x 0", "  x/y 1"}));
    // children hang under their parent case-insensitively
    EXPECT_EQ(flatten(buildTagTree({{QStringLiteral("#Proj"), 2}, {QStringLiteral("#proj/sub"), 1}}, true, CountSortOrder::Alphabetical)),
              (QStringList{"Proj 2", "  proj/sub 1"}));
    // natural order of numbers
    EXPECT_EQ(flatten(buildTagTree({{QStringLiteral("#tag10"), 1}, {QStringLiteral("#tag2"), 1}}, false, CountSortOrder::Alphabetical)),
              (QStringList{"tag2 1", "tag10 1"}));
    EXPECT_EQ(countSortOrderFromId(QStringLiteral("frequencyReverse")), CountSortOrder::FrequencyReverse);
    EXPECT_FALSE(countSortOrderFromId(QStringLiteral("bogus")));
}

// ---- outline ----------------------------------------------------------------------------------------

TEST(Outline, TreeAndCurrentSection)
{
    const QString s = QStringLiteral("# A\n\n## B\n\n### C\n\n## D\n\n# E\n\n### F\n");
    const QList<md::HeadingCache> hs{heading(s, 1, 0), heading(s, 2, 2), heading(s, 3, 4), heading(s, 2, 6), heading(s, 1, 8), heading(s, 3, 10)};
    const QList<OutlineNode> tree = buildOutline(hs);
    ASSERT_EQ(tree.size(), 2);
    EXPECT_EQ(tree[0].text, QStringLiteral("A"));
    ASSERT_EQ(tree[0].children.size(), 2);
    EXPECT_EQ(tree[0].children[0].text, QStringLiteral("B"));
    EXPECT_EQ(tree[0].children[0].children[0].text, QStringLiteral("C"));
    EXPECT_EQ(tree[0].children[1].text, QStringLiteral("D"));
    EXPECT_EQ(tree[1].children[0].text, QStringLiteral("F"));
    EXPECT_EQ(tree[1].children[0].index, 5);
    EXPECT_EQ(headingIndexAtLine(hs, 5), 2);
    EXPECT_EQ(headingIndexAtLine(hs, 6), 3);
    EXPECT_EQ(headingIndexAtLine(hs, 100), 5);
    const QList<md::HeadingCache> late{heading(s, 2, 2)};
    EXPECT_EQ(headingIndexAtLine(late, 0), -1);
}

TEST(Outline, DisplayText)
{
    EXPECT_EQ(headingDisplayText(QStringLiteral("**Bold** and *it* `code` [[Note#Sec|alias]] [[Target]] [md](x.md) ==mark== ~~del~~%%c%% <b>x</b>")),
              QStringLiteral("Bold and it code alias Target md mark del x"));
    EXPECT_EQ(headingDisplayText(QStringLiteral("[[Note#Sec]]")), QStringLiteral("Note > Sec"));
    EXPECT_EQ(headingDisplayText(QStringLiteral("snake_case_name")), QStringLiteral("snake_case_name"));
    EXPECT_EQ(headingDisplayText(QStringLiteral("%%only%%")), QStringLiteral("%%only%%")); // nothing left: the raw heading
}

TEST(Outline, MoveSection)
{
    const QString s = QStringLiteral("# A\na\n## A1\nx\n# B\nb\n# C\nc\n");
    const QList<md::HeadingCache> hs{heading(s, 1, 0), heading(s, 2, 2), heading(s, 1, 4), heading(s, 1, 6)};
    auto apply = [&](const TextEdit& e) { return s.left(e.from) + e.text + s.mid(e.to); };
    // C before A
    std::optional<TextEdit> e = moveSectionEdit(s, hs, 3, 0);
    ASSERT_TRUE(e);
    EXPECT_EQ(apply(*e), QStringLiteral("# C\nc\n\n# A\na\n## A1\nx\n# B\nb\n"));
    // A (with A1) to the end
    e = moveSectionEdit(s, hs, 0, 4);
    ASSERT_TRUE(e);
    EXPECT_EQ(apply(*e), QStringLiteral("# B\nb\n# C\nc\n\n# A\na\n## A1\nx\n\n"));
    // B before A: the text before the drop point gets a blank line too
    e = moveSectionEdit(s, hs, 2, 1);
    ASSERT_TRUE(e);
    EXPECT_EQ(apply(*e), QStringLiteral("# A\na\n\n# B\nb\n\n## A1\nx\n# C\nc\n"));
    // into its own section / onto itself: nothing
    EXPECT_FALSE(moveSectionEdit(s, hs, 0, 1));
    EXPECT_FALSE(moveSectionEdit(s, hs, 2, 2));
    EXPECT_FALSE(moveSectionEdit(s, hs, 2, 3)); // right after itself
}

// ---- bookmarks --------------------------------------------------------------------------------------

TEST(Bookmarks, JsonRoundTrip)
{
    const QByteArray json = R"({"items":[
      {"type":"file","ctime":1700000000000,"path":"Notes/A.md"},
      {"type":"file","ctime":1700000000001,"path":"Notes/A.md","subpath":"#Heading","title":"Custom"},
      {"type":"folder","ctime":1700000000002,"path":"Notes"},
      {"type":"search","ctime":1700000000003,"query":"tag:#x"},
      {"type":"graph","ctime":1700000000004,"title":"Untitled Graph","options":{"search":"","showTags":false}},
      {"type":"group","ctime":1700000000005,"title":"Group","items":[
        {"type":"file","ctime":1700000000006,"path":"B.md","title":""},
        {"type":"heading","ctime":7,"path":"C.md","subpath":"H"}]},
      {"type":"url","ctime":1700000000008,"url":"https://obsidian.md","title":"Obsidian"}]})";
    const QJsonObject in = QJsonDocument::fromJson(json).object();
    BookmarkModel m;
    ASSERT_TRUE(m.load(in));
    EXPECT_EQ(m.toJson(), in);
    const QByteArray out = QJsonDocument(m.toJson()).toJson(QJsonDocument::Compact);
    EXPECT_TRUE(out.contains("\"ctime\":1700000000000")) << out.constData(); // integers, like Obsidian
    ASSERT_EQ(m.items().size(), 7u);
    EXPECT_EQ(m.all().size(), 9);
    Bookmark* group = m.items()[5].get();
    EXPECT_TRUE(group->isGroup());
    EXPECT_EQ(group->items[0]->parent, group);
    // titles (Obsidian's getItemTitle)
    EXPECT_EQ(BookmarkModel::title(*m.items()[0]), QStringLiteral("A"));
    EXPECT_EQ(BookmarkModel::title(*m.items()[1]), QStringLiteral("Custom"));
    EXPECT_EQ(BookmarkModel::title(*m.items()[2]), QStringLiteral("Notes"));
    EXPECT_EQ(BookmarkModel::title(*m.items()[3]), QStringLiteral("tag:#x"));
    EXPECT_EQ(BookmarkModel::title(*group->items[0]), QStringLiteral("B")); // an empty custom title shows the default
    const Bookmark withSub = [] {
        Bookmark b;
        b.type = QStringLiteral("file");
        b.path = QStringLiteral("x/Y.md");
        b.subpath = QStringLiteral("#^abc");
        return b;
    }();
    EXPECT_EQ(BookmarkModel::title(withSub), QStringLiteral("Y#^abc"));
    // invalid shape
    BookmarkModel bad;
    EXPECT_FALSE(bad.load(QJsonObject{{QStringLiteral("items"), 5}}));
    EXPECT_TRUE(bad.load(QJsonObject{}));
}

TEST(Bookmarks, EditingTheModel)
{
    BookmarkModel m;
    int changes = 0;
    QObject::connect(&m, &BookmarkModel::changed, [&] { ++changes; });
    Bookmark* a = m.add(Bookmark::file(QStringLiteral("a.md")));
    Bookmark* b = m.add(Bookmark::folder(QStringLiteral("dir")));
    Bookmark* c = m.add(Bookmark::search(QStringLiteral("q")));
    Bookmark* g = m.add(Bookmark::group(QStringLiteral("G")));
    EXPECT_EQ(changes, 4);
    EXPECT_GT(a->ctime, 0);
    // Obsidian's moveItem: moving down within the list counts the index before removal
    m.move(a, nullptr, 2);
    EXPECT_EQ(m.indexOf(a), 1);
    EXPECT_EQ(m.items()[0].get(), b);
    m.move(c, g, 0);
    EXPECT_EQ(c->parent, g);
    EXPECT_EQ(g->items.size(), 1u);
    m.move(g, g, 0); // into itself: ignored
    EXPECT_EQ(g->parent, nullptr);
    EXPECT_EQ(m.findByPath(QStringLiteral("a.md")), a);
    EXPECT_EQ(m.findByPath(QStringLiteral("dir")), b);
    Bookmark* h = m.add(Bookmark::file(QStringLiteral("dir/n.md"), QStringLiteral("#H")), g);
    EXPECT_EQ(m.findByPath(QStringLiteral("dir/n.md")), nullptr); // subpath bookmarks aren't the file's
    m.renamePath(QStringLiteral("dir"), QStringLiteral("folder"));
    EXPECT_EQ(b->path, QStringLiteral("folder"));
    EXPECT_EQ(h->path, QStringLiteral("folder/n.md"));
    m.remove(g); // with its items
    EXPECT_EQ(m.all().size(), 2);
    EXPECT_FALSE(m.contains(h));
    const QJsonObject json = m.toJson();
    EXPECT_EQ(json.value(QStringLiteral("items")).toArray().size(), 2);
    EXPECT_EQ(json.value(QStringLiteral("items")).toArray()[0].toObject().keys(), (QStringList{"ctime", "path", "type"}));
}

// ---- properties -------------------------------------------------------------------------------------

TEST(Properties, TypesAndCounts)
{
    EXPECT_EQ(inferPropertyType(QVariant()), QStringLiteral("text"));
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-31")), QStringLiteral("date"));
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-31T10:30")), QStringLiteral("datetime"));
    EXPECT_EQ(inferPropertyType(QStringLiteral("hello")), QStringLiteral("text"));
    EXPECT_EQ(inferPropertyType(3.5), QStringLiteral("number"));
    EXPECT_EQ(inferPropertyType(true), QStringLiteral("checkbox"));
    EXPECT_EQ(inferPropertyType(QVariantList{QStringLiteral("a"), QVariant()}), QStringLiteral("multitext"));
    EXPECT_EQ(inferPropertyType(QVariantList{1.0}), QStringLiteral("unknown"));
    EXPECT_EQ(propertyTypeIcon(QStringLiteral("number")), QStringLiteral("binary"));
    EXPECT_EQ(propertyTypeIcon(QStringLiteral("bogus")), QStringLiteral("file-question"));

    auto meta = [](QList<md::FrontmatterValue> values) {
        md::CachedMetadata m;
        m.frontmatter = values;
        return m;
    };
    const md::CachedMetadata a = meta({{QStringLiteral("Status"), QStringLiteral("draft")}, {QStringLiteral("tags"), QVariantList{QStringLiteral("x")}}});
    const md::CachedMetadata b = meta({{QStringLiteral("status"), QStringLiteral("done")}, {QStringLiteral("due"), QVariant()}});
    const md::CachedMetadata c = meta({{QStringLiteral("due"), QStringLiteral("2024-02-01")}, {QStringLiteral("count"), 2.0}});
    const md::CachedMetadata none;
    QHash<QString, QPair<QString, QString>> assigned = reservedPropertyTypes();
    assigned.insert(QStringLiteral("priority"), {QStringLiteral("priority"), QStringLiteral("number")});
    const QHash<QString, PropertyInfo> all = collectProperties({&a, &b, &c, &none}, assigned);
    EXPECT_EQ(all.value(QStringLiteral("status")).occurrences, 2);
    EXPECT_EQ(all.value(QStringLiteral("status")).name, QStringLiteral("Status")); // first spelling
    EXPECT_EQ(all.value(QStringLiteral("status")).type, QStringLiteral("text"));
    EXPECT_EQ(all.value(QStringLiteral("due")).type, QStringLiteral("date")); // the first non-null value
    EXPECT_EQ(all.value(QStringLiteral("tags")).type, QStringLiteral("tags"));
    EXPECT_EQ(all.value(QStringLiteral("tags")).occurrences, 1);
    EXPECT_EQ(all.value(QStringLiteral("cssclasses")).occurrences, 0);
    EXPECT_EQ(all.value(QStringLiteral("priority")).type, QStringLiteral("number"));

    QList<PropertyInfo> list = all.values();
    sortProperties(list, CountSortOrder::Frequency);
    QStringList names;
    for (const PropertyInfo& p : std::as_const(list))
        names << p.name + QLatin1Char(':') + QString::number(p.occurrences);
    EXPECT_EQ(names, (QStringList{"due:2", "Status:2", "count:1", "tags:1", "aliases:0", "cssclasses:0", "priority:0"}));
    sortProperties(list, CountSortOrder::AlphabeticalReverse);
    EXPECT_EQ(list.first().name, QStringLiteral("tags"));
    sortProperties(list, CountSortOrder::FrequencyReverse);
    EXPECT_EQ(list.first().occurrences, 0);
    EXPECT_EQ(list.last().occurrences, 2);
}
