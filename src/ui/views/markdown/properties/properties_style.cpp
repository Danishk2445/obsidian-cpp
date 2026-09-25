#include "ui/views/markdown/properties/properties_style.h"

#include <cmath>

namespace oc::propui {

const Style& style()
{
    static Style s;
    static quint64 generation = ~quint64(0);
    const ThemePtr t = currentTheme();
    if (t->generation() == generation)
        return s;
    generation = t->generation();
    auto px = [&](const char* name, double em, double fallback) { return t->px(QString::fromLatin1(name), em, fallback); };
    auto color = [&](const char* name, const QColor& fallback = QColor()) { return t->color(QString::fromLatin1(name), fallback); };
    s.em = t->px(QStringLiteral("--font-text-size"), -1, 16);
    const double em = s.em;
    s.inputHeight = int(std::round(px("--metadata-input-height", em, em * 1.75)));
    s.labelWidth = int(std::round(px("--metadata-label-width", em, em * 9)));
    s.padY = int(std::round(px("--size-4-1", em, 4)));
    s.padX = int(std::round(px("--size-4-2", em, 8)));
    s.gap = int(std::round(px("--metadata-gap", em, 3)));
    s.radius = int(std::round(px("--metadata-property-radius", em, 6)));
    s.iconSize = int(std::round(px("--icon-m", em, 18)));
    s.iconXs = int(std::round(px("--icon-xs", em, 14)));
    s.containerPadY = int(std::round(px("--size-4-2", em, 8)));
    s.headingPad = int(std::round(px("--size-4-1", em, 4)));
    s.headingGapBelow = int(std::round(px("--size-4-2", em, 8)));
    s.pillGap = int(std::round(px("--size-2-3", em, 6)));
    const double small = px("--metadata-label-font-size", em, em * 0.875);
    s.labelFont = t->interfaceFont(small);
    s.inputFont = t->interfaceFont(px("--metadata-input-font-size", em, em * 0.875));
    s.titleFont = t->interfaceFont(std::max(px("--font-ui-small", em, 13), em));
    s.titleFont.setWeight(QFont::Weight(int(t->number(QStringLiteral("--font-medium"), 500))));
    s.buttonFont = s.labelFont;
    s.text = color("--text-normal");
    s.muted = color("--text-muted");
    s.faint = color("--text-faint");
    s.accent = color("--text-accent");
    s.warning = color("--text-warning");
    s.error = color("--text-error");
    s.labelText = color("--metadata-label-text-color", s.muted);
    s.inputText = color("--metadata-input-text-color", s.text);
    s.placeholder = color("--input-placeholder-color", s.faint);
    s.hoverBg = color("--background-modifier-hover");
    s.ringHover = color("--background-modifier-border-hover");
    s.ringFocus = color("--background-modifier-border-focus");
    s.selectedBg = color("--nav-item-background-selected", s.hoverBg);
    s.iconColor = color("--icon-color", s.muted);
    s.iconHover = color("--icon-color-focused", s.text);
    s.link = color("--link-color", s.accent);
    s.linkHover = color("--link-color-hover", s.link);
    s.linkUnresolved = color("--link-unresolved-color", s.link);
    s.linkUnresolvedOpacity = t->number(QStringLiteral("--link-unresolved-opacity"), 0.7);
    s.linkUnderline = t->raw(QStringLiteral("--link-decoration")).trimmed() != u"none";
    const double tagEm = s.inputFont.pixelSize() > 0 ? s.inputFont.pixelSize() : 14;
    s.tagText = color("--tag-color", s.accent);
    s.tagBg = color("--tag-background");
    s.tagBgHover = color("--tag-background-hover", s.tagBg);
    s.tagPadX = px("--tag-padding-x", tagEm, tagEm * 0.65);
    s.tagPadY = px("--tag-padding-y", tagEm, tagEm * 0.25);
    s.tagRadius = px("--tag-radius", tagEm, tagEm * 2);
    s.pillRemove = color("--pill-color-remove", s.faint);
    s.pillRemoveHover = color("--pill-color-remove-hover", s.accent);
    s.checkboxSize = int(std::round(px("--checkbox-size", em, em)));
    s.checkboxRadius = px("--checkbox-radius", em, 4);
    s.checkboxBorder = color("--checkbox-border-color", s.faint);
    s.checkboxBorderHover = color("--checkbox-border-color-hover", s.muted);
    s.checkboxFill = color("--checkbox-color", color("--interactive-accent"));
    s.checkboxFillHover = color("--checkbox-color-hover", s.checkboxFill);
    s.checkboxMarker = color("--checkbox-marker-color", color("--background-primary"));
    QColor err = s.error;
    err.setAlphaF(0.1);
    s.errorBg = err;
    s.collapseIcon = color("--collapse-icon-color", s.faint);
    return s;
}

} // namespace oc::propui
