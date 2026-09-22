// Core logic of the Templates / Daily notes / Unique note plugins.
#include "core/plugins/note_plugins.h"

#include <QSet>
#include <gtest/gtest.h>

using namespace oc::plugins;

namespace {
QDateTime at(int y, int mo, int d, int h = 9, int mi = 5) { return QDateTime(QDate(y, mo, d), QTime(h, mi)); }
} // namespace

TEST(Templates, Variables)
{
    const QDateTime now = at(2026, 3, 7, 14, 30);
    EXPECT_EQ(applyTemplate("# {{title}} / {{Title}}", {{"title", "Note"}}, {}, {}, now), "# Note / Note");
    EXPECT_EQ(applyTemplate("{{date}} {{time}}", {}, {}, {}, now), "2026-03-07 14:30");
    EXPECT_EQ(applyTemplate("{{date}} {{time}}", {}, "DD.MM.YYYY", "h:mm A", now), "07.03.2026 2:30 PM");
    EXPECT_EQ(applyTemplate("{{date:dddd, MMMM Do}} {{TIME:HH}}", {}, {}, {}, now), "Saturday, March 7th 14");
    EXPECT_EQ(applyTemplate("{{unknown}} {{ date }}", {}, {}, {}, now), "{{unknown}} {{ date }}");
}

TEST(Templates, Frontmatter)
{
    TemplateParts p = splitFrontmatter("---\ntags: [a]\nstatus: new\n---\nBody\n");
    EXPECT_TRUE(p.hasFrontmatter);
    EXPECT_EQ(p.frontmatter, "tags: [a]\nstatus: new");
    EXPECT_EQ(p.body, "Body\n");
    p = splitFrontmatter("No frontmatter\n---\n");
    EXPECT_FALSE(p.hasFrontmatter);
    p = splitFrontmatter("---\n---\nx");
    EXPECT_TRUE(p.hasFrontmatter);
    EXPECT_EQ(p.frontmatter, "");
    EXPECT_EQ(p.body, "x");
    // merge: only missing keys (case-insensitive), nested values kept
    EXPECT_EQ(mergeFrontmatter("---\nStatus: done\n---\ntext", "status: new\nlist:\n  - a\n  - b"),
              "---\nStatus: done\nlist:\n  - a\n  - b\n---\ntext");
    EXPECT_EQ(mergeFrontmatter("text", "a: 1"), "---\na: 1\n---\ntext");
}

TEST(DailyNotes, NamesAndDates)
{
    DailyNotesOptions o;
    EXPECT_EQ(dailyNoteName(o, at(2026, 1, 2)), "2026-01-02");
    o.format = "YYYY/MM/YYYY-MM-DD";
    EXPECT_EQ(dailyNoteName(o, at(2026, 1, 2)), "2026/01/2026-01-02");
    auto d = dailyNoteDate("Journal/2026/01/2026-01-02.md", "Journal", o);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->date(), QDate(2026, 1, 2));
    EXPECT_FALSE(dailyNoteDate("Other/2026/01/2026-01-02.md", "Journal", o)); // outside the folder
    o.format.clear();
    EXPECT_TRUE(dailyNoteDate("2026-01-02.md", "", o));
    EXPECT_FALSE(dailyNoteDate("2026-1-2.md", "", o));      // strict
    EXPECT_FALSE(dailyNoteDate("2026-01-02 notes.md", "", o));
    EXPECT_FALSE(dailyNoteDate("2026-02-30.md", "", o));
    EXPECT_FALSE(dailyNoteDate("2026-01-02.canvas", "", o));
    EXPECT_EQ(withMdExtension("Templates/Daily"), "Templates/Daily.md");
    EXPECT_EQ(withMdExtension("Templates/Daily.txt"), "Templates/Daily.txt");
}

TEST(UniqueNote, Names)
{
    const QDateTime now = at(2026, 5, 6, 7, 8);
    QSet<QString> taken;
    auto isTaken = [&](const QString& prefix) {
        for (const QString& t : taken)
            if (t.startsWith(prefix))
                return true;
        return false;
    };
    EXPECT_EQ(*uniqueNoteName({}, {}, now, isTaken), "202605060708");
    taken << "202605060708.md";
    EXPECT_EQ(*uniqueNoteName({}, {}, now, isTaken), "202605060709"); // next minute
    taken << "202605060709.md";
    EXPECT_EQ(*uniqueNoteName({}, {}, now, isTaken), "202605060710");
    EXPECT_EQ(*uniqueNoteName("YYYY-MM-DD", {}, now, isTaken), "2026-05-06");
    taken << "2026-05-06.md";
    EXPECT_EQ(*uniqueNoteName("YYYY-MM-DD", {}, now, isTaken), "2026-05-07"); // next day
    EXPECT_EQ(*uniqueNoteName({}, "Idea", now, isTaken), "202605060708 Idea");
    EXPECT_FALSE(uniqueNoteName("[fixed]", {}, now, [](const QString&) { return true; })); // can't vary
}
