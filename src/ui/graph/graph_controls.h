#pragma once

// Obsidian's graph controls (".graph-controls"): the floating settings panel in the top-right
// corner of a graph view, --graph-controls-width wide, with four collapsible sections — Filters,
// Groups, Display, Forces — plus "Restore default settings" and close buttons. Closed, it shrinks to
// a settings button (and, in the global graph, the timelapse "Animate" wand).
//
// The panel edits a GraphSettings copy; every user edit emits edited() with what changed. It never
// emits for programmatic setSettings().

#include "graph/graph_settings.h"

#include <QFlags>
#include <QList>
#include <QWidget>

class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace oc {

class IconButton;
class SearchInput;
class ToggleSwitch;
class ValueSlider;

namespace graph_controls_detail {
class Section;
class GroupRow;
} // namespace graph_controls_detail

class GraphControls : public QWidget {
    Q_OBJECT
public:
    enum class Variant { Global, Local };
    enum class Change {
        Search = 0x01,       // search text typed (debounce) — SearchSubmitted: Enter
        SearchSubmitted = 0x02,
        Filters = 0x04,      // toggles, local depth/direction options
        GroupQuery = 0x08,   // a group's query typed (debounce)
        Groups = 0x10,       // group added/removed/recoloured
        Display = 0x20,
        Forces = 0x40,
        Layout = 0x80,       // section collapsed/expanded, panel opened/closed
    };
    Q_DECLARE_FLAGS(Changes, Change)

    explicit GraphControls(Variant variant, QWidget* parent = nullptr);
    ~GraphControls() override;

    Variant variant() const { return m_variant; }

    void setSettings(const graph::GraphSettings& settings);
    const graph::GraphSettings& settings() const { return m_settings; }

    bool isClosed() const { return m_settings.close; }
    void setClosed(bool closed); // like the close / settings buttons (emits Layout)
    void setSectionCollapsed(int section, bool collapsed); // 0 Filters, 1 Groups, 2 Display, 3 Forces (emits Layout)
    // Obsidian's showSearch(): opens the panel and the Filters section and focuses the search box.
    void focusSearch();
    // Search is running: the search row shows Obsidian's loading bar.
    void setSearching(bool searching);

    // Closed-state background: --background-primary in the main area, --background-secondary in a
    // sidebar (".workspace-split:not(.mod-root) .graph-controls.is-close").
    void setInSidebar(bool inSidebar);

    // Rounded panel rect (widget coordinates) when open — the canvas draws its shadow.
    QRectF panelRect() const { return QRectF(rect()); }
    double panelRadius() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

    // Widgets, for tests.
    SearchInput* searchInput() const { return m_search; }
    ToggleSwitch* toggle(const QString& key) const;  // graph.json key ("showTags", "localBacklinks", ...)
    ValueSlider* slider(const QString& key) const;   // "textFadeMultiplier", "centerStrength", "localJumps", ...
    IconButton* closeButton() const { return m_closeBtn; }
    IconButton* openButton() const { return m_openBtn; }
    IconButton* resetButton() const { return m_resetBtn; }
    IconButton* animateButton() const { return m_animateBtn; }
    QPushButton* newGroupButton() const { return m_newGroup; }
    QPushButton* animateTextButton() const { return m_animateText; }
    int groupRowCount() const { return int(m_groupRows.size()); }
    void setGroupQuery(int row, const QString& query); // as if typed (emits GroupQuery)
    void removeGroup(int row);                           // as if the × was clicked (emits Groups)
    void setGroupColor(int row, const QColor& color);    // as if picked (emits Groups)

signals:
    void edited(oc::GraphControls::Changes changes);
    void animateRequested();
    void sizeChanged(); // sizeHint changed (collapse, close, groups)

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    friend class graph_controls_detail::GroupRow;
    void build();
    void rebuildGroups();
    void addGroupRow(const graph::ColorGroupSetting& group);
    void syncWidgets();
    void updateLayout();
    void restoreDefaults();
    void emitEdited(Changes changes);

    Variant m_variant;
    graph::GraphSettings m_settings;
    bool m_inSidebar = false;
    bool m_syncing = false;

    QScrollArea* m_scroll = nullptr;
    QWidget* m_content = nullptr;
    QList<graph_controls_detail::Section*> m_sections;
    SearchInput* m_search = nullptr;
    QWidget* m_searchRow = nullptr;
    QWidget* m_loading = nullptr; // search progress bar
    QHash<QString, ToggleSwitch*> m_toggles;
    QHash<QString, ValueSlider*> m_sliders;
    QWidget* m_groupsContainer = nullptr;
    QVBoxLayout* m_groupsLayout = nullptr;
    QList<graph_controls_detail::GroupRow*> m_groupRows;
    QPushButton* m_newGroup = nullptr;
    QPushButton* m_animateText = nullptr;
    IconButton* m_closeBtn = nullptr;
    IconButton* m_resetBtn = nullptr;
    IconButton* m_openBtn = nullptr;
    IconButton* m_animateBtn = nullptr;
};

} // namespace oc

Q_DECLARE_OPERATORS_FOR_FLAGS(oc::GraphControls::Changes)
