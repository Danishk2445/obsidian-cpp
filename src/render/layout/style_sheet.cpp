#include "render/layout/style_sheet.h"

#include <QFontMetricsF>

#include <cmath>

namespace oc {

namespace {
QFont withPx(QFont f, qreal px)
{
    f.setPixelSize(std::max(1, int(std::lround(px))));
    return f;
}

LineMetrics metricsFor(const QFont& font, qreal lineHeightFactor, const QColor& color)
{
    LineMetrics m;
    m.font = font;
    const QFontMetricsF fm(font);
    m.ascent = fm.ascent();
    m.descent = fm.descent();
    m.lineBox = std::round(font.pixelSize() * lineHeightFactor);
    m.avgCharWidth = fm.horizontalAdvance(QStringLiteral("the quick brown fox jumps over the lazy dog etaoin shrdlu")) / 57.0;
    m.spaceWidth = fm.horizontalAdvance(QLatin1Char(' '));
    m.color = color;
    return m;
}
} // namespace

const LineMetrics& StyleSheet::line(StyleId lineStyle) const
{
    const size_t i = size_t(lineStyle) <= size_t(StyleId::HtmlBlock) ? size_t(lineStyle) : 0;
    return m_lines[i];
}

std::shared_ptr<const StyleSheet> StyleSheet::build(const Theme& t)
{
    auto s = std::make_shared<StyleSheet>();
    s->m_generation = t.generation();
    const qreal base = t.textSize();
    s->m_textSize = base;
    const qreal lh = t.number(QStringLiteral("--line-height-normal"), 1.5);
    const QColor text = t.color(QStringLiteral("--text-normal"));
    const QColor muted = t.color(QStringLiteral("--text-muted"));
    const QColor faint = t.color(QStringLiteral("--text-faint"));
    s->m_faint = faint;
    s->m_muted = muted;

    QFont textFont = t.textFont(base);
    textFont.setHintingPreference(QFont::PreferNoHinting);
    const qreal codePx = t.px(QStringLiteral("--code-size"), base, base * 0.875);
    QFont monoFont = t.monospaceFont(codePx);

    // ---- line styles ----
    s->m_lines[size_t(StyleId::Normal)] = metricsFor(textFont, lh, text);
    for (int level = 1; level <= 6; ++level) {
        const QString p = QStringLiteral("--h%1-").arg(level);
        const qreal px = t.px(p + QStringLiteral("size"), base, base);
        QFont f = withPx(textFont, px);
        f.setWeight(QFont::Weight(int(t.number(p + QStringLiteral("weight"), 600))));
        const qreal ls = t.px(p + QStringLiteral("letter-spacing"), px, 0);
        if (ls != 0)
            f.setLetterSpacing(QFont::AbsoluteSpacing, ls);
        const qreal lineH = t.number(p + QStringLiteral("line-height"), lh);
        LineMetrics m = metricsFor(f, lineH, t.color(p + QStringLiteral("color"), text));
        if (!m.color.isValid())
            m.color = text;
        s->m_lines[size_t(StyleId::Heading1) + size_t(level - 1)] = m;
    }
    const QColor codeNormal = t.color(QStringLiteral("--code-normal"), text);
    s->m_lines[size_t(StyleId::CodeBlock)] = metricsFor(monoFont, lh, codeNormal);
    s->m_lines[size_t(StyleId::MathBlock)] = metricsFor(monoFont, lh, codeNormal);
    s->m_lines[size_t(StyleId::Frontmatter)] = metricsFor(monoFont, lh, codeNormal);
    s->m_lines[size_t(StyleId::HrLine)] = metricsFor(monoFont, lh, faint);
    s->m_lines[size_t(StyleId::CommentBlock)] = metricsFor(textFont, lh, t.color(QStringLiteral("--code-comment"), faint));
    s->m_lines[size_t(StyleId::HtmlBlock)] = metricsFor(monoFont, lh, t.color(QStringLiteral("--code-tag"), text));

    // ---- marks ----
    auto fg = [&](StyleId id, const QColor& c) { s->m_specs[size_t(id)].format.setForeground(c); };
    fg(StyleId::Formatting, faint);
    fg(StyleId::ListMarker, t.color(QStringLiteral("--list-marker-color"), faint));
    {
        QTextCharFormat& f = s->m_specs[size_t(StyleId::Strong)].format;
        f.setFontWeight(QFont::Bold);
        const QColor c = t.color(QStringLiteral("--bold-color"));
        if (c.isValid())
            f.setForeground(c);
    }
    s->m_specs[size_t(StyleId::Emphasis)].format.setFontItalic(true);
    s->m_specs[size_t(StyleId::StrongEmphasis)].format.setFontWeight(QFont::Bold);
    s->m_specs[size_t(StyleId::StrongEmphasis)].format.setFontItalic(true);
    s->m_specs[size_t(StyleId::Strikethrough)].format.setFontStrikeOut(true);
    {
        StyleSpec& sp = s->m_specs[size_t(StyleId::Highlight)];
        sp.format.setBackground(t.color(QStringLiteral("--text-highlight-bg")));
    }
    {
        StyleSpec& sp = s->m_specs[size_t(StyleId::InlineCode)];
        sp.format.setFontFamilies({t.monospaceFamily()});
        sp.format.setFontPointSize(-1);
        sp.format.setProperty(QTextFormat::FontPixelSize, int(std::lround(codePx)));
        sp.format.setForeground(codeNormal);
        sp.box.enabled = true;
        sp.box.background = t.color(QStringLiteral("--code-background"));
        sp.box.radius = t.px(QStringLiteral("--code-radius"), base, 4);
        sp.box.padX = 0.3 * codePx;
    }
    {
        const QColor link = t.color(QStringLiteral("--link-color"), t.color(QStringLiteral("--text-accent")));
        QTextCharFormat& f = s->m_specs[size_t(StyleId::InternalLink)].format;
        f.setForeground(link);
        f.setFontUnderline(true);
        QTextCharFormat& u = s->m_specs[size_t(StyleId::InternalLinkUnresolved)].format;
        QColor ul = t.color(QStringLiteral("--link-unresolved-color"), link);
        ul.setAlphaF(ul.alphaF() * float(t.number(QStringLiteral("--link-unresolved-opacity"), 0.7)));
        u.setForeground(ul);
        u.setFontUnderline(true);
        u.setUnderlineColor(t.color(QStringLiteral("--link-unresolved-decoration-color"), ul));
        QTextCharFormat& e = s->m_specs[size_t(StyleId::ExternalLink)].format;
        e.setForeground(t.color(QStringLiteral("--link-external-color"), link));
        e.setFontUnderline(true);
        QTextCharFormat& url = s->m_specs[size_t(StyleId::Url)].format;
        url.setForeground(t.color(QStringLiteral("--link-external-color"), link));
        url.setFontUnderline(true);
    }
    {
        StyleSpec& sp = s->m_specs[size_t(StyleId::Tag)];
        sp.format.setForeground(t.color(QStringLiteral("--tag-color")));
        const qreal tagPx = t.px(QStringLiteral("--tag-size"), base, base * 0.875);
        sp.format.setProperty(QTextFormat::FontPixelSize, int(std::lround(tagPx)));
        sp.box.enabled = true;
        sp.box.background = t.color(QStringLiteral("--tag-background"));
        sp.box.radius = 1000; // pill (2em)
        sp.box.padX = t.px(QStringLiteral("--tag-padding-x"), tagPx, 0.65 * tagPx);
    }
    {
        QTextCharFormat& f = s->m_specs[size_t(StyleId::Math)].format;
        f.setFontItalic(true);
        f.setForeground(codeNormal);
        f.setProperty(QTextFormat::FontPixelSize, int(std::lround(codePx)));
    }
    fg(StyleId::Comment, t.color(QStringLiteral("--code-comment"), faint));
    fg(StyleId::Html, t.color(QStringLiteral("--code-tag"), text));
    fg(StyleId::FootnoteRef, muted);
    fg(StyleId::BlockId, faint);
    fg(StyleId::Escape, faint);
    {
        QTextCharFormat& f = s->m_specs[size_t(StyleId::TaskDone)].format;
        f.setForeground(t.color(QStringLiteral("--checklist-done-color"), muted));
        f.setFontStrikeOut(true);
    }
    // Obsidian: .obsidian-search-match-highlight { box-shadow: 0 0 0 3px var(--text-accent); border-radius: 2px }
    for (StyleId id : {StyleId::SearchMatch, StyleId::SearchMatchCurrent}) {
        BoxStyle& b = s->m_specs[size_t(id)].box;
        b.enabled = true;
        b.radius = 2;
        b.ring = t.color(QStringLiteral("--text-accent"));
        b.ringWidth = 3;
    }
    {
        QTextCharFormat& f = s->m_specs[size_t(StyleId::SpellError)].format;
        f.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
        f.setUnderlineColor(t.color(QStringLiteral("--text-error")));
    }
    {
        QTextCharFormat& f = s->m_specs[size_t(StyleId::Preedit)].format;
        f.setFontUnderline(true);
        f.setUnderlineStyle(QTextCharFormat::DotLine);
    }
    s->m_specs[size_t(StyleId::Invisible)].format.setForeground(QColor(0, 0, 0, 0));
    fg(StyleId::CodeComment, t.color(QStringLiteral("--code-comment"), faint));
    fg(StyleId::CodeKeyword, t.color(QStringLiteral("--code-keyword")));
    fg(StyleId::CodeFunction, t.color(QStringLiteral("--code-function")));
    fg(StyleId::CodeString, t.color(QStringLiteral("--code-string")));
    fg(StyleId::CodeValue, t.color(QStringLiteral("--code-value")));
    fg(StyleId::CodeOperator, t.color(QStringLiteral("--code-operator")));
    fg(StyleId::CodeProperty, t.color(QStringLiteral("--code-property")));
    fg(StyleId::CodePunctuation, t.color(QStringLiteral("--code-punctuation"), muted));
    fg(StyleId::CodeTag, t.color(QStringLiteral("--code-tag")));
    fg(StyleId::CodeImportant, t.color(QStringLiteral("--code-important")));

    // ---- block visuals ----
    s->m_listIndent = t.px(QStringLiteral("--list-indent"), base, base * 2.25);
    s->m_pSpacing = t.px(QStringLiteral("--p-spacing"), base, 16);
    s->m_listSpacing = t.px(QStringLiteral("--list-spacing"), base, base * 0.075);
    s->m_codePadX = t.px(QStringLiteral("--size-4-4"), base, 16);
    s->m_codeBg = t.color(QStringLiteral("--code-background"));
    s->m_codeRadius = t.px(QStringLiteral("--code-radius"), base, 4);
    s->m_quoteBar = t.px(QStringLiteral("--blockquote-border-thickness"), base, 2);
    s->m_quoteColor = t.color(QStringLiteral("--blockquote-border-color"), t.color(QStringLiteral("--interactive-accent")));
    s->m_quoteIndent = t.px(QStringLiteral("--size-4-6"), base, 24);
    s->m_selection = t.color(QStringLiteral("--text-selection"));
    s->m_caret = t.color(QStringLiteral("--caret-color"), text);
    s->m_guide = t.color(QStringLiteral("--indentation-guide-color"));
    s->m_guideActive = t.color(QStringLiteral("--indentation-guide-color-active"));
    s->m_hr = t.color(QStringLiteral("--hr-color"), t.color(QStringLiteral("--background-modifier-border")));
    s->m_hrThickness = t.px(QStringLiteral("--hr-thickness"), base, 2);
    return s;
}

} // namespace oc
