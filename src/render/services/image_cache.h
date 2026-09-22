#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>

#include <memory>

namespace oc {

// Asynchronous image loading for embeds (png/jpg/gif/webp/avif/svg...). Sizes are read from the
// file header synchronously (cheap) so layout can reserve space before pixels arrive.
class ImageCache : public QObject {
    Q_OBJECT
public:
    static ImageCache& instance();

    // Natural size in px (invalid if unreadable). Cached by path + mtime.
    QSize naturalSize(const QString& absPath);
    // Decoded image scaled to at most maxWidth logical px at dpr; nullptr while loading (imageReady follows).
    std::shared_ptr<const QImage> image(const QString& absPath, int maxWidth, qreal dpr);
    void invalidate(const QString& absPath);

signals:
    void imageReady(const QString& absPath);

private:
    ImageCache();
    struct Entry {
        qint64 mtime = 0;
        QSize natural;
        QHash<QString, std::shared_ptr<const QImage>> scaled; // key "w@dpr"
    };
    QHash<QString, Entry> m_entries;
    QSet<QString> m_pending;
};

} // namespace oc
