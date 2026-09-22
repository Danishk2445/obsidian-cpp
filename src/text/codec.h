#pragma once

#include "text/text.h"

#include <QByteArray>

namespace oc {

// Result of decoding a file's bytes. Decoding is strict UTF-8; every byte that is not part of a
// valid UTF-8 sequence becomes a lone surrogate U+DC80..U+DCFF ("surrogateescape"), so that
// encodeFile(decodeFile(b)) == b for ANY input. Line terminators are recorded per line.
struct DecodedFile {
    Text text;
    bool bom = false;            // UTF-8 BOM present (kept on save)
    bool hadInvalidUtf8 = false; // some bytes were escaped
    bool hasNul = false;         // contains NUL bytes (binary-ish; opened read-only)
};

DecodedFile decodeFile(const QByteArray& bytes);
QByteArray encodeFile(const Text& text, bool bom);

// Encode one '\n'-free string (surrogate escapes turn back into raw bytes).
QByteArray encodeUtf8Escaped(QStringView s);
QString decodeUtf8Escaped(QByteArrayView bytes, bool* hadInvalid = nullptr);

// 128-bit XXH3 of the raw bytes, as 32 hex chars. Used to detect "no change" writes and self-writes.
QByteArray contentHash(QByteArrayView bytes);

} // namespace oc
