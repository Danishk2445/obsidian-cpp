// Golden vectors for obsidian-cpp's moment.js port (src/core/time/moment_format.*).
// Regenerate in a throwaway npm project with moment@2.30.1 (the version Obsidian bundles):
//   npm install moment@2.30.1 && TZ=UTC node gen_vectors.js > tests/fixtures/moment/vectors.json
'use strict';
const moment = require('moment');
if (moment.version !== '2.30.1') throw new Error('expected moment 2.30.1, got ' + moment.version);
moment.locale('en');

// [year, month(1-12), day, hour, minute, second, ms, utcOffsetMinutes]
const dates = [
  [2024, 1, 1, 0, 0, 0, 0, 0],          // Monday, leap year, midnight
  [2024, 1, 15, 13, 5, 9, 7, 0],        // afternoon, single-digit fields
  [2024, 2, 29, 12, 0, 0, 0, 0],        // leap day, noon (12 PM)
  [2024, 2, 29, 0, 30, 0, 0, 0],        // 12:30 AM
  [2024, 12, 29, 23, 59, 59, 999, 0],   // Sunday: en week 1 of 2025
  [2024, 12, 31, 11, 59, 59, 999, 0],   // ISO week 1 of 2025
  [2023, 1, 1, 9, 0, 0, 0, 0],          // Sunday, ISO week 52 of 2022
  [2022, 1, 1, 18, 45, 30, 250, 0],     // Saturday, en week 1 of 2022, ISO week 52 of 2021
  [2021, 1, 3, 7, 7, 7, 70, 0],         // Sunday, ISO week 53 of 2020
  [2021, 1, 4, 8, 0, 0, 0, 0],          // Monday, ISO week 1 of 2021
  [2020, 12, 31, 22, 10, 0, 0, 0],      // Thursday, ISO week 53
  [2015, 12, 31, 10, 0, 0, 0, 0],       // ISO week 53 of 2015
  [2016, 12, 25, 6, 30, 0, 0, 0],       // en week 53 of 2016
  [2026, 9, 22, 10, 12, 0, 0, 0],       // "today"
  [2026, 12, 31, 23, 0, 0, 0, 0],
  [2027, 1, 1, 0, 0, 1, 1, 0],
  [2000, 1, 1, 0, 0, 0, 0, 0],          // Saturday, 400-year leap
  [1999, 12, 31, 23, 59, 59, 999, 0],
  [1970, 1, 1, 0, 0, 0, 0, 0],          // epoch
  [1969, 12, 31, 23, 59, 59, 500, 0],   // negative timestamp, X floors
  [1900, 3, 1, 12, 0, 0, 0, 0],         // 1900 not leap
  [1600, 2, 29, 1, 2, 3, 4, 0],         // 1600 leap
  [999, 6, 15, 14, 0, 0, 0, 0],         // 3-digit year
  [99, 7, 4, 4, 4, 4, 44, 0],           // 2-digit year (no 1900 remap)
  [1, 1, 1, 0, 0, 0, 0, 0],             // year 1 (AD)
  [0, 6, 1, 12, 0, 0, 0, 0],            // year 0 (1 BC)
  [9999, 12, 31, 23, 59, 59, 999, 0],
  [10000, 1, 1, 0, 0, 0, 0, 0],         // 5-digit year: Y -> +10000
  [2024, 6, 1, 1, 2, 3, 456, 330],      // +05:30
  [2024, 6, 1, 1, 2, 3, 456, -300],     // -05:00
  [2024, 6, 1, 23, 59, 0, 0, 345],      // +05:45
  [2024, 6, 1, 23, 59, 0, 0, -150],     // -02:30
  [2024, 11, 3, 1, 30, 0, 0, 840],      // +14:00
  [2024, 11, 3, 1, 30, 0, 0, -720],     // -12:00
];

const singleTokens = [
  'YYYY', 'YY', 'Y', 'YYYYY', 'YYYYYY', 'Q', 'Qo', 'M', 'Mo', 'MM', 'MMM', 'MMMM', 'D', 'Do', 'DD', 'DDD', 'DDDo', 'DDDD',
  'd', 'do', 'dd', 'ddd', 'dddd', 'e', 'E', 'w', 'wo', 'ww', 'W', 'Wo', 'WW', 'gggg', 'gg', 'ggggg', 'GGGG', 'GG', 'GGGGG',
  'H', 'HH', 'h', 'hh', 'k', 'kk', 'm', 'mm', 's', 'ss', 'S', 'SS', 'SSS', 'SSSS', 'SSSSSS', 'SSSSSSSSS', 'a', 'A',
  'Z', 'ZZ', 'X', 'x', 'z', 'zz', 'LT', 'LTS', 'L', 'LL', 'LLL', 'LLLL', 'l', 'll', 'lll', 'llll',
  'Hmm', 'Hmmss', 'hmm', 'hmmss', 'N', 'NN', 'NNN', 'NNNN', 'NNNNN', 'y', 'yy', 'yyy', 'yyyy', 'yo',
];

const combos = [
  // Common Obsidian formats (daily notes, templates, unique note creator, periodic notes)
  'YYYY-MM-DD', 'YYYYMMDDHHmm', 'YYYYMMDDHHmmss', 'dddd, MMMM Do YYYY', 'YYYY/MM/MMMM', 'YYYY/MM/YYYY-MM-DD', 'gggg-[W]ww',
  'GGGG-[W]WW', 'YYYY-[Q]Q', 'YYYY-MM', 'YYYY', 'HH:mm', 'HH:mm:ss', 'h:mm A', 'h:mm a', 'DD.MM.YYYY', 'MM/DD/YYYY',
  'YYYY-MM-DD HH:mm', 'YYYY-MM-DDTHH:mm:ss.SSSZ', 'YYYY-MM-DDTHH:mm:ssZZ', 'ddd, MMM D YYYY', 'MMM Do, YYYY',
  'dddd [the] Do [of] MMMM', '[Week] w', '[Week] W [of] GGGG', 'wo [week of] gggg', 'DDDo [day of] YYYY',
  'YYYY-MM-DD[T]HH:mm', '[Today is] dddd', 'Qo [quarter]', 'kk:mm', 'hh:mm:ss.SSS a', 'X.SSS', 'LL LTS', 'll, LT',
  // Escapes and tokenizer corner cases
  '[a]b]c', '[a[b]', '[unclosed', '[]', '[[x]]', 'w|W|', '\\YYYY \\\\ \\Y', '\\[LT]', '[LT', 'YYY', 'YYYYYYY', 'MMMMM',
  'DDDDD', 'ddddd', 'ggg', 'gggggg', 'GGG', 'SSSSSSSSSS', 'LLLLL', 'lllll', 'LTSS', 'Mo Do', 'Dot', 'do-Do-DDDo',
  'YYYY\nMM', 'T', 'hello world', 'x X', 'Z ZZ z', 'NNNN y', '[Q]Q-YYYY', 'A a', 'e E d', 'w ww wo W WW Wo',
  'gg GG gggg GGGG', '\\L\\T', 'L[L]L', '[l]ll', 'Ž YYYY – ☃', '[YYYY] YYYY', 'Hmmss', 'hmmss',
];

const out = { moment: moment.version, note: 'date = [year, month, day, hour, minute, second, ms, utcOffsetMinutes]; year 0 = 1 BC', format: [], parse: [] };

function mk(d) {
  const [y, mo, day, h, mi, s, ms, off] = d;
  const m = moment.utc({ year: y, month: mo - 1, date: day, hour: h, minute: mi, second: s, millisecond: ms });
  if (y < 100 && y >= 0) m.year(y); // moment.utc({}) handles 0..99 but be explicit
  return m.utcOffset(off, true);    // keep wall clock, set offset
}

for (let i = 0; i < dates.length; i++) {
  const d = dates[i];
  const m = mk(d);
  if (m.year() !== d[0] || m.month() !== d[1] - 1 || m.date() !== d[2] || m.hour() !== d[3]) throw new Error('bad date ' + d);
  // All single tokens on every date, combos on every date.
  const cases = [];
  for (const f of singleTokens.concat(combos)) cases.push([f, m.format(f)]);
  out.format.push({ date: d, cases });
}

// Strict parsing: moment(text, format, true).isValid() and the parsed wall-clock fields.
// Only formats that fully determine the date (no "today" defaults) record the fields.
const parseCases = [
  ['2024-01-15', 'YYYY-MM-DD'], ['2024-1-15', 'YYYY-MM-DD'], ['2024-02-30', 'YYYY-MM-DD'], ['2024-02-29', 'YYYY-MM-DD'],
  ['2023-02-29', 'YYYY-MM-DD'], [' 2024-01-15', 'YYYY-MM-DD'], ['2024-01-15 ', 'YYYY-MM-DD'], ['2024-01-15x', 'YYYY-MM-DD'],
  ['2024-13-01', 'YYYY-MM-DD'], ['2024-00-10', 'YYYY-MM-DD'], ['2024-01-00', 'YYYY-MM-DD'], ['2024-04-31', 'YYYY-MM-DD'],
  ['1900-02-29', 'YYYY-MM-DD'], ['2000-02-29', 'YYYY-MM-DD'], ['0000-01-01', 'YYYY-MM-DD'], ['0099-12-31', 'YYYY-MM-DD'],
  ['24-01-15', 'YYYY-MM-DD'], ['12024-01-15', 'YYYY-MM-DD'], ['2024_01_15', 'YYYY-MM-DD'], ['２０２４-01-15', 'YYYY-MM-DD'],
  ['2024-01-15 24:00', 'YYYY-MM-DD HH:mm'], ['2024-01-15 24:01', 'YYYY-MM-DD HH:mm'], ['2024-01-15 23:60', 'YYYY-MM-DD HH:mm'],
  ['2024-01-15 25:00', 'YYYY-MM-DD HH:mm'], ['2024-01-15 7:05', 'YYYY-MM-DD H:mm'], ['2024-01-15 07:05', 'YYYY-MM-DD H:mm'],
  ['2024-01-15 0:05', 'YYYY-MM-DD H:mm'], ['2024-01-15 10:5', 'YYYY-MM-DD HH:m'], ['2024-01-15 10:05', 'YYYY-MM-DD HH:m'],
  ['2024-01-15 10:00:7', 'YYYY-MM-DD HH:mm:s'], ['2024-01-15 10:00:60', 'YYYY-MM-DD HH:mm:ss'],
  ['2024-W03', 'gggg-[W]ww'], ['2024-W53', 'gggg-[W]ww'], ['2016-W53', 'gggg-[W]ww'], ['2020-W53', 'GGGG-[W]WW'], ['2021-W53', 'GGGG-[W]WW'],
  ['2024-W1', 'gggg-[W]ww'], ['2024-W01', 'gggg-[W]ww'], ['2024-W00', 'gggg-[W]ww'], ['2025-W01', 'GGGG-[W]WW'], ['2024-W3', 'gggg-[W]w'],
  ['2024 3 1', 'gggg w e'], ['2024 3 1', 'GGGG W E'], ['2024 3 8', 'GGGG W E'], ['2024 3 7', 'gggg w e'], ['2024 3 0', 'GGGG W E'],
  ['2024 3 3', 'gggg w d'], ['2024 3 Wed', 'gggg w ddd'], ['24-W03', 'gg-[W]ww'], ['24-W03', 'GG-[W]WW'],
  ['Monday, January 15th 2024', 'dddd, MMMM Do YYYY'], ['Tuesday, January 15th 2024', 'dddd, MMMM Do YYYY'],
  ['monday, JANUARY 15th 2024', 'dddd, MMMM Do YYYY'], ['Mon, January 15th 2024', 'dddd, MMMM Do YYYY'],
  ['Monday, Jan 15th 2024', 'dddd, MMMM Do YYYY'], ['Monday, January 15st 2024', 'dddd, MMMM Do YYYY'],
  ['Monday, January 15TH 2024', 'dddd, MMMM Do YYYY'], ['Monday, January 15 2024', 'dddd, MMMM Do YYYY'],
  ['Friday, March 1st 2024', 'dddd, MMMM Do YYYY'], ['Friday, March 01st 2024', 'dddd, MMMM Do YYYY'],
  ['202401151230', 'YYYYMMDDHHmm'], ['202401151260', 'YYYYMMDDHHmm'], ['20240115123', 'YYYYMMDDHHmm'], ['2024011512300', 'YYYYMMDDHHmm'],
  ['20240115123059', 'YYYYMMDDHHmmss'], ['2024/01/January', 'YYYY/MM/MMMM'], ['2024/01/February', 'YYYY/MM/MMMM'], ['2024/1/January', 'YYYY/MM/MMMM'],
  ['2024-5', 'YYYY-M'], ['2024-05', 'YYYY-M'], ['2024-12-5', 'YYYY-MM-D'], ['2024-12-05', 'YYYY-MM-D'], ['2024-12', 'YYYY-MM'],
  ['2024-Q3', 'YYYY-[Q]Q'], ['2024-Q5', 'YYYY-[Q]Q'], ['2024-Q0', 'YYYY-[Q]Q'], ['2024', 'YYYY'], ['24', 'YY'], ['70', 'YY'], ['68', 'YY'], ['69', 'YY'],
  ['2024-366', 'YYYY-DDDD'], ['2023-366', 'YYYY-DDDD'], ['2024-060', 'YYYY-DDD'], ['2024-60', 'YYYY-DDD'], ['2024-000', 'YYYY-DDDD'],
  ['1700000000', 'X'], ['1700000000.5', 'X'], ['-1700000000', 'X'], ['1700000000123', 'x'], ['-100', 'x'],
  ['2024-01-15T10:00:00+05:30', 'YYYY-MM-DDTHH:mm:ssZ'], ['2024-01-15T10:00:00+0530', 'YYYY-MM-DDTHH:mm:ssZZ'],
  ['2024-01-15T10:00:00Z', 'YYYY-MM-DDTHH:mm:ssZ'], ['2024-01-15T10:00:00-02', 'YYYY-MM-DDTHH:mm:ssZ'],
  ['2024-01-15 10:00:00.1', 'YYYY-MM-DD HH:mm:ss.S'], ['2024-01-15 10:00:00.12', 'YYYY-MM-DD HH:mm:ss.S'],
  ['2024-01-15 10:00:00.12', 'YYYY-MM-DD HH:mm:ss.SS'], ['2024-01-15 10:00:00.123', 'YYYY-MM-DD HH:mm:ss.SSS'],
  ['2024-01-15 10:00:00.1234', 'YYYY-MM-DD HH:mm:ss.SSSS'], ['2024-01-15 10:00:00.9999', 'YYYY-MM-DD HH:mm:ss.SSSS'],
  ['2024-01-15 Mo', 'YYYY-MM-DD dd'], ['2024-01-15 Tu', 'YYYY-MM-DD dd'], ['2024-01-15 Mon', 'YYYY-MM-DD ddd'],
  ['2024-01-15 Monday', 'YYYY-MM-DD ddd'], ['2024-01-15 1', 'YYYY-MM-DD d'], ['2024-01-15 2', 'YYYY-MM-DD d'],
  ['2024-01-15 1', 'YYYY-MM-DD E'], ['2024-01-15 7', 'YYYY-MM-DD E'], ['2024-01-15 9', 'YYYY-MM-DD d'],
  ['2024 Jan', 'YYYY MMM'], ['2024 jan', 'YYYY MMM'], ['2024 January', 'YYYY MMM'], ['2024 Sept', 'YYYY MMM'], ['2024 May', 'YYYY MMMM'],
  ['2024-01-15 5', 'YYYY-MM-DD g'], ['2024-01-15 g', 'YYYY-MM-DD g'], ['2024-01-15', 'YYYY-MM-DD [ignored]'],
  ['2024-01-15 ignored', 'YYYY-MM-DD [ignored]'], ['2024-01-15 Ignored', 'YYYY-MM-DD [ignored]'],
  ['', 'YYYY'], ['abc', '[abc]'], ['15.01.2024', 'DD.MM.YYYY'], ['2024-01-15', 'YYYY-MM-DD HH:mm'],
  ['2024-01-15 24', 'YYYY-MM-DD k'], ['2024-01-15 0', 'YYYY-MM-DD k'], ['2024-01-15 24:00', 'YYYY-MM-DD kk:mm'],
  ['2024-01-15 1030', 'YYYY-MM-DD Hmm'], ['2024-01-15 930', 'YYYY-MM-DD Hmm'], ['2024-01-15 93000', 'YYYY-MM-DD Hmmss'],
  ['2024-01-15 1330', 'YYYY-MM-DD hmm'], ['2024-01-15 1030 pm', 'YYYY-MM-DD hmm a'],
  ['2024-01-15 12:30 AM', 'YYYY-MM-DD h:mm A'], ['2024-01-15 12:30 PM', 'YYYY-MM-DD h:mm A'], ['2024-01-15 0:30 AM', 'YYYY-MM-DD h:mm A'],
  ['2024-01-15 13:30 PM', 'YYYY-MM-DD h:mm A'], ['2024-01-15 01:30 pm', 'YYYY-MM-DD hh:mm a'], ['2024-01-15 1:30 p.m.', 'YYYY-MM-DD h:mm a'],
  ['2024-01-15 1:30', 'YYYY-MM-DD h:mm'], ['2024-01-15 13:30', 'YYYY-MM-DD h:mm'], ['2024-01-15 13:30 AM', 'YYYY-MM-DD HH:mm A'],
  ['2024-01-15 1:30 xm', 'YYYY-MM-DD h:mm A'], ['2024-01-15 1:30 A', 'YYYY-MM-DD h:mm A'],
  ['AD 2024', 'N YYYY'], ['2024-01-15 AD', 'YYYY-MM-DD N'], ['2024-01-15 BC', 'YYYY-MM-DD N'], ['Anno Domini 2024-01-15', 'NNNN YYYY-MM-DD'],
  ['2024-01-15 Mo', 'YYYY-MM-DD Mo'], ['2024-01-15 z', 'YYYY-MM-DD z'], ['+002024-01-15', 'YYYYYY-MM-DD'], ['-002024-01-15', 'YYYYYY-MM-DD'],
  ['2024-01-15', 'Y-MM-DD'], ['20240115', 'YMMDD'], ['2024 01 15', 'YYYY MM DD'], ['2024  01 15', 'YYYY MM DD'],
  ['2024-01-15 Week 3', 'YYYY-MM-DD [Week] w'], ['2024-01-15 Week 9', 'YYYY-MM-DD [Week] w'],
  ['Jan 15, 2024', 'll'], ['January 15, 2024', 'LL'], ['01/15/2024', 'L'], ['1/15/2024', 'l'], ['1/15/2024', 'L'],
  ['January 15, 2024 1:05 PM', 'LLL'], ['Monday, January 15, 2024 1:05 PM', 'LLLL'], ['2024-01-15 1:05:09 PM', 'YYYY-MM-DD LTS'],
  ['2024-01-15', '\\YYYY-MM-DD'], ['YYYY-01-15', '\\YYYY-MM-DD'], ['2024.01.15', 'YYYY.MM.DD'], ['2024x01x15', 'YYYY.MM.DD'],
  ['2024-01-15 (Mon)', 'YYYY-MM-DD (ddd)'], ['2024-01-15 [Mon]', 'YYYY-MM-DD \\[ddd\\]'], ['15th January 2024', 'Do MMMM YYYY'],
];
// Missing date fields default to "today"; parse under two different "now"s and mark cases whose result depends on it.
const realNow = moment.now;
function parseAt(nowMs, text, format) {
  moment.now = () => nowMs;
  try {
    const m = moment(text, format, true);
    if (!m.isValid()) return null;
    const mm = m.clone().utc(); // TZ=UTC: fields of the parsed instant in UTC
    return [mm.year(), mm.month() + 1, mm.date(), mm.hour(), mm.minute(), mm.second(), mm.millisecond()];
  } finally {
    moment.now = realNow;
  }
}
const nowA = Date.UTC(2026, 8, 22, 10, 0, 0), nowB = Date.UTC(2023, 2, 5, 18, 0, 0);
for (const [text, format] of parseCases) {
  const a = parseAt(nowA, text, format), b = parseAt(nowB, text, format);
  const entry = { text, format, valid: a !== null };
  if (JSON.stringify(a) !== JSON.stringify(b)) entry.dependsOnToday = true;
  else if (a) entry.utc = a;
  out.parse.push(entry);
}

process.stdout.write(JSON.stringify(out, null, 0).replace(/\{"date"/g, '\n{"date"').replace(/\{"text"/g, '\n{"text"') + '\n');
