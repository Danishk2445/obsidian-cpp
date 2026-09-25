#pragma once

// Obsidian's audio view ("audio", lucide-file-audio) and video view ("video", lucide-file): the
// player centred in the view content (padding 12px 12px 32px, "text-align: center"), at most as
// wide as the view. Audio: a full-width 42 px control bar; video: its natural size (never wider than
// the view) with overlay controls. Opening another file stops playback; the view reloads when the
// file changes on disk (keeping the position).

#include "ui/workspace/view.h"

namespace oc::media {

class MediaPlayerWidget;

class MediaFileView : public FileView {
    Q_OBJECT
public:
    enum class Kind { Audio, Video };
    MediaFileView(Kind kind, WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~MediaFileView() override;

    QString viewType() const override;
    QString icon() const override;
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;
    void onClose() override;
    void focusContent() override;

    MediaPlayerWidget* player() const { return m_player; }

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void placePlayer();
    Kind m_kind;
    MediaPlayerWidget* m_player;
    QMetaObject::Connection m_modifiedConn;
};

} // namespace oc::media
