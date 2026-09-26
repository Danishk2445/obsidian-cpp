#pragma once

// Metrics and colors of Obsidian's Bases UI (app.css .bases-*), resolved from the theme tokens:
// the header / toolbar (--bases-header-height 40px, .text-icon-button), the table (--bases-table-*:
// 30px rows, 40-300px auto columns, 10px group gaps, --font-smaller cells), cards (--bases-cards-*),
// the list view, group headings and the toolbar menus (.menu, --menu-*). `embedded` gives the values
// of a base shown inside a note (the note's text font; header padding 0).

#include "render/theme/theme.h"

#include <QColor>
#include <QFont>

namespace oc::basesui {

struct Style {
    double em = 15;          // the container font size (--font-ui-medium in a .base view, the text size in notes)
    // header / toolbar
    int headerHeight = 40;   // --bases-header-height
    int headerPadStart = 8;  // --bases-header-padding-start (.base view: --size-4-2)
    int headerPadEnd = 8;
    int toolbarGap = 4;      // column-gap --size-2-2
    int buttonPad = 4;       // .text-icon-button padding --size-2-2
    int buttonRadius = 4;    // --button-radius
    int iconS = 16, iconXs = 14, iconM = 18;
    QFont uiSmall;           // --font-ui-small (13px)
    QFont uiSmaller;         // --font-ui-smaller (12px)
    QFont uiMedium;          // --font-ui-medium
    // table
    int rowHeight = 30;      // --bases-table-row-height
    int colMinWidth = 40;
    int colMaxWidth = 300;
    int groupGap = 10;       // --bases-table-group-gap
    int cellPadX = 8;        // --metadata-input-padding
    int cellPadY = 4;
    QFont cellFont;          // --bases-table-font-size (--font-smaller)
    QFont headerFont;        // same size, --bases-table-header-weight
    QFont groupPropertyFont; // --bases-group-heading-property-size (--font-ui-smaller)
    QFont groupValueFont;    // --bases-group-heading-value-size (--font-smaller), semibold
    // cards
    int cardSize = 200;
    int cardRadius = 8;      // --bases-cards-radius (--radius-m)
    int cardLineHeight = 24; // --bases-cards-line-height
    int cardGap = 12;        // .bases-cards-group gap --size-4-3
    int cardPadTop = 8, cardPadBottom = 2, cardPadX = 0;
    int cardPropGap = 8;     // .bases-cards-item gap --size-4-2
    QFont cardTitleFont;     // --font-ui-medium, bold
    QFont cardLabelFont;     // --font-ui-smaller
    QFont cardFont;          // --bases-cards-font-size (--font-smaller)
    // list
    QFont listFont;
    int listIndent = 32;     // --list-indent-editing
    int listSpacing = 1;     // --list-spacing
    // menus
    int menuRadius = 8;
    int menuPadding = 4;
    int menuItemPadX = 8, menuItemPadY = 4;
    QFont menuFont;          // --font-ui-small

    QColor text, muted, faint, accent, error, warning, success;
    QColor bgPrimary, bgPrimaryAlt, bgSecondary;
    QColor hover, activeHover;           // --background-modifier-hover / -active-hover
    QColor border, borderHover, borderFocus;
    QColor tableBorder;                  // --bases-table-border-color
    QColor headerBg, headerBgHover, headerText;
    QColor groupBg;                      // --bases-table-group-background
    QColor rowHover;                     // --bases-table-row-background-hover
    QColor summaryBg;
    QColor cellFocusRing;                // --bases-table-cell-shadow-focus (interactive accent)
    QColor cellActiveRing;               // --bases-table-cell-shadow-active
    QColor cellDisabledBg;
    QColor selection;                    // --table-selection
    QColor iconColor, iconHover, iconActive;
    QColor link, linkUnresolved;
    double linkUnresolvedOpacity = 0.7;
    QColor tagText, tagBg;
    QColor cardsBg, cardsCoverBg, cardsShadow, cardsShadowHover;
    QColor menuBg, menuBorder;
    QColor inputBg;                      // --background-modifier-form-field
    QColor interactiveAccent, textOnAccent;
    QColor listMarker;
};

// The current theme's values (cached per theme generation); `embedded`: inside a note.
const Style& basesStyle(bool embedded = false);

} // namespace oc::basesui
