#pragma once

#include "core/search/search_match.h"
#include "core/search/search_snippet.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QList>
#include <QTimer>

#include <memory>
#include <vector>

namespace oc {

// One file of the global search results (Obsidian's .search-result tree item with its .search-result-file-matches).
struct SearchResultGroup {
    QString path;
    QString title;                      // basename; ".md" dropped
    qint64 mtimeMs = 0;
    qint64 ctimeMs = 0;
    std::shared_ptr<const search::SearchDocument> doc;
    QList<search::ContentMatch> contentMatches;
    QList<MatchRange> titleMatches;     // file name hits, relative to title
    QList<search::Snippet> snippets;
    int moreMatches = 0;                // content matches not covered by the shown snippets
    bool collapsed = false;

    // layout cache (valid for layoutWidth; reset it to -1 after changing the title or the snippets)
    int layoutWidth = -1;
    int titleHeight = 0;
    QList<int> snippetHeights;
    int moreHeight = 0;
    int height = 0;
};

// The painted, virtualized result tree of the search view: file title rows (collapse arrow, name, match count)
// followed by a bordered box of match snippets with highlighted matches, "... and N more matches.", the loading
// bar and the empty state. Rows use Obsidian's .tree-item-self / .search-result-file-match metrics.
class SearchResultsList : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum class Part { None, Collapse, Title, Snippet, More };
    struct Hit {
        int group = -1;
        int snippet = -1;
        Part part = Part::None;
    };

    explicit SearchResultsList(QWidget* parent = nullptr);

    // The groups are owned here; after changing them through mutableGroups() call groupsChanged() (collapsing
    // only needs that; new titles/snippets also need layoutWidth = -1).
    const QList<SearchResultGroup>& groups() const { return m_groups; }
    QList<SearchResultGroup>& mutableGroups() { return m_groups; }
    void groupsChanged(); // re-measures invalidated groups, recomputes offsets, repaints
    void clear();

    void setEmptyText(const QString& text); // shown when there are no groups (e.g. "No matches found.")
    QString emptyText() const { return m_emptyText; }
    void setLoading(bool loading);          // the 3px accent progress bar at the top
    bool isLoading() const { return m_loading; }

    Hit hitTest(const QPoint& viewportPos) const;
    // Viewport rectangle of a group's title row (snippet -1), one of its snippets, or its "more matches" row
    // (snippet == snippets.size()). Empty when collapsed or out of range.
    QRect itemRect(int group, int snippet = -1) const;
    int contentHeight() const;
    void scrollToGroup(int group);

    // Display strings (tests / copy).
    static QString snippetDisplayText(const search::Snippet& s);
    static QString moreMatchesText(int count);

signals:
    void titleClicked(int group, Qt::KeyboardModifiers modifiers);
    void snippetClicked(int group, int snippet, Qt::KeyboardModifiers modifiers);
    void collapseToggled(int group);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool viewportEvent(QEvent* e) override;

private:
    void measure(SearchResultGroup& g, int width) const;
    static int totalHeight(const SearchResultGroup& g);
    int innerWidth() const;
    void updateScrollRange();
    int groupTop(int group) const { return m_offsets[size_t(group)]; }
    void setHover(const Hit& hit);

    QList<SearchResultGroup> m_groups;
    std::vector<int> m_offsets; // content y of each group (size = groups + 1)
    QString m_emptyText;
    bool m_loading = false;
    QElapsedTimer m_loadingClock;
    QTimer m_loadingTimer;
    Hit m_hover;
    Hit m_pressed;
};

} // namespace oc
