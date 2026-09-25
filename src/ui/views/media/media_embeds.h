#pragma once

// Embed providers for PDF, audio and video files (Obsidian's A1 / B1 / V1 embeds), registered with
// the editor's EmbedRegistry by registerMediaEmbeds():
//  - PDF (.internal-embed.pdf-embed): full width, 1px border, radius-s; height = the page at the
//    embed's width + the toolbar ("page", Obsidian's default) or "#height=N" + the toolbar; the
//    viewer opens at "#page=N"; its toolbar ends with an "Open" menu (open in a new tab / to the
//    right) and, in Live Preview, an "Edit this block" button; 450×400 without toolbar in popovers;
//  - audio (.audio-embed): a 42 px control bar, 100% wide;
//  - video (.video-embed): the video's natural size, at most 100% wide (a probe reads it before the
//    player exists); 300×150 until known; files without a picture fall back to the audio bar.
// Embedded files are watched; a change reloads their live widgets and re-lays out the embeds.

#include "editor/embeds/embed_registry.h"

#include <QHash>
#include <QObject>
#include <QSize>
#include <QWidget>

class QFileSystemWatcher;

namespace oc {
class IconButton;
}

namespace oc::media {

class PdfViewer;

// Media metadata (has a picture, size, duration) read by a QMediaPlayer without output, cached by
// path + mtime; asynchronous (at most two probes at a time, 5 s timeout like Obsidian's).
struct MediaInfo {
    bool known = false; // probed (or failed)
    bool valid = false;
    bool hasVideo = false;
    QSize size;
    qint64 duration = 0;
};

class MediaProbe : public QObject {
    Q_OBJECT
public:
    static MediaProbe& instance();
    // Cached info; when unknown and `start`, a probe is queued (probed() follows).
    MediaInfo info(const QString& absPath, bool start = true);
    void forget(const QString& absPath);
    int pending() const { return int(m_queue.size()) + m_running; }

signals:
    void probed(const QString& absPath);

private:
    MediaProbe() = default;
    void pump();
    void finish(const QString& absPath, const MediaInfo& info);
    struct Entry {
        qint64 mtime = 0;
        MediaInfo info;
    };
    QHash<QString, Entry> m_cache;
    QStringList m_queue;
    int m_running = 0;
};

// Keeps QFileSystemWatcher watches for the files of live embed widgets (ref-counted by owner).
class EmbedFileWatcher : public QObject {
    Q_OBJECT
public:
    static EmbedFileWatcher& instance();
    void watch(const QString& absPath, QObject* owner);

signals:
    void fileChanged(const QString& absPath);

private:
    EmbedFileWatcher();
    QFileSystemWatcher* m_watcher;
    QHash<QString, int> m_refs;
};

// The live PDF embed: the viewer inside the embed frame, with the embed actions.
class PdfEmbedWidget : public QWidget {
    Q_OBJECT
public:
    PdfEmbedWidget(const EmbedContext& context, QWidget* parent = nullptr);
    PdfViewer* viewer() const { return m_viewer; }
    IconButton* editButton() const { return m_edit; }
    IconButton* openButton() const { return m_more; }
    const EmbedRequest& request() const { return m_ctx.request; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    void updateActions();
    EmbedContext m_ctx;
    PdfViewer* m_viewer;
    IconButton* m_more = nullptr;
    IconButton* m_edit = nullptr;
    QWidget* m_editSpacer = nullptr;
};

class PdfEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override;
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override;
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
    quint64 signature(const EmbedRequest& request) const override;
};

class AudioEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override;
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override;
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
};

class VideoEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override;
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override;
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
    quint64 signature(const EmbedRequest& request) const override;
};

} // namespace oc::media
