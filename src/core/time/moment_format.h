#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

#include <optional>

namespace oc {

// moment.js 2.30.1 (the version Obsidian bundles) date formatting and strict parsing, as used by daily
// notes, templates ({{date:FORMAT}}), the unique note creator and periodic-note style names.
//
// formatMoment() reproduces moment(dt).format(format) exactly for the "en" locale, including the tokenizer's
// quirks: `[escaped text]` (up to the last ']' before the next '['), `\X` escapes (a backslash escapes the
// whole following token), locale formats (LT LTS L LL LLL LLLL l ll lll llll) expanded before tokenizing,
// unknown characters copied through and line breaks outside brackets dropped. Supported tokens:
//   YYYY YY Y YYYYY YYYYYY  Q Qo  M Mo MM MMM MMMM  D Do DD DDD DDDo DDDD  d do dd ddd dddd e E
//   w wo ww W Wo WW gg gggg ggggg GG GGGG GGGGG  H HH h hh k kk Hmm Hmmss hmm hmmss m mm s ss S..SSSSSSSSS
//   a A Z ZZ X x z zz N NN NNN NNNN NNNNN y yy yyy yyyy yo
// Weeks follow moment's "en" rules (week starts Sunday, the week containing Jan 1st is week 1: dow=0, doy=6);
// W/GGGG are ISO weeks. Fields come from `dt` in its own time spec. Z/ZZ print its UTC offset, X/x its epoch
// time; z/zz print "UTC"/"Coordinated Universal Time" for UTC or fixed-offset date-times (moment's
// utc()/utcOffset() mode) and nothing for local/zone times. Year 0 is 1 BC (QDate year -1). An invalid
// QDateTime formats as "Invalid date".
//
// Other locales are best effort (moment's per-locale tables are not bundled): QLocale month/day names,
// first day of week (ISO week rules when it is Monday), AM/PM texts, "N." ordinals and long date formats
// converted from QLocale's.
QString formatMoment(const QDateTime& dt, const QString& format, const QLocale& locale = QLocale(QLocale::English));

// Equivalent of moment(text, format, true) — strict mode, as Obsidian uses to recognise daily notes by file
// name. Returns the parsed date-time when moment's isValid() would be true, std::nullopt otherwise: every
// token must match at the current position with moment's strict regexes (e.g. MM needs two digits, M
// rejects a leading zero, MMMM/dddd need full names, case-insensitively), all input must be consumed, fields
// must not overflow (Feb 30, 24:01, week 53 of a 52-week year...) and parsed weekday names must agree with
// the date. Missing fields default like moment: year/month/day to "now" (only the leading unspecified ones),
// the rest to their minimum; week-based formats (gggg-[W]ww, GGGG-[W]WW) resolve to the first day of the
// week. The result is local time, or carries the parsed fixed offset when the format has Z/ZZ.
// An empty format parses `text` as ISO 8601 (moment's fallback).
std::optional<QDateTime> parseMomentStrict(const QString& text, const QString& format,
                                           const QLocale& locale = QLocale(QLocale::English));
// Same with an explicit "now" used for the defaulted fields (deterministic tests, "today" in another zone).
std::optional<QDateTime> parseMomentStrict(const QString& text, const QString& format, const QDateTime& now,
                                           const QLocale& locale = QLocale(QLocale::English));

// moment's expandFormat(): replaces the locale formats (LT, LTS, L, LL, LLL, LLLL, l, ll, lll, llll) outside
// brackets/escapes by their definitions, e.g. "LL" -> "MMMM D, YYYY".
QString expandMomentFormat(const QString& format, const QLocale& locale = QLocale(QLocale::English));

// moment(then).from(now) with the default relative-time thresholds and the "en" strings: "a few seconds
// ago", "a minute ago", "5 minutes ago", "an hour ago", "3 days ago", "a month ago", "2 years ago", or
// "in ..." when `then` is after `now`. fromNow() = from(currentDateTime()).
QString momentFrom(const QDateTime& then, const QDateTime& now);
QString momentFromNow(const QDateTime& then);

} // namespace oc
