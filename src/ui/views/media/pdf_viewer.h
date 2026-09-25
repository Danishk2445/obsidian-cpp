#pragma once

// Obsidian 1.13's PDF viewer (its PDF.js integration) as a widget, used by the "pdf" view, PDF embeds
// and hover previews:
//  - .pdf-toolbar: sidebar toggle + sidebar options (Thumbnails / Table of contents / Reveal page in
//    table of contents), zoom out | zoom in + display options (Fit width, Fit height; Single page,
//    Two-page odd / even; Adapt to theme), the page number input with "of N" ("(3 of 10)" with
//    page labels); on the right, whatever the host adds (embeds: an "Open" menu);
//  - the sidebar (thumbnails 140 px, or the outline), resizable in the view, overlaying in embeds;
//  - the find bar (Ctrl+F): "N of M matches", input with a match-case toggle, previous / next,
//    search settings (Highlight all, Whole words), close; F3 / Shift+F3, Mod+G, Enter / Shift+Enter;
//  - PdfPagesView for the pages; a password prompt for protected files.
// Options stored like Obsidian's localStorage (process-wide, persisted through PdfViewer::bindStore):
// "pdfjs-is-themed" and "pdfjs-search-settings".

#include "ui/views/media/pdf_pages.h"
#include "ui/views/media/pdf_search.h"

#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QMenu;
class QTreeView;
class QPdfBookmarkModel;

namespace oc {
class ConfigStore;
class IconButton;
class SearchInput;
class ToggleSwitch;
} // namespace oc

namespace oc::media {

class PdfThumbnails;

class PdfViewer : public QWidget {
    Q_OBJECT
public:
    enum class Mode { View, Embed, Popover };
    enum class SidebarView { None = 0, Thumbnails = 1, Outline = 2 };

    explicit PdfViewer(Mode mode, QWidget* parent = nullptr);
    ~PdfViewer() override;

    Mode mode() const { return m_mode; }
    void load(const QString& absPath); // the same path again keeps the page and zoom
    void reload();                     // the file changed on disk
    QString path() const;
    PdfFile* file() const { return m_file.get(); }
    PdfPagesView* pages() const { return m_pages; }
    PdfSearch* search() const { return m_search; }

    // Obsidian's applySubpath(): "#page=3", "#page=3&offset=x,y[,zoom]" (PDF.js FitBH / XYZ),
    // "#page=3&selection=…"/"&annotation=…" go to the page; anything else is ignored.
    void applySubpath(const QString& subpath);
    int currentPage() const { return m_pages->currentPage(); }
    QString zoom() const { return m_pages->scaleValue(); }
    void setZoom(const QString& value) { m_pages->setScaleValue(value); }

    void setSidebarView(SidebarView view);
    SidebarView sidebarView() const { return m_sidebarView; }
    void toggleSidebar();
    void revealInOutline();

    void showSearch(); // opens the find bar (or focuses / selects its input)
    void closeSearch();
    bool isSearchOpen() const;

    void setThemed(bool themed); // "Adapt to theme" (process-wide option)
    bool isThemed() const;
    void setToolbarVisible(bool visible);

    // A markdown link to a page ("Copy link to page N" in the context menus); unset = no item.
    std::function<QString(int page)> pageLink;

    // Widgets (tests, hosts)
    QWidget* toolbar() const { return m_toolbar; }
    QWidget* toolbarRight() const { return m_toolbarRight; }
    QLineEdit* pageInput() const { return m_pageInput; }
    QLabel* pageNumbers() const { return m_pageNumbers; }
    IconButton* zoomInButton() const { return m_zoomIn; }
    IconButton* zoomOutButton() const { return m_zoomOut; }
    IconButton* sidebarButton() const { return m_sidebarToggle; }
    SearchInput* findInput() const { return m_findInput; }
    QLabel* findCount() const { return m_findCount; }
    QWidget* sidebar() const { return m_sidebar; }
    PdfThumbnails* thumbnails() const { return m_thumbs; }
    QTreeView* outline() const { return m_outline; }
    bool hasOutline() const;
    // The toolbar menus (their actions can be triggered directly).
    QMenu* sidebarMenu(QWidget* parent);
    QMenu* displayMenu(QWidget* parent);
    QMenu* contextMenu(QWidget* parent, int page);

    // Options storage: Obsidian keeps them in localStorage; here the vault's clone config
    // ("local-storage.json"). The first bound store is used (options are process-wide).
    static void bindStore(ConfigStore* store);

signals:
    void stateChanged(); // page / zoom changed (debounced)

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void buildToolbar();
    void buildFindBar();
    void buildSidebar();
    void layoutContent();
    void updateToolbar();
    void updatePasswordPrompt();
    void runSearch();
    void applySearchResults();
    void commitPageInput();
    void loadOptions();
    void saveOptions();

    Mode m_mode;
    std::shared_ptr<PdfFile> m_file;
    QMetaObject::Connection m_reloadConn;
    PdfPagesView* m_pages;
    PdfSearch* m_search;
    // toolbar
    QWidget* m_toolbar = nullptr;
    QWidget* m_toolbarRight = nullptr;
    IconButton* m_sidebarToggle = nullptr;
    IconButton* m_sidebarOptions = nullptr;
    IconButton* m_zoomOut = nullptr;
    IconButton* m_zoomIn = nullptr;
    IconButton* m_displayOptions = nullptr;
    QLineEdit* m_pageInput = nullptr;
    QLabel* m_pageNumbers = nullptr;
    bool m_toolbarVisible = true;
    // find bar
    QWidget* m_findBar = nullptr;
    SearchInput* m_findInput = nullptr;
    IconButton* m_caseToggle = nullptr;
    QLabel* m_findCount = nullptr;
    QWidget* m_findSettings = nullptr;
    ToggleSwitch* m_highlightAllToggle = nullptr;
    ToggleSwitch* m_wholeWordToggle = nullptr;
    QTimer m_findDebounce;
    // sidebar
    QWidget* m_sidebar = nullptr;
    PdfThumbnails* m_thumbs = nullptr;
    QTreeView* m_outline = nullptr;
    QPdfBookmarkModel* m_bookmarks = nullptr;
    QWidget* m_resizer = nullptr;
    SidebarView m_sidebarView = SidebarView::None;
    int m_sidebarWidth = 140; // --sidebar-width
    // password prompt / errors
    QWidget* m_prompt = nullptr;
    QLabel* m_promptTitle = nullptr;
    QLabel* m_promptHint = nullptr;
    QLineEdit* m_password = nullptr;
    QTimer m_stateTimer;
};

// The sidebar's thumbnails (PDF.js thumbnail view): 98 px wide pages with their label, the current
// page selected; a click goes to the page.
class PdfThumbnails : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PdfThumbnails(PdfPagesView* pages, QWidget* parent = nullptr);
    void setFile(std::shared_ptr<PdfFile> file);
    QRect thumbnailRect(int page) const; // viewport coordinates of the image (tests)
    void ensureCurrentVisible();
    static constexpr int kThumbWidth = 98; // PDF.js THUMBNAIL_WIDTH

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    bool viewportEvent(QEvent* e) override;

private:
    void relayout();
    int pageAt(const QPoint& pos) const;
    PdfPagesView* m_pagesView;
    std::shared_ptr<PdfFile> m_file;
    QMetaObject::Connection m_renderedConn;
    QList<QRect> m_rects; // content coordinates of each image
    int m_contentHeight = 0;
    int m_hover = -1;
};

} // namespace oc::media
