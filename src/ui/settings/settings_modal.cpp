#include "ui/settings/settings_modal.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/fuzzy/fuzzy.h"
#include "core/vault_session.h"
#include "editor/markdown_commands.h"
#include "editor/plugins/greentext_guard.h"
#include "editor/spell/spell_checker.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/settings/setting_item.h"
#include "ui/settings/settings_pages.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"

#include <QApplication>
#include <QCollator>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace oc {

namespace {
double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
QColor tok(const char* name, const QColor& fallback = {}) { return currentTheme()->color(QString::fromLatin1(name), fallback); }

QScrollArea* makeScrollArea(QWidget* parent)
{
    auto* s = new QScrollArea(parent);
    s->setFrameShape(QFrame::NoFrame);
    s->setWidgetResizable(true);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->viewport()->setAutoFillBackground(false);
    s->setAutoFillBackground(false);
    s->setFocusPolicy(Qt::NoFocus);
    return s;
}

// A column whose side padding centres the setting groups: Obsidian's
// padding-inline: max(<min>, (100% - var(--setting-group-max-width)) / 2).
class CenteredColumn : public QWidget {
public:
    CenteredColumn(int minSide, int top, int bottom, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_min(minSide)
        , m_top(top)
        , m_bottom(bottom)
    {
        auto* v = new QVBoxLayout(this);
        v->setSpacing(0);
        v->setContentsMargins(minSide, top, minSide, bottom);
    }
    QVBoxLayout* column() const { return static_cast<QVBoxLayout*>(layout()); }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        const int side = std::max(m_min, (width() - int(tokPx("--setting-group-max-width", 700))) / 2);
        layout()->setContentsMargins(side, m_top, side, m_bottom);
        QWidget::resizeEvent(e);
    }

private:
    int m_min, m_top, m_bottom;
};

// The sidebar (.vertical-tab-header): --modal-sidebar-background with a --divider-color end border.
class Sidebar : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), tok("--modal-sidebar-background", tok("--modal-background")));
        p.fillRect(QRect(width() - 1, 0, 1, height()), tok("--divider-color", tok("--background-modifier-border")));
    }
};

// A group title in the sidebar (.vertical-tab-header-group-title): 12 px, --text-faint, semibold, 8 px padding.
QLabel* groupTitle(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = currentTheme()->interfaceFont(tokPx("--font-ui-smaller", 12));
    f.setWeight(QFont::Weight(int(currentTheme()->number(QStringLiteral("--font-semibold"), 600))));
    l->setFont(f);
    l->setProperty("ocText", QStringLiteral("faint"));
    l->setContentsMargins(8, 8, 8, 8);
    return l;
}

// Settings search entries (every named, searchable definition; pages recurse).
struct Entry {
    SettingsTab* tab;
    QStringList pagePath;
    SettingDef def;
};

void collectEntries(const QList<SettingDef>& defs, SettingsTab* tab, const QStringList& path, QList<Entry>& out)
{
    for (const SettingDef& d : defs) {
        if (d.visible && !d.visible())
            continue;
        switch (d.type) {
        case SettingDef::Type::Group:
        case SettingDef::Type::List:
            collectEntries(d.items, tab, path, out);
            break;
        case SettingDef::Type::Page:
            if (!d.name.isEmpty())
                out.push_back({tab, path, d});
            collectEntries(d.items, tab, path + QStringList{d.name}, out);
            break;
        case SettingDef::Type::Item:
            if (!d.name.isEmpty() && d.searchable)
                out.push_back({tab, path, d});
            break;
        }
    }
}

const SettingDef* findPage(const QList<SettingDef>& defs, const QString& name)
{
    for (const SettingDef& d : defs) {
        if (d.type == SettingDef::Type::Page && d.name == name)
            return &d;
        if (d.type == SettingDef::Type::Group || d.type == SettingDef::Type::List)
            if (const SettingDef* p = findPage(d.items, name))
                return p;
    }
    return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// SettingsNavItem
// ---------------------------------------------------------------------------------------------

SettingsNavItem::SettingsNavItem(const QString& icon, const QString& title, QWidget* parent)
    : QWidget(parent)
    , m_icon(icon)
    , m_title(title)
{
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus); // tabIndex -1: reachable with the arrow keys, not with Tab
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            updateGeometry();
            update();
        });
}

void SettingsNavItem::setActive(bool active)
{
    m_active = active;
    update();
}

void SettingsNavItem::setSublabel(const QString& text)
{
    m_sublabel = text;
    updateGeometry();
    update();
}

void SettingsNavItem::setIndent(int px)
{
    m_indent = px;
    update();
}

void SettingsNavItem::setTitleMatches(const QList<QPair<int, int>>& ranges)
{
    m_matches = ranges;
    update();
}

QSize SettingsNavItem::sizeHint() const
{
    const double fs = tokPx("--font-ui-small", 13);
    int h = int(std::ceil(fs * 1.5)) + 8; // line-height 1.5 + 4 px padding
    if (!m_sublabel.isEmpty())
        h += int(std::ceil(fs * 0.75 * 1.5));
    return {120, h};
}

QSize SettingsNavItem::minimumSizeHint() const { return {40, sizeHint().height()}; }

void SettingsNavItem::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = tokPx("--radius-m", 8);
    if (m_active || m_hover) {
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), r, r);
        p.fillPath(path, t->color(QStringLiteral("--background-modifier-hover")));
    }
    if (hasFocus() && m_focusVisible) {
        // :focus-visible { box-shadow: 0 0 0 var(--input-border-width-focus) var(--background-modifier-border-focus) }
        p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border-focus")), 2));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), r - 1, r - 1);
    }
    double x = 8 + m_indent;
    const double fs = tokPx("--font-ui-small", 13);
    if (!m_icon.isEmpty()) {
        const int px = int(tokPx("--icon-s", 16));
        const QPixmap pm = IconProvider::instance().pixmap(m_icon, px, t->color(QStringLiteral("--text-muted")),
                                                           tokPx("--icon-s-stroke-width", 2), devicePixelRatioF());
        const double lineTop = 4;
        p.drawPixmap(QPointF(x, lineTop + (fs * 1.5 - px) / 2), pm);
        x += px + 8; // margin-inline-end: var(--size-4-2)
    }
    const double w = width() - x - 8;
    const QFont f = t->interfaceFont(fs);
    const QFontMetricsF fm(f);
    const double lineH = fs * 1.5;
    // Title with bold matches (search results).
    const QString title = fm.elidedText(m_title, Qt::ElideRight, w);
    QTextLayout tl(title, f);
    QList<QTextLayout::FormatRange> formats;
    for (const auto& [from, to] : m_matches) {
        QTextLayout::FormatRange fr;
        fr.start = from;
        fr.length = to - from;
        fr.format.setFontWeight(QFont::Bold);
        formats << fr;
    }
    tl.setFormats(formats);
    tl.beginLayout();
    QTextLine line = tl.createLine();
    line.setLineWidth(std::max(1.0, w));
    tl.endLayout();
    p.setPen(t->color(QStringLiteral("--text-normal")));
    tl.draw(&p, QPointF(x, 4 + (lineH - line.height()) / 2));
    if (!m_sublabel.isEmpty()) {
        QFont sf = t->interfaceFont(fs * 0.75);
        p.setFont(sf);
        p.setPen(t->color(QStringLiteral("--text-muted")));
        p.drawText(QRectF(x, 4 + lineH, w, fs * 0.75 * 1.5), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetricsF(sf).elidedText(m_sublabel, Qt::ElideRight, w));
    }
}

void SettingsNavItem::mouseReleaseEvent(QMouseEvent* e)
{
    if (rect().contains(e->position().toPoint()) && e->button() == Qt::LeftButton)
        emit clicked();
}

void SettingsNavItem::keyPressEvent(QKeyEvent* e)
{
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    switch (e->key()) {
    case Qt::Key_Down:
    case Qt::Key_J:
        if (plain) {
            emit moveFocus(1);
            return;
        }
        break;
    case Qt::Key_Up:
    case Qt::Key_K:
        if (plain) {
            emit moveFocus(-1);
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Right:
    case Qt::Key_L:
        emit clicked();
        return;
    case Qt::Key_Slash:
    case Qt::Key_Backtab:
        emit focusSearch();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

void SettingsNavItem::focusInEvent(QFocusEvent* e)
{
    m_focusVisible = e->reason() != Qt::MouseFocusReason;
    update();
    QWidget::focusInEvent(e);
}

void SettingsNavItem::focusOutEvent(QFocusEvent* e)
{
    update();
    QWidget::focusOutEvent(e);
}

void SettingsNavItem::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void SettingsNavItem::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

// ---------------------------------------------------------------------------------------------
// SettingsModal
// ---------------------------------------------------------------------------------------------

SettingsModal::SettingsModal(MainWindow* window, SettingsController* controller)
    : Modal(window->overlay())
    , m_window(window)
    , m_controller(controller)
    , m_session(window->session())
{
    setObjectName(QStringLiteral("ocSettingsModal"));
    setSidebarLayout(true);

    m_body = new QWidget;
    m_body->setObjectName(QStringLiteral("ocSettingsBody"));
    auto* h = new QHBoxLayout(m_body);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Sidebar: search + navigation (or search results).
    m_sidebar = new Sidebar(m_body);
    m_sidebar->setObjectName(QStringLiteral("ocSettingsSidebar"));
    auto* sv = new QVBoxLayout(m_sidebar);
    sv->setContentsMargins(0, 0, 1, 0);
    sv->setSpacing(0);
    auto* searchBox = new QWidget(m_sidebar);
    auto* sb = new QVBoxLayout(searchBox);
    sb->setContentsMargins(12, 12, 12, 0); // .setting-search-container
    m_search = new SearchInput(searchBox);
    m_search->setPlaceholderText(tr("Search settings..."));
    sb->addWidget(m_search);
    sv->addWidget(searchBox);
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this] { updateSearch(); });

    m_navScroll = makeScrollArea(m_sidebar);
    m_nav = new QWidget;
    m_navLayout = new QVBoxLayout(m_nav);
    m_navLayout->setContentsMargins(12, 0, 12, 12); // .vertical-tab-header-group-container
    m_navLayout->setSpacing(0);
    m_navScroll->setWidget(m_nav);
    sv->addWidget(m_navScroll, 1);

    m_resultsScroll = makeScrollArea(m_sidebar);
    m_resultsWidget = new QWidget;
    new QVBoxLayout(m_resultsWidget);
    m_resultsScroll->setWidget(m_resultsWidget);
    m_resultsScroll->hide();
    sv->addWidget(m_resultsScroll, 1);

    h->addWidget(m_sidebar);

    m_stack = new QStackedWidget(m_body);
    m_stack->setObjectName(QStringLiteral("ocSettingsContent"));
    h->addWidget(m_stack, 1);

    addContentWidget(m_body);
    m_body->installEventFilter(this);

    connect(this, &Modal::closed, this, [this] {
        if (m_active)
            m_active->hidden();
        if (VaultSession* s = session(); s && s->config())
            s->config()->flush(); // Obsidian's requestSaveConfig.run()
        if (m_controller) {
            m_controller->m_modal = nullptr; // deleted later; the next open() builds a new one
            emit m_controller->modalClosed();
        }
    });
}

SettingsModal::~SettingsModal() { clearPages(); }

// Cached: rows may still commit (an input's focus-out) while the window is being destroyed, when its
// methods must not be called anymore; the session outlives the window's overlay.
VaultSession* SettingsModal::session() const { return m_session; }
OverlayLayer* SettingsModal::overlay() const { return m_window ? m_window->overlay() : nullptr; }

void SettingsModal::notice(const QString& message) const
{
    if (m_window && m_window->notices())
        m_window->notices()->show(message);
}

void SettingsModal::addTab(SettingsTab* tab)
{
    tab->setParent(this);
    m_tabs.push_back(tab);
    connect(tab, &SettingsTab::updateRequested, this, [this, tab] {
        if (tab == m_active)
            rerenderActive();
    });
    connect(tab, &SettingsTab::refreshRequested, this, [this, tab] {
        if (tab == m_active)
            refreshActive();
    });
    rebuildNavigation();
}

SettingsTab* SettingsModal::tab(const QString& id) const
{
    for (SettingsTab* t : m_tabs)
        if (t->id() == id)
            return t;
    return nullptr;
}

bool SettingsModal::isTabListed(const SettingsTab* tab) const
{
    if (!tab)
        return false;
    switch (tab->section()) {
    case SettingsTab::Section::Options:
        return true;
    case SettingsTab::Section::CorePlugins:
        return m_controller && m_controller->isCorePluginEnabled(tab->id());
    case SettingsTab::Section::CommunityPlugins:
        return m_controller && m_controller->isCommunityPluginEnabled(tab->id());
    }
    return false;
}

void SettingsModal::rebuildNavigation()
{
    while (QLayoutItem* it = m_navLayout->takeAt(0)) {
        if (QWidget* w = it->widget())
            w->deleteLater();
        delete it;
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto addGroup = [&](const QString& title, SettingsTab::Section section, bool sorted) {
        QList<SettingsTab*> list;
        for (SettingsTab* t : m_tabs)
            if (t->section() == section && isTabListed(t))
                list << t;
        if (list.isEmpty())
            return;
        if (sorted)
            std::stable_sort(list.begin(), list.end(), [&](SettingsTab* a, SettingsTab* b) { return collator.compare(a->name(), b->name()) < 0; });
        auto* group = new QWidget(m_nav);
        auto* gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 12, 0, 12); // .vertical-tab-header-group { padding: 12px 0 }
        gv->setSpacing(2);                    // .vertical-tab-header-group-items { gap: 2px }
        gv->addWidget(groupTitle(title, group));
        for (SettingsTab* t : list) {
            // community plugin items show no icon ([data-section="community-plugins"])
            auto* item = new SettingsNavItem(section == SettingsTab::Section::CommunityPlugins ? QString() : t->icon(), t->name(), group);
            item->setTabId(t->id());
            item->setActive(t == m_active);
            connect(item, &SettingsNavItem::clicked, this, [this, t] { openTab(t); });
            connectNavItem(item);
            gv->addWidget(item);
        }
        m_navLayout->addWidget(group);
    };
    addGroup(tr("Options"), SettingsTab::Section::Options, false);
    addGroup(tr("Core plugins"), SettingsTab::Section::CorePlugins, true);
    addGroup(tr("Community plugins"), SettingsTab::Section::CommunityPlugins, true);
    m_navLayout->addStretch(1);
    // A hidden plugin tab can't stay open.
    if (m_active && !isTabListed(m_active))
        openTab(m_tabs.value(0));
}

QList<SettingsNavItem*> SettingsModal::navItems() const
{
    QList<SettingsNavItem*> out;
    for (int i = 0; i < m_navLayout->count(); ++i)
        if (QWidget* group = m_navLayout->itemAt(i)->widget())
            out += group->findChildren<SettingsNavItem*>();
    return out;
}

QList<SettingsNavItem*> SettingsModal::focusableNavItems() const
{
    QList<SettingsNavItem*> items = m_resultsScroll->isVisible() || !m_search->text().trimmed().isEmpty()
                                        ? m_resultsWidget->findChildren<SettingsNavItem*>()
                                        : navItems();
    items.removeIf([](SettingsNavItem* i) { return i->isHidden(); });
    return items;
}

void SettingsModal::moveNavFocus(SettingsNavItem* from, int delta)
{
    const QList<SettingsNavItem*> items = focusableNavItems();
    const qsizetype i = items.indexOf(from);
    if (items.isEmpty())
        return;
    const qsizetype next = i < 0 ? 0 : std::clamp<qsizetype>(i + delta, 0, items.size() - 1);
    items[next]->setFocus(Qt::TabFocusReason);
    if (QScrollArea* s = m_resultsScroll->isVisible() ? m_resultsScroll : m_navScroll)
        s->ensureWidgetVisible(items[next], 0, 8);
}

void SettingsModal::connectNavItem(SettingsNavItem* item)
{
    connect(item, &SettingsNavItem::moveFocus, this, [this, item](int delta) { moveNavFocus(item, delta); });
    connect(item, &SettingsNavItem::focusSearch, this, [this] {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
    });
}

SettingsTab* SettingsModal::openTab(const QString& id)
{
    SettingsTab* t = tab(id);
    if (!t || !isTabListed(t))
        return nullptr;
    openTab(t);
    return t;
}

void SettingsModal::openTab(SettingsTab* t)
{
    if (!t)
        return;
    clearPages();
    if (m_active && m_active != t)
        m_active->hidden();
    m_active = t;
    if (m_controller)
        m_controller->m_lastTab = t->id();
    for (SettingsNavItem* item : navItems())
        item->setActive(item->tabId() == t->id());
    rerenderActive();
    m_tabScroll->verticalScrollBar()->setValue(0);
    emit tabOpened(t->id());
}

QWidget* SettingsModal::buildTabBody(SettingsTab* t, QScrollArea** scrollOut, RenderedSettings* rendered)
{
    QScrollArea* scroll = makeScrollArea(m_stack);
    scroll->setObjectName(QStringLiteral("ocSettingsTabScroll"));
    // .vertical-tab-content: padding-top 48, padding-bottom 64, padding-inline max(32px, centring offset)
    auto* body = new CenteredColumn(32, 48, 64);
    body->setObjectName(QStringLiteral("ocSettingsTabContent"));
    QVBoxLayout* v = body->column();
    m_tabHandBuilt = false;
    if (QWidget* custom = t->createContent()) {
        m_tabHandBuilt = true;
        v->addWidget(custom);
    } else {
        renderSettings(*t, t->definitions(), v, *rendered, [this](const SettingDef& page) { openPage(page); });
    }
    v->addStretch(1);
    scroll->setWidget(body);
    *scrollOut = scroll;
    return scroll;
}

void SettingsModal::rerenderActive()
{
    if (!m_active)
        return;
    if (!m_pages.isEmpty()) {
        renderPage(*m_pages.last());
        return;
    }
    const int scrollPos = m_tabScroll ? m_tabScroll->verticalScrollBar()->value() : 0;
    QWidget* old = m_tabRoot;
    m_tabRendered = std::make_unique<RenderedSettings>();
    m_tabRoot = buildTabBody(m_active, &m_tabScroll, m_tabRendered.get());
    m_stack->addWidget(m_tabRoot);
    m_stack->setCurrentWidget(m_tabRoot);
    if (old) {
        m_stack->removeWidget(old);
        old->hide();
        old->deleteLater();
    }
    m_tabScroll->widget()->adjustSize();
    QTimer::singleShot(0, m_tabScroll, [s = QPointer<QScrollArea>(m_tabScroll), scrollPos] {
        if (s)
            s->verticalScrollBar()->setValue(scrollPos);
    });
    m_tabScroll->verticalScrollBar()->setValue(scrollPos);
}

void SettingsModal::refreshActive()
{
    if (RenderedSettings* r = rendered())
        r->refresh();
}

RenderedSettings* SettingsModal::rendered() const
{
    if (!m_pages.isEmpty())
        return m_pages.last()->rendered.get();
    return m_tabHandBuilt ? nullptr : m_tabRendered.get();
}

QWidget* SettingsModal::currentBody() const
{
    QScrollArea* s = currentScrollArea();
    return s ? s->widget() : nullptr;
}

QScrollArea* SettingsModal::currentScrollArea() const { return m_pages.isEmpty() ? m_tabScroll : m_pages.last()->scroll; }

SettingItem* SettingsModal::findItem(const QString& name) const
{
    if (RenderedSettings* r = rendered())
        if (SettingItem* item = r->find(name))
            return item;
    if (QWidget* body = currentBody())
        for (SettingItem* item : body->findChildren<SettingItem*>())
            if (item->name() == name)
                return item;
    return nullptr;
}

bool SettingsModal::revealItem(const QString& name)
{
    SettingItem* item = findItem(name);
    QScrollArea* s = currentScrollArea();
    if (!item || !s)
        return false;
    if (item->isHidden()) {
        // A row hidden by its group's search filter: clear the filter.
        for (SettingGroup* g : currentBody()->findChildren<SettingGroup*>())
            if (g->isAncestorOf(item) && g->searchInput())
                g->searchInput()->clear();
    }
    s->widget()->adjustSize();
    const QPoint pos = item->mapTo(s->widget(), QPoint(0, 0));
    s->verticalScrollBar()->setValue(pos.y() + item->height() / 2 - s->viewport()->height() / 2);
    item->flash();
    return true;
}

void SettingsModal::openPage(const SettingDef& def)
{
    if (!m_active)
        return;
    auto* page = new Page;
    page->title = def.name;
    page->def = def;
    m_pages.push_back(page);
    renderPage(*page);
}

void SettingsModal::renderPage(Page& page)
{
    // Fresh definitions (the tab may have changed since the page was opened).
    if (m_active && !page.def.customPage) {
        QList<SettingDef> defs = m_active->definitions();
        const SettingDef* found = nullptr;
        for (Page* p : std::as_const(m_pages)) {
            found = findPage(defs, p->title);
            if (!found)
                break;
            if (p == &page)
                break;
            defs = found->items;
        }
        if (found)
            page.def = *found;
    }
    const int scrollPos = page.scroll ? page.scroll->verticalScrollBar()->value() : 0;
    QWidget* old = page.root;
    page.root = new QWidget(m_stack);
    page.root->setObjectName(QStringLiteral("ocSettingPage")); // .setting-page
    auto* v = new QVBoxLayout(page.root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    // .setting-page-titlebar: back chevron + 13 px semibold title, aligned with the groups.
    auto* bar = new CenteredColumn(12, 12, 0, page.root);
    bar->setObjectName(QStringLiteral("ocSettingPageTitlebar"));
    auto* inner = new QWidget(bar);
    bar->column()->addWidget(inner);
    auto* ih = new QHBoxLayout(inner);
    ih->setContentsMargins(0, 0, 0, 0);
    ih->setSpacing(4);
    auto* back = new IconButton(QStringLiteral("chevron-left"), tr("Back"), inner);
    back->setObjectName(QStringLiteral("ocSettingPageBack"));
    connect(back, &IconButton::clicked, this, [this] { closePage(); });
    ih->addWidget(back);
    auto* title = new QLabel(page.title, inner);
    QFont tf = currentTheme()->interfaceFont(tokPx("--font-ui-small", 13));
    tf.setWeight(QFont::Weight(int(currentTheme()->number(QStringLiteral("--font-semibold"), 600))));
    title->setFont(tf);
    ih->addWidget(title, 1);
    v->addWidget(bar);

    page.scroll = makeScrollArea(page.root);
    auto* body = new CenteredColumn(32, 12, 64); // .setting-page-content { margin-top: 12px }
    body->setObjectName(QStringLiteral("ocSettingPageContent"));
    QVBoxLayout* bv = body->column();
    page.rendered = std::make_unique<RenderedSettings>();
    if (page.def.customPage) {
        if (QWidget* w = page.def.customPage(*this)) {
            bv->addWidget(w);
        }
    } else if (m_active) {
        renderSettings(*m_active, page.def.items, bv, *page.rendered, [this](const SettingDef& p) { openPage(p); });
    }
    bv->addStretch(1);
    page.scroll->setWidget(body);
    v->addWidget(page.scroll, 1);
    m_stack->addWidget(page.root);
    m_stack->setCurrentWidget(page.root);
    if (old) {
        m_stack->removeWidget(old);
        old->hide();
        old->deleteLater();
    }
    page.scroll->verticalScrollBar()->setValue(scrollPos);
}

void SettingsModal::closePage()
{
    if (m_pages.isEmpty())
        return;
    Page* page = m_pages.takeLast();
    m_stack->removeWidget(page->root);
    page->root->hide();
    page->root->deleteLater();
    delete page;
    // Obsidian re-displays what is under the page (values may have changed there).
    rerenderActive();
}

void SettingsModal::clearPages()
{
    for (Page* page : std::as_const(m_pages)) {
        if (page->root) {
            if (m_stack)
                m_stack->removeWidget(page->root);
            page->root->hide();
            page->root->deleteLater();
        }
        delete page;
    }
    m_pages.clear();
    if (m_stack && m_tabRoot)
        m_stack->setCurrentWidget(m_tabRoot);
}

QString SettingsModal::currentTitle() const
{
    if (!m_pages.isEmpty())
        return m_pages.last()->title;
    return m_active ? m_active->name() : tr("Settings");
}

// ---- search ------------------------------------------------------------------------------------

void SettingsModal::setSearchQuery(const QString& query)
{
    m_search->setText(query);
    updateSearch();
}

void SettingsModal::buildSearchResults()
{
    m_results.clear();
    const QString query = m_search->text().trimmed();
    if (query.isEmpty())
        return;
    const SimpleSearch search(query);
    QList<Entry> entries;
    for (SettingsTab* t : std::as_const(m_tabs))
        if (isTabListed(t))
            collectEntries(t->definitions(), t, {}, entries);
    auto groupFor = [this](SettingsTab* t, const QStringList& path) -> SearchGroup& {
        for (SearchGroup& g : m_results)
            if (g.tab == t && g.pagePath == path)
                return g;
        SearchGroup g;
        g.tab = t;
        g.pagePath = path;
        g.title = path.isEmpty() ? t->name() : path.last();
        g.bestScore = -1e18;
        m_results.push_back(g);
        return m_results.last();
    };
    for (const Entry& e : std::as_const(entries)) {
        double best = -1e18;
        bool hit = false;
        for (const QString& text : QStringList{e.def.name, e.def.desc} + e.def.aliases) {
            if (text.isEmpty())
                continue;
            if (const auto m = search.match(text)) {
                best = std::max(best, m->score);
                hit = true;
            }
        }
        if (!hit)
            continue;
        SearchGroup& g = groupFor(e.tab, e.pagePath);
        g.hits.push_back({e.def.name, best});
        g.bestScore = std::max(g.bestScore, best);
    }
    for (SettingsTab* t : std::as_const(m_tabs)) {
        if (!isTabListed(t))
            continue;
        if (const auto m = search.match(t->name())) {
            SearchGroup& g = groupFor(t, {});
            g.tabNameMatch = true;
            g.bestScore = std::max(g.bestScore, m->score);
        }
    }
    for (SearchGroup& g : m_results)
        std::stable_sort(g.hits.begin(), g.hits.end(), [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    std::stable_sort(m_results.begin(), m_results.end(), [](const SearchGroup& a, const SearchGroup& b) { return a.bestScore > b.bestScore; });
}

void SettingsModal::updateSearch()
{
    const QString query = m_search->text().trimmed();
    if (query.isEmpty()) {
        m_results.clear();
        m_resultsScroll->hide();
        m_navScroll->show();
        return;
    }
    buildSearchResults();
    m_navScroll->hide();
    m_resultsScroll->show();
    m_resultsWidget = new QWidget;
    auto* v = new QVBoxLayout(m_resultsWidget);
    v->setContentsMargins(12, 12, 12, 12); // .setting-search-results { padding: 12px }
    v->setSpacing(4);
    const SimpleSearch search(query);
    auto ranges = [&search](const QString& text) {
        QList<QPair<int, int>> out;
        if (const auto m = search.match(text))
            for (const MatchRange& r : m->matches)
                out.append({r.from, r.to});
        return out;
    };
    if (m_results.isEmpty()) {
        auto* empty = new QLabel(tr("No settings found."), m_resultsWidget);
        empty->setFont(currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)));
        empty->setProperty("ocText", QStringLiteral("muted"));
        empty->setContentsMargins(8, 8, 8, 8);
        v->addWidget(empty);
    }
    for (int gi = 0; gi < m_results.size(); ++gi) {
        const SearchGroup& g = m_results[gi];
        auto* group = new QWidget(m_resultsWidget);
        auto* gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->setSpacing(2);
        auto* head = new SettingsNavItem(g.tab->section() == SettingsTab::Section::CommunityPlugins ? QString() : g.tab->icon(), g.title, group);
        head->setObjectName(QStringLiteral("ocSettingSearchResultTab"));
        head->setTabId(g.tab->id());
        head->setTitleMatches(ranges(g.title));
        if (!g.pagePath.isEmpty())
            head->setSublabel(g.tab->name());
        connect(head, &SettingsNavItem::clicked, this, [this, gi] { navigateToSearchResult(gi); });
        connectNavItem(head);
        gv->addWidget(head);
        QList<SettingsNavItem*> more;
        for (int hi = 0; hi < g.hits.size(); ++hi) {
            auto* item = new SettingsNavItem(QString(), g.hits[hi].name, group);
            item->setObjectName(QStringLiteral("ocSettingSearchResultItem"));
            item->setIndent(24); // padding-inline-start: 32px (8 of it is the item padding)
            item->setTitleMatches(ranges(g.hits[hi].name));
            connect(item, &SettingsNavItem::clicked, this, [this, gi, hi] { navigateToSearchResult(gi, hi); });
            connectNavItem(item);
            gv->addWidget(item);
            if (hi >= 3) {
                item->hide();
                more << item;
            }
        }
        if (!more.isEmpty()) {
            auto* showMore = new SettingsNavItem(QString(), tr("%1 more").arg(more.size()), group);
            showMore->setObjectName(QStringLiteral("ocSettingSearchShowMore"));
            showMore->setIndent(24);
            connect(showMore, &SettingsNavItem::clicked, showMore, [showMore, more] {
                for (SettingsNavItem* i : more)
                    i->show();
                showMore->hide();
                if (!more.isEmpty())
                    more.first()->setFocus(Qt::OtherFocusReason);
            });
            connectNavItem(showMore);
            gv->addWidget(showMore);
        }
        v->addWidget(group);
    }
    v->addStretch(1);
    m_resultsScroll->setWidget(m_resultsWidget); // deletes the previous results
}

void SettingsModal::navigateToSearchResult(int groupIndex, int hitIndex)
{
    if (groupIndex < 0 || groupIndex >= m_results.size())
        return;
    const SearchGroup g = m_results[groupIndex];
    const QString hit = hitIndex >= 0 && hitIndex < g.hits.size() ? g.hits[hitIndex].name : QString();
    bool samePages = m_active == g.tab && m_pages.size() == g.pagePath.size();
    for (int i = 0; samePages && i < m_pages.size(); ++i)
        samePages = m_pages[i]->title == g.pagePath[i];
    if (!samePages) {
        openTab(g.tab);
        QList<SettingDef> defs = g.tab->definitions();
        for (const QString& name : g.pagePath) {
            const SettingDef* p = findPage(defs, name);
            if (!p)
                break;
            const SettingDef copy = *p;
            openPage(copy);
            defs = copy.items;
        }
    }
    if (!hit.isEmpty()) {
        if (QWidget* body = currentBody())
            body->adjustSize();
        const QString name = hit;
        QTimer::singleShot(0, this, [self = QPointer<SettingsModal>(this), name] {
            if (self)
                self->revealItem(name);
        });
    }
}

// ---- keys --------------------------------------------------------------------------------------

void SettingsModal::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && !m_pages.isEmpty()) {
        closePage();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_F && (e->modifiers() & Qt::ControlModifier)) {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
        e->accept();
        return;
    }
    Modal::keyPressEvent(e);
}

bool SettingsModal::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_body && ev->type() == QEvent::Resize) {
        // .vertical-tab-header { flex: 0 0 25%; min-width: 200px; max-width: 250px }
        m_sidebar->setFixedWidth(std::clamp(int(m_body->width() * 0.25), 200, 250));
    }
    if (obj == m_search && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        // Enter, Tab or ↓ move into the navigation (or the results); Enter there opens the item.
        const bool tab = ke->key() == Qt::Key_Tab && ke->modifiers() == Qt::NoModifier;
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter || ke->key() == Qt::Key_Down || tab) {
            const QList<SettingsNavItem*> items = focusableNavItems();
            if (!items.isEmpty())
                items.first()->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (ke->key() == Qt::Key_Backtab)
            return true; // stays in the search input
        if (ke->key() == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear();
            return true;
        }
    }
    return Modal::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------------------------
// SettingsController
// ---------------------------------------------------------------------------------------------

SettingsController* SettingsController::of(MainWindow* window)
{
    if (!window)
        return nullptr;
    if (auto* c = window->findChild<SettingsController*>(QString(), Qt::FindDirectChildrenOnly))
        return c;
    return new SettingsController(window);
}

SettingsController::SettingsController(MainWindow* window)
    : QObject(window)
    , m_window(window)
    , m_session(window->session())
{
    if (VaultConfig* cfg = m_session ? m_session->vaultConfig() : nullptr) {
        static const QSet<QString> keys = {QStringLiteral("showRibbon"),      QStringLiteral("smartIndentList"), QStringLiteral("autoPairBrackets"),
                                           QStringLiteral("autoPairMarkdown"), QStringLiteral("useTab"),          QStringLiteral("tabSize"),
                                           QStringLiteral("spellcheck"),       QStringLiteral("spellcheckLanguages"),
                                           QStringLiteral("showViewHeader"),   QStringLiteral("focusNewTab")};
        connect(cfg, &VaultConfig::configChanged, this, [this](const QString& key) {
            if (key.isEmpty() || keys.contains(key))
                applyOptions();
        });
    }
}

SettingsModal* SettingsController::open(const QString& tabId)
{
    if (!m_window || !m_window->overlay())
        return nullptr;
    if (!m_modal) {
        m_modal = new SettingsModal(m_window, this);
        addSettingsTabs(*m_modal);
        m_modal->open();
        emit modalOpened(m_modal);
    }
    const QString id = !tabId.isEmpty() ? tabId : m_lastTab;
    if (!m_modal->openTab(id) && !m_modal->activeTab())
        m_modal->openTab(m_modal->tabs().value(0));
    m_modal->searchInput()->setFocus(Qt::OtherFocusReason);
    return m_modal;
}

bool SettingsController::isCorePluginEnabled(const QString& id) const
{
    // Same rule as oc::isCorePluginEnabled(window, id), read through the cached session.
    const QJsonObject cfg = m_session && m_session->config() ? m_session->config()->object(QStringLiteral("core-plugins")) : QJsonObject();
    if (cfg.contains(id))
        return cfg.value(id).toBool();
    for (const CorePluginInfo& p : corePluginInfos())
        if (p.id == id)
            return p.defaultOn;
    return true;
}

void SettingsController::setCorePluginEnabled(const QString& id, bool enabled)
{
    VaultSession* s = m_session;
    if (!s || !s->config() || isCorePluginEnabled(id) == enabled)
        return;
    // Obsidian writes the state of every core plugin.
    QJsonObject map = s->config()->object(QStringLiteral("core-plugins"));
    for (const CorePluginInfo& p : corePluginInfos())
        if (!map.contains(p.id))
            map.insert(p.id, isCorePluginEnabled(p.id));
    map.insert(id, enabled);
    s->config()->setObject(QStringLiteral("core-plugins"), map);

    // Live effects the settings can apply: the note plugins' commands.
    static const QHash<QString, QStringList> noteCommands = {
        {QStringLiteral("daily-notes"), {QStringLiteral("daily-notes"), QStringLiteral("daily-notes:goto-prev"), QStringLiteral("daily-notes:goto-next")}},
        {QStringLiteral("templates"), {QStringLiteral("insert-template"), QStringLiteral("insert-current-date"), QStringLiteral("insert-current-time")}},
        {QStringLiteral("zk-prefixer"), {QStringLiteral("zk-prefixer"), QStringLiteral("insert-unique-link")}},
        {QStringLiteral("random-note"), {QStringLiteral("random-note")}},
    };
    if (CommandRegistry* r = s->commands(); r && noteCommands.contains(id)) {
        if (enabled)
            registerNotePluginCommands(*r, m_window);
        else
            for (const QString& cmd : noteCommands.value(id))
                r->remove(cmd);
    }
    emit corePluginToggled(id, enabled);
    if (m_modal)
        m_modal->rebuildNavigation();
}

bool SettingsController::isCommunityPluginEnabled(const QString& id) const
{
    VaultSession* s = m_session;
    if (!s || !s->config())
        return true;
    return s->config()->value(QStringLiteral("community-plugins"), id).toBool(true);
}

void SettingsController::setCommunityPluginEnabled(const QString& id, bool enabled)
{
    VaultSession* s = m_session;
    if (!s || !s->config() || isCommunityPluginEnabled(id) == enabled)
        return;
    s->config()->setValue(QStringLiteral("community-plugins"), id, enabled);
    if (id == u"greentext-guard") {
        if (enabled)
            GreentextGuard::instance().install();
        else
            GreentextGuard::instance().uninstall();
    }
    emit communityPluginToggled(id, enabled);
    if (m_modal)
        m_modal->rebuildNavigation();
}

void SettingsController::applyAppearance()
{
    ThemeManager* tm = ThemeManager::instance();
    VaultSession* s = m_session;
    if (!tm || !s || !s->config())
        return;
    // Every ThemeManager setter restyles the whole application: only call the ones that change.
    const QJsonObject a = s->config()->object(QStringLiteral("appearance"));
    const auto mode = ThemeManager::modeFromObsidianName(a.value(QStringLiteral("theme")).toString(QStringLiteral("system")));
    if (tm->mode() != mode)
        tm->setMode(mode);
    const QString accentName = a.value(QStringLiteral("accentColor")).toString();
    const QColor accent = accentName.isEmpty() ? QColor() : QColor(accentName);
    if (tm->accent() != accent)
        tm->setAccent(accent);
    FontSettings fonts = tm->fonts();
    // Keep what the application resolved (e.g. --deterministic fonts) for lists that are unset.
    auto family = [&a](const char* key, QString& into) {
        const QString list = a.value(QLatin1String(key)).toString();
        if (!list.isEmpty() || into != resolveFontList(list))
            into = resolveFontList(list);
    };
    family("interfaceFontFamily", fonts.interfaceFamily);
    family("textFontFamily", fonts.textFamily);
    family("monospaceFontFamily", fonts.monospaceFamily);
    fonts.baseFontSize = a.value(QStringLiteral("baseFontSize")).toDouble(16);
    const FontSettings& cur = tm->fonts();
    if (fonts.interfaceFamily != cur.interfaceFamily || fonts.textFamily != cur.textFamily || fonts.monospaceFamily != cur.monospaceFamily
        || fonts.baseFontSize != cur.baseFontSize)
        tm->setFonts(fonts);
    const css_snippets::Overrides ov = css_snippets::collect(s->config());
    if (ov.body != m_overrides.body || ov.light != m_overrides.light || ov.dark != m_overrides.dark) {
        m_overrides = ov;
        tm->setOverrides(ov.body, ov.light, ov.dark);
    }
}

void SettingsController::applyOptions()
{
    VaultSession* s = m_session;
    VaultConfig* cfg = s ? s->vaultConfig() : nullptr;
    if (!cfg || !m_window)
        return;
    if (Ribbon* r = m_window->ribbon())
        r->setVisible(cfg->flag(QStringLiteral("showRibbon")));
    if (Workspace* ws = m_window->workspace()) {
        ws->setShowViewHeader(cfg->flag(QStringLiteral("showViewHeader")));
        ws->setFocusNewTab(cfg->flag(QStringLiteral("focusNewTab")));
    }
    mdcmd::Settings& md = mdcmd::settings();
    md.smartIndentList = cfg->flag(QStringLiteral("smartIndentList"));
    md.autoPairBrackets = cfg->flag(QStringLiteral("autoPairBrackets"));
    md.autoPairMarkdown = cfg->flag(QStringLiteral("autoPairMarkdown"));
    md.useTab = cfg->useTab();
    md.tabSize = cfg->tabSize();
    SpellChecker& spell = SpellChecker::instance();
    spell.setEnabled(cfg->flag(QStringLiteral("spellcheck")));
    const QJsonArray langs = cfg->get(QStringLiteral("spellcheckLanguages")).toArray();
    if (!langs.isEmpty() && langs.first().toString() != spell.language())
        spell.setLanguage(langs.first().toString());
}

// ---------------------------------------------------------------------------------------------

void registerSettingsCommands(CommandRegistry& registry, MainWindow* window)
{
    QPointer<MainWindow> w = window;
    Command c;
    c.id = QStringLiteral("app:open-settings");
    c.name = QObject::tr("Open settings");
    c.icon = QStringLiteral("settings");
    c.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+,"))};
    c.callback = [w] {
        if (w)
            openSettings(w);
    };
    registry.add(c);
}

SettingsModal* openSettings(MainWindow* window, const QString& tabId)
{
    SettingsController* c = SettingsController::of(window);
    return c ? c->open(tabId) : nullptr;
}

} // namespace oc
