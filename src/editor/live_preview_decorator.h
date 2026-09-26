#pragma once

#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"
#include "text/selection.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/State>

#include <QHash>

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
// comments/block ids are hidden.
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

    // Link resolution (M3 wires the metadata cache): does a wikilink target exist?
    std::function<bool(const QString& target)> isResolved;
    // Absolute path of an embedded attachment ("image.png" -> "/vault/attachments/image.png"), "" if missing.
    std::function<QString(const QString& linkpath)> resolveAttachment;
    // Extra per-line decorations (search matches, IME preedit...).
    std::function<void(int line, LineDecoration&)> extra;

    LineDecoration decorate(int line) override;
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
    bool previousVisibleLineBlank(int line) const;

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
    qreal m_contentWidth = 700;
    QHash<int, FenceCache> m_fenceCache;
};

} // namespace oc
