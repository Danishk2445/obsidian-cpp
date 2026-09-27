#pragma once

#include "editor/indent_guides.h"
#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"
#include "text/selection.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/State>

#include <QHash>
#include <QVarLengthArray>

#include <functional>
#include <optional>
#include <vector>

namespace oc {

namespace md {
class DocSource;
}
namespace footnotes {
class Index;
}

// Obsidian "Live Preview" (and Reading) decorations. Markdown syntax is hidden unless the selection
// touches the element (inline) or the line (headings, quotes, fences, HR), bullets/checkboxes/math/
// images become widgets, links show their display text. In reading mode nothing is active and
// comments/block ids are hidden. List geometry follows Obsidian's CSS: Live Preview keeps the source
// indentation (CM6 lines) with the marker padding, tab stops and hanging indent of its editor; Reading view
// places a line by its blockquotes and list items (readingChain()), hiding the source prefix, with the list
// markers as float widgets (LineDecoration::floatWidgets) and the quote bars at fixed positions.
class LivePreviewDecorator : public LineDecorator {
public:
    explicit LivePreviewDecorator(const md::DocSource* doc) : m_doc(doc) {}
    void setDocument(const md::DocSource* doc)
    {
        m_doc = doc;
        m_fenceCache.clear();
        m_footnotes.reset();
        m_footnoteSection.reset();
    }
    void setStyleSheet(const StyleSheet* ss) { m_styles = ss; }
    void setReading(bool reading) { m_reading = reading; }
    bool reading() const { return m_reading; }
    void setSelection(const Selection& sel) { m_sel = sel; }
    void setContentWidth(qreal w) { m_contentWidth = w; }

    // Obsidian's indentation guides (app.json "showIndentGuide", on by default). Editing: every line's indent units
    // (editor/indent_guides.h) offset by --indentation-guide-editing-indent (0 in code blocks), the unit of the
    // list item owning the main selection (setActiveIndent) in the active colour. Reading
    // (.markdown-rendered.show-indentation-guide li > ul::before, li > ol::before): a line down each nested list,
    // -0.85em from its start (through the parent item's marker), connecting the list's lines.
    void setIndentGuides(bool on) { m_indentGuides = on; }
    bool indentGuides() const { return m_indentGuides; }
    void setActiveIndent(const indent::ActiveRange& r) { m_activeIndent = r; }
    const indent::ActiveRange& activeIndent() const { return m_activeIndent; }
    // app.json "strictLineBreaks" (off by default), Reading view only: Obsidian's markdown renderer runs with
    // breaks = !strictLineBreaks. Off, every line break inside a paragraph is a line break; on, a single one is a
    // soft break shown as a space: the paragraph's lines up to each hard break (two trailing spaces or a
    // backslash) become one text unit (BlockRange::textRun). Live Preview's own lines aren't affected (its
    // rendered blocks, callouts, are nested reading renderings and are).
    void setStrictLineBreaks(bool on) { m_strictLineBreaks = on; }
    bool strictLineBreaks() const { return m_strictLineBreaks; }
    // Copies the rendering options above (indentation guides, strict line breaks) from another decorator: nested
    // renderings (callout bodies, the footnotes section, canvas cards) follow their parent's.
    void copyRenderOptions(const LivePreviewDecorator& from)
    {
        m_indentGuides = from.m_indentGuides;
        m_strictLineBreaks = from.m_strictLineBreaks;
    }

    // Link resolution (M3 wires the metadata cache): does a wikilink target exist?
    std::function<bool(const QString& target)> isResolved;
    // Absolute path of an embedded attachment ("image.png" -> "/vault/attachments/image.png"), "" if missing.
    std::function<QString(const QString& linkpath)> resolveAttachment;
    // Extra per-line decorations (search matches, IME preedit...).
    std::function<void(int line, LineDecoration&)> extra;

    LineDecoration decorate(int line) override;
    LineDecoration decorateRun(int fromLine, int toLine) override;
    bool hasTextRuns() const override { return m_reading && m_strictLineBreaks; }
    StyleId quickLineStyle(int line) override;
    qreal quickVerticalPadding(int line) override;

    // Lines whose decorations may differ between two selections (to invalidate on selection change).
    QList<QPair<int, int>> affectedLines(const Selection& a, const Selection& b) const;

    // Inline decorations for the part [winFrom, winTo] (document offsets) of an inline content, with
    // columns relative to `colBase`. Used for editor lines, table cells and callout titles.
    void inlineDecorationsFor(const md::LeafContent& content, const md::InlineTreePtr& tree, qsizetype winFrom, qsizetype winTo,
                              qsizetype colBase, LineDecoration& d, StyleId lineStyle, bool forceInactive) const;
    void blockRanges(int fromLine, int toLine, QList<BlockRange>& out) override;
    // Nesting depth of rendered content (callout bodies render nested documents).
    void setNestingDepth(int depth) { m_depth = depth; }

    struct LinkHit {
        enum class Kind { WikiLink, Embed, Link, Url, Tag } kind = Kind::Link;
        QString target;    // wikilink target (+subpath), URL, or tag
        qsizetype from = 0, to = 0;
        // Footnote references: the in-document anchor a click scrolls to in Reading view ("fn-2"; "fn" when not
        // numbered). Such hits aren't followed as links (target is "#[^id]" for hover previews).
        QString anchor;
    };
    std::optional<LinkHit> linkAt(qsizetype pos) const;

    // Footnotes (editor/footnotes): this document's Reading-view numbering, built on demand and cached per
    // document state; nested renderings (callout bodies, the footnotes section) use their parent's.
    std::shared_ptr<const footnotes::Index> footnoteIndex() const;
    void setSharedFootnotes(std::shared_ptr<const footnotes::Index> index) { m_sharedFootnotes = std::move(index); }
    // Reading view of a whole note (definitions leave the flow, the footnotes section follows the last line), of
    // a section of it (no footnotes section) or of one footnote definition (hover preview of "#[^1]": the
    // definition shows its content).
    enum class FootnoteMode { Document, Section, Definition };
    void setFootnoteMode(FootnoteMode mode) { m_footnoteMode = mode; }
    FootnoteMode footnoteMode() const { return m_footnoteMode; }
    // Reading view: the footnotes section (nullptr when the note has no numbered footnote).
    std::shared_ptr<BlockWidget> trailer() override;
    // The document offset of the reference an anchor names ("fnref-1", "fnref-1-2").
    std::optional<qsizetype> footnoteRefPosition(const QString& anchor) const;

private:
    bool touches(qsizetype from, qsizetype to) const;
    bool lineActive(int line) const;
    bool codeBlockEmbed(int line, int leaf, md::LineRole role, LineDecoration& d); // registered code block languages
    void inlineDecorations(int line, int leaf, LineDecoration& d);
    int m_depth = 0;
    QHash<quint64, std::shared_ptr<BlockWidget>> m_blockCache;
    void codeHighlight(int line, int leaf, LineDecoration& d);

    struct FenceCache {
        quint64 version = 0;
        KSyntaxHighlighting::Definition def;
        std::vector<KSyntaxHighlighting::State> states;
        std::vector<QList<Mark>> marks;
    };

    bool hidesFootnoteDefinitions() const { return m_reading && m_footnoteMode != FootnoteMode::Definition; }
    void paragraphRuns(int fromLine, int toLine, QList<BlockRange>& out) const;
    bool previousVisibleLineBlank(int line) const;

    // Live Preview: a list item's marker on its first line (bullet, number, checkbox; CSS in the .cpp).
    void listMarker(int line, int item, LineDecoration& d) const;
    // Reading view (.markdown-rendered): the boxes a line sits in, outermost first. Blockquotes (a 2px
    // border and 24px padding) and list items (margin-inline-start: 3ch, or --list-indent in a list nested in a
    // list of its own type) move the content right; lists have no padding.
    struct ReadingItem {
        int node = -1;
        qreal x = 0; // the item's content start (px)
        bool ordered = false;
    };
    struct ReadingChain {
        qreal x = 0; // the line's content start (px)
        QList<qreal> quoteBars;
        QVarLengthArray<ReadingItem, 8> items; // the line's list items, outermost first
        QVarLengthArray<int, 8> lists;         // the line's lists, outermost first
    };
    ReadingChain readingChain(int line) const;
    void readingLayout(int line, md::LineRole role, const ReadingChain& chain, LineDecoration& d);
    void readingGuides(int line, const ReadingChain& chain, LineDecoration& d) const;
    bool blankBetweenListItems(int line) const;
    int itemMarkerLine(int item) const;
    int lastContentLine(int node) const;
    bool blockNextTo(int line, int container, int dir) const;
    int listNumber(int item) const;

    const md::DocSource* m_doc;
    const StyleSheet* m_styles = nullptr;
    mutable std::shared_ptr<const footnotes::Index> m_footnotes;
    mutable quint64 m_footnotesKey = 0;
    mutable quint64 m_footnotesSerial = 0;
    std::shared_ptr<const footnotes::Index> m_sharedFootnotes;
    FootnoteMode m_footnoteMode = FootnoteMode::Document;
    std::shared_ptr<BlockWidget> m_footnoteSection;
    quint64 m_footnoteSectionKey = 0;
    Selection m_sel;
    bool m_reading = false;
    bool m_indentGuides = true;
    bool m_strictLineBreaks = false;
    indent::ActiveRange m_activeIndent;
    qreal m_contentWidth = 700;
    QHash<int, FenceCache> m_fenceCache;
    mutable QHash<int, int> m_listNumbers; // Reading view: ordered item -> its rendered number
    mutable quint64 m_listNumbersKey = 0;
};

} // namespace oc
