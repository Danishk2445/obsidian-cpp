#pragma once

#include "render/theme/theme.h"

#include <QFont>
#include <QTextCharFormat>

#include <array>

namespace oc {

// Named styles used by decorations. Line styles set a line's base font; mark styles are merged on
// top as character formats. Values come from Obsidian's CSS variables (see StyleSheet::build).
enum class StyleId : quint16 {
    Normal = 0,
    // line styles
    Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
    CodeBlock,
    MathBlock,
    Frontmatter,
    HrLine,
    CommentBlock,
    HtmlBlock,
    // marks
    Formatting,          // faint markup (#, >, **, fences...)
    ListMarker,
    Strong,
    Emphasis,
    StrongEmphasis,
    Strikethrough,
    Highlight,
    InlineCode,
    InternalLink,
    InternalLinkUnresolved,
    ExternalLink,
    Url,
    Tag,
    Math,
    Comment,
    Html,
    FootnoteRef,
    BlockId,
    Escape,
    TaskDone,            // checked task item text (strike + muted)
    SearchMatch,
    SearchMatchCurrent,
    SpellError,
    Preedit,             // IME composition text
    // code token styles (KSyntaxHighlighting text styles -> Obsidian --code-* tokens)
    CodeComment,
    CodeKeyword,
    CodeFunction,
    CodeString,
    CodeValue,
    CodeOperator,
    CodeProperty,
    CodePunctuation,
    CodeTag,
    CodeImportant,
    Count
};

// Extra painting for marks that CSS draws with backgrounds/borders (inline code, tags, highlights).
struct BoxStyle {
    bool enabled = false;
    QColor background;
    qreal radius = 0;
    qreal padX = 0; // horizontal padding added around the run (painted, not laid out)
};

struct StyleSpec {
    QTextCharFormat format;  // merged onto the line's base format
    BoxStyle box;
};

struct LineMetrics {
    QFont font;              // base font for the line
    qreal lineBox = 24;      // px height of one visual line (font px * line-height)
    qreal ascent = 0;        // of the base font
    qreal descent = 0;
    qreal avgCharWidth = 8;  // for height estimation
    qreal spaceWidth = 4;
    QColor color;            // base text color
};

// Theme-resolved formats for every StyleId, rebuilt when the theme changes.
class StyleSheet {
public:
    static std::shared_ptr<const StyleSheet> build(const Theme& theme);

    const StyleSpec& spec(StyleId id) const { return m_specs[size_t(id)]; }
    const LineMetrics& line(StyleId lineStyle) const;
    bool isLineStyle(StyleId id) const { return id <= StyleId::HtmlBlock; }

    quint64 generation() const { return m_generation; }
    qreal textSize() const { return m_textSize; }
    qreal listIndent() const { return m_listIndent; }     // --list-indent in px (tab stop in list lines)
    qreal pSpacing() const { return m_pSpacing; }         // --p-spacing px (heading line top padding)
    qreal listSpacing() const { return m_listSpacing; }   // --list-spacing px
    qreal codePaddingX() const { return m_codePadX; }     // code block line left padding
    qreal quoteBarWidth() const { return m_quoteBar; }
    qreal quoteIndent() const { return m_quoteIndent; }
    QColor codeBackground() const { return m_codeBg; }
    qreal codeRadius() const { return m_codeRadius; }
    QColor quoteBarColor() const { return m_quoteColor; }
    QColor selection() const { return m_selection; }
    QColor caret() const { return m_caret; }
    QColor guideColor() const { return m_guide; }
    QColor guideActiveColor() const { return m_guideActive; }
    QColor faint() const { return m_faint; }
    QColor muted() const { return m_muted; }
    QColor hrColor() const { return m_hr; }
    qreal hrThickness() const { return m_hrThickness; }
    int tabSizeSpaces() const { return 4; }

private:
    std::array<StyleSpec, size_t(StyleId::Count)> m_specs;
    std::array<LineMetrics, size_t(StyleId::HtmlBlock) + 1> m_lines;
    quint64 m_generation = 0;
    qreal m_textSize = 16, m_listIndent = 36, m_pSpacing = 16, m_listSpacing = 1.2, m_codePadX = 16;
    qreal m_quoteBar = 2, m_quoteIndent = 24, m_codeRadius = 4, m_hrThickness = 2;
    QColor m_codeBg, m_quoteColor, m_selection, m_caret, m_guide, m_guideActive, m_faint, m_muted, m_hr;
};

using StyleSheetPtr = std::shared_ptr<const StyleSheet>;

} // namespace oc
