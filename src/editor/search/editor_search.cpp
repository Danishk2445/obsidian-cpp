#include "editor/search/editor_search.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"

#include <algorithm>

namespace oc {

EditorSearch::EditorSearch(EditorView* view, QObject* parent)
    : QObject(parent)
    , m_view(view)
{
    m_layer = view->addDecorationLayer([this](int line, LineDecoration& d) {
        if (m_highlights.isEmpty() || !m_doc)
            return;
        const Text& t = m_doc->text();
        if (line >= t.lineCount())
            return;
        const qsizetype ls = t.lineStart(line), le = t.lineEnd(line);
        auto it = std::lower_bound(m_highlights.cbegin(), m_highlights.cend(), ls,
                                   [](const SearchMatch& m, qsizetype pos) { return m.to <= pos; });
        for (; it != m_highlights.cend() && it->from <= le; ++it) {
            const int f = int(std::max(it->from, ls) - ls), to = int(std::min(it->to, le) - ls);
            if (to > f)
                d.marks.append(Mark{f, to, StyleId::SearchMatch});
        }
    });
}

EditorSearch::~EditorSearch()
{
    disconnect(m_docConn);
    if (m_view)
        m_view->removeDecorationLayer(m_layer);
}

Document* EditorSearch::bindDocument()
{
    Document* doc = m_view ? m_view->document() : nullptr;
    if (doc != m_doc) {
        disconnect(m_docConn);
        m_doc = doc;
        m_current.reset();
        m_highlights.clear();
        if (doc)
            m_docConn = connect(doc, &Document::changed, this, &EditorSearch::onDocChanged);
        if (m_view)
            m_view->refreshDecorations();
    }
    return doc;
}

void EditorSearch::onDocChanged(const DocUpdate& u)
{
    const ChangeDesc desc = u.tr.changes.desc();
    const QList<SearchMatch> before = m_highlights;
    QList<SearchMatch> mapped;
    mapped.reserve(m_highlights.size());
    for (const SearchMatch& m : std::as_const(m_highlights)) {
        const SearchMatch n{desc.mapPos(m.from, 1), desc.mapPos(m.to, -1)};
        if (n.to > n.from)
            mapped.append(n);
    }
    m_highlights = mapped;
    if (m_current) {
        const SearchMatch n{desc.mapPos(m_current->from, 1), desc.mapPos(m_current->to, -1)};
        if (n.to > n.from)
            m_current = n;
        else
            m_current.reset();
    }
    m_anchorFrom = desc.mapPos(m_anchorFrom, -1);
    m_anchorTo = desc.mapPos(m_anchorTo, 1);
    if (!before.isEmpty() || !m_highlights.isEmpty()) {
        m_view->refreshDecorations(); // highlight positions changed with the text
        emit resultsChanged();
    }
}

void EditorSearch::refreshLines(const QList<SearchMatch>& a, const QList<SearchMatch>& b)
{
    if (!m_view || !m_doc)
        return;
    if (a.size() + b.size() > 64) {
        m_view->refreshDecorations();
        return;
    }
    const Text& t = m_doc->text();
    for (const QList<SearchMatch>* list : {&a, &b}) {
        for (const SearchMatch& m : *list) {
            const int l1 = t.lineAt(std::min(m.from, t.length())), l2 = t.lineAt(std::min(m.to, t.length()));
            m_view->refreshDecorations(l1, l2 + 1);
        }
    }
}

void EditorSearch::setHighlights(QList<SearchMatch> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const SearchMatch& x, const SearchMatch& y) { return x.from < y.from; });
    if (ranges == m_highlights)
        return;
    const QList<SearchMatch> old = std::exchange(m_highlights, std::move(ranges));
    refreshLines(old, m_highlights);
}

bool EditorSearch::show(std::optional<SearchMatch> m)
{
    if (m) {
        m_anchorFrom = m->from;
        m_anchorTo = m->to;
        m_current = m;
        setHighlights({*m});
        m_view->unfoldAround(m->from, m->to); // a match inside a fold unfolds it (CodeMirror's search + foldState)
        m_view->scrollToPos(m->from, true);
    } else {
        m_current.reset();
        setHighlights({});
    }
    emit resultsChanged();
    return m.has_value();
}

bool EditorSearch::setQuery(const SearchQuery& query)
{
    Document* doc = bindDocument();
    m_searcher = DocSearcher(query);
    m_current.reset();
    if (!doc || !m_searcher.isValid()) {
        setHighlights({});
        emit resultsChanged();
        return false;
    }
    const SelRange main = m_view->selection().main();
    m_anchorFrom = main.from();
    m_anchorTo = main.to();
    return findNext();
}

bool EditorSearch::findNext()
{
    Document* doc = bindDocument();
    if (!doc || !m_searcher.isValid())
        return false;
    return show(m_searcher.next(doc->text(), m_anchorTo));
}

bool EditorSearch::findPrevious()
{
    Document* doc = bindDocument();
    if (!doc || !m_searcher.isValid())
        return false;
    return show(m_searcher.previous(doc->text(), m_anchorFrom));
}

int EditorSearch::findAll()
{
    Document* doc = bindDocument();
    if (!doc || !m_searcher.isValid())
        return 0;
    QList<SearchMatch> all = m_searcher.all(doc->text());
    if (!all.isEmpty())
        m_view->scrollToPos(all.first().from, true);
    const int n = int(all.size());
    setHighlights(std::move(all));
    emit resultsChanged();
    return n;
}

bool EditorSearch::replaceCurrent(const QString& replacement)
{
    Document* doc = bindDocument();
    if (!doc || !m_current || m_view->isReadOnly())
        return false;
    const Text& t = doc->text();
    const SearchMatch cur = *m_current;
    // Only replace what still matches (the text may have been edited since it was found).
    const int line = t.lineAt(cur.from);
    const qsizetype ls = t.lineStart(line);
    const auto m = m_searcher.matchInLine(t.lineView(line), int(cur.from - ls));
    if (!m || ls + m->first != cur.from || ls + m->second != cur.to) {
        findNext();
        return false;
    }
    Transaction tr;
    tr.changes = ChangeSet::of({{cur.from, cur.to, replacement}}, t.length());
    tr.userEvent = QStringLiteral("input.replace");
    tr.scrollIntoView = false;
    m_view->dispatch(tr); // maps the anchor past the replacement
    findNext();
    return true;
}

int EditorSearch::replaceAll(const QString& replacement)
{
    Document* doc = bindDocument();
    if (!doc || !m_searcher.isValid() || m_view->isReadOnly())
        return 0;
    const QList<SearchMatch> all = m_searcher.all(doc->text());
    if (all.isEmpty())
        return 0;
    QList<ChangeSpec> specs;
    specs.reserve(all.size());
    for (const SearchMatch& m : all)
        specs.push_back({m.from, m.to, replacement});
    Transaction tr;
    tr.changes = ChangeSet::of(specs, doc->text().length());
    tr.userEvent = QStringLiteral("input.replace.all");
    tr.scrollIntoView = false;
    m_view->dispatch(tr);
    m_current.reset();
    setHighlights({});
    emit resultsChanged();
    return int(all.size());
}

std::pair<int, int> EditorSearch::indexAndCount() const
{
    if (!m_doc)
        return {0, 0};
    return m_searcher.indexAndCount(m_doc->text(), m_current);
}

void EditorSearch::selectCurrent()
{
    if (m_view && m_current)
        m_view->setSelection(Selection::single(m_current->from, m_current->to));
}

void EditorSearch::clear()
{
    m_searcher = DocSearcher(SearchQuery{});
    m_current.reset();
    setHighlights({});
    emit resultsChanged();
}

} // namespace oc
