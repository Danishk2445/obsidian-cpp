#include "text/codec.h"

#include <xxhash.h>

namespace oc {

QString decodeUtf8Escaped(QByteArrayView bytes, bool* hadInvalid)
{
    QString out;
    out.reserve(bytes.size());
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const qsizetype n = bytes.size();
    qsizetype i = 0;
    bool invalid = false;
    auto escape = [&](unsigned char c) {
        out += QChar(char16_t(0xDC00 + c));
        invalid = true;
    };
    while (i < n) {
        const unsigned char c = b[i];
        if (c < 0x80) {
            out += QChar(char16_t(c));
            ++i;
            continue;
        }
        int len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
            min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
            min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
            min = 0x10000;
        } else {
            escape(c);
            ++i;
            continue;
        }
        if (i + len > n) {
            escape(c);
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = b[i + k];
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            escape(c);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            out += QChar(QChar::highSurrogate(cp));
            out += QChar(QChar::lowSurrogate(cp));
        } else {
            out += QChar(char16_t(cp));
        }
        i += len;
    }
    if (hadInvalid)
        *hadInvalid = invalid;
    return out;
}

QByteArray encodeUtf8Escaped(QStringView s)
{
    QByteArray out;
    out.reserve(s.size() + s.size() / 4);
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t u = s[i].unicode();
        if (u < 0x80) {
            out += char(u);
        } else if (u < 0x800) {
            out += char(0xC0 | (u >> 6));
            out += char(0x80 | (u & 0x3F));
        } else if (QChar::isHighSurrogate(u) && i + 1 < n && QChar::isLowSurrogate(s[i + 1].unicode())) {
            const char32_t cp = QChar::surrogateToUcs4(u, s[i + 1].unicode());
            ++i;
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else if (u >= 0xDC80 && u <= 0xDCFF) {
            out += char(u - 0xDC00); // escaped raw byte
        } else if (QChar::isSurrogate(u)) {
            // Unpaired surrogate that isn't an escape (can only come from pasted text): encode U+FFFD.
            out += "\xEF\xBF\xBD";
        } else {
            out += char(0xE0 | (u >> 12));
            out += char(0x80 | ((u >> 6) & 0x3F));
            out += char(0x80 | (u & 0x3F));
        }
    }
    return out;
}

DecodedFile decodeFile(const QByteArray& bytes)
{
    DecodedFile d;
    QByteArrayView view(bytes);
    if (view.startsWith("\xEF\xBB\xBF")) {
        d.bom = true;
        view = view.mid(3);
    }
    d.hasNul = view.contains('\0');
    const QString s = decodeUtf8Escaped(view, &d.hadInvalidUtf8);
    d.text = Text::fromString(s);
    return d;
}

QByteArray encodeFile(const Text& text, bool bom)
{
    QByteArray out;
    out.reserve(text.length() + text.lineCount() + 3);
    if (bom)
        out += "\xEF\xBB\xBF";
    text.forEachLine(0, text.lineCount(), [&](int line, qsizetype, QStringView s) {
        out += encodeUtf8Escaped(s);
        switch (text.lineEol(line)) {
        case Eol::LF:
            out += '\n';
            break;
        case Eol::CRLF:
            out += "\r\n";
            break;
        case Eol::CR:
            out += '\r';
            break;
        case Eol::None:
            break;
        }
    });
    return out;
}

QByteArray contentHash(QByteArrayView bytes)
{
    const XXH128_hash_t h = XXH3_128bits(bytes.data(), size_t(bytes.size()));
    QByteArray raw(16, Qt::Uninitialized);
    for (int i = 0; i < 8; ++i) {
        raw[i] = char((h.high64 >> (56 - 8 * i)) & 0xFF);
        raw[8 + i] = char((h.low64 >> (56 - 8 * i)) & 0xFF);
    }
    return raw.toHex();
}

} // namespace oc
