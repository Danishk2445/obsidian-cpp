#include "ui/views/media/pdf_viewer.h"

#include "core/config/config_store.h"
#include "editor/embeds/embed_registry.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPdfBookmarkModel>
#include <QScrollBar>
#include <QTreeView>

#include <cmath>

namespace oc::media {

namespace {
constexpr int kFindBarHeight = 42;     // input 30 + padding --size-2-3 (6) × 2
constexpr int kEmbedThumbsWidth = 140; // .pdf-embed .pdf-sidebar-container[data-view="1"]
constexpr int kEmbedOutlineWidth = 275;
constexpr const char* kThemedKey = "pdfjs-is-themed";
constexpr const char* kSearchKey = "pdfjs-search-settings";
constexpr const char* kStoreFile = "local-storage";

// Process-wide PDF options (Obsidian's localStorage keys), shared by every viewer.
class PdfOptions : public QObject {
    Q_OBJECT
public:
    static PdfOptions& instance()
    {
        static PdfOptions o;
        return o;
    }
    bool themed = false;
    PdfSearch::Options search;
    QPointer<ConfigStore> store;

    void bind(ConfigStore* s)
    {
        if (store || !s)
            return;
        store = s;
        const QJsonObject o = s->object(QString::fromLatin1(kStoreFile));
        themed = o.value(QLatin1String(kThemedKey)).toString() == u"true";
        const QJsonObject so = QJsonDocument::fromJson(o.value(QLatin1String(kSearchKey)).toString().toUtf8()).object();
        search.highlightAll = so.value(QStringLiteral("highlightAll")).toBool();
        search.caseSensitive = so.value(QStringLiteral("caseSensitive")).toBool();
        search.entireWord = so.value(QStringLiteral("entireWord")).toBool();
        emit changed();
    }
    void save()
    {
        if (store) {
            // localStorage values are strings; "pdfjs-is-themed" is removed when off
            store->setValue(QString::fromLatin1(kStoreFile), QString::fromLatin1(kThemedKey),
                            themed ? QJsonValue(QStringLiteral("true")) : QJsonValue());
            const QJsonObject so{{QStringLiteral("highlightAll"), search.highlightAll},
                                 {QStringLiteral("caseSensitive"), search.caseSensitive},
                                 {QStringLiteral("matchDiacritics"), false},
                                 {QStringLiteral("entireWord"), search.entireWord}};
            store->setValue(QString::fromLatin1(kStoreFile), QString::fromLatin1(kSearchKey),
                            QString::fromUtf8(QJsonDocument(so).toJson(QJsonDocument::Compact)));
        }
        emit changed();
    }

signals:
    void changed();
};

IconButton* toolButton(const QString& icon, const QString& tip, QWidget* parent)
{
    auto* b = new IconButton(icon, tip, parent);
    b->setButtonSize(QSize(30, 26)); // .clickable-icon: 18px icon, padding 4px 6px
    return b;
}

// .pdf-toolbar-divider: 1px × --icon-size, --background-modifier-border
class Divider : public QWidget {
public:
    explicit Divider(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(1, 18);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-modifier-border")));
    }
};

// A widget painting a background colour and optional bottom / right border.
class Panel : public QWidget {
public:
    Panel(QString bgToken, QString borderToken, Qt::Edge edge, QWidget* parent)
        : QWidget(parent)
        , m_bg(std::move(bgToken))
        , m_border(std::move(borderToken))
        , m_edge(edge)
    {
        if (auto* tm = ThemeManager::instance())
            connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const ThemePtr t = currentTheme();
        QPainter p(this);
        p.fillRect(rect(), t->color(m_bg, t->color(QStringLiteral("--background-primary"))));
        if (m_border.isEmpty())
            return;
        const QColor c = t->color(m_border);
        if (m_edge == Qt::BottomEdge)
            p.fillRect(QRect(0, height() - 1, width(), 1), c);
        else if (m_edge == Qt::RightEdge)
            p.fillRect(QRect(width() - 1, 0, 1, height()), c);
    }

private:
    QString m_bg, m_border;
    Qt::Edge m_edge;
};

// The rounded popup of the find bar's search settings (.pdf-findbar-settings).
class SettingsPopup : public QWidget {
public:
    explicit SettingsPopup(QWidget* parent)
        : QWidget(parent)
    {
        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(18);
        shadow->setOffset(0, 4);
        shadow->setColor(QColor(0, 0, 0, 70));
        setGraphicsEffect(shadow);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const ThemePtr t = currentTheme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        const double r = t->px(QStringLiteral("--radius-m"), -1, 8);
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), r, r);
        p.fillPath(path, t->color(QStringLiteral("--background-primary")));
        p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border")), 1));
        p.drawPath(path);
    }
};

QLabel* uiLabel(const QString& text, int px, const QString& colorToken, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = currentTheme()->interfaceFont(px);
    f.setFeature(QFont::Tag("tnum"), 1);
    l->setFont(f);
    l->setProperty("ocColorToken", colorToken);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, currentTheme()->color(colorToken));
    l->setPalette(pal);
    return l;
}

void refreshLabel(QLabel* l)
{
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, currentTheme()->color(l->property("ocColorToken").toString()));
    l->setPalette(pal);
}

QIcon menuIcon(const QString& name) { return IconProvider::instance().icon(name, 16, 2.0); }
} // namespace

// ---------------------------------------------------------------------------------------------
// PdfViewer
// ---------------------------------------------------------------------------------------------
void PdfViewer::bindStore(ConfigStore* store) { PdfOptions::instance().bind(store); }

PdfViewer::PdfViewer(Mode mode, QWidget* parent)
    : QWidget(parent)
    , m_mode(mode)
    , m_pages(new PdfPagesView(this))
    , m_search(new PdfSearch(this))
{
    setObjectName(QStringLiteral("ocPdfViewer"));
    setFocusPolicy(Qt::ClickFocus);
    m_pages->setEmbedded(mode != Mode::View);
    m_pages->setArrowPaging(mode == Mode::View);
    buildToolbar();
    buildSidebar();
    buildFindBar();

    // password prompt (.pdf-password-dialog) / load errors
    m_prompt = new QWidget(this);
    auto* pv = new QVBoxLayout(m_prompt);
    pv->setSpacing(4);
    auto* lock = new QLabel(m_prompt);
    lock->setObjectName(QStringLiteral("ocPdfLock"));
    lock->setAlignment(Qt::AlignCenter);
    m_promptTitle = uiLabel(QString(), 15, QStringLiteral("--text-normal"), m_prompt);
    m_promptTitle->setAlignment(Qt::AlignCenter);
    m_promptHint = uiLabel(QString(), 13, QStringLiteral("--text-muted"), m_prompt);
    m_promptHint->setAlignment(Qt::AlignCenter);
    m_password = new QLineEdit(m_prompt);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setFixedWidth(220);
    pv->addWidget(lock);
    pv->addWidget(m_promptTitle);
    pv->addWidget(m_promptHint);
    pv->addSpacing(8);
    pv->addWidget(m_password, 0, Qt::AlignHCenter);
    m_prompt->hide();
    connect(m_password, &QLineEdit::returnPressed, this, [this] {
        if (m_file)
            m_file->setPassword(m_password->text());
    });

    connect(m_pages, &PdfPagesView::currentPageChanged, this, [this] {
        updateToolbar();
        if (m_thumbs && m_thumbs->isVisible()) {
            m_thumbs->viewport()->update();
            m_thumbs->ensureCurrentVisible();
        }
        m_stateTimer.start();
    });
    connect(m_pages, &PdfPagesView::scaleChanged, this, [this] {
        updateToolbar();
        m_stateTimer.start();
    });
    connect(m_pages, &PdfPagesView::contextMenuWanted, this, [this](const QPoint& global) {
        const QPoint local = m_pages->viewport()->mapFromGlobal(global);
        const QPointF c = QPointF(local) + m_pages->scrollOffset();
        int page = m_pages->currentPage();
        for (int i = 0; i < m_pages->pageCount(); ++i)
            if (m_pages->pageRect(i).contains(c))
                page = i;
        QMenu* menu = contextMenu(this, page);
        if (menu->actions().isEmpty()) {
            delete menu;
            return;
        }
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(global);
    });
    connect(m_search, &PdfSearch::resultsChanged, this, &PdfViewer::applySearchResults);
    connect(m_search, &PdfSearch::currentChanged, this, [this](int index) {
        applySearchResults();
        m_pages->revealHighlight(index);
    });
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(250);
    connect(&m_stateTimer, &QTimer::timeout, this, &PdfViewer::stateChanged);

    PdfOptions& opts = PdfOptions::instance();
    connect(&opts, &PdfOptions::changed, this, &PdfViewer::loadOptions);
    loadOptions();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            for (QLabel* l : findChildren<QLabel*>())
                if (l->property("ocColorToken").isValid()) {
                    l->setFont([&] {
                        QFont f = currentTheme()->interfaceFont(l->font().pixelSize() > 0 ? l->font().pixelSize() : 13);
                        f.setFeature(QFont::Tag("tnum"), 1);
                        return f;
                    }());
                    refreshLabel(l);
                }
            update();
        });
    if (mode == Mode::Popover)
        setToolbarVisible(false); // .hover-popover .pdf-toolbar { display: none }
    updateToolbar();
}

PdfViewer::~PdfViewer() = default;

QString PdfViewer::path() const { return m_file ? m_file->path() : QString(); }

void PdfViewer::load(const QString& absPath)
{
    if (m_file && m_file->path() == absPath) {
        reload();
        return;
    }
    disconnect(m_reloadConn);
    m_file = PdfFile::open(absPath);
    m_reloadConn = connect(m_file.get(), &PdfFile::reloaded, this, [this] {
        m_search->setFile(m_file);
        if (m_bookmarks)
            m_bookmarks->setDocument(m_file->document());
        updatePasswordPrompt();
        updateToolbar();
    });
    m_pages->setFile(m_file);
    m_search->setFile(m_file);
    if (m_thumbs)
        m_thumbs->setFile(m_file);
    if (m_bookmarks)
        m_bookmarks->setDocument(m_file->document());
    updatePasswordPrompt();
    updateToolbar();
    layoutContent();
}

void PdfViewer::reload()
{
    if (m_file)
        m_file->reload(); // every viewer of the file follows (PdfFile::reloaded)
}

void PdfViewer::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.fillRect(rect(), t->color(QStringLiteral("--pdf-background"), t->color(QStringLiteral("--background-primary"))));
}

// ---------------------------------------------------------------------------------------------
// toolbar
// ---------------------------------------------------------------------------------------------
void PdfViewer::buildToolbar()
{
    auto* bar = new Panel(QStringLiteral("--background-primary"), QStringLiteral("--background-modifier-border"), Qt::BottomEdge, this);
    bar->setObjectName(QStringLiteral("ocPdfToolbar"));
    m_toolbar = bar;
    bar->setFixedHeight(kToolbarHeight);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(12, 4, 12, 5); // padding --size-4-1 --size-4-3 (+1px border)
    h->setSpacing(4);
    m_sidebarToggle = toolButton(QStringLiteral("layout-list"), tr("Toggle sidebar"), bar);
    m_sidebarToggle->setCheckable(true);
    m_sidebarOptions = toolButton(QStringLiteral("chevron-down"), tr("Sidebar options"), bar);
    m_zoomOut = toolButton(QStringLiteral("zoom-out"), tr("Zoom out"), bar);
    m_zoomIn = toolButton(QStringLiteral("zoom-in"), tr("Zoom in"), bar);
    m_displayOptions = toolButton(QStringLiteral("chevron-down"), tr("Display options"), bar);
    m_pageInput = new QLineEdit(bar);
    m_pageInput->setObjectName(QStringLiteral("ocPdfPageInput"));
    m_pageInput->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont f = currentTheme()->interfaceFont(13);
    f.setFeature(QFont::Tag("tnum"), 1);
    m_pageInput->setFont(f);
    m_pageInput->setFixedWidth(int(std::ceil(QFontMetricsF(f).horizontalAdvance(QLatin1Char('0')) * 7)) + 4); // width: 7ch
    m_pageInput->installEventFilter(this);
    m_pageNumbers = uiLabel(QString(), 13, QStringLiteral("--text-normal"), bar);
    m_pageNumbers->setObjectName(QStringLiteral("ocPdfPageNumbers"));
    m_pageNumbers->setContentsMargins(0, 0, 4, 0);

    h->addWidget(m_sidebarToggle);
    h->addWidget(m_sidebarOptions);
    h->addSpacing(12 - 4); // .pdf-toolbar-spacer (12px) between two gaps
    h->addWidget(m_zoomOut);
    h->addWidget(new Divider(bar));
    h->addWidget(m_zoomIn);
    h->addWidget(m_displayOptions);
    h->addSpacing(12 - 4);
    h->addWidget(m_pageInput);
    h->addWidget(m_pageNumbers);
    h->addStretch(1);
    m_toolbarRight = new QWidget(bar);
    auto* rh = new QHBoxLayout(m_toolbarRight);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(4);
    h->addWidget(m_toolbarRight);

    connect(m_sidebarToggle, &QAbstractButton::clicked, this, [this] {
        toggleSidebar();
        updateToolbar();
    });
    connect(m_sidebarOptions, &QAbstractButton::clicked, this, [this] {
        QMenu* m = sidebarMenu(this);
        m->exec(m_sidebarOptions->mapToGlobal(QPoint(0, m_sidebarOptions->height())));
        delete m;
    });
    connect(m_displayOptions, &QAbstractButton::clicked, this, [this] {
        QMenu* m = displayMenu(this);
        m->exec(m_displayOptions->mapToGlobal(QPoint(0, m_displayOptions->height())));
        delete m;
    });
    connect(m_zoomIn, &QAbstractButton::clicked, this, [this] { m_pages->zoomIn(); });
    connect(m_zoomOut, &QAbstractButton::clicked, this, [this] { m_pages->zoomOut(); });
    connect(m_pageInput, &QLineEdit::returnPressed, this, &PdfViewer::commitPageInput);
    connect(m_pageInput, &QLineEdit::editingFinished, this, [this] {
        if (!m_pageInput->hasFocus())
            updateToolbar(); // abandoned edit: show the current page again
    });
}

void PdfViewer::setToolbarVisible(bool visible)
{
    m_toolbarVisible = visible;
    m_toolbar->setVisible(visible);
    layoutContent();
}

void PdfViewer::commitPageInput()
{
    if (!m_file)
        return;
    const QString text = m_pageInput->text().trimmed();
    int page = -1;
    if (m_file->hasPageLabels())
        page = m_file->pageForLabel(text);
    if (page < 0) {
        bool ok = false;
        const int n = text.toInt(&ok);
        if (ok && n >= 1 && n <= m_file->pageCount())
            page = n - 1;
    }
    if (page >= 0)
        m_pages->goToPage(page);
    updateToolbar();
    m_pages->setFocus();
}

void PdfViewer::updateToolbar()
{
    const int count = m_file ? m_file->pageCount() : 0;
    const int page = m_pages->currentPage();
    if (!m_pageInput->hasFocus()) {
        if (m_file && m_file->hasPageLabels())
            m_pageInput->setText(m_file->pageLabel(page));
        else
            m_pageInput->setText(count > 0 ? QString::number(page + 1) : QString());
    }
    if (m_file && m_file->hasPageLabels())
        m_pageNumbers->setText(tr("(%1 of %2)").arg(page + 1).arg(count));
    else
        m_pageNumbers->setText(tr("of %1").arg(count));
    m_zoomIn->setEnabled(m_pages->canZoomIn());
    m_zoomOut->setEnabled(m_pages->canZoomOut());
    m_sidebarToggle->setChecked(m_sidebarView != SidebarView::None);
}

QMenu* PdfViewer::sidebarMenu(QWidget* parent)
{
    auto* menu = new QMenu(parent);
    const bool thumbs = m_sidebarView == SidebarView::Thumbnails;
    const bool outline = m_sidebarView == SidebarView::Outline;
    QAction* a = menu->addAction(menuIcon(QStringLiteral("layout-grid")), tr("Thumbnails"), this, [this, thumbs] {
        setSidebarView(thumbs ? SidebarView::None : SidebarView::Thumbnails);
    });
    a->setCheckable(true);
    a->setChecked(thumbs);
    a = menu->addAction(menuIcon(QStringLiteral("list")), tr("Table of contents"), this, [this, outline] {
        setSidebarView(outline ? SidebarView::None : SidebarView::Outline);
    });
    a->setCheckable(true);
    a->setChecked(outline);
    a->setEnabled(hasOutline());
    menu->addSeparator();
    a = menu->addAction(menuIcon(QStringLiteral("list-start")), tr("Reveal page in table of contents"), this, &PdfViewer::revealInOutline);
    a->setEnabled(hasOutline());
    return menu;
}

QMenu* PdfViewer::displayMenu(QWidget* parent)
{
    auto* menu = new QMenu(parent);
    const QString scale = m_pages->scaleValue();
    auto check = [](QAction* a, bool on) {
        a->setCheckable(true);
        a->setChecked(on);
    };
    check(menu->addAction(menuIcon(QStringLiteral("move-horizontal")), tr("Fit width"), this,
                          [this] { m_pages->setScaleValue(QStringLiteral("page-width")); }),
          scale == u"page-width");
    check(menu->addAction(menuIcon(QStringLiteral("move-vertical")), tr("Fit height"), this,
                          [this] { m_pages->setScaleValue(QStringLiteral("page-height")); }),
          scale == u"page-height");
    menu->addSeparator();
    const auto spread = m_pages->spreadMode();
    check(menu->addAction(menuIcon(QStringLiteral("rectangle-vertical")), tr("Single page"), this,
                          [this] { m_pages->setSpreadMode(PdfPagesView::SpreadMode::None); }),
          spread == PdfPagesView::SpreadMode::None);
    check(menu->addAction(menuIcon(QStringLiteral("columns-2")), tr("Two-page (odd)"), this,
                          [this] { m_pages->setSpreadMode(PdfPagesView::SpreadMode::Odd); }),
          spread == PdfPagesView::SpreadMode::Odd);
    check(menu->addAction(menuIcon(QStringLiteral("columns-2")), tr("Two-page (even)"), this,
                          [this] { m_pages->setSpreadMode(PdfPagesView::SpreadMode::Even); }),
          spread == PdfPagesView::SpreadMode::Even);
    menu->addSeparator();
    check(menu->addAction(menuIcon(QStringLiteral("palette")), tr("Adapt to theme"), this, [this] { setThemed(!isThemed()); }), isThemed());
    return menu;
}

QMenu* PdfViewer::contextMenu(QWidget* parent, int page)
{
    auto* menu = new QMenu(parent);
    if (m_pages->hasSelection()) {
        QAction* copy = menu->addAction(menuIcon(QStringLiteral("copy")), tr("Copy"), m_pages, &PdfPagesView::copySelection);
        copy->setShortcut(QKeySequence::Copy);
    }
    if (pageLink && page >= 0) {
        const QString link = pageLink(page);
        if (!link.isEmpty())
            menu->addAction(menuIcon(QStringLiteral("link")), tr("Copy link to page %1").arg(page + 1), this, [link] {
                if (QClipboard* cb = QGuiApplication::clipboard())
                    cb->setText(link);
            });
    }
    return menu;
}

// ---------------------------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------------------------
void PdfViewer::loadOptions()
{
    const PdfOptions& o = PdfOptions::instance();
    m_pages->setThemed(o.themed);
    if (m_caseToggle)
        m_caseToggle->setChecked(o.search.caseSensitive);
    if (m_highlightAllToggle) {
        const QSignalBlocker b1(m_highlightAllToggle), b2(m_wholeWordToggle);
        m_highlightAllToggle->setChecked(o.search.highlightAll);
        m_wholeWordToggle->setChecked(o.search.entireWord);
    }
    m_search->setOptions(o.search);
    if (m_thumbs)
        m_thumbs->viewport()->update();
}

void PdfViewer::saveOptions() { PdfOptions::instance().save(); }

void PdfViewer::setThemed(bool themed)
{
    PdfOptions::instance().themed = themed;
    saveOptions();
}

bool PdfViewer::isThemed() const { return PdfOptions::instance().themed; }

// ---------------------------------------------------------------------------------------------
// find bar
// ---------------------------------------------------------------------------------------------
void PdfViewer::buildFindBar()
{
    auto* bar = new Panel(QStringLiteral("--background-primary"), QString(), Qt::BottomEdge, this);
    bar->setObjectName(QStringLiteral("ocPdfFindBar"));
    m_findBar = bar;
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(6, 6, 6, 6);
    h->setSpacing(4);
    h->addStretch(1); // right-aligned (row-reverse)
    m_findCount = uiLabel(QString(), 13, QStringLiteral("--text-muted"), bar);
    m_findCount->setObjectName(QStringLiteral("ocPdfFindCount"));
    QFont cf = m_findCount->font();
    cf.setItalic(true);
    m_findCount->setFont(cf);
    m_findCount->setContentsMargins(8, 0, 8, 0);
    m_findInput = new SearchInput(bar);
    m_findInput->setObjectName(QStringLiteral("ocPdfFindInput"));
    m_findInput->setPlaceholderText(tr("Search..."));
    m_findInput->setMinimumWidth(300);
    m_findInput->setRightReserve(28);
    m_findInput->installEventFilter(this);
    m_caseToggle = new IconButton(QStringLiteral("case-sensitive"), tr("Match case"), m_findInput);
    m_caseToggle->setIconSize(16);
    m_caseToggle->setButtonSize(QSize(24, 22));
    m_caseToggle->setCheckable(true);
    auto* prev = toolButton(QStringLiteral("arrow-up"), tr("Previous") + QStringLiteral("\nShift + F3"), bar);
    auto* next = toolButton(QStringLiteral("arrow-down"), tr("Next") + QStringLiteral("\nF3"), bar);
    auto* settings = toolButton(QStringLiteral("sliders-horizontal"), tr("Search settings"), bar);
    settings->setObjectName(QStringLiteral("ocPdfFindSettingsButton"));
    auto* close = toolButton(QStringLiteral("x"), tr("Close"), bar);
    h->addWidget(m_findCount);
    h->addWidget(m_findInput);
    h->addWidget(prev);
    h->addWidget(next);
    h->addWidget(settings);
    h->addWidget(new Divider(bar));
    h->addWidget(close);
    bar->hide();

    // settings popup: Highlight all / Whole words
    auto* popup = new SettingsPopup(this);
    popup->setObjectName(QStringLiteral("ocPdfFindSettings"));
    m_findSettings = popup;
    auto* pv = new QVBoxLayout(popup);
    pv->setContentsMargins(8, 4, 8, 4);
    pv->setSpacing(0);
    auto row = [&](const QString& name, ToggleSwitch** out) {
        auto* w = new QWidget(popup);
        auto* rh = new QHBoxLayout(w);
        rh->setContentsMargins(0, 4, 0, 4);
        rh->addWidget(uiLabel(name, 13, QStringLiteral("--text-normal"), w), 1);
        auto* t = new ToggleSwitch(w);
        rh->addWidget(t);
        pv->addWidget(w);
        *out = t;
    };
    row(tr("Highlight all"), &m_highlightAllToggle);
    row(tr("Whole words"), &m_wholeWordToggle);
    popup->setFixedWidth(200);
    popup->adjustSize();
    popup->hide();

    m_findDebounce.setSingleShot(true);
    m_findDebounce.setInterval(250);
    connect(&m_findDebounce, &QTimer::timeout, this, &PdfViewer::runSearch);
    connect(m_findInput, &QLineEdit::textChanged, this, [this] { m_findDebounce.start(); });
    connect(m_caseToggle, &QAbstractButton::clicked, this, [this](bool on) {
        PdfOptions::instance().search.caseSensitive = on;
        saveOptions();
    });
    connect(m_highlightAllToggle, &QAbstractButton::toggled, this, [this](bool on) {
        PdfOptions::instance().search.highlightAll = on;
        saveOptions();
    });
    connect(m_wholeWordToggle, &QAbstractButton::toggled, this, [this](bool on) {
        PdfOptions::instance().search.entireWord = on;
        saveOptions();
    });
    connect(prev, &QAbstractButton::clicked, m_search, &PdfSearch::previous);
    connect(next, &QAbstractButton::clicked, m_search, &PdfSearch::next);
    connect(close, &QAbstractButton::clicked, this, &PdfViewer::closeSearch);
    connect(settings, &QAbstractButton::clicked, this, [this, settings] {
        if (m_findSettings->isVisible()) {
            m_findSettings->hide();
            return;
        }
        const QPoint br = settings->mapTo(this, QPoint(settings->width(), settings->height()));
        m_findSettings->move(br.x() - m_findSettings->width(), br.y() + 6);
        m_findSettings->show();
        m_findSettings->raise();
    });
}

void PdfViewer::showSearch()
{
    if (m_findBar->isHidden()) {
        m_findBar->show();
        layoutContent();
    }
    m_findInput->setFocus(Qt::ShortcutFocusReason);
    m_findInput->selectAll();
    if (!m_findInput->text().isEmpty())
        runSearch();
}

void PdfViewer::closeSearch()
{
    if (m_findBar->isHidden())
        return;
    m_findBar->hide();
    m_findSettings->hide();
    m_search->setQuery(QString());
    layoutContent();
    m_pages->setFocus();
}

bool PdfViewer::isSearchOpen() const { return !m_findBar->isHidden(); }

void PdfViewer::runSearch()
{
    m_findDebounce.stop();
    m_search->setQuery(m_findInput->text(), m_pages->currentPage());
}

void PdfViewer::applySearchResults()
{
    m_pages->setHighlights(m_search->highlights(), m_search->current(), m_search->options().highlightAll);
    m_findCount->setText(m_search->countText());
    m_findCount->setVisible(!m_findCount->text().isEmpty()); // .pdf-find-results-count:empty
    m_findInput->setNoMatch(!m_search->query().isEmpty() && !m_search->isPending() && m_search->count() == 0);
}

// ---------------------------------------------------------------------------------------------
// sidebar
// ---------------------------------------------------------------------------------------------
void PdfViewer::buildSidebar()
{
    auto* side = new Panel(QStringLiteral("--pdf-sidebar-background"), QStringLiteral("--divider-color"), Qt::RightEdge, this);
    side->setObjectName(QStringLiteral("ocPdfSidebar"));
    m_sidebar = side;
    auto* v = new QVBoxLayout(side);
    v->setContentsMargins(0, 0, 1, 0);
    v->setSpacing(0);
    m_thumbs = new PdfThumbnails(m_pages, side);
    v->addWidget(m_thumbs);
    m_outline = new QTreeView(side);
    m_outline->setObjectName(QStringLiteral("ocPdfOutline"));
    m_outline->setHeaderHidden(true);
    m_outline->setFrameShape(QFrame::NoFrame);
    m_outline->setUniformRowHeights(true);
    m_outline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_outline->setFont(currentTheme()->interfaceFont(13));
    m_outline->setIndentation(14);
    auto styleOutline = [this] {
        const ThemePtr t = currentTheme();
        m_outline->setStyleSheet(QStringLiteral("QTreeView { background: transparent; color: %1; padding: 8px; }"
                                                "QTreeView::item { padding: 3px 4px; border-radius: 4px; }"
                                                "QTreeView::item:hover { background: %2; color: %3; }"
                                                "QTreeView::item:selected { background: %2; color: %3; }")
                                     .arg(t->color(QStringLiteral("--nav-item-color"), t->color(QStringLiteral("--text-muted"))).name(QColor::HexArgb),
                                          t->color(QStringLiteral("--background-modifier-hover")).name(QColor::HexArgb),
                                          t->color(QStringLiteral("--text-normal")).name(QColor::HexArgb)));
    };
    styleOutline();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, m_outline, styleOutline);
    m_bookmarks = new QPdfBookmarkModel(this);
    m_outline->setModel(m_bookmarks);
    v->addWidget(m_outline);
    connect(m_outline, &QAbstractItemView::clicked, this, [this](const QModelIndex& idx) {
        const int page = m_bookmarks->data(idx, int(QPdfBookmarkModel::Role::Page)).toInt();
        const QPointF loc = m_bookmarks->data(idx, int(QPdfBookmarkModel::Role::Location)).toPointF();
        m_pages->goToLocation(page, QPointF(-1, loc.y() > 0 ? loc.y() : -1));
    });
    side->hide();

    // the view's sidebar is resizable (.pdf-sidebar-resizer)
    class Resizer : public QWidget {
    public:
        explicit Resizer(PdfViewer* v)
            : QWidget(v)
            , viewer(v)
        {
            setCursor(Qt::SplitHCursor);
            setAttribute(Qt::WA_Hover);
        }
        PdfViewer* viewer;
        int startX = 0, startW = 0;

    protected:
        void paintEvent(QPaintEvent*) override
        {
            if (!underMouse())
                return;
            QPainter p(this);
            p.fillRect(rect(), currentTheme()->color(QStringLiteral("--divider-color-hover"), currentTheme()->color(QStringLiteral("--interactive-accent"))));
        }
        void enterEvent(QEnterEvent*) override { update(); }
        void leaveEvent(QEvent*) override { update(); }
        void mousePressEvent(QMouseEvent* e) override
        {
            startX = int(e->globalPosition().x());
            startW = viewer->m_sidebarWidth;
        }
        void mouseMoveEvent(QMouseEvent* e) override
        {
            if (!(e->buttons() & Qt::LeftButton))
                return;
            const int maxW = std::max(140, viewer->width() / 2);
            viewer->m_sidebarWidth = std::clamp(startW + int(e->globalPosition().x()) - startX, 140, maxW);
            viewer->layoutContent();
        }
    };
    m_resizer = new Resizer(this);
    m_resizer->hide();
}

bool PdfViewer::hasOutline() const { return m_bookmarks && m_bookmarks->rowCount() > 0; }

void PdfViewer::setSidebarView(SidebarView view)
{
    if (view == SidebarView::Outline && !hasOutline())
        view = SidebarView::Thumbnails;
    m_sidebarView = view;
    m_thumbs->setVisible(view == SidebarView::Thumbnails);
    m_outline->setVisible(view == SidebarView::Outline);
    m_sidebar->setVisible(view != SidebarView::None);
    if (view == SidebarView::Outline)
        m_outline->expandToDepth(0);
    layoutContent();
    updateToolbar();
    if (view == SidebarView::Thumbnails)
        m_thumbs->ensureCurrentVisible();
}

void PdfViewer::toggleSidebar()
{
    // PDF.js: toggling reopens the last view, thumbnails by default
    setSidebarView(m_sidebarView == SidebarView::None ? SidebarView::Thumbnails : SidebarView::None);
}

void PdfViewer::revealInOutline()
{
    if (!hasOutline())
        return;
    if (m_sidebarView != SidebarView::Outline)
        setSidebarView(SidebarView::Outline);
    // the last outline entry at or before the current page, depth first
    const int page = m_pages->currentPage();
    QModelIndex best;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m_bookmarks->rowCount(parent); ++r) {
            const QModelIndex idx = m_bookmarks->index(r, 0, parent);
            if (m_bookmarks->data(idx, int(QPdfBookmarkModel::Role::Page)).toInt() <= page)
                best = idx;
            walk(idx);
        }
    };
    walk(QModelIndex());
    if (best.isValid()) {
        m_outline->setCurrentIndex(best);
        m_outline->scrollTo(best);
    }
}

// ---------------------------------------------------------------------------------------------
// layout, keys
// ---------------------------------------------------------------------------------------------
void PdfViewer::layoutContent()
{
    const int w = width(), h = height();
    int y = 0;
    if (m_toolbarVisible && m_toolbar) {
        const int th = kToolbarHeight;
        m_toolbar->setGeometry(0, 0, w, th);
        y = th;
    }
    if (m_findBar && !m_findBar->isHidden()) {
        m_findBar->setGeometry(0, y, w, kFindBarHeight);
        y += kFindBarHeight; // .findbarOpen: the viewer and the sidebar move down
    }
    int sw = 0;
    if (m_sidebarView != SidebarView::None) {
        sw = m_mode == Mode::View ? m_sidebarWidth : (m_sidebarView == SidebarView::Outline ? kEmbedOutlineWidth : kEmbedThumbsWidth);
        sw = std::min(sw, w);
        m_sidebar->setGeometry(0, y, sw, h - y);
    }
    // the view's pages move right of the sidebar; in embeds the sidebar overlays them
    const int px = m_mode == Mode::View ? sw : 0;
    m_pages->setGeometry(px, y, w - px, h - y);
    if (m_sidebarView != SidebarView::None)
        m_sidebar->raise();
    if (m_mode == Mode::View && m_sidebarView != SidebarView::None) {
        m_resizer->setGeometry(sw - 2, y, 5, h - y);
        m_resizer->show();
        m_resizer->raise();
    } else {
        m_resizer->hide();
    }
    if (m_prompt && !m_prompt->isHidden()) {
        m_prompt->adjustSize();
        const QRect area(px, y, w - px, h - y);
        m_prompt->move(area.center() - QPoint(m_prompt->width() / 2, m_prompt->height() / 2));
        m_prompt->raise();
    }
    if (m_findSettings && m_findSettings->isVisible())
        m_findSettings->raise();
}

void PdfViewer::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutContent();
}

void PdfViewer::updatePasswordPrompt()
{
    const bool locked = m_file && m_file->needsPassword();
    const bool failed = m_file && !locked && m_file->error() != QPdfDocument::Error::None;
    if (auto* lock = m_prompt->findChild<QLabel*>(QStringLiteral("ocPdfLock")))
        lock->setPixmap(IconProvider::instance().pixmap(locked ? QStringLiteral("lock") : QStringLiteral("file-x"), 32,
                                                        currentTheme()->color(QStringLiteral("--text-muted")), 1.75, devicePixelRatioF()));
    if (locked) {
        const bool retry = !m_password->text().isEmpty();
        m_promptTitle->setText(tr("This PDF is password protected"));
        m_promptHint->setText(retry ? tr("Invalid password") : tr("Enter the password below"));
        m_password->show();
    } else if (failed) {
        m_promptTitle->setText(tr("Failed to load PDF"));
        m_promptHint->setText(QFileInfo(m_file->path()).fileName());
        m_password->hide();
    }
    m_prompt->setVisible(locked || failed);
    m_pages->setVisible(!(locked || failed));
    if (!locked)
        m_password->clear();
    layoutContent();
}

void PdfViewer::keyPressEvent(QKeyEvent* e)
{
    if (e->matches(QKeySequence::Find)) {
        showSearch();
        return;
    }
    if (e->key() == Qt::Key_Escape && isSearchOpen()) {
        closeSearch();
        return;
    }
    if (isSearchOpen() && (e->key() == Qt::Key_F3 || (e->key() == Qt::Key_G && (e->modifiers() & Qt::ControlModifier)))) {
        if (e->modifiers() & Qt::ShiftModifier)
            m_search->previous();
        else
            m_search->next();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool PdfViewer::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_findInput && ev->type() == QEvent::Resize) {
        m_caseToggle->move(m_findInput->width() - m_caseToggle->width() - 3, (m_findInput->height() - m_caseToggle->height()) / 2);
    } else if (obj == m_findInput && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const bool shift = ke->modifiers() & Qt::ShiftModifier;
        switch (ke->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_findDebounce.isActive() || m_search->query() != m_findInput->text())
                runSearch();
            else if (shift)
                m_search->previous();
            else
                m_search->next();
            return true;
        case Qt::Key_F3:
            shift ? m_search->previous() : m_search->next();
            return true;
        case Qt::Key_G:
            if (ke->modifiers() & Qt::ControlModifier) {
                shift ? m_search->previous() : m_search->next();
                return true;
            }
            break;
        case Qt::Key_Escape:
            closeSearch();
            return true;
        default:
            break;
        }
    } else if (obj == m_pageInput && ev->type() == QEvent::MouseButtonRelease) {
        m_pageInput->selectAll(); // Obsidian selects the number on click
    } else if (obj == m_pageInput && ev->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        updateToolbar();
        m_pages->setFocus();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

void PdfViewer::applySubpath(const QString& subpath)
{
    if (subpath.isEmpty() || !m_file || !m_file->isLoaded())
        return;
    EmbedRequest r;
    r.subpath = subpath;
    if (!r.hasParam(QStringLiteral("page")))
        return; // named destinations are not supported by QtPdf
    const int page = std::clamp(r.intParam(QStringLiteral("page"), 1), 1, m_file->pageCount()) - 1;
    const QStringList offset = r.param(QStringLiteral("offset")).split(QLatin1Char(','));
    auto num = [&](int i) -> std::optional<double> {
        bool ok = false;
        const double v = offset.value(i).toDouble(&ok);
        return ok ? std::optional<double>(v) : std::nullopt;
    };
    const QSizeF pts = m_file->pagePointSize(page);
    if (const auto zoom = num(2)) {
        // XYZ destination: left, top (PDF user space, bottom-up), zoom
        if (*zoom > 0)
            m_pages->setScaleValue(QString::number(*zoom > 10 ? *zoom / 100 : *zoom));
        m_pages->goToLocation(page, QPointF(num(0).value_or(-1), num(1) ? pts.height() - *num(1) : -1));
    } else {
        // FitBH: fit the width, top at `offset[1]` if given
        m_pages->setScaleValue(QStringLiteral("page-width"));
        if (const auto y = num(1))
            m_pages->goToLocation(page, QPointF(-1, pts.height() - *y));
        else
            m_pages->goToPage(page);
    }
    updateToolbar();
}

// ---------------------------------------------------------------------------------------------
// PdfThumbnails
// ---------------------------------------------------------------------------------------------
namespace {
constexpr int kThumbPadding = 12; // .pdf-thumbnail-view padding --size-4-3
constexpr int kThumbBorder = 8;   // .thumbnail border --size-4-2
constexpr int kThumbGap = 4;      // gap --size-4-1
} // namespace

PdfThumbnails::PdfThumbnails(PdfPagesView* pages, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_pagesView(pages)
{
    setObjectName(QStringLiteral("ocPdfThumbnails"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, viewport(), qOverload<>(&QWidget::update));
}

void PdfThumbnails::setFile(std::shared_ptr<PdfFile> file)
{
    disconnect(m_renderedConn);
    m_file = std::move(file);
    if (m_file)
        m_renderedConn = connect(m_file.get(), &PdfFile::rendered, viewport(), qOverload<>(&QWidget::update));
    relayout();
}

void PdfThumbnails::relayout()
{
    m_rects.clear();
    const int count = m_file ? m_file->pageCount() : 0;
    const int avail = std::max(1, viewport()->width() - 2 * kThumbPadding);
    const int itemW = kThumbWidth + 2 * kThumbBorder;
    const int perRow = std::max(1, (avail + kThumbGap) / (itemW + kThumbGap));
    int y = kThumbPadding;
    for (int i = 0; i < count; i += perRow) {
        const int n = std::min(perRow, count - i);
        int rowH = 0;
        int x = kThumbPadding + (avail - (n * itemW + (n - 1) * kThumbGap)) / 2 + kThumbBorder;
        for (int k = 0; k < n; ++k) {
            const QSizeF pts = m_file->pagePointSize(i + k);
            const int h = std::max(1, int(std::lround(kThumbWidth * pts.height() / std::max(1.0, pts.width()))));
            m_rects.append(QRect(x, y + kThumbBorder, kThumbWidth, h));
            rowH = std::max(rowH, h + 2 * kThumbBorder);
            x += itemW + kThumbGap;
        }
        y += rowH + kThumbGap;
    }
    m_contentHeight = y - kThumbGap + kThumbPadding;
    verticalScrollBar()->setRange(0, std::max(0, m_contentHeight - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(40);
    viewport()->update();
}

QRect PdfThumbnails::thumbnailRect(int page) const
{
    return page >= 0 && page < m_rects.size() ? m_rects[page].translated(0, -verticalScrollBar()->value()) : QRect();
}

void PdfThumbnails::ensureCurrentVisible()
{
    const QRect r = thumbnailRect(m_pagesView->currentPage());
    if (r.isNull())
        return;
    if (r.top() - kThumbBorder < 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top() - kThumbBorder - kThumbPadding);
    else if (r.bottom() + kThumbBorder > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() + kThumbBorder + kThumbPadding - viewport()->height());
}

void PdfThumbnails::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
}

void PdfThumbnails::scrollContentsBy(int, int) { viewport()->update(); }

void PdfThumbnails::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const ThemePtr t = currentTheme();
    p.fillRect(viewport()->rect(), t->color(QStringLiteral("--pdf-sidebar-background"), t->color(QStringLiteral("--background-primary"))));
    if (!m_file)
        return;
    const double dpr = devicePixelRatioF();
    const bool themed = m_pagesView->isThemed() && t->isDark();
    const QColor pageBg = t->color(QStringLiteral("--pdf-page-background"), t->color(QStringLiteral("--background-primary")));
    const int current = m_pagesView->currentPage();
    const double radius = t->px(QStringLiteral("--radius-s"), -1, 4);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_rects.size(); ++i) {
        const QRect r = thumbnailRect(i);
        if (r.bottom() + kThumbBorder < 0 || r.top() - kThumbBorder > viewport()->height())
            continue;
        const QRectF outer = QRectF(r).adjusted(-kThumbBorder, -kThumbBorder, kThumbBorder, kThumbBorder);
        if (i == current || i == m_hover) { // border-color: --background-modifier-hover
            QPainterPath path;
            path.addRoundedRect(outer, radius, radius);
            QPainterPath inner;
            inner.addRect(QRectF(r));
            p.fillPath(path.subtracted(inner), t->color(QStringLiteral("--background-modifier-hover")));
        }
        // ::before box-shadow: --pdf-thumbnail-shadow
        if (t->isDark()) {
            p.fillRect(QRectF(r).adjusted(-1, -1, 1, 1), t->color(QStringLiteral("--background-modifier-border")));
        } else {
            for (int k = 1; k <= 3; ++k) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 12 - 3 * k));
                p.drawRoundedRect(QRectF(r).translated(0, 2).adjusted(-k, -k, k, k), k, k);
            }
            p.fillRect(QRectF(r).adjusted(-1, -1, 1, 1), QColor(0, 0, 0, 38));
        }
        p.fillRect(r, themed ? pageBg : QColor(Qt::white));
        const int px = int(std::lround(kThumbWidth * dpr));
        QImage img = m_file->cachedImage(i, px, themed, pageBg.rgb());
        if (img.isNull()) {
            m_file->requestImage(i, px, themed, pageBg.rgb());
            img = m_file->anyImage(i, themed);
        }
        if (!img.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRectF(r), img);
        }
        // ::after page label: bottom/right --size-4-1, --background-secondary, radius 3, 12px muted
        const QString label = m_file->pageLabel(i);
        QFont f = t->interfaceFont(12);
        f.setFeature(QFont::Tag("tnum"), 1);
        p.setFont(f);
        const QFontMetricsF fm(f);
        const QSizeF ls(fm.horizontalAdvance(label) + 8, fm.height() * 0 + 12 + 4);
        const QRectF lr(r.right() + 1 - 4 - ls.width(), r.bottom() + 1 - 4 - ls.height(), ls.width(), ls.height());
        p.setPen(Qt::NoPen);
        p.setBrush(t->color(QStringLiteral("--background-secondary")));
        p.drawRoundedRect(lr, 3, 3);
        p.setPen(t->color(QStringLiteral("--text-muted")));
        p.drawText(lr, Qt::AlignCenter, label);
    }
}

int PdfThumbnails::pageAt(const QPoint& pos) const
{
    for (int i = 0; i < m_rects.size(); ++i)
        if (thumbnailRect(i).adjusted(-kThumbBorder, -kThumbBorder, kThumbBorder, kThumbBorder).contains(pos))
            return i;
    return -1;
}

void PdfThumbnails::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int page = pageAt(e->position().toPoint());
    if (page >= 0) {
        m_pagesView->goToPage(page);
        viewport()->update();
    }
}

void PdfThumbnails::mouseMoveEvent(QMouseEvent* e)
{
    const int page = pageAt(e->position().toPoint());
    if (page != m_hover) {
        m_hover = page;
        viewport()->update();
    }
}

bool PdfThumbnails::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && m_hover >= 0) {
        m_hover = -1;
        viewport()->update();
    }
    return QAbstractScrollArea::viewportEvent(e);
}

} // namespace oc::media

#include "pdf_viewer.moc"
