#include "ui/navigation/search_view.h"

#include "core/metadata/metadata_cache.h"
#include "core/search/search_snippet.h"
#include "core/util/busy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/navigation/navigation_util.h"
#include "ui/navigation/search_results_list.h"
#include "ui/navigation/search_view_widgets.h"
#include "ui/navigation/vault_search_source.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QClipboard>
#include <QCollator>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace oc {

namespace {

// .search-results-info: count and sort dropdown, padding 0 12px 8px, 1px bottom border.
class ResultsInfoBar : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(QRect(0, height() - 1, width(), 1), currentTheme()->color(QStringLiteral("--background-modifier-border")));
    }
};

// .search-params .setting-item.mod-toggle: name (--font-ui-small) and the toggle.
QWidget* toggleRow(QWidget* parent, const QString& label, ToggleSwitch** out)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto* name = new QLabel(label, row);
    name->setObjectName(QStringLiteral("ocSearchParamName"));
    h->addWidget(name, 1);
    auto* toggle = new ToggleSwitch(row);
    toggle->setFocusPolicy(Qt::NoFocus);
    toggle->setAccessibleName(label);
    h->addWidget(toggle, 0, Qt::AlignVCenter);
    *out = toggle;
    return row;
}

const QCollator& nameCollator()
{
    static const QCollator c = [] {
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        return col;
    }();
    return c;
}

QString titleFor(const QString& path)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) && name.size() > 3)
        return name.left(name.size() - 3);
    return name;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// sort orders
// ---------------------------------------------------------------------------------------------

QString SearchView::sortOrderKey(SortOrder order)
{
    switch (order) {
    case SortOrder::Alphabetical: return QStringLiteral("alphabetical");
    case SortOrder::AlphabeticalReverse: return QStringLiteral("alphabeticalReverse");
    case SortOrder::ByModifiedTime: return QStringLiteral("byModifiedTime");
    case SortOrder::ByModifiedTimeReverse: return QStringLiteral("byModifiedTimeReverse");
    case SortOrder::ByCreatedTime: return QStringLiteral("byCreatedTime");
    case SortOrder::ByCreatedTimeReverse: return QStringLiteral("byCreatedTimeReverse");
    }
    return QStringLiteral("alphabetical");
}

SearchView::SortOrder SearchView::sortOrderFromKey(const QString& key)
{
    for (SortOrder o : {SortOrder::Alphabetical, SortOrder::AlphabeticalReverse, SortOrder::ByModifiedTime, SortOrder::ByModifiedTimeReverse,
                        SortOrder::ByCreatedTime, SortOrder::ByCreatedTimeReverse})
        if (sortOrderKey(o) == key)
            return o;
    return SortOrder::Alphabetical;
}

QString SearchView::sortOrderLabel(SortOrder order)
{
    switch (order) {
    case SortOrder::Alphabetical: return tr("File name (A to Z)");
    case SortOrder::AlphabeticalReverse: return tr("File name (Z to A)");
    case SortOrder::ByModifiedTime: return tr("Modified time (new to old)");
    case SortOrder::ByModifiedTimeReverse: return tr("Modified time (old to new)");
    case SortOrder::ByCreatedTime: return tr("Created time (new to old)");
    case SortOrder::ByCreatedTimeReverse: return tr("Created time (old to new)");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------------------------

SearchView::SearchView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
{
    setObjectName(QStringLiteral("ocSearchView"));
    m_source = VaultSearchSource::forSession(session());

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // .search-row: margin 12px 12px 8px, gap 4px
    auto* row = new QWidget(this);
    auto* rh = new QHBoxLayout(row);
    rh->setContentsMargins(12, 12, 12, 8);
    rh->setSpacing(4);
    m_input = new GlobalSearchInput(row);
    m_input->setPlaceholderText(tr("Search..."));
    rh->addWidget(m_input, 1);
    auto* settings = new ActiveIconButton(QStringLiteral("sliders-horizontal"), tr("Search settings"), row);
    m_settingsButton = settings;
    rh->addWidget(settings, 0, Qt::AlignVCenter);
    v->addWidget(row);

    // .search-params: margin 4px 16px, gap 8px
    m_params = new QWidget(this);
    m_params->setObjectName(QStringLiteral("ocSearchParams"));
    auto* pv = new QVBoxLayout(m_params);
    pv->setContentsMargins(16, 4, 16, 4);
    pv->setSpacing(8);
    pv->addWidget(toggleRow(m_params, tr("Explain search terms"), &m_explainToggle));
    pv->addWidget(toggleRow(m_params, tr("Collapse results"), &m_collapseToggle));
    pv->addWidget(toggleRow(m_params, tr("Show more context"), &m_contextToggle));
    m_params->hide();
    v->addWidget(m_params);

    m_explainPanel = new ExplainPanel(this);
    m_explainPanel->hide();
    v->addWidget(m_explainPanel);

    auto* info = new ResultsInfoBar(this);
    m_info = info;
    info->setObjectName(QStringLiteral("ocSearchResultsInfo"));
    auto* ih = new QHBoxLayout(info);
    ih->setContentsMargins(12, 0, 12, 8);
    ih->setSpacing(8);
    m_count = new QLabel(info);
    m_count->setObjectName(QStringLiteral("ocSearchResultCount"));
    ih->addWidget(m_count, 0, Qt::AlignVCenter);
    ih->addStretch(1);
    m_sort = new SortDropdown(info);
    QStringList labels;
    for (SortOrder o : {SortOrder::Alphabetical, SortOrder::AlphabeticalReverse, SortOrder::ByModifiedTime, SortOrder::ByModifiedTimeReverse,
                        SortOrder::ByCreatedTime, SortOrder::ByCreatedTimeReverse})
        labels << sortOrderLabel(o);
    m_sort->setOptions(labels);
    m_sort->setCurrentIndex(int(m_sortOrder));
    ih->addWidget(m_sort, 0, Qt::AlignVCenter);
    info->hide();
    v->addWidget(info);

    m_results = new SearchResultsList(this);
    m_results->setSnippetLimit(kMaxSnippets);
    m_results->setPreparer([this](SearchResultGroup& g) { buildSnippets(g); });
    v->addWidget(m_results, 1);

    auto applyFonts = [this] {
        const ThemePtr t = currentTheme();
        QFont small = t->interfaceFont(12);
        small.setFeature(QFont::Tag("tnum"), 1);
        m_count->setFont(small);
        QPalette pal = m_count->palette();
        pal.setColor(QPalette::WindowText, t->color(QStringLiteral("--text-muted")));
        m_count->setPalette(pal);
        for (QLabel* name : m_params->findChildren<QLabel*>(QStringLiteral("ocSearchParamName"))) {
            name->setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13)));
            QPalette np = name->palette();
            np.setColor(QPalette::WindowText, t->color(QStringLiteral("--text-normal")));
            name->setPalette(np);
        }
    };
    applyFonts();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, applyFonts);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { startSearch(false); });
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(kRefreshDebounceMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &SearchView::applyPendingChanges);

    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_applyingState)
            return;
        emitStateChanged();
        if (text.trimmed().isEmpty())
            startSearch(false); // clearing is immediate
        else
            m_debounce.start();
    });
    connect(m_input, &QLineEdit::returnPressed, this, [this] { startSearch(false); });
    m_input->installEventFilter(this);
    connect(m_results, &SearchResultsList::exitTop, this, [this] { focusInput(false); });
    connect(m_input->matchCaseButton(), &QAbstractButton::toggled, this, [this](bool on) { setMatchCase(on); });
    connect(settings, &QAbstractButton::toggled, this, [this](bool on) { setSettingsVisible(on); });
    connect(m_explainToggle, &QAbstractButton::toggled, this, [this](bool on) { setExplainSearch(on); });
    connect(m_collapseToggle, &QAbstractButton::toggled, this, [this](bool on) { setCollapseAll(on); });
    connect(m_contextToggle, &QAbstractButton::toggled, this, [this](bool on) { setExtraContext(on); });
    for (ToggleSwitch* t : {m_explainToggle, m_collapseToggle, m_contextToggle}) {
        // a click on the setting's name toggles as well
        QWidget* row = t->parentWidget();
        auto* name = row->findChild<QLabel*>(QStringLiteral("ocSearchParamName"));
        name->setAttribute(Qt::WA_Hover);
        name->installEventFilter(this);
        name->setProperty("ocToggle", QVariant::fromValue<QObject*>(t));
    }
    connect(m_sort, &SortDropdown::activated, this, [this](int i) { setSortOrder(SortOrder(i)); });

    connect(m_results, &SearchResultsList::titleClicked, this, [this](int g, Qt::KeyboardModifiers mods) { openResult(g, mods); });
    connect(m_results, &SearchResultsList::snippetClicked, this,
            [this](int g, int s, Qt::KeyboardModifiers mods) { openMatch(g, s, mods); });
    connect(m_results, &SearchResultsList::rowHovered, this, [this](int g, const QRect& rect) {
        // Obsidian's page preview for search results (source "search": with Mod by default)
        auto* w = MainWindow::of(window());
        if (!w)
            return;
        QWidget* vp = m_results->viewport();
        if (g < 0 || g >= resultCount()) {
            PagePreview::of(w)->onLinkUnhover(vp);
            return;
        }
        PagePreview::of(w)->onLinkHover(QStringLiteral("search"), displayPath(m_results->groups()[g].path), QString(),
                                        QRect(vp->mapToGlobal(rect.topLeft()), rect.size()), vp);
    });
    connect(m_results, &SearchResultsList::collapseToggled, this, [this](int g) {
        auto& groups = m_results->mutableGroups();
        if (g < 0 || g >= groups.size())
            return;
        groups[g].collapsed = !groups[g].collapsed;
        if (groups[g].collapsed != m_collapseAll)
            m_collapsedPaths.insert(groups[g].path);
        else
            m_collapsedPaths.remove(groups[g].path);
        m_results->groupsChanged();
    });

    if (m_source) {
        connect(m_source, &VaultSearchSource::pathChanged, this, &SearchView::onPathChanged);
        connect(m_source, &VaultSearchSource::indexReady, this, [this] {
            if (!m_query.isEmpty())
                startSearch(true);
        });
    }
    updateChrome();
}

SearchView::~SearchView()
{
    m_generation += 1;
    m_updateGeneration += 1;
    for (search::SearchEngine* engine : {&m_engine, &m_updateEngine})
        if (auto job = engine->currentJob()) {
            job->cancel();
            job->waitForFinished();
        }
    setSearching(false);
    setUpdating(false);
}

bool SearchView::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::MouseButtonRelease) {
        if (auto* toggle = qobject_cast<ToggleSwitch*>(obj->property("ocToggle").value<QObject*>())) {
            toggle->toggle();
            return true;
        }
    }
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Down && ke->modifiers() == Qt::NoModifier && resultCount() > 0) {
            m_results->focusFirstRow();
            return true;
        }
    }
    return View::eventFilter(obj, ev);
}

void SearchView::resizeEvent(QResizeEvent* e)
{
    View::resizeEvent(e);
    // .search-results-info .dropdown { max-width: 60% }
    m_sort->setMaximumWidth(std::max(40, int(width() * 0.6)));
}

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------

QJsonObject SearchView::state() const
{
    return QJsonObject{{QStringLiteral("query"), m_input->text()},
                       {QStringLiteral("matchingCase"), m_matchCase},
                       {QStringLiteral("explainSearch"), m_explain},
                       {QStringLiteral("collapseAll"), m_collapseAll},
                       {QStringLiteral("extraContext"), m_extraContext},
                       {QStringLiteral("sortOrder"), sortOrderKey(m_sortOrder)}};
}

void SearchView::setState(const QJsonObject& state)
{
    m_applyingState = true;
    const QString query = state.value(QStringLiteral("query")).toString();
    if (m_input->text() != query)
        m_input->setText(query);
    setMatchCase(state.value(QStringLiteral("matchingCase")).toBool());
    setExplainSearch(state.value(QStringLiteral("explainSearch")).toBool());
    setCollapseAll(state.value(QStringLiteral("collapseAll")).toBool());
    setExtraContext(state.value(QStringLiteral("extraContext")).toBool());
    setSortOrder(sortOrderFromKey(state.value(QStringLiteral("sortOrder")).toString()));
    m_applyingState = false;
    startSearch(false);
}

void SearchView::emitStateChanged()
{
    if (!m_applyingState)
        emit stateChanged();
}

QString SearchView::query() const { return m_input->text(); }

void SearchView::setQuery(const QString& query)
{
    m_applyingState = true;
    m_input->setText(query);
    m_applyingState = false;
    emitStateChanged();
    startSearch(false);
}

void SearchView::focusInput(bool selectAll)
{
    m_input->setFocus(Qt::OtherFocusReason);
    if (selectAll)
        m_input->selectAll();
}

void SearchView::setMatchCase(bool on)
{
    if (m_input->matchCaseButton()->isChecked() != on) {
        const QSignalBlocker b(m_input->matchCaseButton());
        m_input->matchCaseButton()->setChecked(on);
        m_input->matchCaseButton()->update();
    }
    if (m_matchCase == on)
        return;
    m_matchCase = on;
    emitStateChanged();
    if (!m_applyingState)
        startSearch(false);
}

void SearchView::setExplainSearch(bool on)
{
    if (m_explainToggle->isChecked() != on) {
        const QSignalBlocker b(m_explainToggle);
        m_explainToggle->setChecked(on);
    }
    if (m_explain == on)
        return;
    m_explain = on;
    emitStateChanged();
    updateChrome();
}

void SearchView::setCollapseAll(bool on)
{
    if (m_collapseToggle->isChecked() != on) {
        const QSignalBlocker b(m_collapseToggle);
        m_collapseToggle->setChecked(on);
    }
    if (m_collapseAll == on)
        return;
    m_collapseAll = on;
    m_collapsedPaths.clear();
    for (SearchResultGroup& g : m_results->mutableGroups())
        g.collapsed = on;
    for (SearchResultGroup& g : m_pending)
        g.collapsed = on;
    m_results->groupsChanged();
    emitStateChanged();
}

void SearchView::setExtraContext(bool on)
{
    if (m_contextToggle->isChecked() != on) {
        const QSignalBlocker b(m_contextToggle);
        m_contextToggle->setChecked(on);
    }
    if (m_extraContext == on)
        return;
    m_extraContext = on;
    for (SearchResultGroup& g : m_results->mutableGroups())
        g.prepared = false; // rebuilt with the new context when laid out
    for (SearchResultGroup& g : m_pending)
        g.prepared = false;
    m_results->groupsChanged();
    emitStateChanged();
    emit resultsChanged();
}

void SearchView::setSortOrder(SortOrder order)
{
    m_sort->setCurrentIndex(int(order));
    if (m_sortOrder == order)
        return;
    m_sortOrder = order;
    resort();
    emitStateChanged();
    emit resultsChanged();
}

bool SearchView::settingsVisible() const { return !m_params->isHidden(); }

void SearchView::setSettingsVisible(bool visible)
{
    if (m_settingsButton->isChecked() != visible) {
        const QSignalBlocker b(m_settingsButton);
        m_settingsButton->setChecked(visible);
        m_settingsButton->update();
    }
    m_params->setVisible(visible);
}

void SearchView::populatePaneMenu(QMenu* menu)
{
    QAction* copy = menu->addAction(IconProvider::instance().icon(QStringLiteral("copy"), 16), tr("Copy search results"), this, &SearchView::copyResults);
    copy->setEnabled(resultCount() > 0);
}

// ---------------------------------------------------------------------------------------------
// searching
// ---------------------------------------------------------------------------------------------

void SearchView::setSearching(bool on)
{
    m_searching = on;
    if (on && !m_busy) {
        Busy::begin();
        m_busy = true;
    } else if (!on && m_busy) {
        Busy::end();
        m_busy = false;
    }
    m_results->setLoading(on);
}

void SearchView::cancelSearch()
{
    ++m_generation;
    if (auto job = m_engine.currentJob())
        job->cancel();
    setSearching(false);
}

void SearchView::startSearch(bool keepResults)
{
    m_debounce.stop();
    m_refreshTimer.stop();
    m_changed.clear();
    cancelUpdate();
    cancelSearch();
    m_query = search::parseSearchQuery(m_input->text(), search::SearchOptions{m_matchCase});
    m_refreshing = keepResults && !m_query.isEmpty() && resultCount() > 0;
    m_pending.clear();
    if (!m_refreshing) {
        m_results->clear();
        if (!keepResults)
            m_collapsedPaths.clear();
    }
    if (m_query.isEmpty()) {
        m_snapshot.reset();
        updateChrome();
        emit resultsChanged();
        emit searchFinished();
        return;
    }
    m_snapshot = m_source ? m_source->snapshot(m_query.canMatchNonNotes()) : std::make_shared<SearchSnapshot>();
    const quint64 gen = ++m_generation;
    setSearching(true);
    updateChrome();
    emit searchStarted();
    search::SearchCallbacks cb;
    cb.context = this;
    cb.onResults = [this, gen](QList<search::SearchResult> batch) { onBatch(batch, gen); };
    cb.onFinished = [this, gen](const search::SearchStats&) { onFinished(gen); };
    m_engine.search(m_query, m_snapshot->source(), std::move(cb));
}

void SearchView::onBatch(const QList<search::SearchResult>& batch, quint64 generation)
{
    if (generation != m_generation || !m_snapshot)
        return;
    QList<SearchResultGroup> fresh;
    fresh.reserve(batch.size());
    for (const search::SearchResult& r : batch) {
        if (r.index < 0 || r.index >= m_snapshot->count())
            continue;
        const SearchSnapshot::Entry& e = m_snapshot->entry(r.index);
        const bool collapsed = m_collapsedPaths.contains(e.path) ? !m_collapseAll : m_collapseAll;
        fresh.append(makeGroup(r, m_snapshot->document(r.index), e.mtimeMs, e.ctimeMs, collapsed));
    }
    if (m_refreshing)
        m_pending.append(std::move(fresh));
    else
        mergeSorted(std::move(fresh));
    if (!m_refreshing) {
        m_results->groupsChanged();
        updateChrome();
        emit resultsChanged();
    }
}

void SearchView::onFinished(quint64 generation)
{
    if (generation != m_generation)
        return;
    if (m_refreshing) {
        std::stable_sort(m_pending.begin(), m_pending.end(), [this](const SearchResultGroup& a, const SearchResultGroup& b) { return lessThan(a, b); });
        m_results->mutableGroups() = std::move(m_pending);
        m_pending.clear();
        m_refreshing = false;
        m_results->groupsChanged();
    }
    setSearching(false);
    updateChrome();
    emit resultsChanged();
    emit searchFinished();
}

SearchResultGroup SearchView::makeGroup(const search::SearchResult& r, const std::shared_ptr<const search::SearchDocument>& doc, qint64 mtimeMs,
                                        qint64 ctimeMs, bool collapsed) const
{
    SearchResultGroup g;
    g.path = r.path;
    g.title = titleFor(r.path);
    g.sortKey = nameCollator().sortKey(g.title);
    g.mtimeMs = mtimeMs;
    g.ctimeMs = ctimeMs;
    g.doc = doc;
    g.contentMatches = r.contentMatches;
    g.collapsed = collapsed;
    // file name hits (ranges in the path) shown on the title
    const int nameStart = int(r.path.lastIndexOf(QLatin1Char('/')) + 1);
    const int titleEnd = nameStart + int(g.title.size());
    for (const MatchRange& m : r.fileMatches) {
        const int a = std::max(m.from, nameStart), b = std::min(m.to, titleEnd);
        if (b > a)
            g.titleMatches.append({a - nameStart, b - nameStart});
    }
    return g; // snippets are built when the group is laid out (see the results list's preparer)
}

void SearchView::buildSnippets(SearchResultGroup& g) const
{
    g.snippets.clear();
    g.moreMatches = 0;
    g.layoutWidth = -1;
    if (!g.doc || g.contentMatches.isEmpty())
        return;
    search::SnippetOptions opts;
    opts.contextChars = kContextChars;
    opts.contextLines = m_extraContext ? kExtraContextLines : 0;
    opts.maxSnippets = kMaxSnippets;
    g.snippets = search::buildSnippets(g.doc->content, g.contentMatches, opts);
    qsizetype covered = 0;
    for (const search::Snippet& s : std::as_const(g.snippets))
        covered += s.matches.size();
    g.moreMatches = int(std::max<qsizetype>(0, g.contentMatches.size() - covered));
}

bool SearchView::lessThan(const SearchResultGroup& a, const SearchResultGroup& b) const
{
    auto byName = [](const SearchResultGroup& x, const SearchResultGroup& y) {
        const int c = x.sortKey && y.sortKey ? x.sortKey->compare(*y.sortKey) : nameCollator().compare(x.title, y.title);
        return c != 0 ? c < 0 : x.path < y.path;
    };
    switch (m_sortOrder) {
    case SortOrder::Alphabetical: return byName(a, b);
    case SortOrder::AlphabeticalReverse: return byName(b, a);
    case SortOrder::ByModifiedTime: return a.mtimeMs != b.mtimeMs ? a.mtimeMs > b.mtimeMs : byName(a, b);
    case SortOrder::ByModifiedTimeReverse: return a.mtimeMs != b.mtimeMs ? a.mtimeMs < b.mtimeMs : byName(a, b);
    case SortOrder::ByCreatedTime: return a.ctimeMs != b.ctimeMs ? a.ctimeMs > b.ctimeMs : byName(a, b);
    case SortOrder::ByCreatedTimeReverse: return a.ctimeMs != b.ctimeMs ? a.ctimeMs < b.ctimeMs : byName(a, b);
    }
    return byName(a, b);
}

void SearchView::insertSorted(SearchResultGroup g)
{
    auto& groups = m_results->mutableGroups();
    const auto it = std::upper_bound(groups.begin(), groups.end(), g, [this](const SearchResultGroup& x, const SearchResultGroup& y) { return lessThan(x, y); });
    groups.insert(it, std::move(g));
}

void SearchView::mergeSorted(QList<SearchResultGroup> fresh)
{
    // A streamed batch: sort it, then one linear merge (instead of shifting the list for every result).
    const auto cmp = [this](const SearchResultGroup& a, const SearchResultGroup& b) { return lessThan(a, b); };
    std::stable_sort(fresh.begin(), fresh.end(), cmp);
    auto& groups = m_results->mutableGroups();
    QList<SearchResultGroup> merged;
    merged.reserve(groups.size() + fresh.size());
    std::merge(std::make_move_iterator(groups.begin()), std::make_move_iterator(groups.end()), std::make_move_iterator(fresh.begin()),
               std::make_move_iterator(fresh.end()), std::back_inserter(merged), cmp);
    groups = std::move(merged);
}

void SearchView::resort()
{
    auto& groups = m_results->mutableGroups();
    std::stable_sort(groups.begin(), groups.end(), [this](const SearchResultGroup& a, const SearchResultGroup& b) { return lessThan(a, b); });
    m_results->groupsChanged();
}

// ---------------------------------------------------------------------------------------------
// live updates
// ---------------------------------------------------------------------------------------------

void SearchView::onPathChanged(const QString& path)
{
    if (m_query.isEmpty() || (m_source && !m_source->isIndexReady()))
        return; // nothing shown, or the index-ready re-run will cover it
    m_changed.insert(path);
    m_refreshTimer.start();
}

void SearchView::applyPendingChanges()
{
    const QSet<QString> paths = std::exchange(m_changed, {});
    if (m_query.isEmpty() || paths.isEmpty())
        return;
    Vault* vault = session() ? session()->vault() : nullptr;
    constexpr int kIncrementalLimit = 50;
    if (m_searching || !vault || !m_source || paths.contains(QString()) || paths.size() > kIncrementalLimit) {
        startSearch(true);
        return;
    }
    // Files still being re-evaluated by an unfinished update are evaluated again with the new ones.
    QStringList evaluate(m_updatePaths.cbegin(), m_updatePaths.cend());
    QStringList removed;
    for (const QString& p : paths) {
        TAbstractFile* item = vault->item(p);
        if (!item || item->isFolder())
            removed << p;
        if (item && item->isFolder()) {
            const QString prefix = p + QLatin1Char('/');
            for (TFile* f : vault->files())
                if (f->path().startsWith(prefix))
                    evaluate << f->path();
        } else if (item) {
            evaluate << p;
        }
    }
    evaluate.removeDuplicates(); // a new folder and its new file are both reported
    if (evaluate.size() > kIncrementalLimit) {
        startSearch(true);
        return;
    }
    if (!removed.isEmpty()) {
        // deleted or moved away: gone at once (what is re-evaluated stays until its result is in)
        const QSet<QString> keep(evaluate.cbegin(), evaluate.cend());
        const qsizetype n = m_results->mutableGroups().removeIf([&](const SearchResultGroup& g) {
            if (keep.contains(g.path))
                return false;
            for (const QString& r : std::as_const(removed))
                if (g.path == r || g.path.startsWith(r + QLatin1Char('/')))
                    return true;
            return false;
        });
        if (n > 0) {
            m_results->groupsChanged();
            updateChrome();
            emit resultsChanged();
        }
    }
    if (evaluate.isEmpty())
        return;
    startUpdate(evaluate);
}

void SearchView::startUpdate(const QStringList& paths)
{
    cancelUpdate();
    m_updatePaths = QSet<QString>(paths.cbegin(), paths.cend());
    m_updateSnapshot = m_source->snapshot(paths, m_query.canMatchNonNotes());
    const quint64 gen = ++m_updateGeneration;
    setUpdating(true);
    search::SearchCallbacks cb;
    cb.context = this;
    cb.onResults = [this, gen](QList<search::SearchResult> batch) {
        if (gen == m_updateGeneration)
            m_updateResults.append(batch);
    };
    cb.onFinished = [this, gen](const search::SearchStats&) { finishUpdate(gen); };
    m_updateEngine.search(m_query, m_updateSnapshot->source(), std::move(cb));
}

void SearchView::finishUpdate(quint64 generation)
{
    if (generation != m_updateGeneration || !m_updateSnapshot)
        return;
    auto& groups = m_results->mutableGroups();
    groups.removeIf([this](const SearchResultGroup& g) { return m_updatePaths.contains(g.path); });
    for (const search::SearchResult& r : std::as_const(m_updateResults)) {
        if (r.index < 0 || r.index >= m_updateSnapshot->count())
            continue;
        const SearchSnapshot::Entry& e = m_updateSnapshot->entry(r.index);
        const bool collapsed = m_collapsedPaths.contains(e.path) ? !m_collapseAll : m_collapseAll;
        insertSorted(makeGroup(r, m_updateSnapshot->document(r.index), e.mtimeMs, e.ctimeMs, collapsed));
    }
    m_updatePaths.clear();
    m_updateResults.clear();
    m_updateSnapshot.reset();
    setUpdating(false);
    m_results->groupsChanged();
    updateChrome();
    emit resultsChanged();
}

void SearchView::cancelUpdate()
{
    ++m_updateGeneration;
    if (auto job = m_updateEngine.currentJob())
        job->cancel();
    m_updatePaths.clear();
    m_updateResults.clear();
    m_updateSnapshot.reset();
    setUpdating(false);
}

void SearchView::setUpdating(bool on)
{
    if (on == m_updating)
        return;
    m_updating = on;
    if (on)
        Busy::begin();
    else
        Busy::end();
}

// ---------------------------------------------------------------------------------------------
// chrome
// ---------------------------------------------------------------------------------------------

void SearchView::updateChrome()
{
    const bool hasQuery = !m_query.isEmpty();
    const QString explain = hasQuery && m_explain ? search::explainSearchQuery(m_query) : QString();
    m_explainPanel->setText(explain);
    m_explainPanel->setVisible(!explain.isEmpty());
    m_info->setVisible(hasQuery);
    m_count->setText(resultCountText());
    m_results->setEmptyText(hasQuery && !m_searching && resultCount() == 0 ? tr("No matches found.") : QString());
}

int SearchView::resultCount() const { return int(m_results->groups().size()); }

QStringList SearchView::resultPaths() const
{
    QStringList out;
    for (const SearchResultGroup& g : m_results->groups())
        out << g.path;
    return out;
}

const SearchResultGroup* SearchView::result(const QString& path)
{
    const auto& groups = m_results->groups();
    for (int i = 0; i < groups.size(); ++i)
        if (groups[i].path == path)
            return &m_results->preparedGroup(i);
    return nullptr;
}

QString SearchView::resultCountText() const
{
    const int n = resultCount();
    return n == 1 ? tr("%1 result").arg(n) : tr("%1 results").arg(n);
}

QString SearchView::explanation() const { return m_explainPanel->isVisible() ? m_explainPanel->text() : QString(); }

// ---------------------------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------------------------

QString SearchView::resultsText(bool showPath, LinkStyle link, ListPrefix prefix) const
{
    QStringList lines;
    int n = 0;
    MetadataCache* mc = session() ? session()->metadata() : nullptr;
    for (const SearchResultGroup& g : m_results->groups()) {
        ++n;
        const QString name = showPath ? displayPath(g.path) : g.title;
        QString item;
        switch (link) {
        case LinkStyle::None:
            item = name;
            break;
        case LinkStyle::Wikilink:
            // the shortest unambiguous link text (the full path with "Show path")
            item = QStringLiteral("[[%1]]").arg(showPath || !mc ? name : mc->resolver().linkText(g.path, QString(), LinkResolver::LinkFormat::Shortest, true));
            break;
        case LinkStyle::Markdown:
            item = QStringLiteral("[%1](%2)").arg(name, QString::fromUtf8(QUrl::toPercentEncoding(g.path, "/")));
            break;
        }
        switch (prefix) {
        case ListPrefix::None: break;
        case ListPrefix::Dash: item.prepend(QStringLiteral("- ")); break;
        case ListPrefix::Asterisk: item.prepend(QStringLiteral("* ")); break;
        case ListPrefix::Numbered: item.prepend(QStringLiteral("%1. ").arg(n)); break;
        }
        lines << item;
    }
    return lines.join(QLatin1Char('\n'));
}

void SearchView::copyResults()
{
    QGuiApplication::clipboard()->setText(resultsText());
    if (auto* w = MainWindow::of(window()); w && w->notices())
        w->notices()->show(tr("Results copied to your clipboard."));
}

QJsonObject SearchView::matchEState(int group, int snippet)
{
    if (group < 0 || group >= m_results->groups().size())
        return {};
    const SearchResultGroup& g = m_results->preparedGroup(group);
    if (!g.doc || snippet < 0 || snippet >= g.snippets.size())
        return {};
    const search::Snippet& s = g.snippets[snippet];
    QJsonArray matches;
    for (const MatchRange& m : s.matches)
        matches.append(QJsonArray{s.from + m.from, s.from + m.to});
    return QJsonObject{{QStringLiteral("match"), QJsonObject{{QStringLiteral("content"), g.doc->content}, {QStringLiteral("matches"), matches}}}};
}

void SearchView::openResult(int group, Qt::KeyboardModifiers modifiers)
{
    const auto& groups = m_results->groups();
    Workspace* ws = workspace();
    Vault* vault = session() ? session()->vault() : nullptr;
    if (group < 0 || group >= groups.size() || !ws || !vault)
        return;
    if (TFile* f = vault->file(groups[group].path))
        ws->openFile(f, newLeafForModifiers(modifiers));
}

void SearchView::openMatch(int group, int snippet, Qt::KeyboardModifiers modifiers)
{
    const auto& groups = m_results->groups();
    Workspace* ws = workspace();
    Vault* vault = session() ? session()->vault() : nullptr;
    if (group < 0 || group >= groups.size() || !ws || !vault)
        return;
    const QString path = groups[group].path;
    const QJsonObject eState = matchEState(group, snippet);
    if (TFile* f = vault->file(path))
        ws->openFile(f, newLeafForModifiers(modifiers), eState);
}

// ---------------------------------------------------------------------------------------------
// registration / API
// ---------------------------------------------------------------------------------------------

void registerSearchView(ViewRegistry& registry)
{
    registry.registerView(QStringLiteral("search"), [](WorkspaceLeaf* leaf) -> View* { return new SearchView(leaf); });
}

SearchView* openGlobalSearch(VaultSession* session, const QString& query, bool focus)
{
    Workspace* ws = session ? session->workspace() : nullptr;
    if (!ws)
        return nullptr;
    WorkspaceLeaf* leaf = ws->ensureSideLeaf(QStringLiteral("search"), Workspace::Side::Left, true);
    if (!leaf)
        return nullptr;
    ws->revealLeaf(leaf);
    auto* view = qobject_cast<SearchView*>(leaf->view());
    if (!view)
        return nullptr;
    if (!query.isNull())
        view->setQuery(query);
    if (focus)
        view->focusInput(true);
    return view;
}

} // namespace oc
