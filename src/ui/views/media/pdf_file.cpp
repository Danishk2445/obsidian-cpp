#include "ui/views/media/pdf_file.h"

#include "core/util/busy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QPainter>
#include <QPdfDocumentRenderOptions>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfSelection>
#include <QPointer>
#include <QThreadPool>

#include <cmath>

namespace oc::media {

namespace {
constexpr qint64 kCacheBytes = 96ll * 1024 * 1024; // rendered pages kept per file
constexpr int kCacheEntries = 64;
constexpr int kMaxQueued = 24;

QHash<QString, std::weak_ptr<PdfFile>>& openFiles()
{
    static QHash<QString, std::weak_ptr<PdfFile>> files;
    return files;
}

// One worker for all PDF rendering (QtPdf serializes pdfium calls with a global lock anyway).
QThreadPool* renderPool()
{
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(1);
        pool->setObjectName(QStringLiteral("ocPdfRender"));
    }
    return pool;
}

std::shared_ptr<QPdfDocument> newDocument()
{
    // Render tasks may hold the last reference on the worker thread: delete on the owning thread.
    return std::shared_ptr<QPdfDocument>(new QPdfDocument, [](QPdfDocument* d) { d->deleteLater(); });
}

qint64 mtimeOf(const QString& path) { return QFileInfo(path).lastModified().toMSecsSinceEpoch(); }

// PDF.js's rotateRGB180: hue + 180° in HSV.
QRgb rotateHue180(int r, int g, int b)
{
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx == mn)
        return qRgb(r, g, b);
    const double d = mx - mn;
    double h;
    if (mx == r)
        h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g)
        h = (b - r) / d + 2;
    else
        h = (r - g) / d + 4;
    h = std::fmod(h / 6 + 0.5, 1.0);
    const double s = d / mx, v = mx / 255.0;
    const int i = int(std::floor(h * 6));
    const double f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    double R, G, B;
    switch (i % 6) {
    case 0: R = v, G = t, B = p; break;
    case 1: R = q, G = v, B = p; break;
    case 2: R = p, G = v, B = t; break;
    case 3: R = p, G = q, B = v; break;
    case 4: R = t, G = p, B = v; break;
    default: R = v, G = p, B = q; break;
    }
    return qRgb(int(std::lround(R * 255)), int(std::lround(G * 255)), int(std::lround(B * 255)));
}
} // namespace

void themePdfImage(QImage& image, QRgb background)
{
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    const int br = qRed(background), bg = qGreen(background), bb = qBlue(background);
    QHash<QRgb, QRgb> memo; // pages are mostly a handful of colours
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x] | 0xff000000;
            if (const auto it = memo.constFind(px); it != memo.cend()) {
                line[x] = it.value();
                continue;
            }
            const QRgb rot = rotateHue180(255 - qRed(px), 255 - qGreen(px), 255 - qBlue(px));
            // inverted paper (black) becomes the page background, white stays white
            const auto mix = [](int c, int base) { return base + c * (255 - base) / 255; };
            const QRgb out = qRgb(mix(qRed(rot), br), mix(qGreen(rot), bg), mix(qBlue(rot), bb));
            if (memo.size() > 4096)
                memo.clear();
            memo.insert(px, out);
            line[x] = out;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// PdfFile
// ---------------------------------------------------------------------------------------------
std::shared_ptr<PdfFile> PdfFile::open(const QString& absPath)
{
    auto& files = openFiles();
    if (auto it = files.constFind(absPath); it != files.cend())
        if (std::shared_ptr<PdfFile> f = it->lock())
            return f;
    std::shared_ptr<PdfFile> f(new PdfFile(absPath));
    files.insert(absPath, f);
    return f;
}

std::shared_ptr<PdfFile> PdfFile::find(const QString& absPath) { return openFiles().value(absPath).lock(); }

PdfFile::PdfFile(QString absPath)
    : m_path(std::move(absPath))
{
    load();
}

PdfFile::~PdfFile()
{
    for (qsizetype i = 0; i < m_queue.size(); ++i)
        Busy::end();
    auto& files = openFiles();
    if (auto it = files.find(m_path); it != files.end() && it->expired())
        files.erase(it);
}

QPdfDocument::Status PdfFile::status() const { return m_doc ? m_doc->status() : QPdfDocument::Status::Null; }

void PdfFile::load()
{
    for (qsizetype i = 0; i < m_queue.size(); ++i)
        Busy::end();
    m_queue.clear();
    const QFileInfo fi(m_path);
    m_mtime = fi.lastModified().toMSecsSinceEpoch();
    m_size = fi.exists() ? fi.size() : -1;
    std::shared_ptr<QPdfDocument> doc = newDocument();
    if (!m_password.isEmpty())
        doc->setPassword(m_password);
    m_error = doc->load(m_path);
    m_sizes.clear();
    m_labels.clear();
    m_hasLabels = false;
    if (m_error == QPdfDocument::Error::None) {
        const int n = doc->pageCount();
        m_sizes.reserve(n);
        for (int i = 0; i < n; ++i) {
            QSizeF s = doc->pagePointSize(i);
            if (!s.isValid() || s.isEmpty())
                s = QSizeF(612, 792); // US Letter
            m_sizes.append(s);
            const QString label = doc->pageLabel(i);
            m_labels.append(label);
            if (!label.isEmpty() && label != QString::number(i + 1))
                m_hasLabels = true;
        }
    }
    m_doc = std::move(doc);
    ++m_generation;
    m_text.clear();
    m_links.clear();
    m_cache.clear();
}

void PdfFile::setPassword(const QString& password)
{
    m_password = password;
    load();
    emit reloaded();
}

void PdfFile::reload(bool force)
{
    const QFileInfo fi(m_path);
    if (!force && fi.lastModified().toMSecsSinceEpoch() == m_mtime && (fi.exists() ? fi.size() : -1) == m_size)
        return; // every viewer of the file asks; the first one reloads
    load();
    emit reloaded();
}

QSizeF PdfFile::pagePointSize(int page) const { return page >= 0 && page < m_sizes.size() ? m_sizes[page] : QSizeF(612, 792); }

QString PdfFile::pageLabel(int page) const
{
    if (page < 0 || page >= m_labels.size())
        return {};
    return m_labels[page].isEmpty() ? QString::number(page + 1) : m_labels[page];
}

int PdfFile::pageForLabel(const QString& label) const
{
    for (qsizetype i = 0; i < m_labels.size(); ++i)
        if (m_labels[i] == label)
            return int(i);
    return -1;
}

const PdfFile::PageText& PdfFile::pageText(int page)
{
    static const PageText empty;
    if (!m_doc || page < 0 || page >= pageCount())
        return empty;
    auto it = m_text.find(page);
    if (it == m_text.end()) {
        const QPdfSelection all = m_doc->getAllText(page);
        PageText t;
        t.text = all.text();
        for (const QPolygonF& poly : all.bounds())
            if (const QRectF r = poly.boundingRect(); r.width() > 0 && r.height() > 0)
                t.boxes.append(r);
        it = m_text.emplace(page, std::move(t)).first;
    }
    return it->second;
}

const QList<PdfFile::Link>& PdfFile::links(int page)
{
    if (auto it = m_links.find(page); it != m_links.end())
        return it->second;
    QList<Link> out;
    if (m_doc && page >= 0 && page < pageCount()) {
        QPdfLinkModel model;
        model.setDocument(m_doc.get());
        model.setPage(page);
        for (int r = 0; r < model.rowCount({}); ++r) {
            const QModelIndex idx = model.index(r);
            const QPdfLink l = model.data(idx, int(QPdfLinkModel::Role::Link)).value<QPdfLink>();
            Link link;
            link.rect = model.data(idx, int(QPdfLinkModel::Role::Rectangle)).toRectF();
            link.url = l.url();
            link.page = l.url().isValid() ? -1 : l.page();
            link.location = l.location();
            if (link.rect.isValid() && (link.url.isValid() || link.page >= 0))
                out.append(link);
        }
    }
    return m_links.emplace(page, std::move(out)).first->second;
}

QImage PdfFile::cachedImage(int page, int pixelWidth, bool themed, QRgb background) const
{
    const Key key{page, pixelWidth, themed, themed ? background : 0};
    for (const Entry& e : m_cache) {
        if (e.key == key) {
            const_cast<Entry&>(e).use = ++m_useCounter;
            return e.image;
        }
    }
    return {};
}

QImage PdfFile::anyImage(int page, bool themed) const
{
    const Entry* best = nullptr;
    for (const Entry& e : m_cache)
        if (e.key.page == page && e.key.themed == themed && (!best || e.key.width > best->key.width))
            best = &e;
    return best ? best->image : QImage();
}

void PdfFile::requestImage(int page, int pixelWidth, bool themed, QRgb background)
{
    if (!m_doc || page < 0 || page >= pageCount() || pixelWidth <= 0)
        return;
    pixelWidth = std::min(pixelWidth, 8000);
    const Key key{page, pixelWidth, themed, themed ? background : 0};
    for (const Entry& e : m_cache)
        if (e.key == key)
            return;
    if (const qsizetype i = m_queue.indexOf(key); i >= 0) {
        m_queue.move(i, 0);
    } else {
        m_queue.prepend(key);
        Busy::begin();
        while (m_queue.size() > kMaxQueued) {
            m_queue.removeLast();
            Busy::end();
        }
    }
    pump();
}

void PdfFile::pump()
{
    if (m_inFlight || m_queue.isEmpty() || !m_doc)
        return;
    const Key key = m_queue.takeFirst();
    m_inFlight = true;
    const std::shared_ptr<QPdfDocument> doc = m_doc;
    const QSizeF pts = pagePointSize(key.page);
    const quint64 gen = m_generation;
    QPointer<PdfFile> self(this);
    renderPool()->start([self, doc, key, gen, pts] {
        const int w = key.width;
        const int h = std::max(1, int(std::lround(w * pts.height() / std::max(1.0, pts.width()))));
        QPdfDocumentRenderOptions opts;
        opts.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
        const QImage raw = doc->render(key.page, QSize(w, h), opts);
        // pdfium leaves the paper transparent: composite on white like PDF.js's canvas
        QImage page(QSize(w, h), QImage::Format_RGB32);
        page.fill(Qt::white);
        if (!raw.isNull()) {
            QPainter p(&page);
            p.drawImage(0, 0, raw);
        }
        if (key.themed)
            themePdfImage(page, key.background);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, key, gen, img = std::move(page)]() mutable {
                if (self)
                    self->finished(key, gen, std::move(img));
                else
                    Busy::end();
            },
            Qt::QueuedConnection);
    });
}

void PdfFile::finished(Key key, quint64 generation, QImage image)
{
    m_inFlight = false;
    Busy::end();
    if (generation == m_generation && !image.isNull()) {
        m_cache.append(Entry{key, std::move(image), ++m_useCounter});
        evict();
        emit rendered(key.page);
    }
    pump();
}

void PdfFile::evict()
{
    auto bytes = [this] {
        qint64 n = 0;
        for (const Entry& e : std::as_const(m_cache))
            n += e.image.sizeInBytes();
        return n;
    };
    while (m_cache.size() > 1 && (m_cache.size() > kCacheEntries || bytes() > kCacheBytes)) {
        qsizetype oldest = 0;
        for (qsizetype i = 1; i < m_cache.size(); ++i)
            if (m_cache[i].use < m_cache[oldest].use)
                oldest = i;
        m_cache.removeAt(oldest);
    }
}

QList<QSizeF> PdfFile::pageSizes(const QString& absPath)
{
    struct Info {
        qint64 mtime = 0;
        QList<QSizeF> sizes;
    };
    static QHash<QString, Info> cache;
    const qint64 mtime = mtimeOf(absPath);
    auto it = cache.find(absPath);
    if (it != cache.end() && it->mtime == mtime)
        return it->sizes;
    Info info;
    info.mtime = mtime;
    if (auto open = openFiles().value(absPath).lock(); open && open->isLoaded()) {
        info.sizes = open->m_sizes;
    } else {
        QPdfDocument doc;
        if (doc.load(absPath) == QPdfDocument::Error::None)
            for (int i = 0; i < doc.pageCount(); ++i)
                info.sizes.append(doc.pagePointSize(i));
    }
    if (cache.size() > 256)
        cache.clear();
    cache.insert(absPath, info);
    return info.sizes;
}

} // namespace oc::media
