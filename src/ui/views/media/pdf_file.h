#pragma once

// One open PDF shared by every viewer of the file (the "pdf" view, embeds, hover previews): the
// QPdfDocument, page geometry and labels, page text and links (cached), and an asynchronous page
// renderer with an LRU image cache. Rendering runs on one worker thread (pdfium is serialized by
// QtPdf anyway), newest request first, with at most one render in flight; pending renders keep
// oc::Busy non-idle so headless screenshots wait for pages.

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPdfDocument>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QUrl>

#include <memory>
#include <unordered_map>

namespace oc::media {

class PdfFile : public QObject {
    Q_OBJECT
public:
    // Shared per absolute path while anyone holds it (loaded synchronously on first open).
    static std::shared_ptr<PdfFile> open(const QString& absPath);
    ~PdfFile() override;

    const QString& path() const { return m_path; }
    QPdfDocument* document() const { return m_doc.get(); }
    QPdfDocument::Status status() const;
    QPdfDocument::Error error() const { return m_error; }
    bool isLoaded() const { return m_error == QPdfDocument::Error::None && pageCount() > 0; }
    bool needsPassword() const { return m_error == QPdfDocument::Error::IncorrectPassword; }
    void setPassword(const QString& password); // reloads
    void reload();                             // the file changed on disk
    quint64 generation() const { return m_generation; }

    int pageCount() const { return int(m_sizes.size()); }
    QSizeF pagePointSize(int page) const; // PDF points (1/72 in)
    QString pageLabel(int page) const;    // "" when the document has none for that page
    bool hasPageLabels() const { return m_hasLabels; }
    int pageForLabel(const QString& label) const; // -1 if none

    // All text of a page with its glyph boxes (QPdfDocument::getAllText, cached; boxes are glyphs or
    // glyph runs, in points, top-left origin).
    struct PageText {
        QString text;
        QList<QRectF> boxes;
    };
    const PageText& pageText(int page);
    struct Link {
        QRectF rect;        // points, top-left origin
        QUrl url;           // external link, or
        int page = -1;      // internal destination
        QPointF location;   // points on the destination page
    };
    const QList<Link>& links(int page);

    // Rendering. `pixelWidth` is in device pixels; `themed` renders Obsidian's "Adapt to theme"
    // colours (inverted, hue-rotated, on `background`).
    QImage cachedImage(int page, int pixelWidth, bool themed, QRgb background = 0) const;
    QImage anyImage(int page, bool themed) const; // the best cached image at any size (placeholder)
    void requestImage(int page, int pixelWidth, bool themed, QRgb background = 0);
    int pendingRenders() const { return int(m_queue.size()) + (m_inFlight ? 1 : 0); }

    // Page sizes of a PDF without keeping it open (embed layout), cached by path and mtime.
    static QList<QSizeF> pageSizes(const QString& absPath);

signals:
    void rendered(int page);
    void reloaded();

private:
    explicit PdfFile(QString absPath);
    struct Key {
        int page = 0;
        int width = 0;
        bool themed = false;
        QRgb background = 0;
        bool operator==(const Key&) const = default;
    };
    struct Entry {
        Key key;
        QImage image;
        quint64 use = 0;
    };
    void load();
    void pump();
    void finished(Key key, quint64 generation, QImage image);
    void evict();

    QString m_path;
    QString m_password;
    std::shared_ptr<QPdfDocument> m_doc;
    QPdfDocument::Error m_error = QPdfDocument::Error::None;
    quint64 m_generation = 0;
    QList<QSizeF> m_sizes;
    QStringList m_labels;
    bool m_hasLabels = false;
    std::unordered_map<int, PageText> m_text;      // node-stable: callers keep references
    std::unordered_map<int, QList<Link>> m_links;
    QList<Entry> m_cache;
    mutable quint64 m_useCounter = 0;
    QList<Key> m_queue; // newest first
    bool m_inFlight = false;
};

// Obsidian's "Adapt to theme" page colours: PDF.js inverts every colour and rotates its hue by 180°
// on a --pdf-page-background page. Applied to a rendered page (white paper -> background).
void themePdfImage(QImage& image, QRgb background);

} // namespace oc::media
