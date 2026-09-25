#include "ui/views/media/pdf_search.h"

#include <QPdfLink>
#include <QPdfSearchModel>

namespace oc::media {

namespace {
bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == u'_' || c.category() == QChar::Mark_NonSpacing; }
} // namespace

PdfSearch::PdfSearch(QObject* parent)
    : QObject(parent)
    , m_model(new QPdfSearchModel(this))
{
    m_rebuild.setSingleShot(true);
    m_rebuild.setInterval(0);
    connect(&m_rebuild, &QTimer::timeout, this, &PdfSearch::rebuild);
    // QPdfSearchModel searches page by page on a timer; when nothing arrived for a while it is done.
    m_settle.setSingleShot(true);
    m_settle.setInterval(120);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        m_pending = false;
        rebuild();
    });
    auto changed = [this] {
        m_rebuild.start();
        if (m_pending)
            m_settle.start();
    };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, changed);
    connect(m_model, &QAbstractItemModel::modelReset, this, changed);
    connect(m_model, &QPdfSearchModel::countChanged, this, changed);
}

PdfSearch::~PdfSearch() = default;

void PdfSearch::setFile(std::shared_ptr<PdfFile> file)
{
    m_file = std::move(file);
    m_model->setDocument(m_file ? m_file->document() : nullptr);
    const QString q = m_query;
    m_query.clear();
    setQuery(q, m_fromPage);
}

void PdfSearch::setQuery(const QString& query, int fromPage)
{
    m_fromPage = fromPage;
    if (query == m_query && !m_matches.isEmpty())
        return;
    m_query = query;
    m_matches.clear();
    m_current = -1;
    m_currentChosen = false;
    m_pending = !query.isEmpty() && m_file && m_file->isLoaded();
    m_model->setSearchString(m_pending ? query : QString());
    if (m_pending)
        m_settle.start();
    emit resultsChanged();
    emit currentChanged(m_current);
}

void PdfSearch::setOptions(const Options& options)
{
    const bool refilter = options.caseSensitive != m_options.caseSensitive || options.entireWord != m_options.entireWord;
    m_options = options;
    if (refilter) {
        m_currentChosen = false;
        rebuild();
    } else {
        emit resultsChanged();
    }
}

void PdfSearch::rebuild()
{
    const int oldCurrent = m_current;
    const int oldPage = oldCurrent >= 0 && oldCurrent < m_matches.size() ? m_matches[oldCurrent].page : -1;
    const int oldIndex = oldCurrent >= 0 && oldCurrent < m_matches.size() ? m_matches[oldCurrent].index : -1;
    m_matches.clear();
    const int len = int(m_query.size());
    if (m_file && len > 0) {
        // The model's results come page by page in text order, but QtPdf reports no usable index on
        // the page: pair them with the case-insensitive occurrences in the page text (same order).
        const int n = m_model->rowCount({});
        for (int i = 0; i < n;) {
            const int page = m_model->data(m_model->index(i), int(QPdfSearchModel::Role::Page)).toInt();
            int end = i;
            while (end < n && m_model->data(m_model->index(end), int(QPdfSearchModel::Role::Page)).toInt() == page)
                ++end;
            const QString& text = m_file->pageText(page).text;
            QList<int> at;
            for (qsizetype k = text.indexOf(m_query, 0, Qt::CaseInsensitive); k >= 0; k = text.indexOf(m_query, k + len, Qt::CaseInsensitive))
                at.append(int(k));
            const bool paired = at.size() == end - i;
            for (int r = i; r < end; ++r) {
                const int idx = paired ? at[r - i] : -1;
                if (idx >= 0 && m_options.caseSensitive && QStringView(text).mid(idx, len) != m_query)
                    continue;
                if (idx >= 0 && m_options.entireWord
                    && ((idx > 0 && isWordChar(text.at(idx - 1)) && isWordChar(m_query.front()))
                        || (idx + len < text.size() && isWordChar(text.at(idx + len)) && isWordChar(m_query.back()))))
                    continue;
                m_matches.append(Match{page, idx, m_model->resultAtIndex(r).rectangles()});
            }
            i = end;
        }
    }
    // keep the current match; else the first one on or after the page the search started from
    m_current = -1;
    for (qsizetype i = 0; i < m_matches.size() && oldPage >= 0 && m_currentChosen; ++i)
        if (m_matches[i].page == oldPage && m_matches[i].index == oldIndex)
            m_current = int(i);
    if (m_current < 0 && !m_matches.isEmpty()) {
        for (qsizetype i = 0; i < m_matches.size(); ++i) {
            if (m_matches[i].page >= m_fromPage) {
                m_current = int(i);
                break;
            }
        }
        if (m_current < 0 && !m_pending)
            m_current = 0; // wrapped: every match is before the starting page
        m_currentChosen = m_current >= 0;
    }
    emit resultsChanged();
    if (m_current != oldCurrent || (m_current >= 0 && oldPage < 0))
        emit currentChanged(m_current);
}

void PdfSearch::next()
{
    if (m_matches.isEmpty())
        return;
    m_current = m_current < 0 ? 0 : int((m_current + 1) % m_matches.size());
    m_currentChosen = true;
    emit currentChanged(m_current);
}

void PdfSearch::previous()
{
    if (m_matches.isEmpty())
        return;
    m_current = m_current <= 0 ? int(m_matches.size()) - 1 : m_current - 1;
    m_currentChosen = true;
    emit currentChanged(m_current);
}

QList<PdfPagesView::Highlight> PdfSearch::highlights() const
{
    QList<PdfPagesView::Highlight> out;
    out.reserve(m_matches.size());
    for (const Match& m : m_matches)
        out.append(PdfPagesView::Highlight{m.page, m.rects});
    return out;
}

QString PdfSearch::countText() const
{
    const int total = count();
    if (total == 0)
        return {};
    const int current = m_current + 1;
    if (total > 1000)
        return tr("%1 of over %2 matches").arg(current).arg(1000);
    if (total > 1)
        return tr("%1 of %2 matches").arg(current).arg(total);
    return tr("%1 of %2 match").arg(current).arg(total);
}

} // namespace oc::media
