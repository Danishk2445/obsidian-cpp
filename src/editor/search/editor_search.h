#pragma once

#include "editor/search/doc_searcher.h"

#include <QObject>
#include <QPointer>

#include <optional>

namespace oc {

class Document;
class EditorView;
struct DocUpdate;

// The logic behind Obsidian's in-file search bar, bound to one EditorView: a query searched from the
// editor's selection, the current match (ringed and centered, the editor selection is left alone),
// "find all" ringing every match, and replace / replace all as undoable transactions. Highlights
// follow edits made while the bar is open.
class EditorSearch : public QObject {
    Q_OBJECT
public:
    explicit EditorSearch(EditorView* view, QObject* parent = nullptr);
    ~EditorSearch() override;

    EditorView* view() const { return m_view; }

    // A new query: searches forward from the end of the editor's main selection (so a selected
    // occurrence is skipped, as in Obsidian). An empty query clears everything. Returns whether a
    // match was found.
    bool setQuery(const SearchQuery& query);
    const SearchQuery& query() const { return m_searcher.query(); }

    std::optional<SearchMatch> current() const { return m_current; }
    const QList<SearchMatch>& highlights() const { return m_highlights; }

    bool findNext();
    bool findPrevious();
    int findAll();                                   // ring every match; returns how many
    bool replaceCurrent(const QString& replacement); // then moves on to the next match
    int replaceAll(const QString& replacement);      // a single undo step; returns how many
    std::pair<int, int> indexAndCount() const;       // the "i / n" counter
    void selectCurrent();                            // select the current match in the editor
    void clear();                                    // forget the query, remove the highlights

signals:
    void resultsChanged(); // current match or highlights changed

private:
    Document* bindDocument();
    void onDocChanged(const DocUpdate& u);
    bool show(std::optional<SearchMatch> m);
    void setHighlights(QList<SearchMatch> ranges);
    void refreshLines(const QList<SearchMatch>& a, const QList<SearchMatch>& b);

    QPointer<EditorView> m_view;
    QPointer<Document> m_doc;
    QMetaObject::Connection m_docConn;
    DocSearcher m_searcher{SearchQuery{}};
    std::optional<SearchMatch> m_current;
    qsizetype m_anchorFrom = 0; // Obsidian's search cursor positions: the next search starts at
    qsizetype m_anchorTo = 0;   // m_anchorTo, the previous one ends at m_anchorFrom
    QList<SearchMatch> m_highlights; // sorted, non-overlapping
    int m_layer = -1;
};

} // namespace oc
