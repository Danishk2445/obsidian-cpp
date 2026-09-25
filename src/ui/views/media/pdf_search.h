#pragma once

// Find in a PDF (Obsidian's PDF.js find bar): QPdfSearchModel finds case-insensitive matches page by
// page in the background; matches are filtered for "match case" and "whole words" against the page
// text. The current match starts at the first match on or after the current page and wraps.

#include "ui/views/media/pdf_pages.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

class QPdfSearchModel;

namespace oc::media {

class PdfSearch : public QObject {
    Q_OBJECT
public:
    struct Options {
        bool caseSensitive = false;
        bool entireWord = false;
        bool highlightAll = false;
    };
    struct Match {
        int page = 0;
        int index = 0; // in the page text (-1 when it could not be told)
        QList<QRectF> rects;
    };

    explicit PdfSearch(QObject* parent = nullptr);
    ~PdfSearch() override;

    void setFile(std::shared_ptr<PdfFile> file);
    void setQuery(const QString& query, int fromPage = 0);
    QString query() const { return m_query; }
    void setOptions(const Options& options);
    const Options& options() const { return m_options; }

    const QList<Match>& matches() const { return m_matches; }
    int count() const { return int(m_matches.size()); }
    int current() const { return m_current; } // -1 = none
    bool isPending() const { return m_pending; }
    void next();
    void previous();
    QList<PdfPagesView::Highlight> highlights() const;
    // "3 of 12 matches" (Obsidian's pdf.msgSearchCount), "" when nothing matched.
    QString countText() const;

signals:
    void resultsChanged();
    void currentChanged(int index);

private:
    void rebuild();

    std::shared_ptr<PdfFile> m_file;
    QPdfSearchModel* m_model;
    QString m_query;
    Options m_options;
    QList<Match> m_matches;
    int m_current = -1;
    int m_fromPage = 0;
    bool m_pending = false;
    bool m_currentChosen = false;
    QTimer m_rebuild;
    QTimer m_settle;
};

} // namespace oc::media
