#include "markdown/md_refdef.h"

#include "markdown/md_scan.h"

namespace oc::md {

using scan::isAsciiPunct;
using scan::skipSpaces;

namespace {

bool isTitleOpen(QChar c) { return c == u'"' || c == u'\'' || c == u'('; }

quint8 delimCode(QChar open)
{
    return open == u'"' ? 1 : open == u'\'' ? 2 : 3;
}

QChar delimClose(quint8 bits)
{
    switch (bits >> RefState::kDelimShift) {
    case 1:
        return u'"';
    case 2:
        return u'\'';
    default:
        return u')';
    }
}

// cmark_isspace (destination scanning stops at any of these)
bool isCmarkSpace(QChar c) { return c == u' ' || c == u'\t' || c == u'\v' || c == u'\f' || c == u'\r' || c == u'\n'; }

void fail(RefState& st, RefScratch& sc, RefSink& sink)
{
    st = RefState{};
    sink.contentFrom(sc.refStart);
}

void complete(RefState& st, RefScratch& sc, RefSink& sink, bool withTitle)
{
    LinkRefDefinition def;
    def.label = scan::normalizeLabel(sc.label);
    def.destination = scan::unescapeBackslashes(sc.dest);
    def.title = withTitle ? sc.title : QString();
    def.line = sc.refStart;
    sink.refdefCompleted(def);
    st = RefState{RefMode::Start, 0, 0};
}

// The title did not close properly. A title that started on the destination's line takes the
// whole definition down with it; otherwise the definition ends at the destination and the title
// lines become content (they start with a quote/paren, so no further refdef can follow).
void titleFail(RefState& st, RefScratch& sc, RefSink& sink)
{
    if (st.bits & RefState::kOnDestLine) {
        fail(st, sc, sink);
        return;
    }
    complete(st, sc, sink, false);
    st = RefState{};
    sink.contentFrom(sc.titleStart);
}

} // namespace

RefState refInitial(QStringView firstLine)
{
    const int p = skipSpaces(firstLine, 0);
    if (p < firstLine.size() && firstLine[p] == u'[')
        return RefState{RefMode::Start, 0, 0};
    return RefState{};
}

bool refHasContentIfClosed(const RefState& st)
{
    switch (st.mode) {
    case RefMode::Start:
    case RefMode::Dest:
        return false;
    default:
        return true;
    }
}

void refFeed(RefState& st, RefScratch& sc, QStringView s, int pos, int line, RefSink& sink)
{
    const int n = int(s.size());
    int i = pos;
    for (;;) {
        switch (st.mode) {
        case RefMode::Content:
            return;

        case RefMode::Start:
            if (i == pos && i < n && s[i] == u'[') {
                sc.refStart = line;
                sc.label.clear();
                sc.dest.clear();
                sc.title.clear();
                st = RefState{RefMode::Label, 0, 0};
                ++i;
                continue;
            }
            st = RefState{};
            sink.contentFrom(line);
            return;

        case RefMode::Label:
            while (i < n) {
                const QChar c = s[i];
                if (c == u'\\') {
                    sc.label += c;
                    ++st.n;
                    ++i;
                    st.bits |= RefState::kNonSpace;
                    if (i < n && isAsciiPunct(s[i])) {
                        sc.label += s[i];
                        ++st.n;
                        ++i;
                    }
                } else if (c == u'[') {
                    fail(st, sc, sink);
                    return;
                } else if (c == u']') {
                    if (!(st.bits & RefState::kNonSpace)) {
                        fail(st, sc, sink);
                        return;
                    }
                    ++i;
                    if (i < n && s[i] == u':') {
                        ++i;
                        st = RefState{RefMode::Colon, 0, 0};
                        break;
                    }
                    fail(st, sc, sink);
                    return;
                } else {
                    sc.label += c;
                    ++st.n;
                    if (c != u' ' && c != u'\t')
                        st.bits |= RefState::kNonSpace;
                    ++i;
                }
                if (st.n > 1000) {
                    fail(st, sc, sink);
                    return;
                }
            }
            if (st.mode == RefMode::Label) {
                // Line break inside the label.
                sc.label += u'\n';
                ++st.n;
                if (st.n > 1000)
                    fail(st, sc, sink);
                return;
            }
            continue; // label closed with "]:", go on with the destination

        case RefMode::Colon: {
            i = skipSpaces(s, i);
            if (i >= n)
                return; // destination on the next line (spnl crosses one line break)
            int destEnd = 0;
            if (s[i] == u'<') {
                int j = i + 1;
                while (j < n && s[j] != u'>') {
                    if (s[j] == u'\\') {
                        j += 2;
                        continue;
                    }
                    if (s[j] == u'<') {
                        fail(st, sc, sink);
                        return;
                    }
                    ++j;
                }
                if (j >= n) {
                    fail(st, sc, sink);
                    return;
                }
                sc.dest = s.mid(i + 1, j - i - 1).toString();
                destEnd = j + 1;
            } else {
                int j = i;
                int depth = 0;
                while (j < n) {
                    const QChar c = s[j];
                    if (c == u'\\' && j + 1 < n && isAsciiPunct(s[j + 1])) {
                        j += 2;
                        continue;
                    }
                    if (c == u'(') {
                        if (++depth > 32) {
                            fail(st, sc, sink);
                            return;
                        }
                    } else if (c == u')') {
                        if (depth == 0)
                            break;
                        --depth;
                    } else if (isCmarkSpace(c)) {
                        break;
                    }
                    ++j;
                }
                if (depth != 0) {
                    fail(st, sc, sink);
                    return;
                }
                sc.dest = s.mid(i, j - i).toString();
                destEnd = j;
            }
            sc.destLine = line;
            const int t = skipSpaces(s, destEnd);
            if (t >= n) {
                st = RefState{RefMode::Dest, 0, 0};
                return;
            }
            if (t > destEnd && isTitleOpen(s[t])) {
                st = RefState{RefMode::Title, quint8((delimCode(s[t]) << RefState::kDelimShift) | RefState::kOnDestLine), 0};
                sc.titleStart = line;
                sc.title.clear();
                i = t + 1;
                continue;
            }
            fail(st, sc, sink);
            return;
        }

        case RefMode::Dest: {
            // spnl already crossed the line break; leading spaces of this line are skipped too.
            const int t = skipSpaces(s, i);
            if (t < n && isTitleOpen(s[t])) {
                st = RefState{RefMode::Title, quint8(delimCode(s[t]) << RefState::kDelimShift), 0};
                sc.titleStart = line;
                sc.title.clear();
                i = t + 1;
                continue;
            }
            // No title: the definition ended with the previous line; this line starts afresh.
            complete(st, sc, sink, false);
            i = pos;
            continue;
        }

        case RefMode::Title: {
            const QChar close = delimClose(st.bits);
            while (i < n) {
                const QChar c = s[i];
                if (c == u'\\' && i + 1 < n && isAsciiPunct(s[i + 1])) {
                    sc.title += s[i + 1];
                    i += 2;
                    continue;
                }
                if (c == close) {
                    if (skipSpaces(s, i + 1) >= n) {
                        complete(st, sc, sink, true);
                        return;
                    }
                    titleFail(st, sc, sink);
                    return;
                }
                if (close == u')' && c == u'(') {
                    titleFail(st, sc, sink);
                    return;
                }
                sc.title += c;
                ++i;
            }
            sc.title += u'\n';
            return;
        }
        }
    }
}

void refFinish(RefState& st, RefScratch& sc, RefSink& sink)
{
    switch (st.mode) {
    case RefMode::Content:
    case RefMode::Start:
        return;
    case RefMode::Label:
    case RefMode::Colon:
        fail(st, sc, sink);
        return;
    case RefMode::Dest:
        complete(st, sc, sink, false);
        return;
    case RefMode::Title:
        titleFail(st, sc, sink);
        return;
    }
}

} // namespace oc::md
