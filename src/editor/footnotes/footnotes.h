#pragma once

// Footnotes as Obsidian renders them (1.13).
//
// Live Preview / Source keep the syntax: references (.cm-footref) and definition markers (.cm-hmd-footnote) are
// styled, a definition line uses --footnote-size, and Live Preview hides a definition's "[^" / "]:" while the
// selection is away from its marker.
//
// Reading view follows app.js's remark transformer: references become "[1]" superscript links numbered in the
// order of their first reference (inline "^[...]" footnotes are numbered in the same sequence; the k-th repeat
// of a reference reads "[1-k]"), a reference without a definition becomes its lowercased label as plain text,
// definitions leave the flow (unreferenced ones disappear), and a "footnotes" section at the end of the note
// (an <hr> and an ordered list, --footnote-size) holds each footnote's content followed by one "↩︎" back-link
// per reference.

#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"
#include "render/layout/doc_layout.h"
#include "render/layout/style_sheet.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

namespace oc {

class LivePreviewDecorator;
namespace md {
class StaticDocSource;
}

namespace footnotes {

struct Note {
    QString id;               // lowercased label ("" for inline footnotes)
    bool isInline = false;
    int defFromLine = -1;     // named: the definition's lines [from, to) (the last one when defined twice)
    int defToLine = -1;
    qsizetype inlineFrom = -1; // inline: the text between "^[" and "]" (document offsets)
    qsizetype inlineTo = -1;
    QString inlineText;
    QList<qsizetype> refs;    // document offsets of its references, in order
};

// The numbering of one document state (build() it again when the text or tree changes).
class Index {
public:
    static std::shared_ptr<const Index> build(const md::SyntaxTree& tree, const Text& text);

    struct Ref {
        int note = -1;        // index into notes(); -1: no definition
        int occurrence = 0;   // 0 for the first reference to the note
    };
    const QList<Note>& notes() const { return m_notes; }
    bool isEmpty() const { return m_notes.isEmpty(); }
    // The reference starting at a document offset (nullopt: not a reference this index knows).
    std::optional<Ref> refAt(qsizetype docFrom) const;
    // Position-independent lookups (nested renderings: callout bodies, the footnotes section).
    int noteForId(const QString& id) const { return m_byId.value(id.toLower(), -1); }
    bool isDefined(const QString& id) const { return m_defined.contains(id.toLower()); }
    int noteForInlineText(const QString& text) const;
    // "1", "1-2": what a reference shows (without the brackets); also the anchor suffix ("fn-1", "fnref-1-2").
    static QString refLabel(int note, int occurrence);
    // The document offset of the reference an anchor ("fnref-1", "fnref-1-2") names.
    std::optional<qsizetype> refPosition(const QString& anchor) const;

private:
    QList<Note> m_notes;
    QHash<QString, int> m_byId;
    QHash<QString, QPair<int, int>> m_defined; // id -> definition lines
    QHash<qsizetype, Ref> m_refs;
};

// Lines of a footnote definition (its first line and its continuation lines); cheap for other lines.
bool isDefinitionLine(const md::SyntaxTree& tree, const Text& text, int line);
// Any line inside a footnote definition (blank lines between its paragraphs included).
bool inDefinition(const md::SyntaxTree& tree, int line);

// The footnotes section: an <hr> and the ordered list of the notes' contents (rendered as Markdown by a nested
// reading-mode layout at --footnote-size) with their back-links.
class SectionWidget : public BlockWidget {
public:
    SectionWidget(std::shared_ptr<const Index> index, const md::SyntaxTree& tree, const Text& text, StyleSheetPtr styles,
                  const LivePreviewDecorator* parent, qreal topGap);
    ~SectionWidget() override;

    qreal layout(qreal width) override;
    void paint(QPainter* p, const QRectF& rect) override;
    quint64 signature() const override { return m_signature; }
    qsizetype docPosAt(const QPointF&) const override { return -1; }
    // "#fnref-1" / "#fnref-1-2" on a back-link, "#fn-2" on a reference inside a footnote, else a link's target.
    QString linkAt(const QPointF& local) const override;
    qreal anchorY(const QString& anchor) const override; // "fn-1"

    // The generated Markdown of the list (tests) and where the section may be split across pages (PDF export).
    const Text& listText() const;
    QList<qreal> breakPoints() const;

private:
    struct Item {
        int firstLine = 0;
        int lastLine = 0;
        QList<QPair<qsizetype, qsizetype>> backrefs; // ranges in listText()
        QStringList anchors;                         // "fnref-1", "fnref-1-1"...
    };
    QPointF toList(const QPointF& local) const;

    StyleSheetPtr m_styles;
    std::shared_ptr<const Index> m_index;
    std::unique_ptr<md::StaticDocSource> m_source;
    std::unique_ptr<LivePreviewDecorator> m_decorator;
    mutable DocLayout m_layout;
    QList<Item> m_items;
    quint64 m_signature = 0;
    qreal m_topGap = 32;
    qreal m_scale = 0.875; // --footnote-size relative to the text size
    qreal m_width = -1;
    qreal m_height = 0;
    qreal m_listTop = 0;
};

} // namespace footnotes
} // namespace oc
