#include "ui/views/bases/bases_style.h"

#include <cmath>

namespace oc::basesui {

namespace {

void build(Style& s, const Theme& t, bool embedded)
{
    auto color = [&](const char* name, const QColor& fallback = QColor()) { return t.color(QString::fromLatin1(name), fallback); };
    const double text = t.px(QStringLiteral("--font-text-size"), -1, 16);
    // A .base view inherits the body's --font-ui-medium in the interface font; an embed the note's text.
    s.em = embedded ? text : t.px(QStringLiteral("--font-ui-medium"), text, 15);
    const double em = s.em;
    auto px = [&](const char* name, double fallback, double relTo = -1) {
        return t.px(QString::fromLatin1(name), relTo > 0 ? relTo : em, fallback);
    };
    auto ipx = [&](const char* name, double fallback) { return int(std::lround(px(name, fallback))); };
    auto font = [&](double size) { return embedded ? t.textFont(size) : t.interfaceFont(size); };

    s.headerHeight = ipx("--bases-header-height", 40);
    s.headerPadStart = embedded ? 0 : ipx("--size-4-2", 8);
    // embeds keep room for the embed action (--bases-header-padding-end: --icon-m + --size-4-4)
    s.headerPadEnd = embedded ? ipx("--icon-m", 18) + ipx("--size-4-4", 16) : ipx("--size-4-2", 8);
    s.toolbarGap = ipx("--size-2-2", 4);
    s.buttonPad = ipx("--size-2-2", 4);
    s.buttonRadius = ipx("--button-radius", 4);
    s.iconS = ipx("--icon-s", 16);
    s.iconXs = ipx("--icon-xs", 14);
    s.iconM = ipx("--icon-m", 18);
    s.uiSmall = t.interfaceFont(px("--font-ui-small", 13));
    s.uiSmaller = t.interfaceFont(px("--font-ui-smaller", 12));
    s.uiMedium = t.interfaceFont(px("--font-ui-medium", 15));

    s.rowHeight = ipx("--bases-table-row-height", 30);
    s.colMinWidth = int(t.number(QStringLiteral("--bases-table-column-min-width"), 40));
    s.colMaxWidth = int(t.number(QStringLiteral("--bases-table-column-max-width"), 300));
    s.groupGap = ipx("--bases-table-group-gap", 10);
    s.cellPadX = ipx("--size-4-2", 8);
    s.cellPadY = ipx("--size-4-1", 4);
    const double smaller = px("--font-smaller", em * 0.875, em);
    s.cellFont = font(smaller);
    s.headerFont = font(smaller);
    s.headerFont.setWeight(QFont::Weight(int(t.number(QStringLiteral("--font-normal"), 400))));
    s.groupPropertyFont = t.interfaceFont(px("--font-ui-smaller", 12));
    s.groupValueFont = font(smaller);
    s.groupValueFont.setWeight(QFont::Weight(int(t.number(QStringLiteral("--font-semibold"), 600))));

    s.cardRadius = ipx("--radius-m", 8);
    s.cardLineHeight = ipx("--bases-cards-line-height", 24);
    s.cardGap = ipx("--size-4-3", 12);
    s.cardPadTop = ipx("--size-4-2", 8);
    s.cardPadBottom = ipx("--size-2-1", 2);
    s.cardPropGap = ipx("--size-4-2", 8);
    s.cardTitleFont = font(px("--font-ui-medium", 15));
    s.cardTitleFont.setWeight(QFont::Weight(int(t.number(QStringLiteral("--font-normal"), 400) + t.number(QStringLiteral("--bold-modifier"), 200))));
    s.cardLabelFont = t.interfaceFont(px("--font-ui-smaller", 12));
    s.cardFont = font(smaller);

    s.listFont = font(em);
    s.listIndent = ipx("--list-indent-editing", em * 0.75);
    s.listSpacing = std::max(0, ipx("--list-spacing", 1));

    s.menuRadius = ipx("--menu-radius", 8);
    s.menuPadding = ipx("--size-2-2", 4);
    s.menuItemPadX = ipx("--size-4-2", 8);
    s.menuItemPadY = ipx("--size-4-1", 4);
    s.menuFont = t.interfaceFont(px("--font-ui-small", 13));

    s.text = color("--text-normal");
    s.muted = color("--text-muted");
    s.faint = color("--text-faint");
    s.accent = color("--text-accent");
    s.error = color("--text-error");
    s.warning = color("--text-warning");
    s.success = color("--text-success", s.accent);
    s.bgPrimary = color("--background-primary");
    s.bgPrimaryAlt = color("--background-primary-alt");
    s.bgSecondary = color("--background-secondary");
    s.hover = color("--background-modifier-hover");
    s.activeHover = color("--background-modifier-active-hover", s.hover);
    s.border = color("--background-modifier-border");
    s.borderHover = color("--background-modifier-border-hover", s.border);
    s.borderFocus = color("--background-modifier-border-focus", s.borderHover);
    s.tableBorder = color("--bases-table-border-color", color("--table-border-color", s.border));
    s.headerBg = color("--bases-table-header-background", s.bgPrimary);
    s.headerBgHover = color("--bases-table-header-background-hover", s.hover);
    s.headerText = color("--bases-table-header-color", s.muted);
    s.groupBg = color("--bases-table-group-background", s.bgPrimaryAlt);
    s.rowHover = color("--bases-table-row-background-hover", Qt::transparent);
    s.summaryBg = color("--bases-table-summary-background", s.bgPrimary);
    s.interactiveAccent = color("--interactive-accent", s.accent);
    s.textOnAccent = color("--text-on-accent", Qt::white);
    s.cellFocusRing = s.interactiveAccent;
    s.cellActiveRing = s.borderFocus;
    s.cellDisabledBg = color("--bases-table-cell-background-disabled", s.bgPrimaryAlt);
    s.selection = color("--table-selection", color("--text-selection"));
    s.iconColor = color("--icon-color", s.muted);
    s.iconHover = color("--icon-color-hover", s.text);
    s.iconActive = color("--icon-color-active", s.accent);
    s.link = color("--link-color", s.accent);
    s.linkUnresolved = color("--link-unresolved-color", s.link);
    s.linkUnresolvedOpacity = t.number(QStringLiteral("--link-unresolved-opacity"), 0.7);
    s.tagText = color("--tag-color", s.accent);
    s.tagBg = color("--tag-background");
    s.cardsBg = color("--bases-cards-background", s.bgPrimary);
    s.cardsCoverBg = color("--bases-cards-cover-background", s.bgPrimaryAlt);
    s.cardsShadow = s.border;
    s.cardsShadowHover = s.borderHover;
    s.menuBg = color("--menu-background", s.bgSecondary);
    s.menuBorder = color("--menu-border-color", s.borderHover);
    s.inputBg = color("--background-modifier-form-field", s.bgPrimary);
    s.listMarker = color("--list-marker-color", s.faint);
}

} // namespace

const Style& basesStyle(bool embedded)
{
    static Style styles[2];
    static quint64 generations[2] = {~quint64(0), ~quint64(0)};
    const ThemePtr t = currentTheme();
    const int i = embedded ? 1 : 0;
    if (generations[i] != t->generation()) {
        generations[i] = t->generation();
        styles[i] = Style();
        build(styles[i], *t, embedded);
    }
    return styles[i];
}

} // namespace oc::basesui
