// moment.js port: golden vectors produced by moment@2.30.1 (tests/fixtures/moment/gen_vectors.js).
#include "core/time/moment_format.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <gtest/gtest.h>

#include <ctime>

using namespace oc;

namespace {

// The vectors were generated with TZ=UTC; local-time results must be compared in the same zone.
const bool kUtcZone = [] {
    qputenv("TZ", "UTC");
    tzset();
    return true;
}();

QJsonObject loadVectors()
{
    QFile f(QStringLiteral(OC_FIXTURES_DIR "/moment/vectors.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

int toQDateYear(int astronomical) { return astronomical <= 0 ? astronomical - 1 : astronomical; }
int toAstronomical(int qdateYear) { return qdateYear < 0 ? qdateYear + 1 : qdateYear; }

// [year, month, day, hour, minute, second, ms, utcOffsetMinutes]
QDateTime dateFromVector(const QJsonArray& d)
{
    const QDate date(toQDateYear(d[0].toInt()), d[1].toInt(), d[2].toInt());
    const QTime time(d[3].toInt(), d[4].toInt(), d[5].toInt(), d[6].toInt());
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(d[7].toInt() * 60));
}

QDateTime dt(int y, int mo, int d, int h = 0, int mi = 0, int s = 0, int ms = 0)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms), QTimeZone::UTC);
}

const QDateTime kNow = QDateTime(QDate(2026, 9, 22), QTime(10, 0), QTimeZone::UTC); // generator's "now"

} // namespace

TEST(MomentFormat, GoldenFormatVectors)
{
    ASSERT_TRUE(kUtcZone);
    const QJsonObject v = loadVectors();
    ASSERT_EQ(v.value("moment").toString(), "2.30.1");
    const QJsonArray groups = v.value("format").toArray();
    ASSERT_GT(groups.size(), 30);
    int total = 0, failures = 0;
    for (const QJsonValue& g : groups) {
        const QJsonArray date = g.toObject().value("date").toArray();
        const QDateTime when = dateFromVector(date);
        ASSERT_TRUE(when.isValid()) << QJsonDocument(date).toJson().constData();
        for (const QJsonValue& c : g.toObject().value("cases").toArray()) {
            const QString format = c.toArray()[0].toString();
            const QString expected = c.toArray()[1].toString();
            const QString got = formatMoment(when, format);
            ++total;
            if (got != expected && ++failures <= 25)
                ADD_FAILURE() << "date " << QJsonDocument(date).toJson(QJsonDocument::Compact).constData() << " format \""
                              << format.toStdString() << "\": expected \"" << expected.toStdString() << "\" got \"" << got.toStdString() << '"';
        }
    }
    EXPECT_EQ(failures, 0) << failures << " of " << total << " vectors differ";
    EXPECT_GT(total, 5000);
}

TEST(MomentFormat, GoldenStrictParseVectors)
{
    const QJsonArray cases = loadVectors().value("parse").toArray();
    ASSERT_GT(cases.size(), 150);
    int failures = 0;
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString text = c.value("text").toString(), format = c.value("format").toString();
        const bool valid = c.value("valid").toBool();
        const std::optional<QDateTime> got = parseMomentStrict(text, format, kNow);
        const std::string what = "\"" + text.toStdString() + "\" / \"" + format.toStdString() + "\"";
        if (got.has_value() != valid) {
            ++failures;
            ADD_FAILURE() << what << ": expected " << (valid ? "valid" : "invalid") << ", got "
                          << (got ? got->toString(Qt::ISODateWithMs).toStdString() : std::string("invalid"));
            continue;
        }
        if (!got || !c.contains("utc"))
            continue;
        const QJsonArray e = c.value("utc").toArray();
        const QDateTime u = got->toUTC();
        const int fields[7] = {toAstronomical(u.date().year()), u.date().month(), u.date().day(), u.time().hour(),
                               u.time().minute(), u.time().second(), u.time().msec()};
        for (int i = 0; i < 7; ++i) {
            if (fields[i] != e[i].toInt()) {
                ++failures;
                ADD_FAILURE() << what << ": expected " << QJsonDocument(e).toJson(QJsonDocument::Compact).constData() << " got "
                              << u.toString(Qt::ISODateWithMs).toStdString();
                break;
            }
        }
    }
    EXPECT_EQ(failures, 0);
}

TEST(MomentFormat, CommonObsidianFormats)
{
    const QDateTime d = dt(2026, 9, 22, 14, 5, 9);
    EXPECT_EQ(formatMoment(d, "YYYY-MM-DD"), "2026-09-22");
    EXPECT_EQ(formatMoment(d, "YYYYMMDDHHmm"), "202609221405");
    EXPECT_EQ(formatMoment(d, "dddd, MMMM Do YYYY"), "Tuesday, September 22nd 2026");
    EXPECT_EQ(formatMoment(d, "YYYY/MM/MMMM"), "2026/09/September");
    EXPECT_EQ(formatMoment(d, "gggg-[W]ww"), "2026-W39");
    EXPECT_EQ(formatMoment(d, "GGGG-[W]WW"), "2026-W39");
    EXPECT_EQ(formatMoment(d, "[Week] w"), "Week 39");
    EXPECT_EQ(formatMoment(d, "HH:mm"), "14:05");
    EXPECT_EQ(formatMoment(d, "h:mm a"), "2:05 pm");
    EXPECT_EQ(formatMoment(d, "LLLL"), "Tuesday, September 22, 2026 2:05 PM");
    EXPECT_EQ(formatMoment(d, "[YYYY] \\YYYY YYYY"), "YYYY YYYY 2026");
    EXPECT_EQ(formatMoment(QDateTime(), "YYYY"), "Invalid date");
}

TEST(MomentFormat, WeekYearBoundaries)
{
    // 2024-12-29 (Sunday) starts US week 1 of 2025; ISO still counts it in 2024-W52.
    EXPECT_EQ(formatMoment(dt(2024, 12, 29), "gggg-[W]ww GGGG-[W]WW"), "2025-W01 2024-W52");
    // 2021-01-03 (Sunday) belongs to ISO week 53 of 2020.
    EXPECT_EQ(formatMoment(dt(2021, 1, 3), "GGGG-[W]WW E"), "2020-W53 7");
    EXPECT_EQ(formatMoment(dt(2021, 1, 4), "GGGG-[W]WW E"), "2021-W01 1");
}

TEST(MomentFormat, OffsetsAndTimestamps)
{
    const QDateTime ist(QDate(2024, 6, 1), QTime(1, 2, 3, 456), QTimeZone::fromSecondsAheadOfUtc(330 * 60));
    EXPECT_EQ(formatMoment(ist, "Z ZZ z"), "+05:30 +0530 UTC");
    EXPECT_EQ(formatMoment(ist, "x"), QString::number(ist.toMSecsSinceEpoch()));
    const QDateTime local(QDate(2024, 6, 1), QTime(1, 2, 3)); // local time (TZ=UTC in this test)
    EXPECT_EQ(formatMoment(local, "Z z zz"), "+00:00  ");
    EXPECT_EQ(formatMoment(dt(1969, 12, 31, 23, 59, 59, 500), "X"), "-1");
}

TEST(MomentFormat, ExpandFormat)
{
    EXPECT_EQ(expandMomentFormat("LL"), "MMMM D, YYYY");
    EXPECT_EQ(expandMomentFormat("llll"), "ddd, MMM D, YYYY h:mm A");
    EXPECT_EQ(expandMomentFormat("[LT] \\LT LT"), "[LT] \\LT h:mm A");
}

TEST(MomentFormat, NonEnglishLocaleIsBestEffort)
{
    const QLocale de(QLocale::German, QLocale::Germany);
    const QDateTime d = dt(2026, 9, 22, 14, 5);
    EXPECT_EQ(formatMoment(d, "dddd, D. MMMM YYYY", de), de.dayName(2) + ", 22. " + de.monthName(9) + " 2026");
    EXPECT_EQ(formatMoment(d, "Do", de), "22.");
    EXPECT_EQ(formatMoment(dt(2024, 12, 29), "w", de), "52"); // German weeks are ISO weeks
    const auto parsed = parseMomentStrict(QStringLiteral("22. ") + de.monthName(9) + QStringLiteral(" 2026"), "D. MMMM YYYY", de);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->date(), QDate(2026, 9, 22));
}

TEST(MomentParse, DailyNoteNames)
{
    EXPECT_EQ(parseMomentStrict("2026-09-22", "YYYY-MM-DD")->date(), QDate(2026, 9, 22));
    EXPECT_FALSE(parseMomentStrict("2026-09-22 copy", "YYYY-MM-DD").has_value());
    EXPECT_FALSE(parseMomentStrict("Untitled", "YYYY-MM-DD").has_value());
    EXPECT_EQ(parseMomentStrict("Tuesday, September 22nd 2026", "dddd, MMMM Do YYYY")->date(), QDate(2026, 9, 22));
    EXPECT_FALSE(parseMomentStrict("Monday, September 22nd 2026", "dddd, MMMM Do YYYY").has_value());
    EXPECT_EQ(parseMomentStrict("2026-W39", "gggg-[W]ww")->date(), QDate(2026, 9, 20)); // Sunday of that week
    EXPECT_EQ(parseMomentStrict("2026-W39", "GGGG-[W]WW")->date(), QDate(2026, 9, 21)); // ISO Monday
    const auto withOffset = parseMomentStrict("2026-09-22T10:00:00+05:30", "YYYY-MM-DDTHH:mm:ssZ");
    ASSERT_TRUE(withOffset.has_value());
    EXPECT_EQ(withOffset->offsetFromUtc(), 330 * 60);
    EXPECT_EQ(withOffset->time(), QTime(10, 0));
}

TEST(MomentParse, DefaultsComeFromNow)
{
    const auto t = parseMomentStrict("14:30", "HH:mm", kNow);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->date(), QDate(2026, 9, 22));
    EXPECT_EQ(t->time(), QTime(14, 30));
    const auto m = parseMomentStrict("March", "MMMM", kNow);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->date(), QDate(2026, 3, 1));
}

TEST(MomentFormat, RoundTripsThroughStrictParse)
{
    const QStringList formats = {"YYYY-MM-DD", "YYYYMMDDHHmmss", "dddd, MMMM Do YYYY h:mm:ss a", "DD.MM.YYYY HH:mm",
                                 "GGGG-[W]WW-E", "gggg-[W]ww-e", "YYYY-DDDD", "ll LTS", "YYYY-MM-DD[T]HH:mm:ss.SSSZ"};
    QDateTime d(QDate(2019, 12, 30), QTime(0, 0), QTimeZone::UTC);
    for (int i = 0; i < 800; ++i) {
        d = d.addSecs(3600 * 11 + 61 * 7 + 13); // walk through years, week boundaries and times
        for (const QString& f : formats) {
            const QString s = formatMoment(d, f);
            const auto back = parseMomentStrict(s, f, kNow);
            ASSERT_TRUE(back.has_value()) << s.toStdString() << " / " << f.toStdString();
            EXPECT_EQ(formatMoment(back->toUTC(), f), s) << f.toStdString();
        }
    }
}
