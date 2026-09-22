#pragma once

// Graph view options with Obsidian's graph.json key names (global graph) and the local graph's
// view-state options (same keys minus showOrphans, plus localJumps/localBacklinks/
// localForelinks/localInterlinks).

#include "graph/force_simulation.h"
#include "graph/graph_data.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace oc::graph {

struct ColorGroupSetting {
    QString query;
    GraphColor color;
    friend bool operator==(const ColorGroupSetting&, const ColorGroupSetting&) = default;
};

struct GraphSettings {
    enum class Variant { Global, Local };

    // Filters
    bool collapseFilter = true; // "collapse-filter"
    QString search;
    bool showTags = false;
    bool showAttachments = false;
    bool hideUnresolved = false; // "Existing files only"
    bool showOrphans = true;     // global only
    // Local graph filters
    int localJumps = 1;
    bool localBacklinks = true;
    bool localForelinks = true;
    bool localInterlinks = false;
    // Groups
    bool collapseColorGroups = true; // "collapse-color-groups"
    QList<ColorGroupSetting> colorGroups;
    // Display
    bool collapseDisplay = true; // "collapse-display"
    bool showArrow = false;
    double textFadeMultiplier = 0; // -3..3
    double nodeSizeMultiplier = 1; // 0.1..5
    double lineSizeMultiplier = 1; // 0.1..5
    // Forces
    bool collapseForces = true;                 // "collapse-forces"
    double centerStrength = 0.518713248970312;  // 0..1, = unmapSlider(0.1)
    double repelStrength = 10;                  // 0..20
    double linkStrength = 1;                    // 0..1
    double linkDistance = 250;                  // 30..500
    // View
    double scale = 1;   // zoom (renderer targetScale)
    bool close = false; // controls panel closed

    friend bool operator==(const GraphSettings&, const GraphSettings&) = default;

    ForceParams forces() const { return {centerStrength, repelStrength, linkStrength, linkDistance}; }

    // Obsidian's "Restore default settings" (filters incl. local ones, groups, display, forces;
    // the search text, collapse states, zoom and close state are kept).
    void restoreDefaults();

    // The user's graph.json (docs/obsidian-reference.md).
    static GraphSettings userDefaults();

    // Exactly the keys Obsidian writes for the variant.
    QJsonObject toJson(Variant variant = Variant::Global) const;
    // Missing or mistyped keys keep their defaults.
    static GraphSettings fromJson(const QJsonObject& json);
};

} // namespace oc::graph
