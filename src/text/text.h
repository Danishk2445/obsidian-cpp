#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <functional>
#include <memory>
#include <vector>

namespace oc {

// Line terminator after a line. The last line of a document has Eol::None when the file does not
// end with a newline. Per-line EOLs make saving byte-exact for mixed-EOL files.
enum class Eol : quint8 { None = 0, LF = 1, CRLF = 2, CR = 3 };

inline QLatin1StringView eolString(Eol e)
{
    switch (e) {
    case Eol::LF:
        return QLatin1StringView("\n");
    case Eol::CRLF:
        return QLatin1StringView("\r\n");
    case Eol::CR:
        return QLatin1StringView("\r");
    default:
        return QLatin1StringView("");
    }
}

// Immutable document text stored as chunks of lines. Positions are UTF-16 offsets into the
// "\n-joined" representation (every line terminator counts as exactly one character, whatever its
// real EOL), exactly like CodeMirror 6. Copies are O(1) (shared chunks); edits copy only the
// affected chunks plus a small index, so snapshots can be handed to worker threads cheaply.
class Text {
public:
    Text(); // one empty line

    static Text fromString(QStringView s);                     // splits on \n, \r\n, \r (records each EOL)
    static Text fromLines(const QList<QString>& lines, const QList<Eol>& eols); // eols.size() == lines.size()

    qsizetype length() const;          // chars incl. one separator per line break
    int lineCount() const;             // always >= 1
    bool isEmpty() const { return length() == 0; }

    QString line(int i) const;         // without terminator
    QStringView lineView(int i) const; // valid while this Text (or a copy) is alive
    int lineLength(int i) const;
    Eol lineEol(int i) const;          // terminator after line i
    qsizetype lineStart(int i) const;  // offset of the line's first char
    qsizetype lineEnd(int i) const;    // offset just after its last char (before the separator)
    int lineAt(qsizetype pos) const;   // line containing pos; pos == lineEnd(i) belongs to line i

    QString slice(qsizetype from, qsizetype to) const; // '\n' between lines
    QString toString() const;                          // '\n'-joined
    QChar charAt(qsizetype pos) const;                 // '\n' at separators, QChar() past end

    // Replace [from, to) with `insert` ('\n' separated). New line breaks get `newEol`; the last
    // resulting line keeps the terminator that followed position `to`.
    Text replace(qsizetype from, qsizetype to, QStringView insert, Eol newEol = Eol::LF) const;

    Eol dominantEol() const;            // most common terminator (LF when none)
    bool hasFinalNewline() const { return lineEol(lineCount() - 1) != Eol::None; }

    // Iterate lines [from, to) with their start offsets.
    void forEachLine(int from, int to, const std::function<void(int line, qsizetype start, QStringView text)>& fn) const;

    bool contentEquals(const Text& other) const; // same text and same EOLs
    // Structural sharing check (cheap): same underlying chunks.
    bool sharesStorageWith(const Text& other) const { return m_index == other.m_index; }

    static constexpr int kChunkLines = 512;

private:
    struct Chunk {
        std::vector<QString> lines;
        std::vector<Eol> eols;
        std::vector<qsizetype> starts; // starts[i] = offset of line i within chunk; starts[n] = span
        void finalize();
        qsizetype span() const { return starts.back(); } // sum(len + 1)
    };
    struct Index {
        std::vector<std::shared_ptr<const Chunk>> chunks;
        std::vector<int> lineBase;        // first line of chunk k; size chunks+1
        std::vector<qsizetype> charBase;  // start offset of chunk k; size chunks+1
        void finalize();
        int lines() const { return lineBase.back(); }
        qsizetype span() const { return charBase.back(); }
    };

    explicit Text(std::shared_ptr<const Index> index);
    int chunkOfLine(int line) const;
    int chunkOfPos(qsizetype pos) const;
    static std::shared_ptr<const Index> build(std::vector<QString>&& lines, std::vector<Eol>&& eols);

    std::shared_ptr<const Index> m_index;
};

} // namespace oc
