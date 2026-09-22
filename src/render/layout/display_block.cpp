#include "render/layout/display_block.h"

#include <QFontMetricsF>
#include <QHash>

#include <algorithm>

namespace oc {

quint64 LineDecoration::signature() const
{
    quint64 h = qHashMulti(0, int(lineStyle), paddingTop, paddingBottom, indent, hangingColumn, tabStop, quoteDepth, int(code), hrRule, hideCaret);
    for (const Mark& m : marks)
        h = qHashMulti(h, m.from, m.to, int(m.style));
    for (const Replace& r : replaces) {
        h = qHashMulti(h, r.from, r.to, int(r.kind), r.text, int(r.style));
        if (r.widget)
            h = qHashMulti(h, r.widget->signature());
    }
    for (int c : guideColumns)
        h = qHashMulti(h, c);
    for (int c : quoteColumns)
        h = qHashMulti(h, c, 7);
    h = qHashMulti(h, guideOffset, hidden, fixedHeight);
    return h;
}

DisplayBlock DisplayBlock::build(QStringView text, const LineDecoration& deco, const StyleSheet& styles)
{
    DisplayBlock b;
    b.m_docLength = int(text.size());
    const LineMetrics& lm = styles.line(deco.lineStyle);
    b.m_display.reserve(text.size());

    int pos = 0;
    auto addText = [&](int to) {
        if (to <= pos)
            return;
        Segment s;
        s.kind = Segment::Kind::Text;
        s.docFrom = pos;
        s.docTo = to;
        s.dispFrom = int(b.m_display.size());
        b.m_display += text.mid(pos, to - pos);
        s.dispTo = int(b.m_display.size());
        b.m_segments.push_back(s);
        pos = to;
    };
    for (const Replace& r : deco.replaces) {
        const int from = std::clamp(r.from, pos, int(text.size()));
        const int to = std::clamp(r.to, from, int(text.size()));
        addText(from);
        Segment s;
        s.docFrom = from;
        s.docTo = to;
        s.dispFrom = int(b.m_display.size());
        switch (r.kind) {
        case Replace::Kind::Hide:
            s.kind = Segment::Kind::Hidden;
            break;
        case Replace::Kind::Text: {
            s.kind = Segment::Kind::Substitute;
            b.m_display += r.text;
            if (r.style != StyleId::Normal) {
                QTextLayout::FormatRange fr;
                fr.start = s.dispFrom;
                fr.length = int(r.text.size());
                fr.format = styles.spec(r.style).format;
                b.m_formats.push_back(fr);
            }
            break;
        }
        case Replace::Kind::Widget: {
            s.kind = Segment::Kind::Widget;
            b.m_display += QChar(0x00A0);
            WidgetSlot slot;
            slot.dispPos = s.dispFrom;
            slot.widget = r.widget;
            slot.size = r.widget ? r.widget->size(lm) : QSizeF();
            slot.ascent = r.widget ? r.widget->ascent(lm) : 0;
            // Reserve exactly slot.size.width() for the placeholder via absolute letter spacing.
            const qreal natural = QFontMetricsF(lm.font).horizontalAdvance(QChar(0x00A0));
            QTextLayout::FormatRange fr;
            fr.start = s.dispFrom;
            fr.length = 1;
            fr.format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            fr.format.setFontLetterSpacing(slot.size.width() - natural);
            fr.format.setForeground(Qt::transparent);
            b.m_formats.push_back(fr);
            b.m_widgets.push_back(slot);
            break;
        }
        }
        s.dispTo = int(b.m_display.size());
        b.m_segments.push_back(s);
        pos = to;
    }
    addText(int(text.size()));
    if (b.m_segments.isEmpty())
        b.m_segments.push_back(Segment{Segment::Kind::Text, 0, 0, 0, 0});

    // Marks -> format ranges (only over Text segments).
    for (const Mark& m : deco.marks) {
        if (m.to <= m.from)
            continue;
        const StyleSpec& spec = styles.spec(m.style);
        for (const Segment& s : b.m_segments) {
            if (s.kind != Segment::Kind::Text)
                continue;
            const int from = std::max(m.from, s.docFrom), to = std::min(m.to, s.docTo);
            if (to <= from)
                continue;
            QTextLayout::FormatRange fr;
            fr.start = s.dispFrom + (from - s.docFrom);
            fr.length = to - from;
            fr.format = spec.format;
            b.m_formats.push_back(fr);
            if (spec.box.enabled) {
                // merge with the previous box when contiguous (a code span split around hidden backticks)
                if (!b.m_boxes.isEmpty() && b.m_boxes.last().dispTo == fr.start && b.m_boxes.last().style.sameLook(spec.box))
                    b.m_boxes.last().dispTo = fr.start + fr.length;
                else
                    b.m_boxes.push_back(Box{fr.start, fr.start + fr.length, spec.box});
            }
        }
    }
    return b;
}

int DisplayBlock::docToDisp(int docCol, int assoc) const
{
    docCol = std::clamp(docCol, 0, m_docLength);
    for (qsizetype i = 0; i < m_segments.size(); ++i) {
        const Segment& s = m_segments[i];
        if (docCol < s.docFrom)
            return s.dispFrom;
        if (docCol > s.docTo)
            continue;
        switch (s.kind) {
        case Segment::Kind::Text:
            if (docCol == s.docTo && assoc > 0 && i + 1 < m_segments.size() && m_segments[i + 1].docFrom == docCol
                && m_segments[i + 1].kind == Segment::Kind::Text)
                continue;
            return s.dispFrom + (docCol - s.docFrom);
        case Segment::Kind::Hidden:
            return s.dispFrom;
        case Segment::Kind::Substitute:
        case Segment::Kind::Widget:
            if (docCol == s.docFrom)
                return s.dispFrom;
            if (docCol == s.docTo)
                return s.dispTo;
            return assoc < 0 ? s.dispFrom : s.dispTo;
        }
    }
    return m_segments.isEmpty() ? 0 : m_segments.last().dispTo;
}

int DisplayBlock::dispToDoc(int dispPos, int assoc) const
{
    dispPos = std::clamp(dispPos, 0, int(m_display.size()));
    for (qsizetype i = 0; i < m_segments.size(); ++i) {
        const Segment& s = m_segments[i];
        if (dispPos > s.dispTo)
            continue;
        switch (s.kind) {
        case Segment::Kind::Text:
            if (dispPos == s.dispTo && dispPos != s.dispFrom && i + 1 < m_segments.size()) {
                // At a boundary with a following hidden range: prefer staying before it (assoc<0) or
                // skipping over it (assoc>0), so the caret lands next to the clicked glyph.
                const Segment& n = m_segments[i + 1];
                if (n.kind == Segment::Kind::Hidden && assoc > 0 && i + 2 < m_segments.size())
                    return n.docTo;
            }
            return s.docFrom + (dispPos - s.dispFrom);
        case Segment::Kind::Hidden:
            if (dispPos == s.dispFrom)
                return assoc < 0 ? s.docFrom : s.docTo;
            continue;
        case Segment::Kind::Substitute:
        case Segment::Kind::Widget:
            if (dispPos == s.dispFrom)
                return s.docFrom;
            if (dispPos == s.dispTo)
                return s.docTo;
            return assoc < 0 ? s.docFrom : s.docTo;
        }
    }
    return m_docLength;
}

} // namespace oc
