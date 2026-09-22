#include "core/util/word_count.h"

namespace oc {

namespace {
constexpr bool in(char16_t c, char16_t a, char16_t b) { return c >= a && c <= b; }

// Characters that each count as one word.
bool isSingleCharWord(char16_t c)
{
    return c == 0x0F00 || in(c, 0x0F40, 0x0F47) || in(c, 0x0F49, 0x0F6C) || in(c, 0x0F88, 0x0F8C) // Tibetan
        || in(c, 0x3041, 0x3096) || in(c, 0x309D, 0x309F)                                          // Hiragana
        || in(c, 0x30A1, 0x30FA) || in(c, 0x30FC, 0x30FF)                                          // Katakana
        || in(c, 0x4E00, 0x9FD5);                                                                  // CJK ideographs
}

// Characters forming multi-character words.
bool isRunChar(char16_t c)
{
    if (c == u'-' || c == u'\'' || c == 0x2019)
        return true;
    if (in(c, 0x0B80, 0x0BFF) || in(c, 0xAC00, 0xD7A3) || in(c, 0xA960, 0xA97C) || in(c, 0xD7B0, 0xD7C6))
        return true; // Tamil block, Hangul
    if (QChar::isSurrogate(c) || isSingleCharWord(c) || in(c, 0x3040, 0x30FF) || in(c, 0x4E00, 0x9FFF))
        return false;
    return QChar::isLetter(c);
}

bool isDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
} // namespace

int countWords(QStringView s)
{
    int words = 0;
    const qsizetype n = s.size();
    qsizetype i = 0;
    while (i < n) {
        const char16_t c = s[i].unicode();
        if (isDigit(c)) {
            while (i < n && isDigit(s[i].unicode()))
                ++i;
            // grouped/decimal numbers: 1,234.56
            while (i + 1 < n && (s[i] == u',' || s[i] == u'.') && isDigit(s[i + 1].unicode())) {
                ++i;
                while (i < n && isDigit(s[i].unicode()))
                    ++i;
            }
            ++words;
        } else if (isRunChar(c)) {
            while (i < n && isRunChar(s[i].unicode()))
                ++i;
            ++words;
        } else {
            if (isSingleCharWord(c))
                ++words;
            ++i;
        }
    }
    return words;
}

QStringView withoutFrontmatter(QStringView note)
{
    // "---" on the first line, closed by a "---" line; the content starts after the closing line.
    auto lineEnd = [&](qsizetype from) {
        const qsizetype nl = note.indexOf(u'\n', from);
        return nl < 0 ? note.size() : nl;
    };
    const qsizetype firstEnd = lineEnd(0);
    if (note.left(firstEnd).trimmed() != u"---" || !note.startsWith(u"---"))
        return note;
    qsizetype pos = firstEnd + 1;
    while (pos <= note.size() && pos > 0) {
        const qsizetype end = lineEnd(pos);
        QStringView line = note.mid(pos, end - pos);
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (line == u"---" || line == u"...")
            return end >= note.size() ? QStringView() : note.mid(end + 1);
        if (end >= note.size())
            break;
        pos = end + 1;
    }
    return note; // unterminated: not frontmatter
}

TextCounts countNote(QStringView note)
{
    const QStringView body = withoutFrontmatter(note);
    return {countWords(body), int(body.size())};
}

TextCounts countSelection(QStringView selection) { return {countWords(selection), int(selection.size())}; }

} // namespace oc
