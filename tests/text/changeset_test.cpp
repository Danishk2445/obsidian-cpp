#include "text/changeset.h"
#include "text/diff.h"
#include "text/history.h"
#include "text/selection.h"

#include <QRandomGenerator>
#include <gtest/gtest.h>

using namespace oc;

namespace {
QString applyStr(const ChangeSet& cs, const QString& s) { return cs.apply(Text::fromString(s)).toString(); }

ChangeSet randomChange(QRandomGenerator& rng, qsizetype len, int maxSpecs = 3)
{
    QList<ChangeSpec> specs;
    const int n = 1 + rng.bounded(maxSpecs);
    for (int i = 0; i < n; ++i) {
        const qsizetype from = rng.bounded(int(len + 1));
        const qsizetype to = std::min<qsizetype>(len, from + rng.bounded(5));
        static const QString pool[] = {QString(), QStringLiteral("x"), QStringLiteral("yz"), QStringLiteral("\n"), QStringLiteral("a\nb")};
        specs.push_back({from, to, pool[rng.bounded(5)]});
    }
    return ChangeSet::of(specs, len);
}
} // namespace

TEST(ChangeSet, OfApplySimple)
{
    const QString doc = QStringLiteral("hello world");
    ChangeSet cs = ChangeSet::of({{0, 5, QStringLiteral("HELLO")}, {6, 6, QStringLiteral("big ")}}, doc.size());
    EXPECT_EQ(applyStr(cs, doc), "HELLO big world");
    EXPECT_EQ(cs.length(), 11);
    EXPECT_EQ(cs.newLength(), 15);
}

TEST(ChangeSet, OfOutOfOrderSpecs)
{
    const QString doc = QStringLiteral("abcdef");
    ChangeSet cs = ChangeSet::of({{4, 5, QStringLiteral("E")}, {1, 2, QStringLiteral("B")}}, doc.size());
    EXPECT_EQ(applyStr(cs, doc), "aBcdEf");
}

TEST(ChangeSet, MapPosAssoc)
{
    ChangeSet ins = ChangeSet::of({{2, 2, QStringLiteral("XX")}}, 5);
    EXPECT_EQ(ins.mapPos(2, -1), 2);
    EXPECT_EQ(ins.mapPos(2, 1), 4);
    EXPECT_EQ(ins.mapPos(3), 5);
    ChangeSet del = ChangeSet::of({{1, 3, QString()}}, 5);
    EXPECT_EQ(del.mapPos(2), 1);
    EXPECT_FALSE(del.mapPosOpt(2, -1, MapMode::TrackDel).has_value());
    EXPECT_EQ(del.mapPos(4), 2);
}

TEST(ChangeSet, InvertComposeMapProperties)
{
    QRandomGenerator rng(99);
    for (int iter = 0; iter < 3000; ++iter) {
        QString doc;
        const int n = rng.bounded(20);
        for (int i = 0; i < n; ++i)
            doc += QChar(rng.bounded(4) == 0 ? u'\n' : char16_t(u'a' + rng.bounded(5)));
        const Text t = Text::fromString(doc);
        const ChangeSet a = randomChange(rng, t.length());
        const Text ta = a.apply(t);
        // invert
        const ChangeSet inv = a.invert(t);
        ASSERT_EQ(inv.apply(ta).toString(), t.toString()) << iter;
        // compose
        const ChangeSet b = randomChange(rng, ta.length());
        const Text tab = b.apply(ta);
        ASSERT_EQ(a.compose(b).apply(t).toString(), tab.toString()) << iter;
        // map: two concurrent changes on t commute: a ∘ b.map(a) == b ∘ a.map(b, true)
        const ChangeSet c = randomChange(rng, t.length());
        const Text r1 = c.map(a.desc()).apply(ta);
        const Text r2 = a.map(c.desc(), true).apply(c.apply(t));
        ASSERT_EQ(r1.toString(), r2.toString()) << iter << " a=" << a.debugString().toStdString() << " c=" << c.debugString().toStdString();
        // desc compose consistency
        ASSERT_EQ(a.desc().composeDesc(b.desc()).newLength(), tab.length());
    }
}

TEST(ChangeSet, IterChangesReportsInsertedText)
{
    ChangeSet cs = ChangeSet::of({{0, 1, QStringLiteral("A")}, {3, 3, QStringLiteral("zz")}}, 5);
    QStringList seen;
    cs.iterChanges([&](qsizetype fa, qsizetype ta, qsizetype fb, qsizetype tb, const QString& t) {
        seen << QStringLiteral("%1-%2>%3-%4:%5").arg(fa).arg(ta).arg(fb).arg(tb).arg(t);
    });
    EXPECT_EQ(seen, (QStringList{"0-1>0-1:A", "3-3>3-5:zz"}));
}

TEST(Selection, NormalizesAndMaps)
{
    Selection s = Selection::create({SelRange(5, 7), SelRange(1, 2), SelRange(6, 9)}, 0);
    ASSERT_EQ(s.size(), 2);
    EXPECT_EQ(s.ranges()[0], SelRange(1, 2));
    EXPECT_EQ(s.ranges()[1].from(), 5);
    EXPECT_EQ(s.ranges()[1].to(), 9);
    EXPECT_EQ(s.mainIndex(), 1);
    ChangeSet ins = ChangeSet::of({{0, 0, QStringLiteral("abc")}}, 10);
    Selection m = s.map(ins.desc());
    EXPECT_EQ(m.ranges()[0], SelRange(4, 5));
}

TEST(History, GroupsAdjacentTypingAndUndoes)
{
    History h;
    Text doc = Text::fromString(u"");
    Selection sel = Selection::cursor(0);
    qint64 t = 1000;
    auto type = [&](const QString& s, const QString& ev = QStringLiteral("input.type")) {
        const qsizetype pos = sel.main().head;
        Transaction tr;
        tr.changes = ChangeSet::of({{pos, pos, s}}, doc.length());
        tr.selection = Selection::cursor(pos + s.size());
        tr.userEvent = ev;
        const Text before = doc;
        const Selection selBefore = sel;
        doc = tr.changes.apply(doc);
        sel = *tr.selection;
        h.record(tr, before, selBefore, t);
        t += 100;
    };
    type("h");
    type("e");
    type("y");
    EXPECT_EQ(doc.toString(), "hey");
    EXPECT_EQ(h.undoDepth(), 1); // grouped
    t += 1000;                  // pause -> new group
    type(" you");
    EXPECT_EQ(h.undoDepth(), 2);
    type("!", "input.paste"); // paste never joins
    EXPECT_EQ(h.undoDepth(), 3);

    auto undo = [&] {
        auto tr = h.undo();
        ASSERT_TRUE(tr.has_value());
        const Text before = doc;
        const Selection selBefore = sel;
        doc = tr->changes.apply(doc);
        sel = *tr->selection;
        h.commitUndo(*tr, before, selBefore);
    };
    auto redo = [&] {
        auto tr = h.redo();
        ASSERT_TRUE(tr.has_value());
        const Text before = doc;
        const Selection selBefore = sel;
        doc = tr->changes.apply(doc);
        sel = *tr->selection;
        h.commitRedo(*tr, before, selBefore);
    };
    undo();
    EXPECT_EQ(doc.toString(), "hey you");
    undo();
    EXPECT_EQ(doc.toString(), "hey");
    EXPECT_EQ(sel.main().head, 3);
    redo();
    EXPECT_EQ(doc.toString(), "hey you");
    undo();
    undo();
    EXPECT_EQ(doc.toString(), "");
    EXPECT_FALSE(h.canUndo());
    redo();
    redo();
    redo();
    EXPECT_EQ(doc.toString(), "hey you!");
    // a new change clears redo
    undo();
    type("?");
    EXPECT_FALSE(h.canRedo());
}

TEST(History, MapsThroughExternalChanges)
{
    History h;
    Text doc = Text::fromString(u"abc");
    Transaction tr;
    tr.changes = ChangeSet::of({{3, 3, QStringLiteral("DEF")}}, 3);
    tr.selection = Selection::cursor(6);
    tr.userEvent = QStringLiteral("input.type");
    const Text before = doc;
    doc = tr.changes.apply(doc);
    h.record(tr, before, Selection::cursor(3), 0);
    // external insert at the start (not in history)
    const ChangeSet ext = ChangeSet::of({{0, 0, QStringLiteral(">> ")}}, doc.length());
    doc = ext.apply(doc);
    h.addMapping(ext.desc());
    auto u = h.undo();
    ASSERT_TRUE(u.has_value());
    doc = u->changes.apply(doc);
    EXPECT_EQ(doc.toString(), ">> abc");
}

TEST(Diff, TextsRoundTripAndLocalizeChanges)
{
    QRandomGenerator rng(5);
    for (int iter = 0; iter < 300; ++iter) {
        QString a;
        for (int i = 0; i < 30; ++i)
            a += QStringLiteral("line %1\n").arg(rng.bounded(10));
        QString b = a;
        for (int e = 0; e < 3; ++e) {
            const qsizetype p = rng.bounded(int(b.size() + 1));
            b.insert(p, rng.bounded(2) ? QStringLiteral("NEW\n") : QStringLiteral("x"));
        }
        if (rng.bounded(2))
            b.remove(rng.bounded(int(b.size())), 3);
        const Text ta = Text::fromString(a), tb = Text::fromString(b);
        const ChangeSet d = diffTexts(ta, tb);
        ASSERT_EQ(d.apply(ta).toString(), tb.toString()) << iter;
    }
    // identical texts -> empty change
    EXPECT_TRUE(diffTexts(Text::fromString(u"same\ntext"), Text::fromString(u"same\ntext")).empty());
    // single char edit stays small
    const ChangeSet small = diffTexts(Text::fromString(u"hello world\nsecond"), Text::fromString(u"hello World\nsecond"));
    qsizetype changed = 0;
    small.iterChangedRanges([&](qsizetype fa, qsizetype ta, qsizetype, qsizetype) { changed += ta - fa; });
    EXPECT_EQ(changed, 1);
}

TEST(Diff, Merge3NonOverlappingAndConflict)
{
    const Text base = Text::fromString(u"a\nb\nc\nd\ne");
    const Text ours = Text::fromString(u"A\nb\nc\nd\ne");
    const Text theirs = Text::fromString(u"a\nb\nc\nd\nE");
    auto m = merge3(base, ours, theirs);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->apply(ours).toString(), "A\nb\nc\nd\nE");
    const Text theirs2 = Text::fromString(u"a2\nb\nc\nd\ne");
    EXPECT_FALSE(merge3(base, ours, theirs2).has_value());
}
