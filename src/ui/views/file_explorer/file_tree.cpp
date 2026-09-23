#include "ui/views/file_explorer/file_tree.h"

#include "core/vault/vault.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDropEvent>
#include <QMimeData>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>
#include <QToolTip>

#include <algorithm>
#include <climits>
#include <cmath>

namespace oc {

namespace {

QColor tok(const ThemePtr& t, const char* name, const QColor& fallback = {})
{
    return t->color(QString::fromLatin1(name), fallback);
}

QString titleOf(const TAbstractFile* item)
{
    if (!item)
        return {};
    if (item->isFolder())
        return item->name();
    return static_cast<const TFile*>(item)->basename(); // Obsidian shows the basename; the tag the extension
}

QCollator makeCollator()
{
    QCollator c;
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    c.setIgnorePunctuation(false);
    return c;
}

// Recursive file / folder counts of a folder (Obsidian's getFileCount / getFolderCount).
void countTree(const TFolder* f, int* files, int* folders)
{
    for (const TAbstractFile* c : f->children()) {
        if (c->isFolder()) {
            ++*folders;
            countTree(static_cast<const TFolder*>(c), files, folders);
        } else {
            ++*files;
        }
    }
}

// Filters the key events of a focused tree before the application's hotkey dispatcher sees them
// (installed on focus-in, so it runs before filters installed earlier).
class TreeKeyGuard : public QObject {
public:
    explicit TreeKeyGuard(FileTree* tree)
        : QObject(tree)
        , m_tree(tree)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() != QEvent::ShortcutOverride && ev->type() != QEvent::KeyPress)
            return false;
        if (obj != m_tree && obj != m_tree->viewport())
            return false;
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (!m_tree->wantsKey(ke))
            return false;
        if (ev->type() == QEvent::ShortcutOverride) {
            ke->accept(); // no shortcut, no global hotkey: the KeyPress comes to the tree
            return true;
        }
        return m_tree->handleKey(ke);
    }

private:
    FileTree* m_tree;
};

constexpr int kFlashMs = 750;
constexpr int kExpandOnHoverMs = 750;
constexpr int kDragThreshold = 5;
constexpr int kAutoScrollEdge = 24;

} // namespace

// ---------------------------------------------------------------------------------------------
// free functions
// ---------------------------------------------------------------------------------------------

QString sortOrderId(ExplorerSortOrder order)
{
    switch (order) {
    case ExplorerSortOrder::Alphabetical: return QStringLiteral("alphabetical");
    case ExplorerSortOrder::AlphabeticalReverse: return QStringLiteral("alphabeticalReverse");
    case ExplorerSortOrder::ByModifiedTime: return QStringLiteral("byModifiedTime");
    case ExplorerSortOrder::ByModifiedTimeReverse: return QStringLiteral("byModifiedTimeReverse");
    case ExplorerSortOrder::ByCreatedTime: return QStringLiteral("byCreatedTime");
    case ExplorerSortOrder::ByCreatedTimeReverse: return QStringLiteral("byCreatedTimeReverse");
    }
    return QStringLiteral("alphabetical");
}

std::optional<ExplorerSortOrder> sortOrderFromId(const QString& id)
{
    for (ExplorerSortOrder o : {ExplorerSortOrder::Alphabetical, ExplorerSortOrder::AlphabeticalReverse, ExplorerSortOrder::ByModifiedTime,
                                ExplorerSortOrder::ByModifiedTimeReverse, ExplorerSortOrder::ByCreatedTime, ExplorerSortOrder::ByCreatedTimeReverse})
        if (sortOrderId(o) == id)
            return o;
    return std::nullopt;
}

int naturalCompare(const QString& a, const QString& b)
{
    static const QCollator collator = makeCollator();
    return collator.compare(a, b);
}

// ---------------------------------------------------------------------------------------------
// FileTree — construction
// ---------------------------------------------------------------------------------------------

FileTree::FileTree(QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_collator(makeCollator())
{
    setObjectName(QStringLiteral("ocFileTree"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAutoFillBackground(false);
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true);

    m_editor = new QLineEdit(viewport());
    m_editor->setObjectName(QStringLiteral("ocFileTreeRename"));
    m_editor->setProperty("ocFlat", true);
    m_editor->setFrame(false);
    m_editor->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_editor->hide();
    m_editor->installEventFilter(this);
    connect(m_editor, &QLineEdit::textEdited, this, &FileTree::onRenameEdited);

    m_flashTimer.setSingleShot(true);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash = nullptr;
        viewport()->update();
    });
    m_expandTimer.setSingleShot(true);
    m_expandTimer.setInterval(kExpandOnHoverMs);
    connect(&m_expandTimer, &QTimer::timeout, this, [this] {
        Node* n = m_drag.expandCandidate;
        if (n && n == highlightedFolder() && n != m_root && !n->expanded)
            setNodeExpanded(n, true, true);
    });
    m_autoScroll.setInterval(16);
    connect(&m_autoScroll, &QTimer::timeout, this, &FileTree::autoScrollTick);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { positionEditor(); });

    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateMetrics();
            rowsChanged();
        });
    updateMetrics();
}

FileTree::~FileTree()
{
    if (m_keyGuard)
        qApp->removeEventFilter(m_keyGuard);
    if (m_drag.active)
        QApplication::restoreOverrideCursor();
    delete m_ghost.data();
    delete m_error.data();
    clearNodes();
}

void FileTree::setVault(Vault* vault)
{
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_vault = vault;
    if (vault) {
        m_connections << connect(vault, &Vault::created, this, [this](TAbstractFile* item, bool) { onCreated(item); });
        m_connections << connect(vault, &Vault::deleted, this, [this](TAbstractFile* item, bool) { onDeleted(item); });
        m_connections << connect(vault, &Vault::renamed, this, [this](TAbstractFile* item, const QString& old, bool) { onRenamed(item, old); });
        m_connections << connect(vault, &Vault::modified, this, [this](TFile* f, bool) { onModified(f); });
        m_connections << connect(vault, &Vault::loaded, this, [this] { rebuild(); });
    }
    rebuild();
}

void FileTree::setSortOrder(ExplorerSortOrder order)
{
    if (order == m_order)
        return;
    m_order = order;
    invalidateAllSorts();
}

void FileTree::setShowUnsupported(bool show)
{
    if (show == m_showUnsupported)
        return;
    m_showUnsupported = show;
    rowsChanged();
}

void FileTree::setSupportedPredicate(std::function<bool(const QString&)> fn)
{
    m_supported = std::move(fn);
    rowsChanged();
}

void FileTree::setReadOnly(bool readOnly)
{
    if (readOnly == m_readOnly)
        return;
    m_readOnly = readOnly;
    if (readOnly) {
        cancelDrag();
        cancelRename();
    }
    viewport()->update();
}

void FileTree::setLeafActive(bool active)
{
    if (active == m_leafActive)
        return;
    m_leafActive = active;
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------------------------

void FileTree::clearNodes()
{
    m_renaming = nullptr;
    if (m_editor)
        m_editor->hide();
    qDeleteAll(m_nodes);
    m_nodes.clear();
    m_root = nullptr;
    m_activeFile = m_focused = m_anchor = m_menuNode = m_flash = nullptr;
    m_selected.clear();
    m_cut.clear();
    m_drag.nodes.clear();
    m_drag.target = m_drag.expandCandidate = nullptr;
    m_externalTarget = nullptr;
    m_rows.clear();
    m_rowIndex.clear();
    m_rowsDirty = true;
    m_hoverRow = -1;
}

void FileTree::rebuild()
{
    const QStringList expanded = m_root ? expandedPaths() : QStringList();
    const QString active = m_activeFile ? m_activeFile->item->path() : QString();
    clearNodes();
    if (m_vault && m_vault->rootFolder()) {
        m_nodes.reserve(m_vault->fileCount() + int(m_vault->folders().size()) + 1);
        m_root = new Node;
        m_root->item = m_vault->rootFolder();
        m_root->folder = true;
        m_root->expanded = true;
        m_nodes.insert(m_root->item, m_root);
        // Iterative DFS (folders can be deep).
        std::vector<Node*> stack{m_root};
        while (!stack.empty()) {
            Node* parent = stack.back();
            stack.pop_back();
            const auto& kids = static_cast<TFolder*>(parent->item)->children();
            parent->children.reserve(kids.size());
            for (TAbstractFile* c : kids) {
                auto* n = new Node;
                n->item = c;
                n->parent = parent;
                n->depth = parent->depth + 1;
                n->folder = c->isFolder();
                parent->children.push_back(n);
                m_nodes.insert(c, n);
                if (n->folder)
                    stack.push_back(n);
            }
        }
        for (const QString& p : expanded)
            if (Node* n = nodeFor(m_vault->item(p)); n && n->folder)
                n->expanded = true;
        if (!active.isEmpty())
            if (Node* n = nodeFor(m_vault->item(active)))
                m_activeFile = n;
    }
    rowsChanged();
}

FileTree::Node* FileTree::addNode(TAbstractFile* item)
{
    if (!item)
        return nullptr;
    if (Node* existing = nodeFor(item))
        return existing;
    TFolder* parentItem = item->parent();
    if (!parentItem)
        return nullptr; // the root is created by rebuild()
    Node* parent = nodeFor(parentItem);
    if (!parent)
        parent = addNode(parentItem);
    if (!parent)
        return nullptr;
    auto* n = new Node;
    n->item = item;
    n->parent = parent;
    n->depth = parent->depth + 1;
    n->folder = item->isFolder();
    parent->children.push_back(n);
    parent->sorted = false;
    m_nodes.insert(item, n);
    if (n->folder) // an adopted folder may already have contents
        for (TAbstractFile* c : static_cast<TFolder*>(item)->children())
            addNode(c);
    return n;
}

void FileTree::removeNode(Node* n)
{
    if (!n || n == m_root)
        return;
    while (!n->children.empty())
        removeNode(n->children.back());
    if (n->parent) {
        auto& kids = n->parent->children;
        kids.erase(std::remove(kids.begin(), kids.end(), n), kids.end());
    }
    if (m_renaming == n)
        cancelRename();
    if (m_activeFile == n)
        m_activeFile = nullptr;
    if (m_focused == n)
        m_focused = nullptr;
    if (m_anchor == n)
        m_anchor = nullptr;
    if (m_menuNode == n)
        m_menuNode = nullptr;
    if (m_flash == n)
        m_flash = nullptr;
    if (m_externalTarget == n)
        m_externalTarget = nullptr;
    m_selected.removeAll(n);
    m_cut.removeAll(n);
    if (m_drag.active) {
        m_drag.nodes.removeAll(n);
        if (m_drag.target == n)
            m_drag.target = nullptr;
        if (m_drag.expandCandidate == n)
            m_drag.expandCandidate = nullptr;
        if (m_drag.nodes.isEmpty())
            cancelDrag();
    }
    m_nodes.remove(n->item);
    delete n;
}

bool FileTree::isShown(const Node* n) const
{
    if (n->folder || m_showUnsupported || !m_supported)
        return true;
    return m_supported(static_cast<const TFile*>(n->item)->extension().toLower());
}

void FileTree::ensureKeys(Node* n)
{
    if (n->key)
        return;
    n->key = m_collator.sortKey(titleOf(n->item));
    n->nameKey = n->folder ? *n->key : m_collator.sortKey(n->item->name());
}

bool FileTree::lessThan(Node* a, Node* b)
{
    if (a->folder != b->folder)
        return a->folder;
    auto byName = [](Node* x, Node* y) {
        const int c = x->key->compare(*y->key);
        if (c != 0)
            return c < 0;
        const int d = x->nameKey->compare(*y->nameKey);
        if (d != 0)
            return d < 0;
        return x->item->name() < y->item->name();
    };
    if (a->folder)
        return byName(a, b);
    const auto* fa = static_cast<const TFile*>(a->item);
    const auto* fb = static_cast<const TFile*>(b->item);
    switch (m_order) {
    case ExplorerSortOrder::Alphabetical:
        return byName(a, b);
    case ExplorerSortOrder::AlphabeticalReverse:
        return byName(b, a);
    case ExplorerSortOrder::ByModifiedTime:
        if (fa->mtimeMs() != fb->mtimeMs())
            return fa->mtimeMs() > fb->mtimeMs();
        return byName(a, b);
    case ExplorerSortOrder::ByModifiedTimeReverse:
        if (fa->mtimeMs() != fb->mtimeMs())
            return fa->mtimeMs() < fb->mtimeMs();
        return byName(a, b);
    case ExplorerSortOrder::ByCreatedTime:
        if (fa->ctimeMs() != fb->ctimeMs())
            return fa->ctimeMs() > fb->ctimeMs();
        return byName(a, b);
    case ExplorerSortOrder::ByCreatedTimeReverse:
        if (fa->ctimeMs() != fb->ctimeMs())
            return fa->ctimeMs() < fb->ctimeMs();
        return byName(a, b);
    }
    return byName(a, b);
}

void FileTree::sortChildren(Node* folder)
{
    if (folder->sorted)
        return;
    for (Node* c : folder->children)
        ensureKeys(c);
    std::stable_sort(folder->children.begin(), folder->children.end(), [this](Node* a, Node* b) { return lessThan(a, b); });
    folder->sorted = true;
}

void FileTree::invalidateSort(Node* folder)
{
    if (folder)
        folder->sorted = false;
    rowsChanged();
}

void FileTree::invalidateAllSorts()
{
    for (Node* n : std::as_const(m_nodes))
        if (n->folder)
            n->sorted = false;
    rowsChanged();
}

void FileTree::ensureRows() const
{
    if (!m_rowsDirty)
        return;
    m_rowsDirty = false;
    m_rows.clear();
    m_rowIndex.clear();
    if (!m_root)
        return;
    auto* self = const_cast<FileTree*>(this);
    // Iterative preorder over expanded folders.
    std::vector<std::pair<Node*, size_t>> stack;
    self->sortChildren(m_root);
    stack.emplace_back(m_root, 0);
    while (!stack.empty()) {
        auto& [folder, index] = stack.back();
        if (index >= folder->children.size()) {
            stack.pop_back();
            continue;
        }
        Node* n = folder->children[index++];
        if (!isShown(n))
            continue;
        m_rowIndex.insert(n, int(m_rows.size()));
        m_rows.push_back(n);
        if (n->folder && n->expanded && !n->children.empty()) {
            self->sortChildren(n);
            stack.emplace_back(n, 0);
        }
    }
}

void FileTree::rowsChanged()
{
    m_rowsDirty = true;
    m_hoverRow = -1;
    updateScrollRange();
    positionEditor();
    viewport()->update();
}

void FileTree::onCreated(TAbstractFile* item)
{
    if (!m_root || nodeFor(item))
        return;
    if (addNode(item))
        rowsChanged();
}

void FileTree::onDeleted(TAbstractFile* item)
{
    Node* n = nodeFor(item);
    if (!n)
        return;
    const bool hadFocus = m_focused != nullptr;
    removeNode(n);
    if (hadFocus && !m_focused && !m_focusAfterDelete.isEmpty() && m_vault) {
        if (Node* next = nodeFor(m_vault->item(m_focusAfterDelete)))
            m_focused = next;
        m_focusAfterDelete.clear();
    }
    rowsChanged();
}

void FileTree::onRenamed(TAbstractFile* item, const QString& oldPath)
{
    Q_UNUSED(oldPath);
    Node* n = nodeFor(item);
    if (!n) {
        onCreated(item);
        return;
    }
    Node* newParent = nodeFor(item->parent());
    if (!newParent)
        newParent = addNode(item->parent());
    if (newParent && newParent != n->parent) {
        auto& kids = n->parent->children;
        kids.erase(std::remove(kids.begin(), kids.end(), n), kids.end());
        n->parent = newParent;
        newParent->children.push_back(n);
        newParent->sorted = false;
        // depths below it change
        std::vector<Node*> stack{n};
        while (!stack.empty()) {
            Node* x = stack.back();
            stack.pop_back();
            x->depth = x->parent->depth + 1;
            for (Node* c : x->children)
                stack.push_back(c);
        }
    }
    n->key.reset();
    n->nameKey.reset();
    if (n->parent)
        n->parent->sorted = false;
    if (n->folder)
        emit expansionChanged(); // expanded paths changed
    rowsChanged();
}

void FileTree::onModified(TFile* file)
{
    if (m_order == ExplorerSortOrder::Alphabetical || m_order == ExplorerSortOrder::AlphabeticalReverse)
        return;
    if (Node* n = nodeFor(file); n && n->parent)
        invalidateSort(n->parent);
}

// ---------------------------------------------------------------------------------------------
// expansion
// ---------------------------------------------------------------------------------------------

bool FileTree::isExpanded(const TAbstractFile* folder) const
{
    const Node* n = nodeFor(folder);
    return n && n->folder && n->expanded;
}

void FileTree::setExpanded(TAbstractFile* folder, bool expanded) { setNodeExpanded(nodeFor(folder), expanded, true); }

void FileTree::setNodeExpanded(Node* n, bool expanded, bool user)
{
    if (!n || !n->folder || n == m_root || n->expanded == expanded)
        return;
    n->expanded = expanded;
    if (expanded && m_allCollapsed) {
        m_allCollapsed = false;
        emit allCollapsedChanged(false);
    }
    rowsChanged();
    if (user)
        emit expansionChanged();
}

QStringList FileTree::expandedPaths() const
{
    QStringList out;
    for (const Node* n : std::as_const(m_nodes))
        if (n->folder && n != m_root && n->expanded)
            out << n->item->path();
    std::sort(out.begin(), out.end());
    return out;
}

void FileTree::setExpandedPaths(const QStringList& paths)
{
    const QSet<QString> want(paths.begin(), paths.end());
    bool any = false;
    for (Node* n : std::as_const(m_nodes)) {
        if (!n->folder || n == m_root)
            continue;
        n->expanded = want.contains(n->item->path());
        any = any || n->expanded;
    }
    if (m_allCollapsed != !any) {
        m_allCollapsed = !any;
        emit allCollapsedChanged(m_allCollapsed);
    }
    rowsChanged();
}

void FileTree::setAllCollapsed(bool collapsed)
{
    for (Node* n : std::as_const(m_nodes))
        if (n->folder && n != m_root)
            n->expanded = !collapsed;
    const bool changed = m_allCollapsed != collapsed;
    m_allCollapsed = collapsed;
    if (changed)
        emit allCollapsedChanged(collapsed);
    rowsChanged();
    emit expansionChanged();
}

// ---------------------------------------------------------------------------------------------
// rows / geometry
// ---------------------------------------------------------------------------------------------

void FileTree::updateMetrics()
{
    const ThemePtr t = currentTheme();
    const double size = t->px(QStringLiteral("--nav-item-size"), -1, 13);
    m_m.font = t->interfaceFont(size);
    m_m.lineHeight = size * t->number(QStringLiteral("--line-height-tight"), 1.3);
    const double padY = t->px(QStringLiteral("--size-4-1"), -1, 4);
    m_m.rowHeight = m_m.lineHeight + 2 * padY;
    m_m.pitch = m_m.rowHeight + t->px(QStringLiteral("--nav-item-margin-bottom"), -1, 2);
    m_m.padTop = t->px(QStringLiteral("--size-4-1"), -1, 4);
    m_m.padBottom = t->px(QStringLiteral("--size-4-6"), -1, 24);
    m_m.padX = t->px(QStringLiteral("--size-4-3"), -1, 12);
    m_m.indent = t->px(QStringLiteral("--nav-item-children-margin-start"), -1, 12) + t->px(QStringLiteral("--nav-indentation-guide-width"), -1, 1)
        + t->px(QStringLiteral("--nav-item-children-padding-start"), -1, 4);
    m_m.textLeft = t->px(QStringLiteral("--size-4-6"), -1, 24);
    m_m.textRight = t->px(QStringLiteral("--size-4-2"), -1, 8);
    m_m.radius = t->px(QStringLiteral("--nav-item-radius"), -1, 4);
    QFont tag = t->interfaceFont(9);
    tag.setWeight(QFont::DemiBold);
    tag.setCapitalization(QFont::AllUppercase);
    tag.setLetterSpacing(QFont::AbsoluteSpacing, 9 * 0.05);
    m_m.tagFont = tag;
    if (m_editor) {
        m_editor->setFont(m_m.font);
        m_editor->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0; margin: 0; min-height: 0; color: %1; "
                                               "selection-background-color: %2; selection-color: %1; }")
                                    .arg(tok(t, "--nav-item-color-active").name(QColor::HexArgb), tok(t, "--text-selection").name(QColor::HexArgb)));
    }
}

void FileTree::updateScrollRange()
{
    const int n = rowCount();
    const int total = int(std::ceil(m_m.padTop + n * m_m.pitch + m_m.padBottom));
    const int vh = viewport()->height();
    QScrollBar* sb = verticalScrollBar();
    sb->setRange(0, std::max(0, total - vh));
    sb->setPageStep(vh);
    sb->setSingleStep(int(std::round(m_m.pitch)));
}

int FileTree::rowCount() const
{
    ensureRows();
    return int(m_rows.size());
}

TAbstractFile* FileTree::itemAt(int row) const
{
    ensureRows();
    return row >= 0 && row < int(m_rows.size()) ? m_rows[size_t(row)]->item : nullptr;
}

int FileTree::rowOf(const TAbstractFile* item) const
{
    ensureRows();
    const Node* n = nodeFor(item);
    return n ? m_rowIndex.value(n, -1) : -1;
}

QStringList FileTree::visiblePaths() const
{
    ensureRows();
    QStringList out;
    out.reserve(int(m_rows.size()));
    for (const Node* n : m_rows)
        out << (n->folder ? n->item->path() + QLatin1Char('/') : n->item->path());
    return out;
}

double FileTree::rowTop(int row) const { return m_m.padTop + row * m_m.pitch; }

QRect FileTree::rowRect(int row) const
{
    ensureRows();
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    const Node* n = m_rows[size_t(row)];
    const int top = int(std::round(rowTop(row))) - verticalScrollBar()->value();
    const int bottom = int(std::round(rowTop(row) + m_m.rowHeight)) - verticalScrollBar()->value();
    const int left = int(std::round(m_m.padX + n->depth * m_m.indent));
    const int right = int(std::round(viewport()->width() - m_m.padX));
    return QRect(left, top, std::max(0, right - left), bottom - top);
}

int FileTree::rowAt(const QPoint& pos) const
{
    ensureRows();
    const double y = pos.y() + verticalScrollBar()->value() - m_m.padTop;
    if (y < 0)
        return -1;
    const int row = int(std::floor(y / m_m.pitch));
    if (row < 0 || row >= int(m_rows.size()))
        return -1;
    if (y - row * m_m.pitch > m_m.rowHeight)
        return -1; // the 2 px gap
    const QRect r = rowRect(row);
    if (pos.x() < r.left() || pos.x() > r.right())
        return -1;
    return row;
}

QRect FileTree::titleRect(int row) const
{
    const QRect r = rowRect(row);
    if (r.isNull())
        return {};
    const Node* n = m_rows[size_t(row)];
    int right = r.right() + 1 - int(m_m.textRight);
    if (!n->folder) {
        const auto* f = static_cast<const TFile*>(n->item);
        if (!f->isMarkdown()) {
            const QFontMetricsF fm(m_m.tagFont);
            right -= int(std::ceil(fm.horizontalAdvance(f->extension()) + 8));
        }
    }
    const int top = r.top() + int(std::round((r.height() - m_m.lineHeight) / 2));
    return QRect(r.left() + int(m_m.textLeft), top, std::max(10, right - r.left() - int(m_m.textLeft)), int(std::round(m_m.lineHeight)));
}

int FileTree::lastDescendantRow(int row) const
{
    ensureRows();
    if (row < 0 || row >= int(m_rows.size()))
        return row;
    const int depth = m_rows[size_t(row)]->depth;
    int last = row;
    for (int i = row + 1; i < int(m_rows.size()) && m_rows[size_t(i)]->depth > depth; ++i)
        last = i;
    return last;
}

void FileTree::scrollToItem(const TAbstractFile* item, int margin)
{
    const int row = rowOf(item);
    if (row < 0)
        return;
    QScrollBar* sb = verticalScrollBar();
    const int top = int(std::round(rowTop(row))) - margin;
    const int bottom = int(std::round(rowTop(row) + m_m.rowHeight)) + margin;
    const int vh = viewport()->height();
    if (top < sb->value())
        sb->setValue(top);
    else if (bottom > sb->value() + vh)
        sb->setValue(bottom - vh);
}

void FileTree::reveal(TAbstractFile* item)
{
    Node* n = nodeFor(item);
    if (!n)
        return;
    bool changed = false;
    for (Node* p = n->parent; p && p != m_root; p = p->parent) {
        if (!p->expanded) {
            p->expanded = true;
            changed = true;
        }
    }
    if (changed) {
        if (m_allCollapsed) {
            m_allCollapsed = false;
            emit allCollapsedChanged(false);
        }
        rowsChanged();
        emit expansionChanged();
    }
    scrollToItem(item, 4);
}

void FileTree::flash(TAbstractFile* item)
{
    m_flash = nodeFor(item);
    m_flashTimer.start(kFlashMs);
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------

void FileTree::setActiveFile(TFile* file)
{
    Node* n = nodeFor(file);
    if (n == m_activeFile)
        return;
    // Obsidian: opening another file clears the selection unless it's the focused item.
    if (n != m_focused)
        m_selected.clear();
    m_activeFile = n;
    viewport()->update();
}

TFile* FileTree::activeFile() const { return m_activeFile ? static_cast<TFile*>(m_activeFile->item) : nullptr; }

TAbstractFile* FileTree::focusedItem() const { return m_focused ? m_focused->item : nullptr; }

void FileTree::setFocusedItem(TAbstractFile* item, bool scroll) { setFocusedNode(nodeFor(item), scroll); }

void FileTree::setFocusedNode(Node* n, bool scroll)
{
    if (n == m_root)
        return;
    m_focused = n;
    if (n && scroll)
        scrollToItem(n->item, 0);
    viewport()->update();
}

QList<TAbstractFile*> FileTree::selectedItems() const
{
    ensureRows();
    QList<const Node*> nodes(m_selected.begin(), m_selected.end());
    std::sort(nodes.begin(), nodes.end(), [this](const Node* a, const Node* b) { return m_rowIndex.value(a, INT_MAX) < m_rowIndex.value(b, INT_MAX); });
    QList<TAbstractFile*> out;
    for (const Node* n : nodes)
        out << n->item;
    return out;
}

void FileTree::setSelectedItems(const QList<TAbstractFile*>& items)
{
    m_selected.clear();
    for (TAbstractFile* i : items)
        if (Node* n = nodeFor(i); n && n != m_root && !m_selected.contains(n))
            m_selected << n;
    viewport()->update();
}

void FileTree::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    viewport()->update();
}

QList<TAbstractFile*> FileTree::actionItems() const
{
    if (!m_selected.isEmpty())
        return selectedItems();
    if (m_focused)
        return {m_focused->item};
    return {};
}

void FileTree::setMenuItem(TAbstractFile* item)
{
    m_menuNode = nodeFor(item);
    viewport()->update();
}

void FileTree::setCutItems(const QList<TAbstractFile*>& items)
{
    m_cut.clear();
    for (TAbstractFile* i : items)
        if (Node* n = nodeFor(i))
            m_cut << n;
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------------------------

void FileTree::paintEvent(QPaintEvent*)
{
    ensureRows();
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    if (m_readOnly)
        p.setOpacity(0.5);
    const ThemePtr t = currentTheme();
    const int scroll = verticalScrollBar()->value();
    const int vh = viewport()->height();
    const int n = int(m_rows.size());
    if (n == 0)
        return;
    const int first = std::clamp(int(std::floor((scroll - m_m.padTop) / m_m.pitch)), 0, n - 1);
    const int last = std::clamp(int(std::ceil((scroll + vh - m_m.padTop) / m_m.pitch)), 0, n - 1);

    // .nav-folder.is-being-dragged-over: the folder's title and its children, accent at 10%.
    if (Node* hl = highlightedFolder(); hl && hl != m_root) {
        const int row = m_rowIndex.value(hl, -1);
        if (row >= 0) {
            const int lastRow = lastDescendantRow(row);
            const QRect a = rowRect(row);
            const int bottom = int(std::round(rowTop(lastRow) + m_m.rowHeight)) - scroll;
            QColor c = tok(t, "--interactive-accent");
            c.setAlphaF(0.1);
            QPainterPath path;
            path.addRoundedRect(QRectF(a.left(), a.top(), a.width(), bottom - a.top()), m_m.radius, m_m.radius);
            p.fillPath(path, c);
        }
    }

    // Indentation guides: the 1 px border of each expanded folder's children container.
    const QColor guide = tok(t, "--nav-indentation-guide-color", QColor(255, 255, 255, 30));
    const double guideW = t->px(QStringLiteral("--nav-indentation-guide-width"), -1, 1);
    const double guideOffset = t->px(QStringLiteral("--nav-item-children-margin-start"), -1, 12);
    for (int i = first; i <= last; ++i) {
        const Node* node = m_rows[size_t(i)];
        const int top = int(std::round(rowTop(i))) - scroll;
        for (int level = 0; level < node->depth; ++level) {
            const bool continues = i + 1 < n && m_rows[size_t(i + 1)]->depth > level;
            const int bottom = int(std::round(rowTop(i) + (continues ? m_m.pitch : m_m.rowHeight))) - scroll;
            const double x = m_m.padX + level * m_m.indent + guideOffset;
            p.fillRect(QRectF(x, top, guideW, bottom - top), guide);
        }
    }

    for (int i = first; i <= last; ++i)
        paintRow(p, i, rowRect(i));
}

void FileTree::paintRow(QPainter& p, int row, const QRect& r)
{
    const ThemePtr t = currentTheme();
    const Node* n = m_rows[size_t(row)];
    const bool dragged = m_drag.active && m_drag.nodes.contains(const_cast<Node*>(n));
    const bool selected = m_selected.contains(const_cast<Node*>(n));
    const bool active = n == m_activeFile;
    const bool hover = !m_drag.active && row == m_hoverRow && !m_readOnly;
    const bool flashing = n == m_flash;
    const bool dragOver = n == highlightedFolder();
    const bool renaming = n == m_renaming;
    const qreal baseOpacity = p.opacity();
    if (m_cut.contains(const_cast<Node*>(n)))
        p.setOpacity(baseOpacity * 0.5);

    QColor bg, fg = tok(t, "--nav-item-color");
    if (dragOver)
        fg = tok(t, "--nav-item-color-highlighted");
    if (hover) {
        bg = tok(t, "--nav-item-background-hover");
        fg = tok(t, "--nav-item-color-hover");
    }
    if (active) {
        bg = tok(t, "--nav-item-background-active");
        fg = dragOver ? tok(t, "--nav-item-color-highlighted") : tok(t, "--nav-item-color-active");
    }
    if (selected) {
        bg = tok(t, "--nav-item-background-selected");
        fg = tok(t, "--nav-item-color-selected");
    }
    if (dragged) {
        bg = tok(t, "--interactive-accent");
        fg = tok(t, "--text-on-accent", Qt::white);
    }
    if (flashing) {
        bg = tok(t, "--text-highlight-bg", QColor(255, 208, 0, 102));
        fg = tok(t, "--text-normal");
    }
    if (renaming)
        fg = tok(t, "--nav-item-color-active");
    const QRectF box(r);
    if (bg.isValid() && bg.alpha() > 0) {
        QPainterPath path;
        path.addRoundedRect(box, m_m.radius, m_m.radius);
        p.fillPath(path, bg);
    }
    // .has-focus (leaf active) / .is-being-renamed / .has-active-menu: 2 px ring (box-shadow).
    const bool focusRing = n == m_focused && m_leafActive;
    if (renaming || focusRing || n == m_menuNode) {
        const QColor ring = renaming && m_editor->hasFocus() ? tok(t, "--interactive-accent") : tok(t, "--background-modifier-border-focus");
        QPainterPath outer, inner;
        outer.addRoundedRect(box.adjusted(-2, -2, 2, 2), m_m.radius + 2, m_m.radius + 2);
        inner.addRoundedRect(box, m_m.radius, m_m.radius);
        p.fillPath(outer.subtracted(inner), ring);
    }

    const double lineTop = r.top() + (r.height() - m_m.lineHeight) / 2;
    // collapse triangle (.collapse-icon: 10 px, stroke 4 in a 24 px box; rotated -90° when collapsed)
    if (n->folder) {
        QColor ic = tok(t, "--nav-collapse-icon-color", tok(t, "--text-faint"));
        if (!n->expanded)
            ic = tok(t, "--nav-collapse-icon-color-collapsed", ic);
        if (dragged)
            ic = tok(t, "--text-on-accent", Qt::white);
        const QPointF c(r.left() + m_m.textLeft - 20 + 8, lineTop + m_m.lineHeight / 2);
        const double s = 10.0 / 24.0;
        p.save();
        p.setOpacity(p.opacity() * t->number(QStringLiteral("--icon-opacity"), 0.85));
        p.translate(c);
        if (!n->expanded)
            p.rotate(-90);
        QPainterPath tri;
        tri.moveTo((3 - 12) * s, (8 - 12) * s);
        tri.lineTo((12 - 12) * s, (17 - 12) * s);
        tri.lineTo((21 - 12) * s, (8 - 12) * s);
        p.setPen(QPen(ic, 4 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(tri);
        p.restore();
    }

    const QRect title = titleRect(row);
    // .nav-file-tag: uppercase extension, 9 px semibold, right-aligned
    if (!n->folder) {
        const auto* f = static_cast<const TFile*>(n->item);
        if (!f->isMarkdown()) {
            const QFontMetricsF fm(m_m.tagFont);
            const double w = fm.horizontalAdvance(f->extension()) + 8;
            const double h = 9 * 1.5;
            const QRectF tagRect(r.right() + 1 - m_m.textRight - w, lineTop + (m_m.lineHeight - h) / 2, w, h);
            QColor tc = tok(t, "--nav-tag-color", tok(t, "--text-faint"));
            if (hover)
                tc = tok(t, "--nav-tag-color-hover", tok(t, "--text-muted"));
            if (active)
                tc = tok(t, "--nav-tag-color-active", tok(t, "--text-muted"));
            if (dragged)
                tc = tok(t, "--text-normal");
            const QColor tagBg = tok(t, "--nav-tag-background");
            if (tagBg.isValid() && tagBg.alpha() > 0) {
                QPainterPath tp;
                tp.addRoundedRect(tagRect, 4, 4);
                p.fillPath(tp, tagBg);
            }
            p.setFont(m_m.tagFont);
            p.setPen(tc);
            p.drawText(tagRect, Qt::AlignCenter, f->extension());
        }
    }
    if (!renaming) {
        p.setFont(m_m.font);
        p.setPen(fg);
        const QFontMetricsF fm(m_m.font);
        const QString text = fm.elidedText(titleOf(n->item), Qt::ElideRight, title.width());
        const double baseline = lineTop + (m_m.lineHeight - (fm.ascent() + fm.descent())) / 2 + fm.ascent();
        p.drawText(QPointF(title.left(), baseline), text);
    }
    p.setOpacity(baseOpacity);
}

void FileTree::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScrollRange();
    positionEditor();
}

void FileTree::scrollContentsBy(int, int)
{
    viewport()->update();
    positionEditor();
}

void FileTree::changeEvent(QEvent* e)
{
    QAbstractScrollArea::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
        updateMetrics();
}

bool FileTree::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && m_hoverRow != -1) {
        m_hoverRow = -1;
        viewport()->update();
        if (m_handlers.unhover)
            m_handlers.unhover();
    }
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int row = rowAt(he->pos());
        if (row < 0 || m_drag.active) {
            QToolTip::hideText();
            return true;
        }
        const Node* n = m_rows[size_t(row)];
        QString text;
        const QRect title = titleRect(row);
        if (QFontMetricsF(m_m.font).horizontalAdvance(titleOf(n->item)) > title.width())
            text = titleOf(n->item);
        if (n->folder) {
            int files = 0, folders = 0;
            countTree(static_cast<const TFolder*>(n->item), &files, &folders);
            const QString f = files == 1 ? tr("%1 file").arg(files) : tr("%1 files").arg(files);
            const QString d = folders == 1 ? tr("%1 folder").arg(folders) : tr("%1 folders").arg(folders);
            text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + f + QStringLiteral(", ") + d;
        } else {
            const auto* f = static_cast<const TFile*>(n->item);
            const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm");
            text += (text.isEmpty() ? QString() : QStringLiteral("\n\n"))
                + tr("Last modified at %1").arg(QDateTime::fromMSecsSinceEpoch(f->mtimeMs()).toString(fmt)) + QLatin1Char('\n')
                + tr("Created at %1").arg(QDateTime::fromMSecsSinceEpoch(f->ctimeMs()).toString(fmt));
        }
        // Obsidian places it beside the sidebar (a side tooltip).
        const QRect r = rowRect(row);
        const QPoint at = viewport()->mapToGlobal(QPoint(viewport()->width() + 4, r.top()));
        QToolTip::showText(at, text, viewport(), r);
        return true;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

// ---------------------------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------------------------

void FileTree::updateHover(const QPoint& pos)
{
    const int row = rowAt(pos);
    if (row == m_hoverRow)
        return;
    m_hoverRow = row;
    viewport()->update();
    const Node* n = row >= 0 ? m_rows[size_t(row)] : nullptr;
    if (n && !n->folder && n != m_renaming && m_handlers.hover) {
        const QRect r = rowRect(row);
        m_handlers.hover(static_cast<TFile*>(n->item), QRect(viewport()->mapToGlobal(r.topLeft()), r.size()));
    } else if (m_handlers.unhover) {
        m_handlers.unhover();
    }
}

void FileTree::leaveEvent(QEvent* e)
{
    if (m_hoverRow != -1) {
        m_hoverRow = -1;
        viewport()->update();
        if (m_handlers.unhover)
            m_handlers.unhover();
    }
    QAbstractScrollArea::leaveEvent(e);
}

void FileTree::mousePressEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    m_pressRow = rowAt(pos);
    m_pressButton = e->button();
    m_pressPos = pos;
    m_pressOnEmpty = m_pressRow < 0;
    if (m_renaming && !(m_pressRow >= 0 && m_rows[size_t(m_pressRow)] == m_renaming)) {
        // The editor's blur: save, and leave the rename even if saving failed.
        if (!commitRename())
            cancelRename();
    }
    if (!hasFocus())
        setFocus(Qt::MouseFocusReason);
    e->accept();
}

void FileTree::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Every click counts (a double click on a folder toggles it twice, like the DOM's click events).
    mousePressEvent(e);
}

void FileTree::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag.active) {
        updateDrag(pos, e->globalPosition().toPoint());
        e->accept();
        return;
    }
    if ((e->buttons() & Qt::LeftButton) && m_pressButton == Qt::LeftButton && m_pressRow >= 0 && !m_readOnly
        && (pos - m_pressPos).manhattanLength() > kDragThreshold) {
        beginDrag(pos);
        if (m_drag.active)
            updateDrag(pos, e->globalPosition().toPoint());
        e->accept();
        return;
    }
    updateHover(pos);
    QAbstractScrollArea::mouseMoveEvent(e);
}

void FileTree::mouseReleaseEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag.active) {
        finishDrag(pos, e->globalPosition().toPoint(), e->button() == Qt::LeftButton);
        m_pressRow = -1;
        e->accept();
        return;
    }
    const int row = rowAt(pos);
    const int pressRow = m_pressRow;
    const bool pressOnEmpty = m_pressOnEmpty;
    m_pressRow = -1;
    m_pressOnEmpty = false;
    if (e->button() != m_pressButton)
        return;
    if (pressRow >= 0 && row == pressRow && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
        clickRow(row, e->button(), e->modifiers());
    } else if (pressOnEmpty && row < 0 && e->button() == Qt::LeftButton) {
        // A click on the container itself clears the selection and the focus.
        clearSelection();
        setFocusedNode(nullptr, false);
    }
    e->accept();
}

void FileTree::contextMenuEvent(QContextMenuEvent* e)
{
    if (m_drag.active)
        return;
    QPoint vp = e->pos(); // delivered through the viewport: viewport coordinates
    int row = e->reason() == QContextMenuEvent::Keyboard ? -1 : rowAt(vp);
    if (e->reason() == QContextMenuEvent::Keyboard && m_focused) {
        row = m_rowIndex.value(m_focused, -1);
        if (row >= 0)
            vp = rowRect(row).center();
    }
    TAbstractFile* item = row >= 0 ? m_rows[size_t(row)]->item : nullptr;
    if (item && item == renamingItem())
        return;
    if (!item) {
        // The empty area: the vault root's menu (Obsidian clears the selection first).
        clearSelection();
        setFocusedNode(nullptr, false);
    }
    if (m_handlers.contextMenu)
        m_handlers.contextMenu(item, e->reason() == QContextMenuEvent::Keyboard ? viewport()->mapToGlobal(vp) : e->globalPos());
    e->accept();
}

void FileTree::clickRow(int row, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    ensureRows();
    if (row < 0 || row >= int(m_rows.size()))
        return;
    Node* n = m_rows[size_t(row)];
    if (n == m_renaming)
        return;
    if (n->folder) {
        if (button != Qt::LeftButton)
            return; // no auxclick on folders
        if (handleSelection(n, mods, true, button))
            return;
        setNodeExpanded(n, !n->expanded, true);
        return;
    }
    if (handleSelection(n, mods, true, button))
        return;
    Workspace::NewLeaf mode = Workspace::NewLeaf::No;
    if (button == Qt::MiddleButton)
        mode = Workspace::NewLeaf::Tab;
    else if (mods & Qt::ControlModifier)
        mode = (mods & Qt::AltModifier) ? ((mods & Qt::ShiftModifier) ? Workspace::NewLeaf::Window : Workspace::NewLeaf::SplitRight)
                                        : Workspace::NewLeaf::Tab;
    openNode(n, mode, false);
}

// Obsidian's tree.handleItemSelection(): true when the click was consumed by the selection logic.
bool FileTree::handleSelection(Node* n, Qt::KeyboardModifiers mods, bool mouse, Qt::MouseButton button)
{
    const bool modEvent = button == Qt::MiddleButton || (mods & Qt::ControlModifier);
    if (!modEvent) {
        if ((mods & Qt::AltModifier) && !(mods & Qt::ShiftModifier)) {
            if (m_handlers.activate)
                m_handlers.activate();
            if (m_selected.contains(n)) {
                m_selected.removeAll(n);
            } else {
                m_selected << n;
                setFocusedNode(n, false);
                m_anchor = n;
            }
            viewport()->update();
            return true;
        }
        if (mods & Qt::ShiftModifier) {
            if (m_handlers.activate)
                m_handlers.activate();
            for (Node* x : m_anchor ? rangeBetween(m_anchor, n) : QList<Node*>{n})
                if (!m_selected.contains(x))
                    m_selected << x;
            viewport()->update();
            return true;
        }
        if (n == m_renaming)
            return true;
        if (n == m_activeFile) {
            // The open file: only focus it (in the explorer).
            if (m_handlers.activate)
                m_handlers.activate();
            setFocusedNode(n, false);
            return true;
        }
    }
    if (mouse) {
        clearSelection();
        setFocusedNode(nullptr, false);
    }
    m_anchor = n;
    return false;
}

QList<FileTree::Node*> FileTree::rangeBetween(Node* a, Node* b) const
{
    ensureRows();
    const int ia = m_rowIndex.value(a, -1);
    const int ib = m_rowIndex.value(b, -1);
    if (ia < 0 || ib < 0)
        return {b};
    QList<Node*> out;
    const int lo = std::min(ia, ib), hi = std::max(ia, ib);
    // Obsidian adds from the later item back to the earlier one.
    for (int i = hi; i >= lo; --i)
        out << m_rows[size_t(i)];
    return out;
}

void FileTree::openNode(Node* n, Workspace::NewLeaf mode, bool keyboardNav)
{
    if (!n || n->folder || !m_handlers.open)
        return;
    m_handlers.open(static_cast<TFile*>(n->item), mode, keyboardNav);
}

// Obsidian's onSelfClick() with a keyboard event (Enter, Space, Ctrl+Enter, → on a file, Ctrl+↑/↓).
void FileTree::keyOpen(Node* n, Qt::KeyboardModifiers mods, bool navKey)
{
    if (!n)
        return;
    if (handleSelection(n, mods, false, Qt::NoButton))
        return;
    if (n->folder) {
        setNodeExpanded(n, !n->expanded, true);
        return;
    }
    Workspace::NewLeaf mode = Workspace::NewLeaf::No;
    if (!navKey && (mods & Qt::ControlModifier))
        mode = (mods & Qt::AltModifier) ? ((mods & Qt::ShiftModifier) ? Workspace::NewLeaf::Window : Workspace::NewLeaf::SplitRight)
                                        : Workspace::NewLeaf::Tab;
    openNode(n, mode, navKey);
}

// ---------------------------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------------------------

bool FileTree::wantsKey(const QKeyEvent* e) const
{
    if (m_renaming)
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const bool none = mods == Qt::NoModifier;
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        return mods == Qt::NoModifier || mods == Qt::ShiftModifier || mods == Qt::ControlModifier;
    case Qt::Key_Left:
    case Qt::Key_Right:
        return mods == Qt::NoModifier || mods == Qt::ControlModifier;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return none || mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::AltModifier);
    case Qt::Key_Space:
    case Qt::Key_F2:
    case Qt::Key_Delete:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        return none;
    case Qt::Key_Escape:
        return none && (m_drag.active || !m_selected.isEmpty());
    case Qt::Key_C:
    case Qt::Key_X:
        return mods == Qt::ControlModifier && (m_focused || !m_selected.isEmpty()) && m_handlers.copy;
    case Qt::Key_V:
        return mods == Qt::ControlModifier && m_handlers.paste;
    default:
        return false;
    }
}

bool FileTree::handleKey(QKeyEvent* e)
{
    if (!wantsKey(e))
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    ensureRows();
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        moveFocus(e->key() == Qt::Key_Down, mods, e);
        return true;
    case Qt::Key_Left: {
        Node* f = m_focused;
        if (!f)
            return true;
        if (f->folder) {
            if (!f->expanded) {
                if (f->parent && f->parent != m_root)
                    setFocusedNode(f->parent, true);
            } else {
                setNodeExpanded(f, false, true);
            }
        } else if (f->parent == m_root) {
            setAllCollapsed(true);
        } else {
            setNodeExpanded(f->parent, false, true);
        }
        Node* x = m_focused;
        while (x && x->parent && x->parent != m_root && !x->parent->expanded)
            x = x->parent;
        setFocusedNode(x, true);
        return true;
    }
    case Qt::Key_Right: {
        Node* f = m_focused;
        if (!f)
            return true;
        if (!f->folder) {
            keyOpen(f, mods, false);
        } else if (!f->expanded) {
            setNodeExpanded(f, true, true);
        } else {
            moveFocus(true, Qt::NoModifier, nullptr);
        }
        return true;
    }
    case Qt::Key_Escape:
        if (m_drag.active)
            cancelDrag();
        else
            clearSelection();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        keyOpen(m_focused, mods, false);
        return true;
    case Qt::Key_F2:
        if (m_focused && !m_readOnly)
            startRename(m_focused->item);
        return true;
    case Qt::Key_Delete:
        if (!m_readOnly)
            deleteSelection();
        return true;
    case Qt::Key_PageUp:
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
        return true;
    case Qt::Key_PageDown:
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
        return true;
    case Qt::Key_Home:
        verticalScrollBar()->setValue(0);
        return true;
    case Qt::Key_End:
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return true;
    case Qt::Key_C:
    case Qt::Key_X: {
        const QList<TAbstractFile*> items = actionItems();
        if (!items.isEmpty() && (e->key() == Qt::Key_C || !m_readOnly))
            m_handlers.copy(items, e->key() == Qt::Key_X);
        return true;
    }
    case Qt::Key_V: {
        if (m_readOnly)
            return true;
        Node* target = m_focused ? (m_focused->folder ? m_focused : m_focused->parent) : m_root;
        if (target)
            m_handlers.paste(static_cast<TFolder*>(target->item));
        return true;
    }
    default:
        return false;
    }
}

void FileTree::keyPressEvent(QKeyEvent* e)
{
    if (handleKey(e)) {
        e->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

// Obsidian's changeFocusedItem() (+ Shift: extend the selection, Ctrl: open the file without focusing it).
void FileTree::moveFocus(bool forward, Qt::KeyboardModifiers mods, QKeyEvent*)
{
    ensureRows();
    const int n = int(m_rows.size());
    if (n == 0)
        return;
    Node* old = m_focused;
    Node* start = m_focused && m_rowIndex.contains(m_focused) ? m_focused : nullptr;
    if (!start && m_anchor && m_rowIndex.contains(m_anchor))
        start = m_anchor;
    if (!start && !m_selected.isEmpty() && m_rowIndex.contains(m_selected.last()))
        start = m_selected.last();
    Node* target = nullptr;
    if (!start) {
        target = forward ? m_rows.front() : m_rows.back();
    } else {
        const int i = m_rowIndex.value(start) + (forward ? 1 : -1);
        if (i >= 0 && i < n)
            target = m_rows[size_t(i)];
    }
    if (target)
        setFocusedNode(target, true);
    if (m_handlers.activate)
        m_handlers.activate();
    if ((mods & Qt::ControlModifier) && target && !target->folder)
        keyOpen(target, mods, true);
    if ((mods & Qt::ShiftModifier) && m_focused) {
        if (old && old != m_root && !m_selected.contains(old))
            m_selected << old;
        if (!m_selected.contains(m_focused))
            m_selected << m_focused;
    }
    viewport()->update();
}

void FileTree::deleteSelection()
{
    const QList<TAbstractFile*> items = actionItems();
    if (items.isEmpty() || !m_handlers.remove)
        return;
    // Where the focus goes afterwards: the previous item not being deleted, else the next one.
    ensureRows();
    QList<Node*> nodes;
    for (TAbstractFile* i : items)
        if (Node* n = nodeFor(i))
            nodes << n;
    auto inDeleted = [&](Node* x) {
        for (Node* d : nodes)
            for (Node* p = x; p; p = p->parent)
                if (p == d)
                    return true;
        return false;
    };
    Node* next = nullptr;
    int i = m_focused ? m_rowIndex.value(m_focused, -1) : -1;
    for (int k = i; k >= 0; --k)
        if (!inDeleted(m_rows[size_t(k)])) {
            next = m_rows[size_t(k)];
            break;
        }
    if (!next && !nodes.isEmpty()) {
        for (int k = m_rowIndex.value(nodes.first(), -1); k >= 0 && k < int(m_rows.size()); ++k)
            if (!inDeleted(m_rows[size_t(k)])) {
                next = m_rows[size_t(k)];
                break;
            }
    }
    m_focusAfterDelete = next ? next->item->path() : QString();
    m_handlers.remove(items);
}

void FileTree::focusInEvent(QFocusEvent* e)
{
    if (!m_keyGuard)
        m_keyGuard = new TreeKeyGuard(this);
    qApp->removeEventFilter(m_keyGuard);
    qApp->installEventFilter(m_keyGuard);
    QAbstractScrollArea::focusInEvent(e);
    viewport()->update();
}

void FileTree::focusOutEvent(QFocusEvent* e)
{
    if (m_keyGuard)
        qApp->removeEventFilter(m_keyGuard);
    QAbstractScrollArea::focusOutEvent(e);
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------------------------

TAbstractFile* FileTree::renamingItem() const { return m_renaming ? m_renaming->item : nullptr; }

void FileTree::startRename(TAbstractFile* item)
{
    Node* n = nodeFor(item);
    if (!n || n == m_root || m_readOnly)
        return;
    if (m_renaming && m_renaming != n) {
        if (!commitRename())
            cancelRename();
    }
    cancelDrag();
    reveal(item);
    m_renaming = n;
    setFocusedNode(n, true);
    m_editor->setText(titleOf(item));
    m_editor->selectAll();
    m_editor->show();
    positionEditor();
    m_editor->setFocus(Qt::OtherFocusReason);
    if (m_error)
        m_error->dismiss();
    viewport()->update();
}

void FileTree::positionEditor()
{
    if (!m_editor || !m_renaming) {
        if (m_editor && m_editor->isVisible())
            m_editor->hide();
        return;
    }
    const int row = rowOf(m_renaming->item);
    if (row < 0) {
        m_editor->hide();
        return;
    }
    QRect r = titleRect(row).adjusted(-2, 0, 0, 0); // QLineEdit's own 2 px text margin
    const int h = std::max(r.height(), m_editor->sizeHint().height());
    r.setTop(r.center().y() - h / 2);
    r.setHeight(h);
    m_editor->setGeometry(r);
    if (!m_editor->isVisible())
        m_editor->show();
}

void FileTree::onRenameEdited()
{
    if (!m_renaming || !m_handlers.validateName)
        return;
    const QString err = m_handlers.validateName(m_renaming->item, m_editor->text().trimmed(), false);
    if (err.isEmpty()) {
        if (m_error)
            m_error->dismiss();
        return;
    }
    showRenameError(err, false);
}

void FileTree::showRenameError(const QString& message, bool transient)
{
    if (!m_renaming)
        return;
    if (!m_error)
        m_error = new ErrorTooltip(window());
    const int row = rowOf(m_renaming->item);
    const QRect r = row >= 0 ? rowRect(row) : QRect();
    m_error->showAt(message, QRect(viewport()->mapToGlobal(r.topLeft()), r.size()), transient ? 2000 : 0);
}

bool FileTree::commitRename()
{
    if (!m_renaming)
        return true;
    Node* n = m_renaming;
    // Obsidian's getNewPathAfterRename(): control characters become spaces, then trim.
    QString name = m_editor->text();
    for (QChar& c : name)
        if (c.unicode() < 0x20)
            c = QLatin1Char(' ');
    name = name.trimmed();
    if (m_handlers.validateName) {
        const QString err = m_handlers.validateName(n->item, name, true);
        if (!err.isEmpty()) {
            showRenameError(err, true);
            return false;
        }
    }
    if (name != titleOf(n->item) && m_handlers.rename) {
        m_committing = true;
        QPointer<FileTree> self(this);
        QString err;
        const bool ok = m_handlers.rename(n->item, name, &err);
        if (!self)
            return ok;
        m_committing = false;
        if (!ok) {
            if (m_renaming == n)
                showRenameError(err.isEmpty() ? tr("Could not rename the file.") : err, true);
            return false;
        }
    }
    cancelRename();
    if (nodeFor(n->item) == n) {
        QTimer::singleShot(0, this, [this, item = n->item] {
            if (nodeFor(item))
                scrollToItem(item, 4);
        });
    }
    return true;
}

void FileTree::cancelRename()
{
    if (!m_renaming)
        return;
    m_renaming = nullptr;
    const bool hadFocus = m_editor->hasFocus();
    m_editor->hide();
    if (m_error)
        m_error->dismiss();
    if (hadFocus)
        setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

bool FileTree::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_editor) {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                commitRename();
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                cancelRename();
                return true;
            }
        } else if (ev->type() == QEvent::FocusOut && m_renaming && !m_committing) {
            const auto reason = static_cast<QFocusEvent*>(ev)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
                // Obsidian's blur: save, then leave the rename even if saving failed.
                QTimer::singleShot(0, this, [this] {
                    if (m_renaming && !m_editor->hasFocus() && !m_committing && !commitRename())
                        cancelRename();
                });
            }
        } else if (ev->type() == QEvent::FocusIn) {
            viewport()->update();
        }
    }
    return QAbstractScrollArea::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------------------------
// drag & drop
// ---------------------------------------------------------------------------------------------

TFolder* FileTree::dropTarget() const
{
    return m_drag.active && m_drag.target ? static_cast<TFolder*>(m_drag.target->item) : nullptr;
}

QList<TAbstractFile*> FileTree::draggedItems() const
{
    QList<TAbstractFile*> out;
    for (const Node* n : m_drag.nodes)
        out << n->item;
    return out;
}

void FileTree::beginDrag(const QPoint& pos)
{
    Q_UNUSED(pos);
    ensureRows();
    if (m_pressRow < 0 || m_pressRow >= int(m_rows.size()))
        return;
    Node* pressed = m_rows[size_t(m_pressRow)];
    if (pressed == m_renaming)
        return;
    QList<Node*> nodes;
    const bool multi = m_selected.size() > 1 || (m_selected.size() == 1 && m_selected.first() != pressed);
    if (multi && m_selected.contains(pressed)) {
        // Obsidian drags the whole selection, minus items inside selected folders.
        for (Node* n : std::as_const(m_selected)) {
            bool inside = false;
            for (Node* p = n->parent; p && !inside; p = p->parent)
                inside = m_selected.contains(p);
            if (!inside)
                nodes << n;
        }
        std::sort(nodes.begin(), nodes.end(), [this](Node* a, Node* b) { return m_rowIndex.value(a) < m_rowIndex.value(b); });
    } else {
        nodes << pressed;
    }
    m_drag = Drag{};
    m_drag.active = true;
    m_drag.nodes = nodes;
    m_hoverRow = -1;
    QString icon, title;
    if (nodes.size() == 1) {
        TAbstractFile* item = nodes.first()->item;
        icon = item->isFolder() ? QStringLiteral("folder-open") : QStringLiteral("file");
        const auto* f = item->isFolder() ? nullptr : static_cast<const TFile*>(item);
        title = f && f->isMarkdown() ? f->basename() : item->name(); // getShortName()
    } else {
        int files = 0, folders = 0;
        for (const Node* n : std::as_const(nodes))
            (n->folder ? folders : files)++;
        icon = QStringLiteral("files");
        if (files > 0 && folders > 0)
            title = tr("%1 files and %2 folders").arg(files).arg(folders);
        else if (folders > 0)
            title = tr("%1 folders").arg(folders);
        else
            title = tr("%1 files").arg(files);
    }
    m_pressRow = -1; // a drag is never a click, even when cancelled
    m_ghost = new DragGhost(window(), icon, title);
    QApplication::setOverrideCursor(Qt::ClosedHandCursor); // body.is-grabbing
    QToolTip::hideText();
    viewport()->update();
}

FileTree::Node* FileTree::dropNodeAt(const QPoint& vp) const
{
    ensureRows();
    if (!m_root)
        return nullptr;
    const double y = vp.y() + verticalScrollBar()->value() - m_m.padTop;
    const int row = y < 0 ? -1 : int(std::floor(y / m_m.pitch));
    if (row < 0 || row >= int(m_rows.size()))
        return m_root; // empty space: the vault root
    Node* n = m_rows[size_t(row)];
    // The innermost .nav-folder element under the pointer: a folder row is its own element; left of an
    // item, the pointer is in an ancestor's children container.
    const int levelAtX = int(std::floor((vp.x() - m_m.padX) / m_m.indent));
    Node* target = n->folder ? n : n->parent;
    while (target && target != m_root && target->depth > levelAtX)
        target = target->parent;
    return target ? target : m_root;
}

bool FileTree::canDropInto(Node* folder) const
{
    if (!folder || !folder->folder)
        return false;
    for (const Node* n : m_drag.nodes) {
        if (n == folder || n == m_root)
            return false;
        for (const Node* p = folder; p; p = p->parent)
            if (p == n)
                return false; // into itself / its own subtree
    }
    return true;
}

void FileTree::updateDrag(const QPoint& vp, const QPoint& global)
{
    if (!m_drag.active)
        return;
    m_drag.lastViewportPos = vp;
    m_drag.lastGlobalPos = global;
    if (m_ghost)
        m_ghost->moveToGlobal(global);
    const bool inside = viewport()->rect().contains(vp);
    if (inside) {
        if (m_drag.outside) {
            m_drag.outside = false;
            m_drag.outsideAccepts = false;
            if (m_handlers.dragOutsideLeave)
                m_handlers.dragOutsideLeave();
        }
        Node* target = dropNodeAt(vp);
        if (!canDropInto(target))
            target = nullptr;
        if (target != m_drag.target) {
            m_drag.target = target;
            viewport()->update();
        }
        if (m_ghost) {
            QString action;
            if (target)
                action = tr("Move into “%1”").arg(target == m_root ? (m_vault ? m_vault->name() : QString()) : target->item->name());
            m_ghost->setAction(action);
        }
        if (target != m_drag.expandCandidate) {
            m_drag.expandCandidate = target;
            m_expandTimer.stop();
            if (target && target != m_root && !target->expanded)
                m_expandTimer.start();
        }
        const int vh = viewport()->height();
        if (vp.y() < kAutoScrollEdge || vp.y() > vh - kAutoScrollEdge) {
            if (!m_autoScroll.isActive())
                m_autoScroll.start();
        } else {
            m_autoScroll.stop();
        }
    } else {
        m_expandTimer.stop();
        m_autoScroll.stop();
        m_drag.expandCandidate = nullptr;
        if (m_drag.target) {
            m_drag.target = nullptr;
            viewport()->update();
        }
        m_drag.outside = true;
        m_drag.outsideAccepts = m_handlers.dragOutsideMove ? m_handlers.dragOutsideMove(draggedItems(), global) : false;
        if (m_ghost)
            m_ghost->setAction({});
    }
}

void FileTree::autoScrollTick()
{
    if (!m_drag.active) {
        m_autoScroll.stop();
        return;
    }
    const QPoint vp = m_drag.lastViewportPos;
    const int vh = viewport()->height();
    QScrollBar* sb = verticalScrollBar();
    int delta = 0;
    if (vp.y() < kAutoScrollEdge)
        delta = -std::max(2, (kAutoScrollEdge - vp.y()) / 2);
    else if (vp.y() > vh - kAutoScrollEdge)
        delta = std::max(2, (vp.y() - (vh - kAutoScrollEdge)) / 2);
    if (delta == 0) {
        m_autoScroll.stop();
        return;
    }
    const int before = sb->value();
    sb->setValue(before + delta);
    if (sb->value() == before)
        m_autoScroll.stop();
    updateDrag(vp, m_drag.lastGlobalPos);
}

void FileTree::finishDrag(const QPoint& vp, const QPoint& global, bool commit)
{
    if (!m_drag.active)
        return;
    if (commit)
        updateDrag(vp, global);
    m_expandTimer.stop();
    m_autoScroll.stop();
    delete m_ghost.data();
    QApplication::restoreOverrideCursor();
    const QList<TAbstractFile*> items = draggedItems();
    Node* target = m_drag.target;
    const bool outside = m_drag.outside;
    m_drag = Drag{};
    viewport()->update();
    if (commit && !items.isEmpty()) {
        if (outside) {
            if (m_handlers.dragOutsideDrop)
                m_handlers.dragOutsideDrop(items, global);
        } else if (target && m_handlers.move) {
            QList<TAbstractFile*> moving;
            for (TAbstractFile* i : items)
                if (i->parent() != target->item)
                    moving << i;
            if (!moving.isEmpty()) {
                if (items.size() > 1 && !target->expanded && target != m_root)
                    clearSelection(); // Obsidian: the moved selection disappears into a collapsed folder
                m_handlers.move(moving, static_cast<TFolder*>(target->item));
            }
        }
    }
    if (outside && m_handlers.dragOutsideLeave)
        m_handlers.dragOutsideLeave();
}

void FileTree::cancelDrag()
{
    if (!m_drag.active)
        return;
    finishDrag(m_drag.lastViewportPos, m_drag.lastGlobalPos, false);
}

void FileTree::setExternalTarget(Node* target)
{
    if (target == m_externalTarget)
        return;
    m_externalTarget = target;
    m_drag.expandCandidate = target;
    m_expandTimer.stop();
    if (target && target != m_root && !target->expanded)
        m_expandTimer.start();
    viewport()->update();
}

void FileTree::dragEnterEvent(QDragEnterEvent* e)
{
    // Files from another application (our own drags don't use QDrag).
    bool local = false;
    if (e->mimeData()->hasUrls())
        for (const QUrl& u : e->mimeData()->urls())
            local = local || u.isLocalFile();
    if (!local || m_readOnly || !m_handlers.importFiles || !m_root) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    setExternalTarget(dropNodeAt(e->position().toPoint()));
}

void FileTree::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_root || m_readOnly) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    setExternalTarget(dropNodeAt(e->position().toPoint()));
}

void FileTree::dragLeaveEvent(QDragLeaveEvent* e)
{
    setExternalTarget(nullptr);
    e->accept();
}

void FileTree::dropEvent(QDropEvent* e)
{
    Node* target = dropNodeAt(e->position().toPoint());
    setExternalTarget(nullptr);
    if (!target || m_readOnly || !m_handlers.importFiles) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    m_handlers.importFiles(e->mimeData()->urls(), static_cast<TFolder*>(target->item));
}

// ---------------------------------------------------------------------------------------------
// ErrorTooltip
// ---------------------------------------------------------------------------------------------

ErrorTooltip::ErrorTooltip(QWidget* window)
    : QWidget(window)
{
    setObjectName(QStringLiteral("ocErrorTooltip"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ErrorTooltip::dismiss);
    hide();
}

void ErrorTooltip::showAt(const QString& message, const QRect& anchorGlobal, int timeoutMs)
{
    m_text = message;
    QWidget* w = parentWidget();
    if (!w)
        return;
    const ThemePtr t = currentTheme();
    const QFont f = [&] {
        QFont x = t->interfaceFont(t->px(QStringLiteral("--font-ui-smaller"), -1, 12));
        x.setWeight(QFont::Medium);
        return x;
    }();
    setFont(f);
    // .tooltip.mod-error: 200 px wide, 4/8 px padding, text centered and wrapped; the arrow points up.
    const int width = 200;
    const QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(QRect(0, 0, width - 16, 10000), Qt::TextWordWrap | Qt::AlignHCenter, message);
    const int height = textRect.height() + 8 + 5;
    const QPoint anchor = w->mapFromGlobal(QPoint(anchorGlobal.center().x(), anchorGlobal.bottom() + 1));
    int x = anchor.x() - width / 2;
    x = std::clamp(x, 4, std::max(4, w->width() - width - 4));
    m_arrowX = anchor.x() - x;
    setGeometry(x, anchor.y() + 3, width, height);
    show();
    raise();
    update();
    if (timeoutMs > 0)
        m_timer.start(timeoutMs);
    else
        m_timer.stop();
}

void ErrorTooltip::dismiss()
{
    m_timer.stop();
    hide();
}

void ErrorTooltip::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor bg = tok(t, "--background-modifier-error", QColor(0xfb, 0x46, 0x4c));
    const QRectF box(0, 5, width(), height() - 5);
    QPainterPath path;
    path.addRoundedRect(box, 4, 4);
    QPainterPath arrow;
    const double ax = std::clamp(double(m_arrowX), 9.0, width() - 9.0);
    arrow.moveTo(ax - 5, 5);
    arrow.lineTo(ax, 0);
    arrow.lineTo(ax + 5, 5);
    arrow.closeSubpath();
    p.fillPath(path.united(arrow), bg);
    p.setPen(tok(t, "--text-on-accent", Qt::white));
    p.setFont(font());
    p.drawText(box.adjusted(8, 4, -8, -4), Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignVCenter, m_text);
}

// ---------------------------------------------------------------------------------------------
// DragGhost
// ---------------------------------------------------------------------------------------------

DragGhost::DragGhost(QWidget* window, const QString& icon, const QString& title)
    : QWidget(window)
    , m_icon(icon)
    , m_title(title)
{
    setObjectName(QStringLiteral("ocDragGhost"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    resizeToContents();
    show();
    raise();
}

void DragGhost::setAction(const QString& action)
{
    if (action == m_action)
        return;
    m_action = action;
    resizeToContents();
    update();
}

void DragGhost::moveToGlobal(const QPoint& globalPos)
{
    if (QWidget* w = parentWidget())
        move(w->mapFromGlobal(globalPos) + QPoint(5, 5)); // Obsidian: 5 px below/right of the pointer
    raise();
}

void DragGhost::resizeToContents()
{
    const ThemePtr t = currentTheme();
    QFont f = t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13));
    f.setWeight(QFont::Medium);
    const QFontMetrics fm(f);
    const QFont af = t->interfaceFont(t->px(QStringLiteral("--font-ui-smaller"), -1, 12));
    const QFontMetrics am(af);
    const int textW = fm.horizontalAdvance(m_title) + 14 + 4;
    const int actionW = m_action.isEmpty() ? 0 : am.horizontalAdvance(m_action);
    const int w = std::min(300, std::max(textW, actionW) + 16);
    const int h = 6 + int(std::round(13 * 1.5)) + (m_action.isEmpty() ? 0 : 2 + int(std::round(12 * 1.5))) + 6;
    resize(w, h);
}

void DragGhost::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 4, 4);
    p.fillPath(path, tok(t, "--drag-ghost-background", QColor(0, 0, 0, 217)));
    const QColor fg = tok(t, "--drag-ghost-text-color", Qt::white);
    QFont f = t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13));
    f.setWeight(QFont::Medium);
    const int lineH = int(std::round(13 * 1.5));
    // icon (--icon-xs, 70%) + title
    p.setOpacity(0.7);
    p.drawPixmap(QPointF(8, 6 + (lineH - 14) / 2.0), IconProvider::instance().pixmap(m_icon, 14, fg, 2.0, devicePixelRatioF()));
    p.setOpacity(1);
    p.setFont(f);
    p.setPen(fg);
    const QFontMetrics fm(f);
    p.drawText(QRect(8 + 14 + 4, 6, width() - 8 - 14 - 4 - 8, lineH), Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(m_title, Qt::ElideRight, width() - 8 - 14 - 4 - 8));
    if (!m_action.isEmpty()) {
        const QFont af = t->interfaceFont(t->px(QStringLiteral("--font-ui-smaller"), -1, 12));
        p.setFont(af);
        p.setOpacity(0.7);
        const QFontMetrics am(af);
        p.drawText(QRect(8, 6 + lineH + 2, width() - 16, int(std::round(12 * 1.5))), Qt::AlignVCenter | Qt::AlignLeft,
                   am.elidedText(m_action, Qt::ElideRight, width() - 16));
    }
}

} // namespace oc
