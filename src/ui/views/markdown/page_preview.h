#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>

#include <memory>

class QLabel;

namespace oc {

class Document;
class EditorView;
class MainWindow;
class OverlayLayer;
class TFile;
class VaultSession;

// A page preview popover (Obsidian's .popover.hover-popover): the linked note rendered like an embed
// (the whole note, or only the linked section / block), or the linked image. 450 px wide, at most
// 400 px tall (scrolls), in the window's overlay layer.
class HoverPopover : public QWidget {
    Q_OBJECT
public:
    HoverPopover(OverlayLayer* layer, VaultSession* session, TFile* file, const QString& subpath, QWidget* hoverParent);
    ~HoverPopover() override;

    TFile* file() const { return m_file; }
    QString subpath() const { return m_subpath; }
    EditorView* editor() const { return m_editor; } // null for images
    QWidget* embed() const { return m_embed; }      // PDF / audio / video (EmbedRegistry), else null
    QWidget* hoverParent() const { return m_hoverParent; }
    QRect anchor() const { return m_anchor; }      // the hovered link (layer coordinates)
    void placeNear(const QRect& anchor);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPointer<VaultSession> m_session;
    TFile* m_file;
    QString m_subpath;
    QPointer<QWidget> m_hoverParent;
    std::shared_ptr<Document> m_doc;
    EditorView* m_editor = nullptr;
    QLabel* m_image = nullptr;
    QWidget* m_embed = nullptr;
    QSize m_embedSize;
    QRect m_anchor;
};

// Obsidian's "page-preview" core plugin for one window. Views report link hovers with a source name
// ("editor", "preview" = reading view, "file-explorer", "search", ...); whether a source needs the Mod
// key comes from the vault's page-preview config (default: yes, except the reading view). The popover
// appears after 300 ms (or when Mod is pressed over the link), nests for links inside it, and hides
// 300 ms after the mouse left it, its link and its children. Esc or a click elsewhere closes all.
class PagePreview : public QObject {
    Q_OBJECT
public:
    static PagePreview* of(MainWindow* window); // created on first use, owned by the window
    explicit PagePreview(MainWindow* window);

    // anchor in global coordinates; hoverParent: the widget showing the link (inside a popover for nesting)
    void onLinkHover(const QString& source, const QString& linktext, const QString& sourcePath, const QRect& anchorGlobal,
                     QWidget* hoverParent);
    void onLinkUnhover(QWidget* hoverParent);
    // Wires an editor's linkHovered()/linkActivated() (popover editors call this for nesting).
    void attachEditor(EditorView* editor, std::function<QString()> sourcePath, std::function<QString()> source);

    bool requiresMod(const QString& source) const;
    QList<HoverPopover*> popovers() const;
    void closeAll();
    void setDelays(int showMs, int hideMs); // tests

signals:
    void popoverShown(oc::HoverPopover* popover);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Pending {
        QString source, linktext, sourcePath;
        QRect anchorGlobal;
        QPointer<QWidget> hoverParent;
        bool active = false;
    };
    void showPending();
    void checkMouse();
    bool isInside(HoverPopover* p, const QPoint& global) const;
    void closeFrom(HoverPopover* p); // p and every popover nested in it

    QPointer<MainWindow> m_window;
    QObject* m_windowObject; // identity for eventFilter()
    QList<QPointer<HoverPopover>> m_popovers; // outermost first
    Pending m_pending;
    QTimer m_showTimer;
    QTimer m_poll;
    QElapsedTimer m_outside;
    int m_hideMs = 300;
};

} // namespace oc
