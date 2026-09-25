#include "ui/widgets/tree_list.h"

#include "core/vault/vault.h" // file_tree.h needs the complete type
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/views/file_explorer/file_tree.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>
#include <QTextLayout>
#include <QToolTip>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>

namespace oc {

namespace {

QColor tok(const ThemePtr& t, const char* name, const QColor& fallback = {})
{
    return t->color(QString::fromLatin1(name), fallback);
}

constexpr int kDragThreshold = 5;
constexpr int kHoverIcon = 12; // the match rows' more-context chevrons (both fit a one-line match)
constexpr int kExpandOnHoverMs = 750;
constexpr int kAutoScrollEdge = 24;

// Filters the key events of a focused tree before the application's hotkey dispatcher sees them
// (installed on focus-in, so it runs before filters installed earlier).
class TreeListKeyGuard : public QObject {
public:
    explicit TreeListKeyGuard(TreeList* tree)
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
            ke->accept();
            return true;
        }
        return m_tree->handleKey(ke);
    }

private:
    TreeList* m_tree;
};

// Lines of a QTextLayout laid out at `width`; returns the height.
int layoutLines(QTextLayout& layout, double width, double lineHeight)
{
    layout.beginLayout();
    double y = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(std::max(1.0, width));
        line.setLeadingIncluded(false);
        const double lead = (lineHeight - line.height()) / 2;
        line.setPosition(QPointF(0, y + lead));
        y += lineHeight;
    }
    layout.endLayout();
    return int(std::ceil(y));
}

QString forLayout(QString s)
{
    for (QChar& c : s)
        if (c == u'\n')
            c = QChar::LineSeparator;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// metrics and node layout
// ---------------------------------------------------------------------------------------------

struct TreeList::Metrics {
    QFont font, headerFont, subFont, flairFont, matchFont, boldFont;
    double lineHeight = 16.9;
    double subLineHeight = 14.4;
    double matchLineHeight = 15.6;
    int padY = 4;
    int padLeft = 24;
    int padRight = 8;
    int headerPadLeft = 8;
    int marginBottom = 2;
    int indent = 17;
    int guideOffset = 12;
    int radius = 4;
    int matchPadTop = 8, matchPadRight = 20, matchPadBottom = 8, matchPadLeft = 12;
    int boxMarginTop = 4, boxMarginBottom = 8;
    int flairGap = 4;
    int iconSize = 14;
};

struct TreeList::Node::Layout {
    int width = -1;
    quint64 generation = 0;
    int height = 0;       // box height
    int textHeight = 0;   // title / snippet lines
    int flairWidth = 0;
    bool wrapped = false; // title needs the QTextLayout
    std::unique_ptr<QTextLayout> text;
    QString shown;        // Match: the laid out text ("..." included)
    int shownOffset = 0;  // Match: index in `shown` of content[from]
    QList<QRect> chips;   // Match: chip boxes relative to the text origin
    int chipsHeight = 0;
};

TreeList::Node* TreeList::Node::add(Kind k, const QString& nodeId)
{
    auto n = std::make_unique<Node>();
    n->kind = k;
    n->id = nodeId;
    return add(std::move(n));
}

TreeList::Node* TreeList::Node::add(std::unique_ptr<Node> child)
{
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

bool TreeList::Node::hasVisibleChildren() const
{
    for (const auto& c : children)
        if (!c->hidden)
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------------------------

TreeList::TreeList(QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_m(std::make_unique<Metrics>())
{
    setObjectName(QStringLiteral("ocTreeList"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAutoFillBackground(false);
    viewport()->setMouseTracking(true);

    m_editor = new QLineEdit(viewport());
    m_editor->setObjectName(QStringLiteral("ocTreeListRename"));
    m_editor->setProperty("ocFlat", true);
    m_editor->setFrame(false);
    m_editor->hide();
    m_editor->installEventFilter(this);

    m_expandTimer.setSingleShot(true);
    m_expandTimer.setInterval(kExpandOnHoverMs);
    connect(&m_expandTimer, &QTimer::timeout, this, [this] {
        if (m_drag.active && m_drag.target && m_drag.placement == DropPlacement::Inside && m_drag.target->collapsed)
            setCollapsed(m_drag.target, false);
    });
    m_autoScroll.setInterval(16);
    connect(&m_autoScroll, &QTimer::timeout, this, [this] {
        if (!m_drag.active) {
            m_autoScroll.stop();
            return;
        }
        const int y = m_drag.lastViewportPos.y();
        const int vh = viewport()->height();
        int delta = 0;
        if (y < kAutoScrollEdge)
            delta = -std::max(2, (kAutoScrollEdge - y) / 2);
        else if (y > vh - kAutoScrollEdge)
            delta = std::max(2, (y - (vh - kAutoScrollEdge)) / 2);
        QScrollBar* sb = verticalScrollBar();
        const int before = sb->value();
        sb->setValue(before + delta);
        if (delta == 0 || sb->value() == before)
            m_autoScroll.stop();
        updateDrag(m_drag.lastViewportPos, m_drag.lastGlobalPos);
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { positionEditor(); });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateMetrics();
            invalidate();
        });
    updateMetrics();
}

TreeList::~TreeList()
{
    if (m_keyGuard)
        qApp->removeEventFilter(m_keyGuard);
    if (m_drag.active)
        QApplication::restoreOverrideCursor();
    delete m_ghost.data();
}

void TreeList::updateMetrics()
{
    const ThemePtr t = currentTheme();
    const double size = t->px(QStringLiteral("--nav-item-size"), -1, 13);
    const double tight = t->number(QStringLiteral("--line-height-tight"), 1.3);
    m_m->font = t->interfaceFont(size);
    m_m->headerFont = m_m->font;
    m_m->headerFont.setWeight(QFont::Medium);
    m_m->boldFont = m_m->font;
    m_m->boldFont.setWeight(QFont::Bold);
    m_m->subFont = t->interfaceFont(size * 0.85);
    m_m->flairFont = t->interfaceFont(t->px(QStringLiteral("--font-ui-smaller"), -1, 12));
    m_m->matchFont = m_m->flairFont;
    m_m->lineHeight = size * tight;
    m_m->subLineHeight = size * 0.85 * tight;
    m_m->matchLineHeight = t->px(QStringLiteral("--font-ui-smaller"), -1, 12) * tight;
    m_m->padY = int(t->px(QStringLiteral("--size-4-1"), -1, 4));
    m_m->padLeft = int(t->px(QStringLiteral("--size-4-6"), -1, 24));
    m_m->padRight = int(t->px(QStringLiteral("--size-4-2"), -1, 8));
    m_m->headerPadLeft = int(t->px(QStringLiteral("--size-4-2"), -1, 8));
    m_m->marginBottom = int(t->px(QStringLiteral("--nav-item-margin-bottom"), -1, 2));
    m_m->guideOffset = int(t->px(QStringLiteral("--nav-item-children-margin-start"), -1, 12));
    m_m->indent = m_m->guideOffset + int(t->px(QStringLiteral("--nav-indentation-guide-width"), -1, 1))
        + int(t->px(QStringLiteral("--nav-item-children-padding-start"), -1, 4));
    m_m->radius = int(t->px(QStringLiteral("--nav-item-radius"), -1, 4));
    m_m->iconSize = int(t->px(QStringLiteral("--icon-xs"), -1, 14));
    if (m_editor) {
        m_editor->setFont(m_m->font);
        m_editor->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0; margin: 0; min-height: 0; color: %1; "
                                               "selection-background-color: %2; selection-color: %1; }")
                                    .arg(tok(t, "--nav-item-color-active").name(QColor::HexArgb), tok(t, "--text-selection").name(QColor::HexArgb)));
    }
}

void TreeList::setContentMargins(int left, int top, int right, int bottom)
{
    m_marginLeft = left;
    m_marginTop = top;
    m_marginRight = right;
    m_marginBottom = bottom;
    invalidate();
}

void TreeList::setLeafActive(bool active)
{
    if (active == m_leafActive)
        return;
    m_leafActive = active;
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------------------------

void TreeList::setRoots(std::vector<NodePtr> roots, bool keepCollapse)
{
    // Remember the state by id.
    QHash<QString, bool> collapsed;
    const QString focused = m_focused ? m_focused->id : QString();
    const QString anchor = m_anchor ? m_anchor->id : QString();
    const QString menu = m_menuNode ? m_menuNode->id : QString();
    QSet<QString> selected;
    for (const Node* n : std::as_const(m_selected))
        selected.insert(n->id);
    if (keepCollapse)
        for (const Node* n : allNodes())
            if (n->collapsible && !n->id.isEmpty())
                collapsed.insert(n->id, n->collapsed);
    if (m_renaming)
        cancelRename();
    if (m_drag.active)
        finishDrag(false);
    m_roots = std::move(roots);
    m_focused = m_anchor = m_menuNode = nullptr;
    m_selected.clear();
    m_hoverRow = -1;
    for (Node* n : allNodes()) {
        for (const auto& c : n->children)
            c->parent = n;
        if (n->id.isEmpty())
            continue;
        if (auto it = collapsed.constFind(n->id); it != collapsed.cend() && n->collapsible)
            n->collapsed = *it;
        if (!focused.isEmpty() && n->id == focused)
            m_focused = n;
        if (!anchor.isEmpty() && n->id == anchor)
            m_anchor = n;
        if (!menu.isEmpty() && n->id == menu)
            m_menuNode = n;
        if (selected.contains(n->id))
            m_selected << n;
    }
    for (const auto& r : m_roots)
        r->parent = nullptr;
    invalidate();
}

void TreeList::clear() { setRoots({}, false); }

QList<TreeList::Node*> TreeList::allNodes() const
{
    QList<Node*> out;
    std::function<void(const std::vector<NodePtr>&)> walk = [&](const std::vector<NodePtr>& list) {
        for (const auto& n : list) {
            out << n.get();
            walk(n->children);
        }
    };
    walk(m_roots);
    return out;
}

TreeList::Node* TreeList::findNode(const QString& id) const
{
    for (Node* n : allNodes())
        if (n->id == id)
            return n;
    return nullptr;
}

void TreeList::nodeChanged(Node* node)
{
    if (node)
        node->layout.reset();
    invalidate();
}

void TreeList::layoutChanged() { invalidate(); }

void TreeList::invalidate()
{
    m_rowsDirty = true;
    m_geometryDirty = true;
    m_hoverRow = -1;
    updateScrollRange();
    positionEditor();
    viewport()->update();
    emit rowsChanged();
}

// ---------------------------------------------------------------------------------------------
// rows and geometry
// ---------------------------------------------------------------------------------------------

void TreeList::ensureRows() const
{
    if (!m_rowsDirty)
        return;
    m_rowsDirty = false;
    m_geometryDirty = true;
    m_rows.clear();
    m_rowIndex.clear();
    std::function<void(const std::vector<NodePtr>&)> walk = [&](const std::vector<NodePtr>& list) {
        for (const auto& n : list) {
            if (n->hidden)
                continue;
            m_rowIndex.insert(n.get(), int(m_rows.size()));
            m_rows.push_back(n.get());
            if (!(n->collapsible && n->collapsed))
                walk(n->children);
        }
    };
    walk(m_roots);
}

int TreeList::depthOf(const Node* n) const
{
    int d = 0;
    for (const Node* p = n->parent; p; p = p->parent)
        if (p->kind == Kind::Item)
            ++d;
    return d;
}

int TreeList::rowLeft(const Node* n) const
{
    if (n->kind == Kind::Match && n->parent)
        return rowLeft(n->parent);
    return m_marginLeft + depthOf(n) * m_m->indent;
}

bool TreeList::isFirstMatch(int row) const
{
    return row <= 0 || m_rows[size_t(row - 1)]->kind != Kind::Match || m_rows[size_t(row - 1)]->parent != m_rows[size_t(row)]->parent;
}

bool TreeList::isLastMatch(int row) const
{
    return row + 1 >= int(m_rows.size()) || m_rows[size_t(row + 1)]->kind != Kind::Match
        || m_rows[size_t(row + 1)]->parent != m_rows[size_t(row)]->parent;
}

int TreeList::textWidth(const Node* n, int rowWidth) const
{
    const QFontMetricsF flair(m_m->flairFont);
    const int flairW = n->flair.isEmpty() ? 0 : int(std::ceil(flair.horizontalAdvance(n->flair))) + m_m->flairGap;
    const int left = n->kind == Kind::Header ? m_m->headerPadLeft : m_m->padLeft;
    return std::max(10, rowWidth - left - m_m->padRight - flairW);
}

void TreeList::layoutNode(const Node* n, int width) const
{
    const quint64 gen = currentTheme()->generation();
    if (n->layout && n->layout->width == width && n->layout->generation == gen)
        return;
    auto l = std::make_shared<Node::Layout>();
    l->width = width;
    l->generation = gen;
    const Metrics& m = *m_m;
    switch (n->kind) {
    case Kind::Item:
    case Kind::Header: {
        const QFont& font = n->kind == Kind::Header ? m.headerFont : m.font;
        const QString text = n->prefix + n->title;
        const int avail = textWidth(n, width);
        const QFontMetricsF fm(font);
        l->flairWidth = n->flair.isEmpty() ? 0 : int(std::ceil(QFontMetricsF(m.flairFont).horizontalAdvance(n->flair)));
        const bool bold = n->boldMatches && !n->matches.isEmpty();
        if (fm.horizontalAdvance(text) <= avail && !text.contains(u'\n') && !bold) {
            l->textHeight = int(std::ceil(m.lineHeight));
        } else {
            l->wrapped = true;
            l->text = std::make_unique<QTextLayout>(forLayout(text), font);
            QTextOption opt;
            opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            l->text->setTextOption(opt);
            if (bold) {
                QList<QTextLayout::FormatRange> formats;
                for (const MatchRange& r : n->matches) {
                    QTextLayout::FormatRange f;
                    f.start = int(n->prefix.size()) + r.from;
                    f.length = r.to - r.from;
                    f.format.setFontWeight(QFont::Bold);
                    formats << f;
                }
                l->text->setFormats(formats);
            }
            l->textHeight = layoutLines(*l->text, avail, m.lineHeight);
        }
        int h = m.padY * 2 + l->textHeight;
        if (!n->subtext.isEmpty())
            h += int(std::ceil(m.subLineHeight));
        l->height = h;
        break;
    }
    case Kind::Match: {
        const QString content = n->content ? *n->content : QString();
        const int from = std::clamp(n->from, 0, int(content.size()));
        const int to = std::clamp(n->to, from, int(content.size()));
        l->shownOffset = n->ellipsisBefore ? 3 : 0;
        l->shown = (n->ellipsisBefore ? QStringLiteral("...") : QString()) + content.mid(from, to - from)
            + (n->ellipsisAfter ? QStringLiteral("...") : QString());
        const int avail = std::max(10, width - 2 - m.matchPadLeft - m.matchPadRight);
        l->text = std::make_unique<QTextLayout>(forLayout(l->shown), m.matchFont);
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        opt.setTabStopDistance(QFontMetricsF(m.matchFont).horizontalAdvance(u' ') * 4);
        l->text->setTextOption(opt);
        QList<QTextLayout::FormatRange> formats;
        const ThemePtr t = currentTheme();
        for (const MatchRange& r : n->matches) {
            const int a = std::max(r.from, from), b = std::min(r.to, to);
            if (b <= a)
                continue;
            QTextLayout::FormatRange f;
            f.start = l->shownOffset + (a - from);
            f.length = b - a;
            f.format.setBackground(tok(t, "--text-highlight-bg", QColor(255, 208, 0, 102)));
            f.format.setForeground(tok(t, "--text-normal"));
            formats << f;
        }
        l->text->setFormats(formats);
        l->textHeight = layoutLines(*l->text, avail, m.matchLineHeight);
        // Destination chips: margin-top 6, 2 px gaps, wrapping.
        if (!n->chips.isEmpty()) {
            const QFontMetricsF fm(m.matchFont);
            const int chipH = int(std::ceil(m.matchLineHeight)) + 8;
            int x = 0, y = 6;
            for (const QString& c : n->chips) {
                const int w = std::min(avail, int(std::ceil(fm.horizontalAdvance(c))) + 12 + m.iconSize + 4);
                if (x > 0 && x + w > avail) {
                    x = 0;
                    y += chipH + 2;
                }
                l->chips << QRect(x, l->textHeight + y, w, chipH);
                x += w + 2;
            }
            l->chipsHeight = y + chipH;
        }
        l->height = m.matchPadTop + l->textHeight + l->chipsHeight + m.matchPadBottom;
        break;
    }
    case Kind::Message: {
        const int avail = n->centered ? std::max(10, width - 32) : std::max(10, width - 8);
        l->text = std::make_unique<QTextLayout>(forLayout(n->title), m.font);
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        if (n->centered)
            opt.setAlignment(Qt::AlignHCenter);
        l->text->setTextOption(opt);
        l->textHeight = layoutLines(*l->text, avail, m.lineHeight);
        l->height = n->centered ? l->textHeight + 64 : l->textHeight + 12;
        break;
    }
    }
    n->layout = std::move(l);
}

int TreeList::rowHeight(const Node* n, int width) const
{
    layoutNode(n, width);
    return n->layout->height;
}

void TreeList::ensureGeometry() const
{
    ensureRows();
    const int vw = viewport()->width();
    const quint64 gen = currentTheme()->generation();
    if (!m_geometryDirty && m_layoutWidth == vw && m_themeGeneration == gen)
        return;
    m_geometryDirty = false;
    m_layoutWidth = vw;
    m_themeGeneration = gen;
    const int n = int(m_rows.size());
    m_tops.assign(size_t(n), 0);
    m_heights.assign(size_t(n), 0);
    int y = m_marginTop;
    for (int i = 0; i < n; ++i) {
        const Node* node = m_rows[size_t(i)];
        y += node->spaceBefore;
        const bool match = node->kind == Kind::Match;
        if (match && isFirstMatch(i))
            y += m_m->boxMarginTop;
        const int left = rowLeft(node);
        const int w = std::max(20, vw - m_marginRight - left);
        int h = rowHeight(node, w);
        if (match && !isLastMatch(i))
            h += 1; // border-bottom
        m_tops[size_t(i)] = y;
        m_heights[size_t(i)] = h;
        y += h;
        if (node->kind == Kind::Item || node->kind == Kind::Header)
            y += m_m->marginBottom;
        if (match && isLastMatch(i))
            y += m_m->boxMarginBottom;
        y += node->spaceAfter;
    }
    m_contentHeight = y + m_marginBottom;
}

void TreeList::setAutoHeight(bool on)
{
    m_autoHeight = on;
    setVerticalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Preferred, on ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    m_reportedHeight = -1;
    updateScrollRange();
    updateGeometry();
}

QSize TreeList::sizeHint() const
{
    if (!m_autoHeight)
        return QAbstractScrollArea::sizeHint();
    ensureGeometry();
    return {QAbstractScrollArea::sizeHint().width(), m_contentHeight + 2 * frameWidth()};
}

QSize TreeList::minimumSizeHint() const
{
    if (!m_autoHeight)
        return QAbstractScrollArea::minimumSizeHint();
    return {0, sizeHint().height()};
}

void TreeList::wheelEvent(QWheelEvent* e)
{
    if (m_autoHeight) {
        e->ignore(); // the note scrolls
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void TreeList::updateScrollRange()
{
    ensureGeometry();
    if (m_autoHeight && m_contentHeight != m_reportedHeight) {
        m_reportedHeight = m_contentHeight;
        updateGeometry();
    }
    const int vh = viewport()->height();
    QScrollBar* sb = verticalScrollBar();
    sb->setRange(0, std::max(0, m_contentHeight - vh));
    sb->setPageStep(vh);
    sb->setSingleStep(int(std::round(m_m->lineHeight + 2 * m_m->padY + m_m->marginBottom)));
}

int TreeList::rowCount() const
{
    ensureRows();
    return int(m_rows.size());
}

TreeList::Node* TreeList::nodeAt(int row) const
{
    ensureRows();
    return row >= 0 && row < int(m_rows.size()) ? m_rows[size_t(row)] : nullptr;
}

int TreeList::rowOf(const Node* node) const
{
    ensureRows();
    return m_rowIndex.value(node, -1);
}

QRect TreeList::rowRect(int row) const
{
    ensureGeometry();
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    const Node* n = m_rows[size_t(row)];
    int left = rowLeft(n);
    int right = viewport()->width() - m_marginRight;
    if (n->kind == Kind::Match) {
        ++left; // the box outline stays visible (.search-result-container: 1 px side padding)
        --right;
    }
    const int top = m_tops[size_t(row)] - verticalScrollBar()->value();
    return QRect(left, top, std::max(0, right - left), m_heights[size_t(row)]);
}

int TreeList::rowAt(const QPoint& pos) const
{
    int row = -1;
    hitTest(pos, &row);
    return row;
}

QStringList TreeList::visibleTitles() const
{
    ensureRows();
    QStringList out;
    for (const Node* n : m_rows) {
        const QString indent(depthOf(n) * 2, u' ');
        if (n->kind == Kind::Match) {
            const QString c = n->content ? n->content->mid(n->from, n->to - n->from) : QString();
            out << indent + QStringLiteral("> ") + c;
        } else {
            out << indent + n->prefix + n->title;
        }
    }
    return out;
}

void TreeList::scrollToNode(const Node* node, int margin)
{
    const int row = rowOf(node);
    if (row < 0)
        return;
    ensureGeometry();
    QScrollBar* sb = verticalScrollBar();
    const int top = m_tops[size_t(row)] - margin;
    const int bottom = m_tops[size_t(row)] + m_heights[size_t(row)] + margin;
    const int vh = viewport()->height();
    if (top < sb->value())
        sb->setValue(top);
    else if (bottom > sb->value() + vh)
        sb->setValue(bottom - vh);
}

QRect TreeList::chevronRect(int row) const
{
    const QRect r = rowRect(row);
    if (r.isNull())
        return {};
    const Node* n = m_rows[size_t(row)];
    if (n->kind != Kind::Item || !n->collapsible)
        return {};
    const int top = r.top() + m_m->padY;
    return QRect(r.left() + m_m->padLeft - 20, top, 16, int(std::ceil(m_m->lineHeight)));
}

QRect TreeList::buttonRect(int row) const
{
    const QRect r = rowRect(row);
    const Node* n = nodeAt(row);
    if (r.isNull() || !n || n->kind != Kind::Match || n->button.isEmpty())
        return {};
    const QFontMetrics fm(m_m->matchFont);
    const int w = fm.horizontalAdvance(n->button) + 16;
    const int h = int(std::ceil(m_m->matchLineHeight)) + 8;
    const int bottom = r.bottom() - (isLastMatch(row) ? 0 : 1) - 5;
    return QRect(r.right() - 24 - w + 1, bottom - h + 1, w, h);
}

QRect TreeList::moreButtonRect(int row, bool before) const
{
    const QRect r = rowRect(row);
    const Node* n = nodeAt(row);
    if (r.isNull() || !n || n->kind != Kind::Match || !(before ? n->moreBefore : n->moreAfter))
        return {};
    const int w = kHoverIcon + 6, h = kHoverIcon + 2; // .search-result-hover-button: padding 1px 3px
    const int bottom = r.bottom() - (isLastMatch(row) ? 0 : 1);
    return before ? QRect(r.right() - 2 - w + 1, r.top() + 2, w, h) : QRect(r.right() - 2 - w + 1, bottom - 2 - h + 1, w, h);
}

QRect TreeList::chipRect(int row, int chip) const
{
    const QRect r = rowRect(row);
    const Node* n = nodeAt(row);
    if (r.isNull() || !n || n->kind != Kind::Match)
        return {};
    layoutNode(n, r.width() + 2);
    if (chip < 0 || chip >= n->layout->chips.size())
        return {};
    return n->layout->chips[chip].translated(r.left() + m_m->matchPadLeft, r.top() + m_m->matchPadTop);
}

// ---------------------------------------------------------------------------------------------
// expansion
// ---------------------------------------------------------------------------------------------

void TreeList::setCollapsed(Node* node, bool collapsed)
{
    if (!node || !node->collapsible || node->collapsed == collapsed)
        return;
    node->collapsed = collapsed;
    if (!collapsed)
        m_allCollapsed = false;
    // a focused / selected descendant that disappears loses its state
    if (collapsed) {
        std::function<void(const Node*)> drop = [&](const Node* p) {
            for (const auto& c : p->children) {
                if (m_focused == c.get())
                    m_focused = node;
                m_selected.removeAll(c.get());
                drop(c.get());
            }
        };
        drop(node);
    }
    invalidate();
}

void TreeList::setAllCollapsed(bool collapsed)
{
    m_allCollapsed = collapsed;
    for (Node* n : allNodes())
        if (n->collapsible && n->kind == Kind::Item)
            n->collapsed = collapsed;
    if (collapsed && m_focused)
        for (Node* p = m_focused->parent; p; p = p->parent)
            if (p->collapsible && p->collapsed)
                m_focused = p;
    invalidate();
}

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------

void TreeList::setFocusedNode(Node* node, bool scroll)
{
    m_focused = node;
    if (node && scroll)
        scrollToNode(node, 4);
    viewport()->update();
}

QList<TreeList::Node*> TreeList::selectedNodes() const
{
    ensureRows();
    QList<Node*> out = m_selected;
    std::sort(out.begin(), out.end(), [this](Node* a, Node* b) { return m_rowIndex.value(a, INT_MAX) < m_rowIndex.value(b, INT_MAX); });
    return out;
}

void TreeList::setSelectedNodes(const QList<Node*>& nodes)
{
    m_selected.clear();
    for (Node* n : nodes)
        if (n && n->selectable && !m_selected.contains(n))
            m_selected << n;
    viewport()->update();
}

void TreeList::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    viewport()->update();
}

QList<TreeList::Node*> TreeList::actionNodes() const
{
    if (!m_selected.isEmpty())
        return selectedNodes();
    if (m_focused)
        return {m_focused};
    return {};
}

void TreeList::setMenuNode(Node* node)
{
    m_menuNode = node;
    viewport()->update();
}

TreeList::Node* TreeList::hoveredNode() const { return nodeAt(m_hoverRow); }

// ---------------------------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------------------------

void TreeList::paintEvent(QPaintEvent* e)
{
    ensureGeometry();
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const int n = int(m_rows.size());
    if (n == 0)
        return;
    // auto height: the widget is as tall as its content, only the exposed rows are painted
    const int scroll = m_autoHeight ? e->rect().top() : verticalScrollBar()->value();
    const int vh = m_autoHeight ? e->rect().height() : viewport()->height();
    // first / last rows intersecting the viewport
    auto firstIt = std::upper_bound(m_tops.begin(), m_tops.end(), scroll);
    int first = std::max(0, int(firstIt - m_tops.begin()) - 1);
    while (first > 0 && m_tops[size_t(first)] + m_heights[size_t(first)] > scroll)
        --first;
    auto lastIt = std::upper_bound(m_tops.begin(), m_tops.end(), scroll + vh);
    const int last = std::clamp(int(lastIt - m_tops.begin()) - 1, 0, n - 1);
    paintGuides(p, first, last);
    for (int i = first; i <= last; ++i) {
        const QRect r = rowRect(i);
        switch (m_rows[size_t(i)]->kind) {
        case Kind::Item:
        case Kind::Header: paintItem(p, i, r); break;
        case Kind::Match: paintMatch(p, i, r); break;
        case Kind::Message: paintMessage(p, i, r); break;
        }
    }
    // drop indicator (.drop-indicator: a 2 px accent border, i.e. a 4 px line)
    if (m_drag.active && m_drag.valid && m_drag.target && m_drag.placement != DropPlacement::Inside) {
        const int row = rowOf(m_drag.target);
        if (row >= 0) {
            const QRect r = rowRect(row);
            const int y = m_drag.placement == DropPlacement::Before ? r.top() - 1 : r.bottom() + 2;
            int left = r.left();
            if (m_drag.placement == DropPlacement::After && m_drag.target->collapsible && !m_drag.target->collapsed
                && m_drag.target->hasVisibleChildren())
                left += m_m->indent;
            p.fillRect(QRect(left, y - 2, r.right() - left + 1, 4), tok(currentTheme(), "--interactive-accent"));
        }
    }
}

void TreeList::paintGuides(QPainter& p, int first, int last)
{
    const ThemePtr t = currentTheme();
    const QColor guide = tok(t, "--nav-indentation-guide-color", QColor(255, 255, 255, 30));
    const double w = t->px(QStringLiteral("--nav-indentation-guide-width"), -1, 1);
    const int scroll = verticalScrollBar()->value();
    const int n = int(m_rows.size());
    for (int i = first; i <= last; ++i) {
        const Node* node = m_rows[size_t(i)];
        if (node->kind != Kind::Item)
            continue;
        const int depth = depthOf(node);
        const int top = m_tops[size_t(i)] - scroll;
        for (int level = 0; level < depth; ++level) {
            bool continues = false;
            if (i + 1 < n) {
                const Node* next = m_rows[size_t(i + 1)];
                continues = next->kind == Kind::Item && depthOf(next) > level;
            }
            const int bottom = continues ? m_tops[size_t(i + 1)] - scroll : top + m_heights[size_t(i)];
            const double x = m_marginLeft + level * m_m->indent + m_m->guideOffset;
            p.fillRect(QRectF(x, top, w, bottom - top), guide);
        }
    }
}

void TreeList::paintItem(QPainter& p, int row, const QRect& r)
{
    const ThemePtr t = currentTheme();
    const Node* n = m_rows[size_t(row)];
    const Metrics& m = *m_m;
    layoutNode(n, r.width());
    const Node::Layout& l = *n->layout;
    const bool header = n->kind == Kind::Header;
    const bool hover = !m_drag.active && row == m_hoverRow;
    const bool selected = m_selected.contains(const_cast<Node*>(n));
    const bool dragged = m_drag.active && m_drag.nodes.contains(const_cast<Node*>(n));
    const bool dropInside = m_drag.active && m_drag.valid && m_drag.target == n && m_drag.placement == DropPlacement::Inside;
    const bool renaming = n == m_renaming;
    bool hasMatchChildren = false;
    for (const auto& c : n->children)
        hasMatchChildren = hasMatchChildren || c->kind == Kind::Match;

    QColor bg;
    QColor fg = tok(t, "--nav-item-color");
    if (header)
        fg = n->collapsed ? tok(t, "--nav-heading-color-collapsed") : tok(t, "--nav-heading-color");
    else if (hasMatchChildren && !n->collapsed)
        fg = tok(t, "--nav-item-color-active"); // .search-result:not(.is-collapsed) .search-result-file-title
    if (hover) {
        bg = tok(t, "--nav-item-background-hover");
        fg = header ? (n->collapsed ? tok(t, "--nav-heading-color-collapsed-hover") : tok(t, "--nav-heading-color-hover"))
                    : tok(t, "--nav-item-color-hover");
    }
    if (n->active) {
        bg = tok(t, "--nav-item-background-active");
        fg = tok(t, "--nav-item-color-active");
    }
    if (selected) {
        bg = tok(t, "--nav-item-background-selected");
        fg = tok(t, "--nav-item-color-selected");
    }
    if (dropInside) {
        QColor c = tok(t, "--interactive-accent");
        c.setAlphaF(0.1);
        bg = c;
        fg = tok(t, "--nav-item-color-highlighted");
    }
    if (dragged) {
        bg = tok(t, "--interactive-accent");
        fg = tok(t, "--text-on-accent", Qt::white);
    }
    const QRectF box(r);
    if (bg.isValid() && bg.alpha() > 0) {
        QPainterPath path;
        path.addRoundedRect(box, m.radius, m.radius);
        p.fillPath(path, bg);
    }
    // .has-focus (leaf active) / .has-active-menu / .is-being-renamed: 2 px ring
    if ((n == m_focused && m_leafActive) || n == m_menuNode || renaming) {
        const QColor ring = renaming && m_editor->hasFocus() ? tok(t, "--interactive-accent") : tok(t, "--background-modifier-border-focus");
        QPainterPath outer, inner;
        outer.addRoundedRect(box.adjusted(-2, -2, 2, 2), m.radius + 2, m.radius + 2);
        inner.addRoundedRect(box, m.radius, m.radius);
        p.fillPath(outer.subtracted(inner), ring);
    }

    const double lineTop = r.top() + m.padY;
    const double lineMid = lineTop + m.lineHeight / 2;
    if (!header && n->collapsible) {
        // .collapse-icon: 10 px right triangle, stroke 4 in a 24 px box, rotated -90° when collapsed
        QColor ic = tok(t, "--nav-collapse-icon-color", tok(t, "--text-faint"));
        if (n->collapsed)
            ic = tok(t, "--nav-collapse-icon-color-collapsed", ic);
        if (dragged)
            ic = tok(t, "--text-on-accent", Qt::white);
        const QPointF c(r.left() + m.padLeft - 20 + 8, lineMid);
        const double s = 10.0 / 24.0;
        p.save();
        p.setOpacity(p.opacity() * t->number(QStringLiteral("--icon-opacity"), 0.85));
        p.translate(c);
        if (n->collapsed)
            p.rotate(-90);
        QPainterPath tri;
        tri.moveTo((3 - 12) * s, (8 - 12) * s);
        tri.lineTo(0, (17 - 12) * s);
        tri.lineTo((21 - 12) * s, (8 - 12) * s);
        p.setPen(QPen(ic, 4 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(tri);
        p.restore();
    } else if (!header && !n->icon.isEmpty()) {
        // .tree-item-icon: --icon-xs, --icon-color at --icon-opacity, centered in the 16 px slot
        const QColor ic = dragged ? tok(t, "--text-on-accent", Qt::white) : tok(t, "--icon-color");
        const double stroke = t->px(QStringLiteral("--icon-s-stroke-width"), -1, 2);
        p.save();
        p.setOpacity(p.opacity() * t->number(QStringLiteral("--icon-opacity"), 0.85));
        const QPixmap pm = IconProvider::instance().pixmap(n->icon, m.iconSize, ic, stroke, devicePixelRatioF());
        p.drawPixmap(QPointF(r.left() + m.padLeft - 20 + 8 - m.iconSize / 2.0, lineMid - m.iconSize / 2.0), pm);
        p.restore();
    }

    const int left = r.left() + (header ? m.headerPadLeft : m.padLeft);
    const int avail = textWidth(n, r.width());
    const QFont& font = header ? m.headerFont : m.font;
    const QFontMetricsF fm(font);
    const double baseline = lineTop + (m.lineHeight - (fm.ascent() + fm.descent())) / 2 + fm.ascent();
    if (!renaming) {
        p.save();
        if (n->faint)
            p.setOpacity(p.opacity() * t->number(QStringLiteral("--link-unresolved-opacity"), 0.7));
        p.setFont(font);
        p.setPen(fg);
        if (l.wrapped && l.text) {
            l.text->draw(&p, QPointF(left, lineTop));
        } else {
            const QString text = n->prefix + n->title;
            // marked highlights (.search-result-file-matched-text)
            if (!n->boldMatches && !n->matches.isEmpty()) {
                const QColor hl = tok(t, "--text-highlight-bg", QColor(255, 208, 0, 102));
                for (const MatchRange& mr : n->matches) {
                    const int a = int(n->prefix.size()) + mr.from, b = int(n->prefix.size()) + mr.to;
                    const double x0 = fm.horizontalAdvance(text.left(a));
                    const double x1 = fm.horizontalAdvance(text.left(b));
                    p.fillRect(QRectF(left + x0, lineTop, x1 - x0, m.lineHeight), hl);
                }
            }
            p.drawText(QPointF(left, baseline), fm.elidedText(text, Qt::ElideRight, avail));
        }
        if (!n->subtext.isEmpty()) {
            p.setFont(m.subFont);
            p.setPen(tok(t, "--text-faint"));
            const QFontMetricsF sm(m.subFont);
            const double subTop = lineTop + l.textHeight;
            const double subBase = subTop + (m.subLineHeight - (sm.ascent() + sm.descent())) / 2 + sm.ascent();
            p.drawText(QPointF(left, subBase), sm.elidedText(n->subtext, Qt::ElideRight, avail));
        }
        p.restore();
    }
    if (!n->flair.isEmpty()) {
        // .tree-item-flair: 12 px, --text-faint (row hover: --text-muted), baseline-aligned with the title
        QColor fc = hover ? tok(t, "--text-muted") : tok(t, "--text-faint");
        if (dragged)
            fc = tok(t, "--text-on-accent", Qt::white);
        const QFontMetricsF ff(m.flairFont);
        p.setFont(m.flairFont);
        p.setPen(fc);
        const double x = r.right() + 1 - m.padRight - l.flairWidth;
        p.drawText(QPointF(x, baseline), n->flair);
    }
}

void TreeList::paintMatch(QPainter& p, int row, const QRect& r)
{
    const ThemePtr t = currentTheme();
    const Node* n = m_rows[size_t(row)];
    const Metrics& m = *m_m;
    layoutNode(n, r.width() + 2);
    const Node::Layout& l = *n->layout;
    const bool first = isFirstMatch(row);
    const bool last = isLastMatch(row);
    const bool hover = !m_drag.active && row == m_hoverRow;
    const int radius = int(t->px(QStringLiteral("--radius-s"), -1, 4));
    const QRect box = r.adjusted(0, 0, 0, last ? 0 : -1);

    auto roundedPath = [&](const QRectF& rect, double rad, bool top, bool bottom) {
        QPainterPath path;
        const double rt = top ? rad : 0, rb = bottom ? rad : 0;
        path.moveTo(rect.left() + rt, rect.top());
        path.lineTo(rect.right() - rt, rect.top());
        if (rt > 0)
            path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + rt);
        path.lineTo(rect.right(), rect.bottom() - rb);
        if (rb > 0)
            path.quadTo(rect.right(), rect.bottom(), rect.right() - rb, rect.bottom());
        path.lineTo(rect.left() + rb, rect.bottom());
        if (rb > 0)
            path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - rb);
        path.lineTo(rect.left(), rect.top() + rt);
        if (rt > 0)
            path.quadTo(rect.left(), rect.top(), rect.left() + rt, rect.top());
        path.closeSubpath();
        return path;
    };
    // .search-result-file-matches: --search-result-background with a 1 px --background-modifier-border
    // outline (box-shadow), rounded at the box's ends; rows separated by 1 px borders.
    const QColor border = tok(t, "--background-modifier-border");
    const QRectF rowArea(r.left(), r.top(), r.width(), r.height());
    p.fillPath(roundedPath(rowArea.adjusted(-1, first ? -1 : 0, 1, last ? 1 : 0), radius + 1, first, last), border);
    p.fillPath(roundedPath(rowArea.adjusted(0, 0, 0, last ? 0 : -1), radius, first, last), tok(t, "--search-result-background", tok(t, "--background-primary")));
    if (hover)
        p.fillPath(roundedPath(QRectF(box), radius, first, last), tok(t, "--text-selection"));
    if (n == m_focused && m_leafActive) {
        QPainterPath outer = roundedPath(QRectF(box), radius, true, true);
        QPainterPath inner = roundedPath(QRectF(box).adjusted(2, 2, -2, -2), std::max(0, radius - 2), true, true);
        p.fillPath(outer.subtracted(inner), tok(t, "--background-modifier-border-focus"));
    }

    // the snippet
    const QPointF origin(r.left() + m.matchPadLeft, r.top() + m.matchPadTop);
    p.setPen(hover ? tok(t, "--text-normal") : tok(t, "--text-muted"));
    p.setFont(m.matchFont);
    if (l.text)
        l.text->draw(&p, origin);

    // destination chips (.search-result-file-match-destination-file)
    for (int i = 0; i < l.chips.size(); ++i) {
        const QRect c = l.chips[i].translated(origin.toPoint());
        const bool chipHover = hover && m_hoverHit == Hit::Chip && m_hoverChip == i;
        QColor cbg = tok(t, "--interactive-normal");
        if (hover)
            cbg = chipHover ? tok(t, "--interactive-hover", cbg) : tok(t, "--background-secondary", cbg);
        QPainterPath cp;
        cp.addRoundedRect(QRectF(c), radius, radius);
        p.fillPath(cp, cbg);
        QColor edge = tok(t, "--background-modifier-border");
        p.setPen(QPen(edge, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(cp);
        const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("link"), m.iconSize, tok(t, "--text-faint"),
                                                           t->px(QStringLiteral("--icon-xs-stroke-width"), -1, 2), devicePixelRatioF());
        p.drawPixmap(QPointF(c.left() + 6, c.center().y() - m.iconSize / 2.0 + 0.5), pm);
        p.setPen(chipHover ? tok(t, "--text-normal") : tok(t, "--text-muted"));
        const QRect textRect(c.left() + 6 + m.iconSize + 4, c.top(), c.width() - 6 - m.iconSize - 4 - 6, c.height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(m.matchFont).elidedText(n->chips[i], Qt::ElideRight, textRect.width()));
    }

    if (hover) {
        // the hover buttons: more context above / below, the action button
        for (bool before : {true, false}) {
            const QRect b = moreButtonRect(row, before);
            if (b.isNull())
                continue;
            const bool hot = m_hoverHit == (before ? Hit::MoreBefore : Hit::MoreAfter);
            if (hot) {
                QPainterPath bp;
                bp.addRoundedRect(QRectF(b), radius, radius);
                p.fillPath(bp, tok(t, "--background-modifier-hover"));
            }
            constexpr int icon = kHoverIcon;
            const QPixmap pm = IconProvider::instance().pixmap(before ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"), icon,
                                                               hot ? tok(t, "--text-normal") : tok(t, "--text-faint"), 2.0, devicePixelRatioF());
            p.drawPixmap(QPointF(b.left() + (b.width() - icon) / 2.0, b.top() + (b.height() - icon) / 2.0), pm);
        }
        const QRect b = buttonRect(row);
        if (!b.isNull()) {
            const bool hot = m_hoverHit == Hit::Button;
            QPainterPath bp;
            bp.addRoundedRect(QRectF(b), radius, radius);
            p.fillPath(bp, hot ? tok(t, "--interactive-hover", tok(t, "--interactive-normal")) : tok(t, "--interactive-normal"));
            QColor edge = tok(t, "--background-modifier-border");
            p.setPen(QPen(edge, 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(bp);
            p.setPen(hot ? tok(t, "--text-normal") : tok(t, "--text-muted"));
            p.setFont(m.matchFont);
            p.drawText(b, Qt::AlignCenter, n->button);
        }
    }
    // separator (border-bottom) unless last
    if (!last)
        p.fillRect(QRect(r.left(), r.bottom(), r.width(), 1), border);
}

void TreeList::paintMessage(QPainter& p, int row, const QRect& r)
{
    const ThemePtr t = currentTheme();
    const Node* n = m_rows[size_t(row)];
    layoutNode(n, r.width());
    p.setPen(tok(t, "--text-faint"));
    p.setFont(m_m->font);
    if (n->centered)
        n->layout->text->draw(&p, QPointF(r.left() + 16, r.top() + 32));
    else
        n->layout->text->draw(&p, QPointF(r.left() + 8, r.top()));
}

void TreeList::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    m_geometryDirty = true;
    updateScrollRange();
    positionEditor();
}

void TreeList::scrollContentsBy(int, int)
{
    viewport()->update();
    positionEditor();
}

void TreeList::changeEvent(QEvent* e)
{
    QAbstractScrollArea::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
        updateMetrics();
        m_geometryDirty = true;
    }
}

// ---------------------------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------------------------

TreeList::Hit TreeList::hitTest(const QPoint& pos, int* rowOut, int* chipOut) const
{
    ensureGeometry();
    *rowOut = -1;
    if (chipOut)
        *chipOut = -1;
    const int y = pos.y() + verticalScrollBar()->value();
    auto it = std::upper_bound(m_tops.begin(), m_tops.end(), y);
    const int row = int(it - m_tops.begin()) - 1;
    if (row < 0 || row >= int(m_rows.size()))
        return Hit::None;
    if (y >= m_tops[size_t(row)] + m_heights[size_t(row)])
        return Hit::None; // a gap
    const QRect r = rowRect(row);
    if (pos.x() < r.left() || pos.x() > r.right())
        return Hit::None;
    *rowOut = row;
    const Node* n = m_rows[size_t(row)];
    if (n->kind == Kind::Message)
        return Hit::None;
    if (chevronRect(row).contains(pos))
        return Hit::Chevron;
    if (n->kind == Kind::Match) {
        if (moreButtonRect(row, true).contains(pos))
            return Hit::MoreBefore;
        if (moreButtonRect(row, false).contains(pos))
            return Hit::MoreAfter;
        if (buttonRect(row).contains(pos))
            return Hit::Button;
        for (int i = 0; i < n->chips.size(); ++i)
            if (chipRect(row, i).contains(pos)) {
                if (chipOut)
                    *chipOut = i;
                return Hit::Chip;
            }
    }
    return Hit::Row;
}

void TreeList::updateHover(const QPoint& pos)
{
    int row = -1, chip = -1;
    const Hit hit = hitTest(pos, &row, &chip);
    const bool rowChanged = row != m_hoverRow;
    if (!rowChanged && hit == m_hoverHit && chip == m_hoverChip)
        return;
    m_hoverRow = row;
    m_hoverHit = hit;
    m_hoverChip = chip;
    viewport()->update();
    if (!rowChanged)
        return;
    Node* n = nodeAt(row);
    if (n && n != m_renaming && m_handlers.hover && (n->kind == Kind::Item || n->kind == Kind::Match)) {
        const QRect r = rowRect(row);
        m_handlers.hover(n, QRect(viewport()->mapToGlobal(r.topLeft()), r.size()));
    } else if (m_handlers.unhover) {
        m_handlers.unhover();
    }
}

void TreeList::leaveEvent(QEvent* e)
{
    if (m_hoverRow != -1) {
        m_hoverRow = -1;
        m_hoverHit = Hit::None;
        viewport()->update();
        if (m_handlers.unhover)
            m_handlers.unhover();
    }
    QAbstractScrollArea::leaveEvent(e);
}

bool TreeList::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && m_hoverRow != -1) {
        m_hoverRow = -1;
        m_hoverHit = Hit::None;
        viewport()->update();
        if (m_handlers.unhover)
            m_handlers.unhover();
    }
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        int row = -1, chip = -1;
        const Hit hit = hitTest(he->pos(), &row, &chip);
        const Node* n = nodeAt(row);
        QString text;
        if (n) {
            if (hit == Hit::MoreBefore || hit == Hit::MoreAfter)
                text = tr("Show more context");
            else if (hit == Hit::Chip)
                text = n->chipTooltips.value(chip);
            else
                text = n->tooltip;
            // an elided one-line title shows in full
            if (text.isEmpty() && (n->kind == Kind::Item || n->kind == Kind::Header) && n->layout && !n->layout->wrapped
                && QFontMetricsF(n->kind == Kind::Header ? m_m->headerFont : m_m->font).horizontalAdvance(n->prefix + n->title)
                    > textWidth(n, rowRect(row).width()))
                text = n->prefix + n->title;
        }
        if (text.isEmpty() || m_drag.active) {
            QToolTip::hideText();
            return true;
        }
        QToolTip::showText(he->globalPos(), text, viewport(), rowRect(row));
        return true;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void TreeList::mousePressEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    m_pressHit = hitTest(pos, &m_pressRow, &m_pressChip);
    m_pressButton = e->button();
    m_pressPos = pos;
    if (m_renaming && nodeAt(m_pressRow) != m_renaming)
        commitRename();
    if (!hasFocus())
        setFocus(Qt::MouseFocusReason);
    e->accept();
}

void TreeList::mouseDoubleClickEvent(QMouseEvent* e) { mousePressEvent(e); }

void TreeList::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag.active) {
        updateDrag(pos, e->globalPosition().toPoint());
        e->accept();
        return;
    }
    if ((e->buttons() & Qt::LeftButton) && m_pressButton == Qt::LeftButton && m_pressRow >= 0 && m_pressHit == Hit::Row && m_dragEnabled
        && (pos - m_pressPos).manhattanLength() > kDragThreshold) {
        beginDrag();
        if (m_drag.active)
            updateDrag(pos, e->globalPosition().toPoint());
        e->accept();
        return;
    }
    updateHover(pos);
    QAbstractScrollArea::mouseMoveEvent(e);
}

void TreeList::mouseReleaseEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag.active) {
        m_drag.lastViewportPos = pos;
        m_drag.lastGlobalPos = e->globalPosition().toPoint();
        finishDrag(e->button() == Qt::LeftButton);
        m_pressRow = -1;
        e->accept();
        return;
    }
    int row = -1, chip = -1;
    const Hit hit = hitTest(pos, &row, &chip);
    const int pressRow = m_pressRow;
    const Hit pressHit = m_pressHit;
    m_pressRow = -1;
    if (e->button() != m_pressButton)
        return;
    if (pressRow >= 0 && row == pressRow && hit == pressHit && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
        clickRow(row, hit, chip, e->button(), e->modifiers());
    } else if (pressRow < 0 && row < 0 && e->button() == Qt::LeftButton) {
        clearSelection();
        setFocusedNode(nullptr, false);
    }
    e->accept();
}

void TreeList::clickRow(int row, Hit hit, int chip, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    Node* n = nodeAt(row);
    if (!n || n == m_renaming)
        return;
    switch (hit) {
    case Hit::Chevron:
        if (button != Qt::LeftButton)
            return;
        if (mods & Qt::ControlModifier) {
            // Mod+click on a chevron: collapse / expand every sibling result (Obsidian's setCollapseAll)
            setAllCollapsed(!n->collapsed);
        } else {
            toggleCollapsed(n);
            if (m_handlers.collapsedChanged)
                m_handlers.collapsedChanged(n, n->collapsed);
        }
        return;
    case Hit::MoreBefore:
    case Hit::MoreAfter:
        if (m_handlers.moreContext)
            m_handlers.moreContext(n, hit == Hit::MoreBefore);
        return;
    case Hit::Button:
        if (m_handlers.button)
            m_handlers.button(n);
        return;
    case Hit::Chip:
        if (m_handlers.chip)
            m_handlers.chip(n, chip);
        return;
    case Hit::Row:
        break;
    case Hit::None:
        return;
    }
    if (m_selectionEnabled && handleSelection(n, mods, true, button))
        return;
    if (m_handlers.activate)
        m_handlers.activate(n, button, mods, false);
}

// Obsidian's tree.handleItemSelection(): true when the click was consumed by the selection logic.
bool TreeList::handleSelection(Node* n, Qt::KeyboardModifiers mods, bool mouse, Qt::MouseButton button)
{
    const bool modEvent = button == Qt::MiddleButton || (mods & Qt::ControlModifier);
    if (!modEvent) {
        if ((mods & Qt::AltModifier) && !(mods & Qt::ShiftModifier)) {
            if (m_handlers.activateView)
                m_handlers.activateView();
            if (m_selected.contains(n)) {
                m_selected.removeAll(n);
            } else if (n->selectable) {
                m_selected << n;
                setFocusedNode(n, false);
                m_anchor = n;
            }
            viewport()->update();
            return true;
        }
        if (mods & Qt::ShiftModifier) {
            if (m_handlers.activateView)
                m_handlers.activateView();
            for (Node* x : m_anchor ? rangeBetween(m_anchor, n) : QList<Node*>{n})
                if (x->selectable && !m_selected.contains(x))
                    m_selected << x;
            viewport()->update();
            return true;
        }
        if (n == m_renaming)
            return true;
        if (n->active && mouse) {
            if (m_handlers.activateView)
                m_handlers.activateView();
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

QList<TreeList::Node*> TreeList::rangeBetween(Node* a, Node* b) const
{
    ensureRows();
    const int ia = m_rowIndex.value(a, -1), ib = m_rowIndex.value(b, -1);
    if (ia < 0 || ib < 0)
        return {b};
    QList<Node*> out;
    for (int i = std::max(ia, ib); i >= std::min(ia, ib); --i)
        if (m_rows[size_t(i)]->kind == Kind::Item)
            out << m_rows[size_t(i)];
    return out;
}

void TreeList::contextMenuEvent(QContextMenuEvent* e)
{
    if (m_drag.active)
        return;
    QPoint vp = e->pos();
    int row = -1;
    if (e->reason() == QContextMenuEvent::Keyboard) {
        row = rowOf(m_focused);
        if (row >= 0)
            vp = rowRect(row).center();
    } else {
        hitTest(vp, &row);
    }
    Node* n = nodeAt(row);
    if (n && n == m_renaming)
        return;
    if (n && n->kind == Kind::Message)
        n = nullptr;
    if (m_handlers.contextMenu)
        m_handlers.contextMenu(n, e->reason() == QContextMenuEvent::Keyboard ? viewport()->mapToGlobal(vp) : e->globalPos());
    e->accept();
}

// ---------------------------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------------------------

bool TreeList::focusable(const Node* n) const { return n && (n->kind == Kind::Item || n->kind == Kind::Match); }

bool TreeList::wantsKey(const QKeyEvent* e) const
{
    if (m_renaming)
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const bool none = mods == Qt::NoModifier;
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        return none || mods == Qt::ShiftModifier || mods == Qt::ControlModifier;
    case Qt::Key_Left:
    case Qt::Key_Right:
        return none || mods == Qt::ControlModifier;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return (none || mods == Qt::ControlModifier) && m_focused;
    case Qt::Key_Space:
        return none && m_focused;
    case Qt::Key_F2:
        return none && m_focused && m_handlers.rename;
    case Qt::Key_Delete:
        return none && m_handlers.remove && (m_focused || !m_selected.isEmpty());
    case Qt::Key_Escape:
        return none && (m_drag.active || !m_selected.isEmpty());
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        return none;
    default:
        return false;
    }
}

bool TreeList::handleKey(QKeyEvent* e)
{
    if (!wantsKey(e))
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    ensureRows();
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        moveFocus(e->key() == Qt::Key_Down, mods);
        return true;
    case Qt::Key_Left: {
        Node* f = m_focused;
        if (!f) {
            setAllCollapsed(true);
            return true;
        }
        Node* parentItem = f->parent && f->parent->kind == Kind::Item ? f->parent : nullptr;
        if (f->collapsible && f->kind == Kind::Item) {
            if (f->collapsed) {
                if (parentItem)
                    setFocusedNode(parentItem, true);
            } else {
                setCollapsed(f, true);
                if (m_handlers.collapsedChanged)
                    m_handlers.collapsedChanged(f, true);
            }
        } else if (!parentItem) {
            setAllCollapsed(true);
        } else {
            setCollapsed(parentItem, true);
            if (m_handlers.collapsedChanged)
                m_handlers.collapsedChanged(parentItem, true);
        }
        Node* x = m_focused;
        while (x && x->parent && x->parent->collapsible && x->parent->collapsed)
            x = x->parent;
        setFocusedNode(x, true);
        return true;
    }
    case Qt::Key_Right: {
        Node* f = m_focused;
        if (!f)
            return true;
        if (f->collapsible && f->collapsed) {
            setCollapsed(f, false);
            if (m_handlers.collapsedChanged)
                m_handlers.collapsedChanged(f, false);
        } else if (f->hasVisibleChildren()) {
            moveFocus(true, Qt::NoModifier);
        }
        return true;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (m_focused && m_handlers.activate)
            m_handlers.activate(m_focused, Qt::LeftButton, mods, true);
        return true;
    case Qt::Key_F2:
        m_handlers.rename(m_focused);
        return true;
    case Qt::Key_Delete:
        m_handlers.remove(actionNodes());
        return true;
    case Qt::Key_Escape:
        if (m_drag.active)
            cancelDrag();
        else
            clearSelection();
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
    default:
        return false;
    }
}

void TreeList::keyPressEvent(QKeyEvent* e)
{
    if (handleKey(e)) {
        e->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

// Obsidian's changeFocusedItem() (+ Shift: extend the selection, Ctrl: open what it lands on).
void TreeList::moveFocus(bool forward, Qt::KeyboardModifiers mods)
{
    ensureRows();
    const int n = int(m_rows.size());
    if (n == 0)
        return;
    Node* old = m_focused;
    int start = rowOf(m_focused);
    if (start < 0 && m_anchor)
        start = rowOf(m_anchor);
    if (start < 0 && !m_selected.isEmpty())
        start = rowOf(m_selected.last());
    if (start < 0)
        for (int i = 0; i < n && start < 0; ++i)
            if (m_rows[size_t(i)]->active)
                start = i;
    Node* target = nullptr;
    if (start < 0) {
        for (int i = forward ? 0 : n - 1; i >= 0 && i < n && !target; i += forward ? 1 : -1)
            if (focusable(m_rows[size_t(i)]))
                target = m_rows[size_t(i)];
    } else {
        for (int i = start + (forward ? 1 : -1); i >= 0 && i < n && !target; i += forward ? 1 : -1)
            if (focusable(m_rows[size_t(i)]))
                target = m_rows[size_t(i)];
    }
    if (target)
        setFocusedNode(target, true);
    if (m_handlers.activateView)
        m_handlers.activateView();
    if ((mods & Qt::ControlModifier) && target && !target->collapsible && m_handlers.activate)
        m_handlers.activate(target, Qt::LeftButton, Qt::NoModifier, true);
    if ((mods & Qt::ShiftModifier) && m_selectionEnabled && m_focused) {
        if (old && old->selectable && !m_selected.contains(old))
            m_selected << old;
        if (m_focused->selectable && !m_selected.contains(m_focused))
            m_selected << m_focused;
    }
    viewport()->update();
}

void TreeList::focusInEvent(QFocusEvent* e)
{
    if (!m_keyGuard)
        m_keyGuard = new TreeListKeyGuard(this);
    qApp->removeEventFilter(m_keyGuard);
    qApp->installEventFilter(m_keyGuard);
    QAbstractScrollArea::focusInEvent(e);
    viewport()->update();
}

void TreeList::focusOutEvent(QFocusEvent* e)
{
    if (m_keyGuard)
        qApp->removeEventFilter(m_keyGuard);
    QAbstractScrollArea::focusOutEvent(e);
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------------------------

void TreeList::startRename(Node* node)
{
    if (!node || node->kind != Kind::Item)
        return;
    if (m_renaming && m_renaming != node)
        commitRename();
    // reveal it
    bool changed = false;
    for (Node* p = node->parent; p; p = p->parent)
        if (p->collapsible && p->collapsed) {
            p->collapsed = false;
            changed = true;
        }
    if (changed)
        invalidate();
    m_renaming = node;
    setFocusedNode(node, true);
    m_editor->setText(node->title);
    m_editor->selectAll();
    m_editor->show();
    positionEditor();
    m_editor->setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

void TreeList::positionEditor()
{
    if (!m_editor)
        return;
    const int row = m_renaming ? rowOf(m_renaming) : -1;
    if (row < 0) {
        if (m_editor->isVisible())
            m_editor->hide();
        return;
    }
    const QRect r = rowRect(row);
    const int left = r.left() + m_m->padLeft - 2;
    const int right = r.right() + 1 - m_m->padRight - (m_renaming->flair.isEmpty() ? 0 : m_renaming->layout ? m_renaming->layout->flairWidth + m_m->flairGap : 0);
    const int h = std::max(int(std::ceil(m_m->lineHeight)), m_editor->sizeHint().height());
    const int cy = r.top() + m_m->padY + int(m_m->lineHeight / 2);
    m_editor->setGeometry(left, cy - h / 2, std::max(20, right - left), h);
    if (!m_editor->isVisible())
        m_editor->show();
}

void TreeList::commitRename()
{
    if (!m_renaming)
        return;
    Node* n = m_renaming;
    const QString text = m_editor->text();
    cancelRename();
    if (m_handlers.renamed)
        m_handlers.renamed(n, text); // may rebuild the tree
}

void TreeList::cancelRename()
{
    if (!m_renaming)
        return;
    m_renaming = nullptr;
    const bool hadFocus = m_editor->hasFocus();
    m_editor->hide();
    if (hadFocus)
        setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

bool TreeList::eventFilter(QObject* obj, QEvent* ev)
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
        } else if (ev->type() == QEvent::FocusOut && m_renaming) {
            const auto reason = static_cast<QFocusEvent*>(ev)->reason();
            if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason)
                QTimer::singleShot(0, this, [this] {
                    if (m_renaming && !m_editor->hasFocus())
                        commitRename();
                });
        } else if (ev->type() == QEvent::FocusIn) {
            viewport()->update();
        }
    }
    return QAbstractScrollArea::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------------------------
// drag & drop
// ---------------------------------------------------------------------------------------------

void TreeList::beginDrag()
{
    Node* pressed = nodeAt(m_pressRow);
    if (!pressed || pressed->kind != Kind::Item || !pressed->draggable || pressed == m_renaming || !m_handlers.drop)
        return;
    QList<Node*> nodes;
    if (m_selected.size() > 1 && m_selected.contains(pressed)) {
        for (Node* n : selectedNodes())
            if (n->draggable)
                nodes << n;
    } else {
        nodes << pressed;
    }
    m_drag = Drag{};
    m_drag.active = true;
    m_drag.nodes = nodes;
    m_hoverRow = -1;
    m_pressRow = -1;
    const QString title = nodes.size() == 1 ? nodes.first()->title : tr("%1 items").arg(nodes.size());
    m_ghost = new DragGhost(window(), nodes.size() == 1 && !nodes.first()->icon.isEmpty() ? nodes.first()->icon : QStringLiteral("bookmark"), title);
    QApplication::setOverrideCursor(Qt::ClosedHandCursor);
    QToolTip::hideText();
    viewport()->update();
}

// Obsidian's drop placement (iI): the row under the pointer, before / after it by its halves; the
// middle 90 % of a collapsible row means "into it".
void TreeList::computeDropTarget(const QPoint& vp)
{
    ensureGeometry();
    m_drag.target = nullptr;
    m_drag.valid = false;
    const int n = int(m_rows.size());
    int lastItem = -1, firstItem = -1;
    for (int i = 0; i < n; ++i)
        if (m_rows[size_t(i)]->kind == Kind::Item) {
            if (firstItem < 0)
                firstItem = i;
            lastItem = i;
        }
    if (firstItem < 0)
        return;
    const int y = vp.y() + verticalScrollBar()->value();
    Node* target = nullptr;
    DropPlacement placement = DropPlacement::Before;
    if (y < m_tops[size_t(firstItem)]) {
        target = m_rows[size_t(firstItem)];
        while (target->parent)
            target = target->parent;
        placement = DropPlacement::Before;
    } else if (y > m_tops[size_t(lastItem)] + m_heights[size_t(lastItem)]) {
        target = m_rows[size_t(lastItem)];
        while (target->parent)
            target = target->parent;
        placement = DropPlacement::After;
    } else {
        int row = int(std::upper_bound(m_tops.begin(), m_tops.end(), y) - m_tops.begin()) - 1;
        while (row > 0 && m_rows[size_t(row)]->kind != Kind::Item)
            --row;
        target = m_rows[size_t(std::max(0, row))];
        const double half = m_heights[size_t(row)] / 2.0;
        const double center = m_tops[size_t(row)] + half;
        placement = y > center ? DropPlacement::After : DropPlacement::Before;
        if (target->collapsible && half > 0 && std::abs(y - center) / half < 0.9)
            placement = DropPlacement::Inside;
    }
    if (!target || target->kind != Kind::Item)
        return;
    for (const Node* d : std::as_const(m_drag.nodes))
        for (const Node* p = target; p; p = p->parent)
            if (p == d)
                return; // onto itself or into its own subtree
    m_drag.target = target;
    m_drag.placement = placement;
    m_drag.valid = !m_handlers.canDrop || m_handlers.canDrop(m_drag.nodes, target, placement);
}

void TreeList::updateDrag(const QPoint& vp, const QPoint& global)
{
    m_drag.lastViewportPos = vp;
    m_drag.lastGlobalPos = global;
    if (m_ghost)
        m_ghost->moveToGlobal(global);
    Node* oldTarget = m_drag.target;
    const DropPlacement oldPlacement = m_drag.placement;
    if (viewport()->rect().contains(vp)) {
        computeDropTarget(vp);
    } else {
        m_drag.target = nullptr;
        m_drag.valid = false;
    }
    if (m_drag.target != oldTarget || m_drag.placement != oldPlacement) {
        m_expandTimer.stop();
        if (m_drag.target && m_drag.placement == DropPlacement::Inside && m_drag.target->collapsed)
            m_expandTimer.start();
        viewport()->update();
    }
    const int vh = viewport()->height();
    if ((vp.y() < kAutoScrollEdge || vp.y() > vh - kAutoScrollEdge) && viewport()->rect().adjusted(0, -40, 0, 40).contains(vp)) {
        if (!m_autoScroll.isActive())
            m_autoScroll.start();
    } else {
        m_autoScroll.stop();
    }
}

void TreeList::finishDrag(bool commit)
{
    if (!m_drag.active)
        return;
    m_expandTimer.stop();
    m_autoScroll.stop();
    delete m_ghost.data();
    QApplication::restoreOverrideCursor();
    const Drag d = m_drag;
    m_drag = Drag{};
    viewport()->update();
    if (commit && d.valid && d.target && m_handlers.drop)
        m_handlers.drop(d.nodes, d.target, d.placement);
}

void TreeList::cancelDrag() { finishDrag(false); }

} // namespace oc
