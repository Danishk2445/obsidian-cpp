#include "ui/views/file_explorer/file_explorer_view.h"

#include "core/commands/command_registry.h"
#include "core/config/clone_paths.h"
#include "core/config/json_file.h"
#include "core/config/vault_config.h"
#include "core/files/file_manager.h"
#include "core/util/owned_menu.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace_p.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace oc {

namespace {

// Views that accept dragged vault files themselves expose "bool ocFileDrag(QStringList paths, QPoint
// globalPos, bool drop)" (e.g. the canvas: files become cards). The explorer's drags are not QDrags, so
// they are offered through this invokable instead of drop events.
constexpr const char* kFileDragSignature = "ocFileDrag(QStringList,QPoint,bool)";

QObject* fileDropTargetAt(const QPoint& globalPos)
{
    QWidget* top = QApplication::topLevelAt(globalPos);
    QWidget* w = top ? top->childAt(top->mapFromGlobal(globalPos)) : nullptr;
    for (; w; w = w->parentWidget())
        if (w->metaObject()->indexOfMethod(kFileDragSignature) >= 0)
            return w;
    return nullptr;
}

bool offerFileDrop(QObject* target, const QList<TAbstractFile*>& items, const QPoint& globalPos, bool drop)
{
    QStringList paths;
    for (const TAbstractFile* i : items)
        paths << i->path();
    bool accepted = false;
    QMetaObject::invokeMethod(target, "ocFileDrag", Qt::DirectConnection, qReturnArg(accepted), paths, globalPos, drop);
    return accepted;
}

QColor tok(const ThemePtr& t, const char* name, const QColor& fallback = {})
{
    return t->color(QString::fromLatin1(name), fallback);
}

QIcon menuIcon(const QString& name, bool warning = false)
{
    if (!warning)
        return IconProvider::instance().icon(name, 16, 2.0);
    const QColor red = currentTheme()->color(QStringLiteral("--text-error"), QColor(0xfb, 0x46, 0x4c));
    QIcon ic;
    for (qreal dpr : {1.0, 2.0})
        ic.addPixmap(IconProvider::instance().pixmap(name, 16, red, 2.0, dpr));
    return ic;
}

// Extensions Obsidian opens itself (so they're "supported" even before this clone has a view for them).
bool isObsidianNative(const QString& ext)
{
    static const QSet<QString> native{
        QStringLiteral("md"),   QStringLiteral("canvas"), QStringLiteral("base"), QStringLiteral("pdf"),  QStringLiteral("bmp"),
        QStringLiteral("png"),  QStringLiteral("jpg"),    QStringLiteral("jpeg"), QStringLiteral("gif"),  QStringLiteral("svg"),
        QStringLiteral("webp"), QStringLiteral("avif"),   QStringLiteral("mp3"),  QStringLiteral("wav"),  QStringLiteral("m4a"),
        QStringLiteral("3gp"),  QStringLiteral("flac"),   QStringLiteral("ogg"),  QStringLiteral("oga"),  QStringLiteral("opus"),
        QStringLiteral("mp4"),  QStringLiteral("webm"),   QStringLiteral("ogv"),  QStringLiteral("mov"),  QStringLiteral("mkv"),
    };
    return native.contains(ext);
}

constexpr const char* kFoldKey = "file-explorer-unfold";

// Obsidian's menu sections, in order; items keep their insertion order within a section.
const QStringList kSections{QStringLiteral("title"), QStringLiteral("open"), QStringLiteral("action-primary"), QStringLiteral("action"),
                            QStringLiteral("info"),  QStringLiteral("info.copy"), QStringLiteral("view"), QStringLiteral("system"),
                            QString(),               QStringLiteral("danger")};

struct MenuItem {
    QString section;
    QString title;
    QString icon;
    std::function<void()> run;
    bool warning = false;
    bool enabled = true;
};

void buildMenu(QMenu* menu, const QList<MenuItem>& items, const QString& submenuSection, const QString& submenuTitle, const QString& submenuIcon)
{
    bool first = true;
    for (const QString& section : kSections) {
        QList<const MenuItem*> in;
        for (const MenuItem& m : items)
            if (m.section == section)
                in << &m;
        if (in.isEmpty())
            continue;
        if (!first)
            menu->addSeparator();
        first = false;
        QMenu* target = menu;
        if (section == submenuSection) {
            target = menu->addMenu(menuIcon(submenuIcon), submenuTitle);
        }
        for (const MenuItem* m : in) {
            QAction* a = target->addAction(menuIcon(m->icon, m->warning), m->title);
            a->setEnabled(m->enabled);
            a->setProperty("ocWarning", m->warning);
            auto run = m->run;
            QObject::connect(a, &QAction::triggered, menu, [run] {
                if (run)
                    run();
            });
        }
    }
}

// The session's vault from a view.
Vault* vaultOf(const View* v)
{
    VaultSession* s = v ? v->session() : nullptr;
    return s ? s->vault() : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// NavActionButton
// ---------------------------------------------------------------------------------------------

NavActionButton::NavActionButton(const QString& icon, const QString& tooltip, QWidget* parent)
    : QAbstractButton(parent)
    , m_icon(icon)
{
    setToolTip(tooltip);
    setAccessibleName(tooltip);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setFixedSize(sizeHint());
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void NavActionButton::setIconName(const QString& icon)
{
    m_icon = icon;
    update();
}

void NavActionButton::setActive(bool active)
{
    m_active = active;
    update();
}

void NavActionButton::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void NavActionButton::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void NavActionButton::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool hover = m_hover && isEnabled();
    const bool active = m_active || isDown() || (isCheckable() && isChecked());
    if (hover || active) {
        QPainterPath path;
        const double r = t->px(QStringLiteral("--clickable-icon-radius"), -1, 4);
        path.addRoundedRect(QRectF(rect()), r, r);
        p.fillPath(path, tok(t, "--background-modifier-hover"));
    }
    QColor fg = tok(t, "--icon-color");
    if (!isEnabled())
        fg = tok(t, "--text-muted");
    else if (active)
        fg = tok(t, "--icon-color-focused");
    else if (hover)
        fg = tok(t, "--icon-color-hover");
    double opacity = (hover || active) ? t->number(QStringLiteral("--icon-opacity-hover"), 1) : t->number(QStringLiteral("--icon-opacity"), 0.85);
    if (!isEnabled())
        opacity = 0.4; // [aria-disabled]
    p.setOpacity(opacity);
    const int px = int(t->px(QStringLiteral("--icon-size"), -1, 18));
    const double stroke = t->px(QStringLiteral("--icon-stroke"), -1, 1.75);
    const QPixmap pm = IconProvider::instance().pixmap(m_icon, px, fg, stroke, devicePixelRatioF());
    p.drawPixmap(QPointF((width() - px) / 2.0, (height() - px) / 2.0), pm);
}

// ---------------------------------------------------------------------------------------------
// FileExplorerView
// ---------------------------------------------------------------------------------------------

FileExplorerView::FileExplorerView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
    , m_tree(new FileTree(this))
    , m_header(new QWidget(this))
{
    setObjectName(QStringLiteral("ocFileExplorer"));

    // .nav-header (8 px padding) > .nav-buttons-container (centered, 2 px gap)
    auto* h = new QHBoxLayout(m_header);
    h->setContentsMargins(8, 8, 8, 8);
    h->setSpacing(2);
    h->addStretch(1);
    m_newNote = new NavActionButton(QStringLiteral("edit"), tr("New note"), m_header);
    m_newFolder = new NavActionButton(QStringLiteral("folder-plus"), tr("New folder"), m_header);
    m_sort = new NavActionButton(QStringLiteral("sort-asc"), tr("Change sort order"), m_header);
    m_autoRevealButton = new NavActionButton(QStringLiteral("gallery-vertical"), tr("Auto-reveal current file"), m_header);
    m_collapseAll = new NavActionButton(QStringLiteral("chevrons-up-down"), tr("Expand all"), m_header);
    for (QWidget* b : {static_cast<QWidget*>(m_newNote), static_cast<QWidget*>(m_newFolder), static_cast<QWidget*>(m_sort),
                       static_cast<QWidget*>(m_autoRevealButton), static_cast<QWidget*>(m_collapseAll)})
        h->addWidget(b);
    h->addStretch(1);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_header);
    v->addWidget(m_tree, 1);

    connect(m_newNote, &QAbstractButton::clicked, this, [this] { createNewNote(QApplication::keyboardModifiers()); });
    connect(m_newFolder, &QAbstractButton::clicked, this, [this] { createFolderIn(nullptr); });
    connect(m_sort, &QAbstractButton::clicked, this, &FileExplorerView::showSortMenu);
    connect(m_autoRevealButton, &QAbstractButton::clicked, this, [this] {
        setAutoReveal(!m_autoReveal);
        if (m_autoReveal)
            revealActiveFile();
        emit stateChanged();
    });
    connect(m_collapseAll, &QAbstractButton::clicked, this, [this] { m_tree->toggleAllCollapsed(); });
    connect(m_tree, &FileTree::allCollapsedChanged, this, &FileExplorerView::updateCollapseButton);
    m_foldSave.setSingleShot(true);
    m_foldSave.setInterval(200);
    connect(&m_foldSave, &QTimer::timeout, this, &FileExplorerView::saveFolds);
    connect(m_tree, &FileTree::expansionChanged, this, [this] {
        if (m_foldsLoaded)
            m_foldSave.start();
    });

    FileTree::Handlers& hd = m_tree->handlers();
    hd.open = [this](TFile* f, Workspace::NewLeaf mode, bool nav) { openFile(f, mode, nav); };
    hd.contextMenu = [this](TAbstractFile* item, const QPoint& g) { showContextMenu(item, g); };
    hd.validateName = [this](TAbstractFile* item, const QString& name, bool final) {
        return validateFileName(vaultOf(this), item, name, final);
    };
    hd.rename = [this](TAbstractFile* item, const QString& name, QString* err) {
        VaultSession* s = session();
        Vault* vault = s ? s->vault() : nullptr;
        if (!vault)
            return false;
        const QString path = pathAfterRename(item, name);
        if (path == item->path())
            return true;
        FileManager* fm = s->fileManager();
        return fm ? fm->renameFile(item, path, err) : vault->rename(item, path, err);
    };
    hd.remove = [this](const QList<TAbstractFile*>& items) { deleteItems(items); };
    hd.move = [this](const QList<TAbstractFile*>& items, TFolder* target) { moveItems(items, target); };
    hd.dragOutsideMove = [this](const QList<TAbstractFile*>& items, const QPoint& g) {
        // A view that takes file drops itself (a canvas: new file cards) comes before the workspace's
        // "open in a split" targets.
        if (QObject* target = fileDropTargetAt(g); target && offerFileDrop(target, items, g, false)) {
            dropOnWorkspace({}, {}, false);
            return true;
        }
        dropOnWorkspace(items, g, false);
        return !m_workspaceTarget.isNull();
    };
    hd.dragOutsideDrop = [this](const QList<TAbstractFile*>& items, const QPoint& g) {
        if (QObject* target = fileDropTargetAt(g); target && offerFileDrop(target, items, g, true)) {
            dropOnWorkspace({}, {}, false);
            return true;
        }
        dropOnWorkspace(items, g, true);
        return true;
    };
    hd.dragOutsideLeave = [this] { dropOnWorkspace({}, {}, false); };
    hd.activate = [this] {
        if (Workspace* ws = workspace(); ws && this->leaf() && ws->activeLeaf() != this->leaf())
            ws->setActiveLeaf(this->leaf(), false);
        if (!m_tree->hasFocus() && !m_tree->renameEditor()->hasFocus())
            m_tree->setFocus(Qt::OtherFocusReason);
    };
    hd.copy = [this](const QList<TAbstractFile*>& items, bool cut) { copyToClipboard(items, cut); };
    hd.paste = [this](TFolder* target) { pasteInto(target); };
    hd.importFiles = [this](const QList<QUrl>& urls, TFolder* target) {
        const QList<TAbstractFile*> created = oc::importFiles(session(), urls, target);
        if (!created.isEmpty()) {
            m_tree->setSelectedItems(created);
            m_tree->reveal(created.first());
        }
    };
    // Page previews (Obsidian's hover-link source "file-explorer": with Mod by default).
    hd.hover = [this](TFile* f, const QRect& globalRect) {
        if (MainWindow* w = sessionWindow(session()))
            PagePreview::of(w)->onLinkHover(QStringLiteral("file-explorer"), f->path(), QString(), globalRect, m_tree->viewport());
    };
    hd.unhover = [this] {
        if (MainWindow* w = sessionWindow(session()))
            PagePreview::of(w)->onLinkUnhover(m_tree->viewport());
    };
    m_tree->setSupportedPredicate([this](const QString& ext) { return isSupported(ext); });
    attach();
}

FileExplorerView::~FileExplorerView()
{
    if (m_foldSave.isActive())
        saveFolds();
    dropOnWorkspace({}, {}, false);
}

QString FileExplorerView::displayText() const { return tr("Files"); }

QAbstractButton* FileExplorerView::newNoteButton() const { return m_newNote; }
QAbstractButton* FileExplorerView::newFolderButton() const { return m_newFolder; }
QAbstractButton* FileExplorerView::sortButton() const { return m_sort; }
QAbstractButton* FileExplorerView::autoRevealButton() const { return m_autoRevealButton; }
QAbstractButton* FileExplorerView::collapseAllButton() const { return m_collapseAll; }

void FileExplorerView::attach()
{
    if (m_attached)
        return;
    VaultSession* s = session();
    Workspace* ws = workspace();
    if (!s || !s->vault())
        return;
    m_attached = true;
    Vault* vault = s->vault();
    m_tree->setVault(vault);
    if (VaultConfig* cfg = s->vaultConfig()) {
        m_tree->setShowUnsupported(cfg->flag(QStringLiteral("showUnsupportedFiles")));
        m_connections << connect(cfg, &VaultConfig::configChanged, this, [this, cfg](const QString& key) {
            if (key == u"showUnsupportedFiles")
                m_tree->setShowUnsupported(cfg->flag(key));
        });
    }
    m_connections << connect(vault, &Vault::onlineChanged, this, [this](bool online) { updateReadOnly(!online); });
    m_connections << connect(vault, &Vault::loaded, this, [this] { loadFolds(); });
    if (ws) {
        m_connections << connect(ws, &Workspace::fileOpened, this, [this](TFile* f) { onFileOpen(f); });
        m_connections << connect(ws, &Workspace::activeLeafChanged, this, [this](WorkspaceLeaf* l) { m_tree->setLeafActive(l && l == leaf()); });
    }
    updateReadOnly(!vault->isOnline());
    loadFolds();
}

void FileExplorerView::onOpen()
{
    attach();
    if (Workspace* ws = workspace()) {
        m_tree->setLeafActive(ws->activeLeaf() == leaf());
        onFileOpen(workspaceActiveFile(ws));
    }
}

void FileExplorerView::onClose()
{
    if (m_foldSave.isActive() || m_foldsLoaded)
        saveFolds();
    m_foldSave.stop();
    m_tree->cancelDrag();
    m_tree->cancelRename();
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
}

void FileExplorerView::focusContent() { m_tree->setFocus(Qt::OtherFocusReason); }

bool FileExplorerView::isSupported(const QString& ext) const
{
    if (isObsidianNative(ext))
        return true;
    VaultSession* s = session();
    ViewRegistry* reg = s ? s->viewRegistry() : nullptr;
    if (!reg && workspace())
        reg = workspace()->viewRegistry();
    return reg && reg->isExtensionRegistered(ext);
}

void FileExplorerView::updateReadOnly(bool offline)
{
    m_tree->setReadOnly(offline);
    m_newNote->setEnabled(!offline);
    m_newFolder->setEnabled(!offline);
}

void FileExplorerView::updateCollapseButton()
{
    if (m_tree->isAllCollapsed()) {
        m_collapseAll->setIconName(QStringLiteral("chevrons-up-down"));
        m_collapseAll->setToolTip(tr("Expand all"));
    } else {
        m_collapseAll->setIconName(QStringLiteral("chevrons-down-up"));
        m_collapseAll->setToolTip(tr("Collapse all"));
    }
}

// ---- state ------------------------------------------------------------------------------------------

QJsonObject FileExplorerView::state() const
{
    return QJsonObject{{QStringLiteral("sortOrder"), sortOrderId(m_tree->sortOrder())},
                       {QStringLiteral("autoReveal"), m_autoReveal},
                       {QStringLiteral("showSearch"), m_showSearch},
                       {QStringLiteral("searchQuery"), m_searchQuery}};
}

void FileExplorerView::setState(const QJsonObject& st)
{
    if (st.value(QStringLiteral("autoReveal")).isBool())
        setAutoReveal(st.value(QStringLiteral("autoReveal")).toBool());
    if (auto order = sortOrderFromId(st.value(QStringLiteral("sortOrder")).toString()))
        m_tree->setSortOrder(*order);
    if (st.value(QStringLiteral("showSearch")).isBool())
        m_showSearch = st.value(QStringLiteral("showSearch")).toBool();
    if (st.value(QStringLiteral("searchQuery")).isString())
        m_searchQuery = st.value(QStringLiteral("searchQuery")).toString();
    if (st.value(QStringLiteral("newFile")).isString()) {
        attach();
        if (Vault* vault = vaultOf(this))
            if (TAbstractFile* item = vault->item(st.value(QStringLiteral("newFile")).toString()))
                afterCreate(item, Workspace::NewLeaf::No);
    }
}

ExplorerSortOrder FileExplorerView::sortOrder() const { return m_tree->sortOrder(); }

void FileExplorerView::setSortOrder(ExplorerSortOrder order)
{
    if (order == m_tree->sortOrder())
        return;
    m_tree->setSortOrder(order);
    if (m_autoReveal && !m_tree->renamingItem())
        revealActiveFile();
    emit stateChanged();
}

void FileExplorerView::setAutoReveal(bool on)
{
    m_autoReveal = on;
    m_autoRevealButton->setActive(on);
}

// ---- folds ------------------------------------------------------------------------------------------

QString FileExplorerView::foldsFile() const
{
    VaultSession* s = session();
    if (!s || s->id().isEmpty())
        return {};
    return ClonePaths::vaultDataDir(s->id()) + QStringLiteral("/file-explorer.json");
}

void FileExplorerView::loadFolds()
{
    Vault* vault = vaultOf(this);
    if (!vault || !vault->isLoaded() || m_foldsLoaded)
        return;
    m_foldsLoaded = true;
    const QString file = foldsFile();
    if (file.isEmpty())
        return;
    const QJsonObject o = JsonFile::read(file);
    QStringList paths;
    for (const QJsonValue& v : o.value(QString::fromLatin1(kFoldKey)).toArray())
        paths << v.toString();
    m_tree->setExpandedPaths(paths);
    updateCollapseButton();
}

void FileExplorerView::saveFolds()
{
    m_foldSave.stop();
    const QString file = foldsFile();
    if (file.isEmpty() || !m_foldsLoaded)
        return;
    QJsonArray arr;
    for (const QString& p : m_tree->expandedPaths())
        arr.append(p);
    QJsonObject o = JsonFile::read(file);
    if (o.value(QString::fromLatin1(kFoldKey)).toArray() == arr)
        return;
    o.insert(QString::fromLatin1(kFoldKey), arr);
    JsonFile::write(file, o); // skipped when unchanged; creates the data dir
}

// ---- active file ------------------------------------------------------------------------------------

void FileExplorerView::onFileOpen(TFile* file)
{
    m_tree->setActiveFile(file);
    if (m_autoReveal && file)
        revealActiveFile();
}

void FileExplorerView::revealActiveFile()
{
    if (TFile* f = m_tree->activeFile())
        m_tree->reveal(f);
}

void FileExplorerView::revealInFolder(TAbstractFile* item)
{
    if (!item)
        return;
    m_tree->reveal(item);
    m_tree->setFocusedItem(item, false);
    m_tree->scrollToItem(item, 4);
    m_tree->flash(item);
    if (Workspace* ws = workspace()) {
        ws->revealLeaf(leaf());
        ws->setActiveLeaf(leaf(), true);
    }
}

// ---- actions ----------------------------------------------------------------------------------------

void FileExplorerView::openFile(TFile* file, Workspace::NewLeaf mode, bool keyboardNav)
{
    Workspace* ws = workspace();
    Vault* vault = vaultOf(this);
    if (!ws || !file)
        return;
    ViewRegistry* reg = ws->viewRegistry();
    if (!reg || reg->typeForExtension(file->extension().toLower()).isEmpty()) {
        openWithDefaultApp(vault, file); // Obsidian hands files it can't show to the system
        return;
    }
    if (keyboardNav) {
        // Ctrl+↑/↓: show it in the current tab without leaving the explorer.
        if (WorkspaceLeaf* l = ws->getLeaf(Workspace::NewLeaf::No)) {
            l->openFile(file);
            m_tree->setActiveFile(file);
        }
        return;
    }
    ws->openFile(file, mode);
}

TFile* FileExplorerView::createNewNote(Qt::KeyboardModifiers mods)
{
    VaultSession* s = session();
    Workspace* ws = workspace();
    if (!s || !s->vault() || !s->vault()->isOnline() || m_tree->isReadOnly())
        return nullptr;
    TFile* active = workspaceActiveFile(ws);
    FileManager* fm = s->fileManager();
    TFolder* parent = fm ? fm->newFileParent(active ? active->path() : QString()) : s->vault()->rootFolder();
    Workspace::NewLeaf mode = Workspace::NewLeaf::Tab; // Obsidian: isModEvent(e) || "tab"
    if (mods & Qt::ControlModifier)
        mode = (mods & Qt::AltModifier) ? Workspace::NewLeaf::SplitRight : Workspace::NewLeaf::Tab;
    return createNoteIn(parent, mode);
}

TFile* FileExplorerView::createNoteIn(TFolder* folder, Workspace::NewLeaf mode)
{
    VaultSession* s = session();
    if (!s || !s->vault())
        return nullptr;
    QString err;
    FileManager* fm = s->fileManager();
    TFile* f = fm ? fm->createNewMarkdownFile(folder, {}, {}, &err)
                  : s->vault()->createFile(s->vault()->availablePath(folder ? folder->path() : QString(), tr("Untitled"), QStringLiteral("md")), {}, &err);
    if (!f) {
        showNotice(s, err);
        return nullptr;
    }
    afterCreate(f, mode);
    return f;
}

TFolder* FileExplorerView::createFolderIn(TFolder* parent)
{
    VaultSession* s = session();
    if (!s || !s->vault() || !s->vault()->isOnline() || m_tree->isReadOnly())
        return nullptr;
    QString err;
    TFolder* folder = createNewFolder(s, parent, &err);
    if (!folder) {
        showNotice(s, err);
        return nullptr;
    }
    afterCreate(folder, Workspace::NewLeaf::No);
    return folder;
}

void FileExplorerView::afterCreate(TAbstractFile* item, Workspace::NewLeaf mode)
{
    if (!item)
        return;
    if (!item->isFolder()) {
        openNewFile(workspace(), static_cast<TFile*>(item), mode);
        return;
    }
    // Next frame (menus close first): rename it in the tree.
    Vault* vault = vaultOf(this);
    const QString path = item->path();
    QPointer<FileExplorerView> self(this);
    auto begin = [self, vault, path] {
        if (!self || !vault)
            return;
        if (TAbstractFile* it = vault->item(path)) {
            if (Workspace* ws = self->workspace()) {
                ws->revealLeaf(self->leaf());
                ws->setActiveLeaf(self->leaf(), false);
            }
            self->startRename(it);
            self->m_tree->scrollToItem(it, 4);
        }
    };
    begin();
    QTimer::singleShot(0, this, [self, vault, path, begin] {
        // A closing menu may have taken the focus back.
        if (self && vault && self->m_tree->renamingItem() == vault->item(path) && !self->m_tree->renameEditor()->hasFocus())
            self->m_tree->renameEditor()->setFocus(Qt::OtherFocusReason);
        else if (self && !self->m_tree->renamingItem())
            begin();
    });
}

void FileExplorerView::startRename(TAbstractFile* item)
{
    if (!item || (item->isFolder() && static_cast<TFolder*>(item)->isRoot()))
        return;
    m_tree->startRename(item);
}

void FileExplorerView::deleteItems(const QList<TAbstractFile*>& items)
{
    if (items.isEmpty())
        return;
    if (items.size() == 1)
        promptForDeletion(session(), items.first());
    else
        promptForDeletion(session(), items);
}

void FileExplorerView::moveItems(const QList<TAbstractFile*>& items, TFolder* target)
{
    oc::moveItems(session(), items, target);
}

TAbstractFile* FileExplorerView::makeCopy(TAbstractFile* item, Workspace::NewLeaf mode)
{
    QString err;
    TAbstractFile* copy = oc::makeCopy(session(), item, &err);
    if (!copy) {
        showNotice(session(), err.isEmpty() ? tr("Failed to copy “%1”.").arg(item ? item->name() : QString()) : err);
        return nullptr;
    }
    afterCreate(copy, mode);
    return copy;
}

// ---- clipboard ----------------------------------------------------------------------------------------

namespace {
const QString kFilesMime = QStringLiteral("obsidian/files");
}

void FileExplorerView::copyToClipboard(const QList<TAbstractFile*>& items, bool cut)
{
    QJsonArray paths;
    for (TAbstractFile* i : outermostItems(items))
        paths.append(i->path());
    if (paths.isEmpty())
        return;
    auto* md = new QMimeData;
    md->setData(kFilesMime, QJsonDocument(QJsonObject{{QStringLiteral("operation"), cut ? QStringLiteral("cut") : QStringLiteral("copy")},
                                                      {QStringLiteral("paths"), paths}})
                                .toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(md);
    if (cut)
        m_tree->setCutItems(items); // .is-cut
}

void FileExplorerView::pasteInto(TFolder* target)
{
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    const QMimeData* md = QGuiApplication::clipboard()->mimeData();
    if (!vault || !target || !md || !md->hasFormat(kFilesMime))
        return;
    const QJsonObject o = QJsonDocument::fromJson(md->data(kFilesMime)).object();
    const bool cut = o.value(QStringLiteral("operation")).toString() == u"cut";
    QStringList paths;
    for (const QJsonValue& v : o.value(QStringLiteral("paths")).toArray())
        paths << v.toString();
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return naturalCompare(a, b) < 0; });
    QList<TAbstractFile*> items;
    for (const QString& p : std::as_const(paths))
        if (TAbstractFile* i = vault->item(p))
            items << i;
    items = outermostItems(items);
    QList<TAbstractFile*> pasted;
    const QString targetPath = target->path();
    if (cut) {
        oc::moveItems(s, items, target);
        for (TAbstractFile* i : std::as_const(items)) // the objects follow their renames
            if (vault->item(i->path()) == i && i->parent() && i->parent()->path() == targetPath)
                pasted << i;
    } else {
        for (TAbstractFile* i : std::as_const(items)) {
            TFolder* dest = vault->folder(targetPath);
            if (!dest || (i->isFolder() && (i == dest || dest->path().startsWith(i->path() + QLatin1Char('/')))))
                continue;
            QString err;
            if (TAbstractFile* c = copyInto(s, i, dest, &err))
                pasted << c;
            else
                showNotice(s, err);
        }
    }
    m_tree->clearSelection();
    m_tree->setSelectedItems(pasted);
    m_tree->setCutItems({});
    if (!pasted.isEmpty()) {
        m_tree->reveal(pasted.first());
        m_tree->setFocusedItem(pasted.first(), true);
    }
}

// ---- menus ------------------------------------------------------------------------------------------

void FileExplorerView::populateSortMenu(QMenu* menu)
{
    const ExplorerSortOrder current = m_tree->sortOrder();
    const QList<QList<QPair<ExplorerSortOrder, QString>>> groups{
        {{ExplorerSortOrder::Alphabetical, tr("File name (A to Z)")}, {ExplorerSortOrder::AlphabeticalReverse, tr("File name (Z to A)")}},
        {{ExplorerSortOrder::ByModifiedTime, tr("Modified time (new to old)")}, {ExplorerSortOrder::ByModifiedTimeReverse, tr("Modified time (old to new)")}},
        {{ExplorerSortOrder::ByCreatedTime, tr("Created time (new to old)")}, {ExplorerSortOrder::ByCreatedTimeReverse, tr("Created time (old to new)")}},
    };
    QPointer<FileExplorerView> self(this);
    for (int g = 0; g < groups.size(); ++g) {
        if (g > 0)
            menu->addSeparator();
        for (const auto& [order, title] : groups[g]) {
            QAction* a = menu->addAction(title);
            a->setCheckable(true);
            a->setChecked(order == current);
            a->setData(sortOrderId(order));
            connect(a, &QAction::triggered, this, [self, order] {
                if (self)
                    self->setSortOrder(order);
            });
        }
    }
}

void FileExplorerView::showSortMenu()
{
    QMenu& menu = *ownedMenu(this);
    populateSortMenu(&menu);
    m_sort->setActive(true); // .has-active-menu
    QPointer<FileExplorerView> self(this);
    menu.exec(m_sort->mapToGlobal(QPoint(0, m_sort->height() + 2)));
    if (self)
        m_sort->setActive(false);
}

void FileExplorerView::populateContextMenu(QMenu* menu, TAbstractFile* item)
{
    VaultSession* s = session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!vault)
        return;
    const bool online = vault->isOnline() && !m_tree->isReadOnly();
    if (!item)
        item = vault->rootFolder();
    const bool isRoot = item->isFolder() && static_cast<TFolder*>(item)->isRoot();
    QPointer<FileExplorerView> self(this);
    const QString path = item->path();
    // Items are resolved again when chosen (the vault may have changed meanwhile).
    auto resolve = [self, path]() -> TAbstractFile* {
        Vault* v = self ? vaultOf(self) : nullptr;
        return v ? v->item(path) : nullptr;
    };
    QList<MenuItem> items;

    const QList<TAbstractFile*> selection = m_tree->selectedItems();
    if (selection.size() > 1 && selection.contains(item)) {
        // Multi-selection menu.
        QStringList paths;
        for (TAbstractFile* i : selection)
            paths << i->path();
        auto resolveAll = [self, paths] {
            QList<TAbstractFile*> out;
            if (Vault* v = self ? vaultOf(self) : nullptr)
                for (const QString& p : paths)
                    if (TAbstractFile* i = v->item(p))
                        out << i;
            return out;
        };
        const int n = int(outermostItems(selection).size());
        items << MenuItem{QStringLiteral("action-primary"),
                          n == 1 ? tr("New folder with selection (%1 item)").arg(n) : tr("New folder with selection (%1 items)").arg(n),
                          QStringLiteral("folder-plus"),
                          [self, resolve, resolveAll] {
                              if (!self)
                                  return;
                              TAbstractFile* t = resolve();
                              TFolder* parent = t ? (t->isFolder() ? static_cast<TFolder*>(t) : t->parent()) : nullptr;
                              const QList<TAbstractFile*> moving = outermostItems(resolveAll());
                              QString err;
                              TFolder* folder = createNewFolder(self->session(), parent, &err);
                              if (!folder) {
                                  showNotice(self->session(), err);
                                  return;
                              }
                              const QString folderPath = folder->path();
                              oc::moveItems(self->session(), moving, folder);
                              self->m_tree->clearSelection();
                              if (Vault* v = vaultOf(self))
                                  self->afterCreate(v->item(folderPath), Workspace::NewLeaf::No);
                          },
                          false, online};
        items << MenuItem{QStringLiteral("action"), tr("Move %1 items to...").arg(selection.size()), QStringLiteral("folder-tree"),
                          [self, resolveAll] {
                              if (self)
                                  openMoveModal(self->session(), resolveAll());
                          },
                          false, online};
        items << MenuItem{QStringLiteral("danger"), tr("Delete"), QStringLiteral("trash-2"),
                          [self, resolveAll] {
                              if (self)
                                  self->deleteItems(resolveAll());
                          },
                          true, online};
        buildMenu(menu, items, {}, {}, {});
        return;
    }

    if (item->isFolder()) {
        items << MenuItem{QStringLiteral("action-primary"), tr("New note"), QStringLiteral("edit"),
                          [self, resolve] {
                              if (auto* f = self ? static_cast<TFolder*>(resolve()) : nullptr)
                                  self->createNoteIn(f, (QApplication::keyboardModifiers() & Qt::ControlModifier) ? Workspace::NewLeaf::Tab
                                                                                                                  : Workspace::NewLeaf::No);
                          },
                          false, online};
        items << MenuItem{QStringLiteral("action-primary"), tr("New folder"), QStringLiteral("folder-open"),
                          [self, resolve] {
                              if (auto* f = self ? static_cast<TFolder*>(resolve()) : nullptr)
                                  self->createFolderIn(f);
                          },
                          false, online};
    }
    if (!isRoot)
        items << MenuItem{QStringLiteral("danger"), tr("Rename..."), QStringLiteral("edit-3"),
                          [self, resolve] {
                              if (self)
                                  QTimer::singleShot(0, self, [self, resolve] {
                                      if (self)
                                          self->startRename(resolve());
                                  });
                          },
                          false, online};
    if (!item->isFolder()) {
        items << MenuItem{QStringLiteral("open"), tr("Open in new tab"), QStringLiteral("file-plus"),
                          [self, resolve] {
                              if (self && resolve())
                                  self->openFile(static_cast<TFile*>(resolve()), Workspace::NewLeaf::Tab, false);
                          }};
        items << MenuItem{QStringLiteral("open"), tr("Open to the right"), QStringLiteral("separator-vertical"),
                          [self, resolve] {
                              if (self && resolve())
                                  self->openFile(static_cast<TFile*>(resolve()), Workspace::NewLeaf::SplitRight, false);
                          }};
        // Obsidian's workspace "file-menu" handler: every file menu can open the file in a pop-out.
        items << MenuItem{QStringLiteral("open"), tr("Open in new window"), QStringLiteral("picture-in-picture-2"),
                          [self, resolve] {
                              if (self && resolve())
                                  self->openFile(static_cast<TFile*>(resolve()), Workspace::NewLeaf::Window, false);
                          }};
    }
    if (!isRoot) {
        items << MenuItem{QStringLiteral("action"), tr("Make a copy"), QStringLiteral("files"),
                          [self, resolve] {
                              if (self && resolve())
                                  self->makeCopy(resolve(), (QApplication::keyboardModifiers() & Qt::ControlModifier) ? Workspace::NewLeaf::Tab
                                                                                                                      : Workspace::NewLeaf::No);
                          },
                          false, online};
        items << MenuItem{QStringLiteral("danger"), tr("Delete"), QStringLiteral("trash-2"),
                          [self, resolve] {
                              if (self && resolve())
                                  self->deleteItems({resolve()});
                          },
                          true, online};
    }
    // App-level additions (Obsidian's "file-menu" handlers).
    if (!item->isFolder()) {
        items << MenuItem{QStringLiteral("info.copy"), tr("as Obsidian URL"), QStringLiteral("link"), [self, resolve] {
                              TAbstractFile* f = self ? resolve() : nullptr;
                              if (!f)
                                  return;
                              QGuiApplication::clipboard()->setText(obsidianUrl(vaultOf(self), static_cast<TFile*>(f)));
                              showNotice(self->session(), tr("Copied to your clipboard"));
                          }};
        items << MenuItem{QStringLiteral("system"), tr("Open in default app"), QStringLiteral("arrow-up-right"), [self, resolve] {
                              if (self && resolve())
                                  openWithDefaultApp(vaultOf(self), resolve());
                          }};
    }
    if (!isRoot) {
        items << MenuItem{QStringLiteral("action"), item->isFolder() ? tr("Move folder to...") : tr("Move file to..."), QStringLiteral("folder-tree"),
                          [self, resolve] {
                              if (self && resolve())
                                  openMoveModal(self->session(), {resolve()});
                          },
                          false, online};
        items << MenuItem{QStringLiteral("info.copy"), tr("from vault folder"), QStringLiteral("vault"), [self, resolve] {
                              TAbstractFile* f = self ? resolve() : nullptr;
                              if (!f)
                                  return;
                              QGuiApplication::clipboard()->setText(f->path());
                              showNotice(self->session(), tr("Copied to your clipboard"));
                          }};
        items << MenuItem{QStringLiteral("info.copy"), tr("from system root"), QStringLiteral("hard-drive"), [self, resolve] {
                              TAbstractFile* f = self ? resolve() : nullptr;
                              if (!f)
                                  return;
                              QGuiApplication::clipboard()->setText(vaultOf(self)->absolutePath(f->path()));
                              showNotice(self->session(), tr("Copied to your clipboard"));
                          }};
        items << MenuItem{QStringLiteral("system"), tr("Show in system explorer"), QStringLiteral("arrow-up-right"), [self, resolve] {
                              if (self && resolve())
                                  showInSystemExplorer(vaultOf(self), resolve());
                          }};
    }
    // Obsidian's "file-menu" event: plugins add their items (bookmarks, note composer, ...).
    QList<MenuEntry> extra;
    FileMenuHooks::collect(extra, item, QStringLiteral("file-explorer-context-menu"), nullptr, this);
    for (const MenuEntry& e : std::as_const(extra))
        items << MenuItem{kSections.contains(e.section) ? e.section : QString(), e.title, e.icon, e.run, e.warning, e.enabled};
    buildMenu(menu, items, QStringLiteral("info.copy"), tr("Copy path"), QStringLiteral("clipboard"));
}

void FileExplorerView::showContextMenu(TAbstractFile* item, const QPoint& globalPos)
{
    if (item && item == m_tree->renamingItem())
        return;
    QMenu& menu = *ownedMenu(this);
    populateContextMenu(&menu, item);
    if (menu.isEmpty()) {
        menu.deleteLater();
        return;
    }
    if (item) {
        m_tree->setFocusedItem(item, false);
        m_tree->setMenuItem(item);
    }
    QPointer<FileExplorerView> self(this);
    menu.exec(globalPos);
    if (self)
        m_tree->setMenuItem(nullptr);
}

// ---- dragging onto the workspace ------------------------------------------------------------------------

// A file dragged out of the tree opens where it's dropped: as a tab of the hovered tab group (at the
// highlighted slot) or in a split at its edge — the workspace's own drop regions and overlay. Uses the
// workspace internals (no public "drop a file" API yet).
void FileExplorerView::dropOnWorkspace(const QList<TAbstractFile*>& items, const QPoint& globalPos, bool drop)
{
    Workspace* ws = workspace();
    WorkspaceImpl* d = ws ? WorkspaceTabs::impl(ws) : nullptr;
    auto clear = [&] {
        m_workspaceTarget = {};
        if (d && d->overlay && !d->drag.active) {
            d->overlay->setTarget({});
            d->overlay->hide();
        }
    };
    TFile* file = items.size() == 1 && !items.first()->isFolder() ? static_cast<TFile*>(items.first()) : nullptr;
    ViewRegistry* reg = ws ? ws->viewRegistry() : nullptr;
    if (!d || d->drag.active || !file || !reg || reg->typeForExtension(file->extension().toLower()).isEmpty()) {
        clear();
        return;
    }
    const WorkspaceImpl::DropTarget t = d->dropTargetAt(nullptr, globalPos);
    if (!t.isValid() || t.kind != WorkspaceImpl::DropTarget::Kind::Tabs || d->topSplit(t.tabs) != d->root) {
        clear();
        return;
    }
    if (!drop) {
        DropOverlay* o = d->ensureOverlay();
        if (!o)
            return;
        QWidget* top = o->parentWidget();
        m_workspaceTarget = QRect(d->widget->mapTo(top, t.rect.topLeft()), t.rect.size());
        o->setTarget(m_workspaceTarget);
        return;
    }
    clear();
    WorkspaceLeaf* leaf = d->createLeafInGroup(t.tabs);
    if (!leaf)
        return;
    leaf->openFile(file);
    if (!d->applyDrop(leaf, t))
        ws->setActiveLeaf(leaf, true);
}

// ---------------------------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------------------------

void registerFileExplorerView(ViewRegistry& registry)
{
    registry.registerView(QStringLiteral("file-explorer"), [](WorkspaceLeaf* leaf) -> View* { return new FileExplorerView(leaf); });
}

namespace {
FileExplorerView* explorerView(Workspace* ws, bool create)
{
    if (!ws)
        return nullptr;
    WorkspaceLeaf* l = ws->leavesOfType(QStringLiteral("file-explorer")).value(0);
    if (!l && create)
        l = ws->ensureSideLeaf(QStringLiteral("file-explorer"), Workspace::Side::Left, false);
    return l ? qobject_cast<FileExplorerView*>(l->view()) : nullptr;
}

Command explorerCommand(const char* id, const QString& name, const char* icon, std::function<bool(bool)> check,
                        std::initializer_list<const char*> hotkeys = {})
{
    Command c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.icon = QString::fromLatin1(icon);
    for (const char* h : hotkeys)
        c.defaultHotkeys.append(Hotkey::parse(QString::fromLatin1(h)));
    c.checkCallback = std::move(check);
    return c;
}

QString tr(const char* s) { return QCoreApplication::translate("oc::FileExplorerView", s); }

// Obsidian's fileManager.createAndOpenMarkdownFile().
bool createAndOpen(VaultSession* s, Workspace::NewLeaf mode, bool checking)
{
    Workspace* ws = s ? s->workspace() : nullptr;
    if (!ws || !s->vault() || !s->vault()->isOnline())
        return false;
    if (checking)
        return true;
    TFile* active = workspaceActiveFile(ws);
    FileManager* fm = s->fileManager();
    TFolder* parent = fm ? fm->newFileParent(active ? active->path() : QString()) : s->vault()->rootFolder();
    QString err;
    TFile* f = fm ? fm->createNewMarkdownFile(parent, {}, {}, &err) : nullptr;
    if (!f) {
        showNotice(s, err.isEmpty() ? tr("Failed to create a new note.") : err);
        return true;
    }
    openNewFile(ws, f, mode);
    return true;
}
} // namespace

void registerFileExplorerCommands(CommandRegistry& r, VaultSession* session)
{
    QPointer<VaultSession> s = session;
    auto ws = [s]() -> Workspace* { return s ? s->workspace() : nullptr; };

    r.add(explorerCommand("file-explorer:open", tr("Show file explorer"), "files", [ws](bool checking) {
        if (!ws())
            return false;
        if (!checking) {
            WorkspaceLeaf* l = ws()->ensureSideLeaf(QStringLiteral("file-explorer"), Workspace::Side::Left, true);
            ws()->setActiveLeaf(l, true);
        }
        return true;
    }));
    r.add(explorerCommand("file-explorer:reveal-active-file", tr("Reveal current file in navigation"), "navigation", [ws](bool checking) {
        TFile* f = workspaceActiveFile(ws());
        if (!f)
            return false;
        if (!checking) {
            FileExplorerView* v = explorerView(ws(), true);
            if (v)
                v->revealInFolder(f);
        }
        return true;
    }));
    r.add(explorerCommand("file-explorer:new-file", tr("Create new note"), "file-plus",
                          [s](bool checking) { return createAndOpen(s, Workspace::NewLeaf::Tab, checking); }, {"Mod+N"}));
    r.add(explorerCommand("file-explorer:new-file-in-current-tab", tr("Create new note in current tab"), "file-plus",
                          [s](bool checking) { return createAndOpen(s, Workspace::NewLeaf::No, checking); }));
    r.add(explorerCommand("file-explorer:new-file-in-new-pane", tr("Create note to the right"), "file-plus",
                          [s](bool checking) { return createAndOpen(s, Workspace::NewLeaf::SplitRight, checking); }, {"Mod+Shift+N"}));
    r.add(explorerCommand("file-explorer:new-folder", tr("Create new folder"), "folder-plus", [s, ws](bool checking) {
        if (!ws() || !s->vault() || !s->vault()->isOnline())
            return false;
        if (checking)
            return true;
        QString err;
        TFolder* folder = createNewFolder(s, nullptr, &err);
        if (!folder) {
            showNotice(s, err);
            return true;
        }
        WorkspaceLeaf* l = ws()->ensureSideLeaf(QStringLiteral("file-explorer"), Workspace::Side::Left, true);
        ws()->setActiveLeaf(l, true);
        if (auto* v = qobject_cast<FileExplorerView*>(l ? l->view() : nullptr))
            v->setState(QJsonObject{{QStringLiteral("newFile"), folder->path()}});
        return true;
    }));
    r.add(explorerCommand("file-explorer:move-file", tr("Move current file to another folder"), "folder-tree", [s, ws](bool checking) {
        TFile* f = workspaceActiveFile(ws());
        if (!f || !s->vault()->isOnline())
            return false;
        if (!checking)
            openMoveModal(s, {f});
        return true;
    }));
    r.add(explorerCommand("file-explorer:duplicate-file", tr("Make a copy of the current file"), "files", [s, ws](bool checking) {
        TFile* f = workspaceActiveFile(ws());
        if (!f || !s->vault()->isOnline())
            return false;
        if (!checking) {
            QString err;
            TAbstractFile* copy = makeCopy(s, f, &err);
            if (!copy)
                showNotice(s, err);
            else
                openNewFile(ws(), static_cast<TFile*>(copy), Workspace::NewLeaf::Tab);
        }
        return true;
    }));
    r.add(explorerCommand("open-with-default-app:open", tr("Open in default app"), "arrow-up-right", [s, ws](bool checking) {
        TFile* f = workspaceActiveFile(ws());
        if (!f)
            return false;
        if (!checking)
            openWithDefaultApp(s->vault(), f);
        return true;
    }));
    r.add(explorerCommand("open-with-default-app:show", tr("Show in system explorer"), "arrow-up-right", [s, ws](bool checking) {
        TFile* f = workspaceActiveFile(ws());
        if (!f)
            return false;
        if (!checking)
            showInSystemExplorer(s->vault(), f);
        return true;
    }));
}

} // namespace oc
