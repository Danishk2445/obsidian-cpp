#include "render/services/image_cache.h"

#include "core/util/busy.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPointer>
#include <QSvgRenderer>
#include <QThreadPool>

namespace oc {

ImageCache& ImageCache::instance()
{
    static ImageCache c;
    return c;
}

ImageCache::ImageCache() = default;

QSize ImageCache::naturalSize(const QString& absPath)
{
    const QFileInfo fi(absPath);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    Entry& e = m_entries[absPath];
    if (e.mtime == mtime && e.natural.isValid())
        return e.natural;
    if (e.mtime != mtime) {
        e = Entry{};
        e.mtime = mtime;
    }
    if (absPath.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgRenderer r(absPath);
        e.natural = r.isValid() ? r.defaultSize() : QSize();
    } else {
        QImageReader reader(absPath);
        reader.setAutoTransform(true);
        e.natural = reader.size();
        if (!e.natural.isValid()) {
            const QImage img = reader.read(); // formats without cheap size info
            e.natural = img.size();
        }
    }
    return e.natural;
}

std::shared_ptr<const QImage> ImageCache::image(const QString& absPath, int maxWidth, qreal dpr)
{
    const QSize nat = naturalSize(absPath);
    if (!nat.isValid())
        return nullptr;
    const int w = std::max(1, std::min(maxWidth, nat.width()));
    const QString key = QStringLiteral("%1@%2").arg(w).arg(dpr);
    Entry& e = m_entries[absPath];
    if (auto it = e.scaled.constFind(key); it != e.scaled.cend())
        return *it;
    const QString pendingKey = absPath + QLatin1Char('|') + key;
    if (m_pending.contains(pendingKey))
        return nullptr;
    m_pending.insert(pendingKey);
    Busy::begin();
    QPointer<ImageCache> self(this);
    QThreadPool::globalInstance()->start([self, absPath, w, dpr, nat, key, pendingKey] {
        QImage out;
        const QSize target(int(w * dpr), int(qint64(w) * nat.height() / std::max(1, nat.width()) * dpr));
        if (absPath.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
            QSvgRenderer r(absPath);
            out = QImage(target, QImage::Format_ARGB32_Premultiplied);
            out.fill(Qt::transparent);
            QPainter p(&out);
            r.render(&p);
        } else {
            QImageReader reader(absPath);
            reader.setAutoTransform(true);
            if (target.width() < nat.width() * dpr)
                reader.setScaledSize(target);
            out = reader.read();
            if (!out.isNull() && out.size() != target)
                out = out.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        out.setDevicePixelRatio(dpr);
        QMetaObject::invokeMethod(
            self.data(),
            [self, absPath, key, pendingKey, img = std::move(out)]() mutable {
                Busy::end();
                if (!self)
                    return;
                self->m_pending.remove(pendingKey);
                self->m_entries[absPath].scaled.insert(key, std::make_shared<const QImage>(std::move(img)));
                emit self->imageReady(absPath);
            },
            Qt::QueuedConnection);
    });
    return nullptr;
}

void ImageCache::invalidate(const QString& absPath) { m_entries.remove(absPath); }

} // namespace oc
