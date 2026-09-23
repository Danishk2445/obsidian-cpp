#pragma once

// Resumable, line-at-a-time parser for link reference definitions at the start of a paragraph
// (CommonMark 0.31.2 §4.7, following cmark's cmark_parse_reference_inline). The persistent part
// (RefState) is small and position independent so it can live in the interned parser state; the
// scratch part (line numbers, collected strings) only lives while a paragraph is being parsed.
//
// Paragraph lines are fed with their leading whitespace stripped (as cmark stores them). A refdef
// always ends at a line end, so the paragraph splits into whole refdef lines [P, C) and content
// lines [C, end).

#include "markdown/syntax_tree.h"

#include <QString>
#include <QStringView>

namespace oc::md {

enum class RefMode : quint8 {
    Content = 0, // no (more) refdefs: the paragraph has content
    Start,       // all lines so far are complete refdefs; the next line may start another
    Label,       // inside "[label" (labels may span lines)
    Colon,       // after "]:" at a line end; the destination follows on the next line
    Dest,        // destination ended the previous line; a title may start on this line
    Title,       // inside a title (may span lines)
};

struct RefState {
    RefMode mode = RefMode::Content;
    quint8 bits = 0; // Label: kNonSpace. Title: delimiter code | kOnDestLine
    qint32 n = 0;    // Label: characters so far
    static constexpr quint8 kNonSpace = 1;
    static constexpr quint8 kOnDestLine = 2;
    static constexpr quint8 kDelimShift = 4; // 1 = '"', 2 = '\'', 3 = ')'
    bool operator==(const RefState&) const = default;
};

struct RefScratch {
    int refStart = -1;   // first line of the refdef being parsed
    int destLine = -1;   // line holding its destination
    int titleStart = -1; // first line of its title
    QString label, dest, title;
};

class RefSink {
public:
    virtual ~RefSink() = default;
    virtual void refdefCompleted(const LinkRefDefinition& def) = 0;
    // The paragraph has content from `line` on (refdef parsing is over).
    virtual void contentFrom(int line) = 0;
};

// Starting state for a paragraph whose first stripped line is `s`.
RefState refInitial(QStringView firstLine);
// Feed one paragraph line (s[pos..] = stripped content of paragraph line `line`).
void refFeed(RefState& st, RefScratch& sc, QStringView s, int pos, int line, RefSink& sink);
// Resolve a pending refdef when the paragraph closes (or converts to a setext heading).
void refFinish(RefState& st, RefScratch& sc, RefSink& sink);
// Would the paragraph have content if it closed now?
bool refHasContentIfClosed(const RefState& st);

} // namespace oc::md
