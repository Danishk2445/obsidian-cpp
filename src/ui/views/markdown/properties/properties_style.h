#pragma once

// Metrics and colors of Obsidian's metadata editor (.metadata-container and friends), resolved from the
// theme tokens: --metadata-* (label width 9em, input height 1.75em, input padding 4px 8px, gap 3px,
// radius 6px, label/input font size --font-smaller), pills (--pill-*, --tag-*), checkbox (--checkbox-*).

#include "render/theme/theme.h"

#include <QColor>
#include <QFont>
#include <QMargins>

namespace oc::propui {

struct Style {
    double em = 16;          // the container's font size (--font-text-size)
    int inputHeight = 28;    // --metadata-input-height
    int labelWidth = 144;    // --metadata-label-width
    int padX = 8, padY = 4;  // --metadata-input-padding
    int gap = 3;             // --metadata-gap (between properties)
    int radius = 6;          // --metadata-property-radius
    int iconSize = 18;       // --icon-size
    int iconXs = 14;         // --icon-xs
    int containerPadY = 8;   // --metadata-padding (vertical)
    int marginBottom = 32;   // .metadata-container margin-block-end: 2rem
    int headingPad = 4;      // .metadata-properties-heading padding (--size-4-1)
    int headingGapBelow = 8; // its margin-bottom (--size-4-2)
    int pillGap = 6;         // .multi-select-container gap (--size-2-3)
    QFont labelFont;         // --metadata-label-font / -font-size
    QFont inputFont;         // --metadata-input-font / -font-size
    QFont titleFont;         // .metadata-properties-title: max(--font-ui-small, 1em), --font-medium
    QFont buttonFont;        // .metadata-add-button
    QColor text, muted, faint, accent, warning, error;
    QColor labelText;        // --metadata-label-text-color
    QColor inputText;        // --metadata-input-text-color
    QColor placeholder;      // --input-placeholder-color
    QColor hoverBg;          // --background-modifier-hover (label/input active backgrounds)
    QColor ringHover;        // --metadata-property-box-shadow-hover (1px)
    QColor ringFocus;        // --metadata-property-box-shadow-focus (2px)
    QColor selectedBg;       // --nav-item-background-selected
    QColor iconColor, iconHover;
    QColor link, linkHover, linkUnresolved;
    double linkUnresolvedOpacity = 0.7;
    bool linkUnderline = true;
    // tags (pills of the "tags" widget)
    QColor tagText, tagBg, tagBgHover;
    double tagPadX = 9, tagPadY = 3.5, tagRadius = 14;
    QColor pillRemove, pillRemoveHover;
    // checkbox
    int checkboxSize = 16;
    double checkboxRadius = 4;
    QColor checkboxBorder, checkboxBorderHover, checkboxFill, checkboxFillHover, checkboxMarker;
    QColor errorBg;
    QColor collapseIcon;
};

// The current theme's values (cached per theme generation).
const Style& style();

} // namespace oc::propui
