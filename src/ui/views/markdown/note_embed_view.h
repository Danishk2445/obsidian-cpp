#pragma once

// Note embeds in the app (the rendering engine is editor/embeds/note_embed.h):
//  - NoteEmbedWidget, the live embed hosted over "![[Note#Heading]]" in Live Preview, Reading view
//    and hover previews: it paints the cached layout, scrolls content taller than
//    --embed-max-height, follows links (Obsidian's openLinkText from the embedded note; Mod or the
//    middle button = new tab) and shows page previews for them (hover source "preview", which needs
//    no Mod, like Obsidian's embeds), opens the note from its "Open link" button (maximize-2; Mod =
//    new tab), opens / creates the note from the generic and "not created yet" boxes, and in Live
//    Preview a click elsewhere selects the embed's markdown (Obsidian's selectElement).
//  - registerNoteEmbeds(session): registers the "md" embed provider (once) and the session as a
//    NoteEmbedHost (open documents, link resolution, change notifications: open documents' edits
//    debounced by kChangeDelayMs, vault events, re-resolved links).

#include "editor/embeds/note_embed.h"

#include <QWidget>

class QScrollBar;

namespace oc {

class IconButton;
class VaultSession;
class Workspace;

class NoteEmbedWidget : public QWidget {
    Q_OBJECT
public:
    explicit NoteEmbedWidget(const EmbedContext& context, QWidget* parent = nullptr);
    ~NoteEmbedWidget() override;

    const EmbedRequest& request() const { return m_ctx.request; }
    std::shared_ptr<NoteEmbedLayout> embedLayout() const; // the layout at the current width
    IconButton* openButton() const { return m_open; }
    QScrollBar* scrollBar() const { return m_bar; }
    qreal scroll() const { return m_scroll; }
    void setScroll(qreal y);
    NoteEmbedLayout::Hit hitAt(const QPoint& pos) const;

    // Follow a hit the way a click does (tests / keyboard): mods Ctrl or button Middle = new tab.
    void activate(const NoteEmbedLayout::Hit& hit, Qt::KeyboardModifiers mods = {}, Qt::MouseButton button = Qt::LeftButton);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool event(QEvent* e) override;

private:
    void updateChrome();
    void updateHover(const QPoint& pos);
    Workspace* workspace() const;

    EmbedContext m_ctx;
    IconButton* m_open = nullptr;
    QScrollBar* m_bar = nullptr;
    qreal m_scroll = 0;
    QString m_hoverLink; // "kind:linktext" of the link under the mouse (page preview)
    bool m_boxHover = false;
    QPoint m_pressPos;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    bool m_syncingBar = false;
};

// The "md" provider with the live widget.
class NoteEmbedViewProvider : public NoteEmbedProvider {
public:
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
};

inline constexpr int kNoteEmbedChangeDelayMs = 250; // edits of open notes reach their embeds after this

// Registers the note embed provider and `session` as a host (idempotent). Call once per session,
// with its vault, documents and metadata cache set (Application::populateWindow).
void registerNoteEmbeds(VaultSession* session);

} // namespace oc
