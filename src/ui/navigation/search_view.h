#pragma once

#include "core/search/search_engine.h"
#include "core/search/search_query.h"
#include "ui/workspace/view.h"

#include <QPointer>
#include <QSet>
#include <QTimer>

#include <memory>

class QLabel;

namespace oc {

class ExplainPanel;
class GlobalSearchInput;
class IconButton;
class SearchResultsList;
class SearchSnapshot;
class SortDropdown;
class ToggleSwitch;
class VaultSearchSource;
class ViewRegistry;
struct SearchResultGroup;

// Obsidian's global search view ("search", core plugin global-search): a sidebar view with the search input
// (magnifier, clear button, "Match case" toggle), the "Search settings" toggle revealing Explain search terms /
// Collapse results / Show more context, the explanation of the query, "N results" with the sort order menu, and
// the streamed results: one collapsible group per file with context snippets whose matches are highlighted.
//
// Typing restarts the search after a short debounce (Enter runs it at once); the SearchEngine cancels the previous
// run. Results follow vault, metadata and open-document changes (small change sets are re-evaluated in place,
// larger ones re-run the search while the old results stay visible).
//
// A click on a match opens the file (Mod: new tab, Mod+Alt: split right) with the ephemeral state
//   {"match": {"content": <document text searched>, "matches": [[from, to], ...]}}
// (offsets into content, the clicked snippet's matches first), which MarkdownView turns into a selection; a click
// on a file name opens the file. View state (workspace.json), Obsidian's keys:
//   {"query", "matchingCase", "explainSearch", "collapseAll", "extraContext", "sortOrder"}
// sortOrder: "alphabetical" | "alphabeticalReverse" | "byModifiedTime" | "byModifiedTimeReverse" |
//            "byCreatedTime" | "byCreatedTimeReverse".
class SearchView : public View {
    Q_OBJECT
public:
    enum class SortOrder { Alphabetical, AlphabeticalReverse, ByModifiedTime, ByModifiedTimeReverse, ByCreatedTime, ByCreatedTimeReverse };
    static QString sortOrderKey(SortOrder order);
    static SortOrder sortOrderFromKey(const QString& key); // unknown -> Alphabetical
    static QString sortOrderLabel(SortOrder order);        // "File name (A to Z)", ...

    // Snippet options: characters of context around a match within its line, whole lines with "Show more context",
    // snippets shown per file before "... and N more matches.".
    static constexpr int kContextChars = 100;
    static constexpr int kExtraContextLines = 2;
    static constexpr int kMaxSnippets = 10;
    static constexpr int kDebounceMs = 250;
    static constexpr int kRefreshDebounceMs = 300;

    explicit SearchView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~SearchView() override;

    QString viewType() const override { return QStringLiteral("search"); }
    QString displayText() const override { return tr("Search"); }
    QString icon() const override { return QStringLiteral("search"); }
    bool navigation() const override { return false; }
    bool showsViewHeader() const override { return false; }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void populatePaneMenu(QMenu* menu) override;
    void focusContent() override { focusInput(); }

    QString query() const;
    void setQuery(const QString& query); // sets the input and searches right away
    void focusInput(bool selectAll = true);

    bool matchCase() const { return m_matchCase; }
    void setMatchCase(bool on);
    bool explainSearch() const { return m_explain; }
    void setExplainSearch(bool on);
    bool collapseAll() const { return m_collapseAll; }
    void setCollapseAll(bool on);
    bool extraContext() const { return m_extraContext; }
    void setExtraContext(bool on);
    SortOrder sortOrder() const { return m_sortOrder; }
    void setSortOrder(SortOrder order);
    bool settingsVisible() const;
    void setSettingsVisible(bool visible);

    // Results in display order.
    bool isSearching() const { return m_searching; }
    // No search running and none scheduled (typing debounce, change refresh).
    bool isIdle() const { return !m_searching && !m_debounce.isActive() && !m_refreshTimer.isActive(); }
    int resultCount() const;
    QStringList resultPaths() const;
    const SearchResultGroup* result(const QString& path) const;
    QString resultCountText() const;
    QString explanation() const; // the explain panel's text ("" when hidden)

    // Obsidian's "Copy search results" text: one line per file, optionally with the full path, as a link and with a
    // list prefix.
    enum class LinkStyle { None, Wikilink, Markdown };
    enum class ListPrefix { None, Dash, Asterisk, Numbered };
    QString resultsText(bool showPath = false, LinkStyle link = LinkStyle::Wikilink, ListPrefix prefix = ListPrefix::Dash) const;
    void copyResults(); // resultsText() with the defaults to the clipboard + notice

    // What clicks do (also used by keyboard handling and tests).
    void openResult(int group, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void openMatch(int group, int snippet, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    QJsonObject matchEState(int group, int snippet) const;

    SearchResultsList* resultsList() const { return m_results; }
    GlobalSearchInput* input() const { return m_input; }

signals:
    void searchStarted();
    void searchFinished();
    void resultsChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void startSearch(bool keepResults);
    void cancelSearch();
    void onBatch(const QList<search::SearchResult>& batch, quint64 generation);
    void onFinished(quint64 generation);
    SearchResultGroup makeGroup(const search::SearchResult& r, const std::shared_ptr<const search::SearchDocument>& doc,
                                qint64 mtimeMs, qint64 ctimeMs, bool collapsed) const;
    void buildSnippets(SearchResultGroup& g) const;
    bool lessThan(const SearchResultGroup& a, const SearchResultGroup& b) const;
    void insertSorted(SearchResultGroup g);
    void resort();
    void onPathChanged(const QString& path);
    void applyPendingChanges();
    void updateChrome(); // explain panel, results info, empty state
    void setSearching(bool on);
    void emitStateChanged();

    VaultSearchSource* m_source = nullptr;
    search::SearchEngine m_engine;
    search::SearchQuery m_query;
    std::shared_ptr<SearchSnapshot> m_snapshot;
    quint64 m_generation = 0;
    bool m_searching = false;
    bool m_refreshing = false; // re-run after changes: keep the old results until it finishes
    bool m_busy = false;
    QList<SearchResultGroup> m_pending;
    QSet<QString> m_collapsedPaths; // groups toggled against collapseAll (kept across refreshes)
    QSet<QString> m_changed;
    QTimer m_debounce;
    QTimer m_refreshTimer;

    bool m_matchCase = false;
    bool m_explain = false;
    bool m_collapseAll = false;
    bool m_extraContext = false;
    SortOrder m_sortOrder = SortOrder::Alphabetical;
    bool m_applyingState = false;

    GlobalSearchInput* m_input = nullptr;
    IconButton* m_settingsButton = nullptr;
    QWidget* m_params = nullptr;
    ToggleSwitch* m_explainToggle = nullptr;
    ToggleSwitch* m_collapseToggle = nullptr;
    ToggleSwitch* m_contextToggle = nullptr;
    ExplainPanel* m_explainPanel = nullptr;
    QWidget* m_info = nullptr;
    QLabel* m_count = nullptr;
    SortDropdown* m_sort = nullptr;
    SearchResultsList* m_results = nullptr;
};

// Registers the "search" view type (replacing the sidebar placeholder registered by registerCoreViews()).
void registerSearchView(ViewRegistry& registry);

// Reveals the search view in the left sidebar (creating it when missing), optionally sets the query (null =
// keep the current one) and focuses the input. Obsidian's openGlobalSearch(); tag clicks use "tag:#tag".
SearchView* openGlobalSearch(VaultSession* session, const QString& query = QString(), bool focus = true);

} // namespace oc
