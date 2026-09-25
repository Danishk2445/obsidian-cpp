#pragma once

#include "render/layout/decoration.h"

#include <QTextLayout>

namespace oc {

// The text actually laid out for one document line: hidden ranges removed, substitutions and
// widget placeholders inserted, marks converted to QTextLayout format ranges in display coordinates.
class DisplayBlock {
public:
    struct Segment {
        enum class Kind : quint8 { Text, Hidden, Substitute, Widget };
        Kind kind = Kind::Text;
        int docFrom = 0, docTo = 0;   // columns in the document line
        int dispFrom = 0, dispTo = 0; // positions in the display string
    };
    struct WidgetSlot {
        int dispPos = 0;
        std::shared_ptr<InlineWidget> widget;
        QSizeF size;
        qreal ascent = 0;
    };

    static DisplayBlock build(QStringView lineText, const LineDecoration& deco, const StyleSheet& styles);

    const QString& display() const { return m_display; }
    const QList<QTextLayout::FormatRange>& formats() const { return m_formats; }
    const QList<WidgetSlot>& widgets() const { return m_widgets; }
    const QList<Segment>& segments() const { return m_segments; }
    int docLength() const { return m_docLength; }

    // Column mapping. assoc < 0 prefers the left side at boundaries (hidden ranges collapse there).
    int docToDisp(int docCol, int assoc = 1) const;
    int dispToDoc(int dispPos, int assoc = 1) const;
    // Box styles (pills) to paint behind marks: display ranges + spec.
    struct Box {
        int dispFrom = 0, dispTo = 0;
        BoxStyle style;
        bool reserved = false; // padX is laid out (letter spacing) around the run, not painted over neighbours
    };
    const QList<Box>& boxes() const { return m_boxes; }

private:
    void reserveBoxPadding(const LineMetrics& lm);

    QString m_display;
    QList<QTextLayout::FormatRange> m_formats;
    QList<WidgetSlot> m_widgets;
    QList<Segment> m_segments;
    QList<Box> m_boxes;
    int m_docLength = 0;
};

} // namespace oc
