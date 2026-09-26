#include "ui/views/bases/bases_controller.h"

#include "core/config/vault_config.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/frontmatter_editor.h"
#include "core/properties/property_types.h"
#include "core/properties/property_yaml.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "ui/navigation/navigation.h"
#include "ui/navigation/navigation_util.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_menus.h"
#include "ui/views/bases/bases_style.h"
#include "ui/views/bases/bases_widgets.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/widgets/search_input.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/workspace.h"
#include "core/util/owned_menu.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QKeyEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace oc::basesui {

using namespace oc::bases;

const QList<ViewTypeInfo>& viewTypes()
{
    static const QList<ViewTypeInfo> types = {
        {QStringLiteral("table"), QCoreApplication::translate("oc::basesui", "Table"), QStringLiteral("table")},
        {QStringLiteral("cards"), QCoreApplication::translate("oc::basesui", "Cards"), QStringLiteral("layout-grid")},
        {QStringLiteral("list"), QCoreApplication::translate("oc::basesui", "List"), QStringLiteral("list")},
    };
    return types;
}

const ViewTypeInfo* viewType(const QString& id)
{
    for (const ViewTypeInfo& t : viewTypes())
        if (t.id == id)
            return &t;
    return nullptr;
}

namespace {

// The main window of the base (also when it is in one of its pop-outs).
MainWindow* mainWindowOf(QWidget* w) { return w ? MainWindow::of(w) : nullptr; }

// The header (.bases-header): paints its bottom border in a .base tab.
class HeaderWidget : public QWidget {
public:
    HeaderWidget(bool border, QWidget* parent) : QWidget(parent), m_border(border) {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!m_border)
            return;
        QPainter p(this);
        p.fillRect(QRect(0, height() - 1, width(), 1), basesStyle().tableBorder);
    }

private:
    bool m_border;
};

} // namespace

BasesController::BasesController(VaultSession* session, Mode mode, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_mode(mode)
{
    setObjectName(mode == Mode::File ? QStringLiteral("ocBasesController") : QStringLiteral("ocBasesEmbedController"));
    setAttribute(Qt::WA_StyledBackground, false);
    const bool embedded = mode == Mode::Embed;
    const Style& s = basesStyle(embedded);
    // the query
    QPointer<VaultSession> ses = session;
    auto ignore = std::make_shared<UserIgnoreFilters>(session ? UserIgnoreFilters(session->vaultConfig()) : UserIgnoreFilters());
    auto ds = std::make_shared<VaultDataSource>(session, [ignore](const QString& path) { return ignore->matches(path); });
    m_runner = new QueryRunner(ds, this);
    m_runner->followSession(session);
    m_runner->setPropertyNames([ses](const QString& lower) {
        PropertyTypeManager* t = ses ? PropertyTypeManager::of(ses) : nullptr;
        return t ? t->propertyInfo(lower).name : QString();
    });
    connect(m_runner, &QueryRunner::updated, this, &BasesController::onDataUpdated);
    connect(m_runner, &QueryRunner::errorsReported, this, [this](const QStringList& errors) {
        for (const QString& e : errors)
            notice(e);
    });
    if (session) {
        if (PropertyTypeManager* t = PropertyTypeManager::of(session))
            connect(t, &PropertyTypeManager::changed, this, [this] { m_runner->republish(); });
        if (VaultConfig* cfg = session->vaultConfig())
            connect(cfg, &VaultConfig::configChanged, this, [this, ignore, ses](const QString& key) {
                if (key == u"userIgnoreFilters" && ses) {
                    *ignore = UserIgnoreFilters(ses->vaultConfig());
                    m_runner->rescan();
                }
            });
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    // header toolbar
    m_header = new HeaderWidget(!embedded, this);
    m_header->setObjectName(QStringLiteral("ocBasesHeader"));
    m_header->setFixedHeight(s.headerHeight);
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(s.headerPadStart, 0, s.headerPadEnd, 0);
    hl->setSpacing(s.toolbarGap);
    m_viewsButton = new TextIconButton(QStringLiteral("table"), QString(), m_header);
    m_viewsButton->setObjectName(QStringLiteral("ocBasesViewsButton"));
    m_viewsButton->setAuxIcon(QStringLiteral("chevrons-up-down"));
    m_viewsButton->setContextMenuPolicy(Qt::CustomContextMenu);
    m_resultsButton = new TextIconButton(QStringLiteral("list-filter"), QString(), m_header);
    m_resultsButton->setObjectName(QStringLiteral("ocBasesResultsButton"));
    m_resultsButton->setMuted(true);
    m_sortButton = new TextIconButton(QStringLiteral("arrow-up-down"), tr("Sort"), m_header);
    m_sortButton->setObjectName(QStringLiteral("ocBasesSortButton"));
    m_filterButton = new TextIconButton(QStringLiteral("list-filter"), tr("Filter"), m_header);
    m_filterButton->setObjectName(QStringLiteral("ocBasesFilterButton"));
    m_propertiesButton = new TextIconButton(QStringLiteral("list"), tr("Properties"), m_header);
    m_propertiesButton->setObjectName(QStringLiteral("ocBasesPropertiesButton"));
    m_searchButton = new TextIconButton(QStringLiteral("search"), tr("Search"), m_header);
    m_searchButton->setObjectName(QStringLiteral("ocBasesSearchButton"));
    m_newButton = new TextIconButton(QStringLiteral("plus"), tr("New"), m_header);
    m_newButton->setObjectName(QStringLiteral("ocBasesNewButton"));
    hl->addWidget(m_viewsButton);
    hl->addWidget(m_resultsButton);
    hl->addStretch(1);
    for (TextIconButton* b : {m_sortButton, m_filterButton, m_propertiesButton, m_searchButton, m_newButton})
        hl->addWidget(b);
    root->addWidget(m_header);
    connect(m_viewsButton, &QAbstractButton::clicked, this, [this] { openViewsMenu(); });
    connect(m_viewsButton, &QWidget::customContextMenuRequested, this, [this] { openConfigureView(m_viewName); });
    connect(m_resultsButton, &QAbstractButton::clicked, this, [this] { openResultsMenu(); });
    connect(m_sortButton, &QAbstractButton::clicked, this, [this] { openSortMenu(); });
    connect(m_filterButton, &QAbstractButton::clicked, this, [this] { openFilterMenu(); });
    connect(m_propertiesButton, &QAbstractButton::clicked, this, [this] { openPropertiesMenu(); });
    connect(m_searchButton, &QAbstractButton::clicked, this, &BasesController::toggleSearch);
    connect(m_newButton, &QAbstractButton::clicked, this, [this] { createNewItem(); });
    // search row
    m_searchRow = new QWidget(this);
    auto* sl = new QHBoxLayout(m_searchRow);
    sl->setContentsMargins(s.headerPadStart, 4, s.headerPadEnd, 4);
    m_searchInput = new SearchInput(m_searchRow);
    m_searchInput->setObjectName(QStringLiteral("ocBasesSearch"));
    m_searchInput->setPlaceholderText(tr("Find..."));
    m_searchInput->installEventFilter(this);
    sl->addWidget(m_searchInput);
    m_searchRow->hide();
    root->addWidget(m_searchRow);
    connect(m_searchInput, &QLineEdit::textChanged, this, [this](const QString& t) { m_runner->setSearch(t); });
    // error line
    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("ocBasesError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_errorLabel->setContentsMargins(8, 8, 8, 8);
    m_errorLabel->setFont(s.uiSmall);
    QPalette ep = m_errorLabel->palette();
    ep.setColor(QPalette::WindowText, s.warning);
    m_errorLabel->setPalette(ep);
    m_errorLabel->hide();
    root->addWidget(m_errorLabel);
    root->addStretch(0);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            if (m_layout)
                m_layout->setData(m_runner->data());
            update();
        });
}

BasesController::~BasesController()
{
    if (MenuPanel* p = MenuPanel::openPanel(this))
        if (p->anchor() && isAncestorOf(p->anchor()))
            p->close();
}

VaultSession* BasesController::session() const { return m_session; }

// ---- model --------------------------------------------------------------------------------------
void BasesController::setBase(const BaseFile& base)
{
    m_base = base;
    m_hasBase = true;
    m_error.clear();
    m_errorLabel->hide();
    m_header->show();
    if (m_viewName.isEmpty() || !m_base.view(m_viewName))
        m_viewName = m_base.views.isEmpty() ? QString() : m_base.views.first().name;
    apply(true);
}

void BasesController::setParseError(const QString& message)
{
    m_error = message;
    m_errorLabel->setText(tr("Unable to parse your base file:") + QStringLiteral("\n\n") + message);
    m_errorLabel->show();
    m_header->hide();
    if (m_layout)
        m_layout->hide();
    emit preferredHeightChanged();
}

void BasesController::editBase(const std::function<void(BaseFile&)>& fn)
{
    if (m_readOnly)
        return;
    BaseFile copy = m_base;
    fn(copy);
    m_base = copy;
    if (m_viewName.isEmpty() || !m_base.view(m_viewName))
        m_viewName = m_base.views.isEmpty() ? QString() : m_base.views.first().name;
    apply(true);
    emit baseEdited(m_base);
}

void BasesController::editView(const std::function<void(BaseView&)>& fn)
{
    editBase([&](BaseFile& b) {
        if (BaseView* v = b.view(m_viewName))
            fn(*v);
    });
}

void BasesController::selectView(const QString& name)
{
    if (!m_base.view(name) || name == m_viewName)
        return;
    if (m_layout)
        m_viewStates.insert(m_viewName, m_layout->ephemeralState());
    m_viewName = name;
    apply(true);
    if (m_layout && m_viewStates.contains(name))
        m_layout->setEphemeralState(m_viewStates.value(name));
    emit viewChanged(name);
}

const BaseView* BasesController::currentView() const { return m_base.view(m_viewName); }

void BasesController::setThisFile(const QString& path) { m_runner->setThisFile(path); }

QString BasesController::thisFile() const { return m_runner->thisFile(); }

void BasesController::setReadOnly(bool ro)
{
    m_readOnly = ro;
    updateToolbar();
}

const ViewData& BasesController::data() const { return m_runner->data(); }

bool BasesController::isReady() const { return m_runner->isReady(); }

bool BasesController::isIdle() const { return m_runner->isIdle(); }

void BasesController::apply(bool rerunQuery)
{
    Q_UNUSED(rerunQuery);
    if (!m_hasBase)
        return;
    ensureLayout();
    m_runner->setQuery(m_base, m_viewName);
    updateToolbar();
}

void BasesController::ensureLayout()
{
    const BaseView* v = currentView();
    const QString type = v ? v->type : QStringLiteral("table");
    if (m_layout && m_layoutType == type)
        return;
    if (m_layout) {
        m_layout->hide();
        m_layout->deleteLater();
        m_layout = nullptr;
    }
    m_layoutType = type;
    auto* root = static_cast<QVBoxLayout*>(layout());
    m_layout = createLayout(type, this, this);
    if (!m_layout) {
        m_errorLabel->setText(tr("Unknown view type: %1").arg(type));
        m_errorLabel->show();
        emit preferredHeightChanged();
        return;
    }
    m_errorLabel->hide();
    m_layout->setObjectName(QStringLiteral("ocBasesLayout"));
    // (the stretch item stays last)
    root->insertWidget(root->count() - 1, m_layout, 1);
    m_layout->show();
    connect(m_layout, &BaseLayout::contentHeightChanged, this, [this] {
        if (preferredHeight() != m_lastPreferredHeight) {
            m_lastPreferredHeight = preferredHeight();
            emit preferredHeightChanged();
        }
    });
    if (m_runner->isReady())
        m_layout->setData(m_runner->data());
}

void BasesController::onDataUpdated()
{
    if (m_layout)
        m_layout->setData(m_runner->data());
    updateToolbar();
    m_searchInput->setDecorator(m_searchRow->isVisible() && !m_searchInput->text().isEmpty()
                                    ? tr("Showing %1").arg(m_runner->data().rows.size())
                                    : QString());
    if (preferredHeight() != m_lastPreferredHeight) {
        m_lastPreferredHeight = preferredHeight();
        emit preferredHeightChanged();
    }
    emit dataUpdated();
}

void BasesController::updateToolbar()
{
    const BaseView* v = currentView();
    const ViewTypeInfo* info = viewType(v ? v->type : QString());
    m_viewsButton->setIconName(info ? info->icon : QStringLiteral("file-question"));
    m_viewsButton->setLabel(m_viewName);
    // "N results" (the limit caps it; is-active then)
    const int total = m_runner->resultCount();
    int shown = total;
    bool limited = false;
    if (v && v->limit > 0 && total > v->limit) {
        shown = int(v->limit);
        limited = true;
    }
    m_resultsButton->setLabel(shown == 1 ? tr("%1 result").arg(shown) : tr("%1 results").arg(QLocale().toString(shown)));
    m_resultsButton->setActive(limited);
    const int sorts = v ? int(v->sortSpecs().size()) : 0;
    m_sortButton->setBadge(QString()); // --bases-toolbar-badge-display: none on desktop
    m_sortButton->setActive(sorts > 0 || (v && v->groupBy));
    auto count = [](const std::optional<Filter>& f) { return !f ? 0 : f->kind == Filter::Kind::Expr ? 1 : int(f->children.size()); };
    const int viewFilters = v ? count(v->filters) : 0;
    m_filterButton->setActive(viewFilters > 0);
    const bool filterError = (m_base.filters && m_base.filters->hasError()) || (v && v->filters && v->filters->hasError());
    m_filterButton->setError(filterError);
    bool formulaError = false;
    for (const auto& [n, f] : m_base.formulas)
        formulaError |= f->isInvalid();
    m_propertiesButton->setError(formulaError);
    for (TextIconButton* b : {m_sortButton, m_filterButton, m_propertiesButton, m_newButton})
        b->setEnabled(!m_readOnly || b == m_sortButton);
    m_newButton->setVisible(!m_readOnly);
    relayoutHeader();
}

void BasesController::relayoutHeader()
{
    // @container (width < 540px): toolbar labels hidden (not the views menu / result count)
    const bool labels = width() >= 540 || width() == 0;
    for (TextIconButton* b : {m_sortButton, m_filterButton, m_propertiesButton, m_searchButton, m_newButton})
        b->setLabelVisible(labels);
}

int BasesController::preferredHeight() const
{
    int h = 0;
    if (!m_header->isHidden())
        h += m_header->height();
    if (!m_searchRow->isHidden())
        h += m_searchRow->sizeHint().height();
    if (!m_errorLabel->isHidden())
        h += m_errorLabel->heightForWidth(std::max(200, width())) > 0 ? m_errorLabel->heightForWidth(std::max(200, width()))
                                                                       : m_errorLabel->sizeHint().height();
    if (m_layout && !m_layout->isHidden() && !hasError()) {
        int content = m_layout->contentHeight();
        if (m_layout->horizontalScrollBar()->isVisible())
            content += m_layout->horizontalScrollBar()->height();
        h += content;
    }
    return h;
}

void BasesController::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    relayoutHeader();
}

void BasesController::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), basesStyle(isEmbedded()).bgPrimary);
}

bool BasesController::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_searchInput && ev->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        toggleSearch();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

// ---- services -----------------------------------------------------------------------------------
void BasesController::openLink(const QString& target, const QString& sourcePath, Qt::KeyboardModifiers mods)
{
    Workspace* ws = m_session ? m_session->workspace() : nullptr;
    if (!ws)
        return;
    MarkdownView::openLinkText(ws, target, 0, sourcePath, newLeafForModifiers(mods));
}

void BasesController::openFile(const QString& path, Qt::KeyboardModifiers mods)
{
    Workspace* ws = m_session ? m_session->workspace() : nullptr;
    TFile* f = m_session && m_session->vault() ? m_session->vault()->file(path) : nullptr;
    if (!ws || !f)
        return;
    ws->openFile(f, newLeafForModifiers(mods));
}

void BasesController::hoverLink(const QString& target, const QString& sourcePath, const QRect& globalRect, QWidget* hoverParent)
{
    if (MainWindow* w = mainWindowOf(this))
        PagePreview::of(w)->onLinkHover(QStringLiteral("bases"), target, sourcePath, globalRect, hoverParent);
}

void BasesController::unhoverLink(QWidget* hoverParent)
{
    if (MainWindow* w = mainWindowOf(this))
        PagePreview::of(w)->onLinkUnhover(hoverParent);
}

void BasesController::openTag(const QString& tag)
{
    if (m_session)
        openGlobalSearchForTag(m_session, tag.startsWith(u'#') ? tag.mid(1) : tag);
}

void BasesController::openUrl(const QString& url)
{
    if (desktopHandlers().openUrl)
        desktopHandlers().openUrl(QUrl(url));
}

bool BasesController::isResolved(const QString& linktext, const QString& sourcePath) const
{
    const qsizetype hash = linktext.indexOf(u'#');
    const QString path = hash < 0 ? linktext : linktext.left(hash);
    if (path.isEmpty())
        return true;
    return !m_runner->dataSource()->resolveLink(path, sourcePath).isEmpty();
}

QString BasesController::imagePath(const QString& target, const QString& sourcePath) const
{
    if (!m_session || !m_session->vault())
        return {};
    QString path = target;
    if (!m_session->vault()->file(path))
        path = m_runner->dataSource()->resolveLink(target, sourcePath);
    if (path.isEmpty() || !m_session->vault()->file(path))
        return {};
    static const QStringList images = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
                                       QStringLiteral("bmp"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
    const QString ext = path.section(u'.', -1).toLower();
    if (!images.contains(ext))
        return {};
    return m_session->vault()->absolutePath(path);
}

QString BasesController::propertyType(const QString& key, const QVariant& value) const
{
    PropertyTypeManager* t = m_session ? PropertyTypeManager::of(m_session) : nullptr;
    if (!t)
        return QStringLiteral("text");
    const PropertyTypeManager::TypeInfo info = t->typeInfo(key, value);
    return value.isValid() ? info.inferred : info.expected;
}

QString BasesController::propertyIcon(const QString& key) const
{
    return props::propertyType(propertyType(key, QVariant())).icon;
}

bool BasesController::writeProperty(const QString& path, const QString& key, const QVariant& raw)
{
    FileManager* fm = m_session ? m_session->fileManager() : nullptr;
    if (!fm || m_readOnly)
        return false;
    const QVariant value = props::normalizePropertyValue(raw);
    QString error;
    const bool ok = fm->editFile(path, [&](const Text& text) -> QList<ChangeSpec> {
        props::FrontmatterEditor ed(text);
        if (!ed.isEditable())
            return {};
        // Obsidian's IA(): the existing spelling of the key
        QString actual = key;
        for (const props::Property& p : ed.properties()) {
            if (p.key == key) {
                actual = p.key;
                break;
            }
            if (p.key.compare(key, Qt::CaseInsensitive) == 0)
                actual = p.key;
        }
        if (!ed.has(actual) && (!value.isValid() || value.isNull()))
            return {};
        if (ed.has(actual) && props::jsonText(ed.value(actual)) == props::jsonText(value) && ed.value(actual).typeId() == value.typeId())
            return {};
        ed.set(actual, value);
        return ed.changes();
    }, &error);
    if (!ok && !error.isEmpty())
        notice(error);
    return ok;
}

void BasesController::showFileMenu(const QString& path, const QPoint& globalPos, QWidget* origin)
{
    Vault* vault = m_session ? m_session->vault() : nullptr;
    TFile* file = vault ? vault->file(path) : nullptr;
    if (!file)
        return;
    QList<MenuEntry> items;
    QPointer<BasesController> self = this;
    items << MenuEntry{QStringLiteral("open"), tr("Open in new tab"), QStringLiteral("file-plus"), [self, path] {
                           if (self)
                               self->openFile(path, Qt::ControlModifier);
                       }};
    items << MenuEntry{QStringLiteral("open"), tr("Open to the right"), QStringLiteral("separator-vertical"), [self, path] {
                           if (self)
                               self->openFile(path, Qt::ControlModifier | Qt::AltModifier);
                       }};
    FileMenuHooks::collect(items, file, QStringLiteral("link-context-menu"), nullptr, origin);
    QPointer<VaultSession> ses = m_session;
    MenuEntry del{QStringLiteral("danger"), tr("Delete file"), QStringLiteral("trash-2"), [ses, path] {
                      if (ses && ses->vault())
                          if (TFile* f = ses->vault()->file(path))
                              promptForDeletion(ses, f);
                  }};
    del.warning = true;
    items << del;
    QMenu* menu = ownedMenu(origin ? origin : this);
    buildSectionedMenu(menu, items);
    menu->popup(globalPos);
}

void BasesController::notice(const QString& message)
{
    if (MainWindow* w = mainWindowOf(this))
        if (w->notices())
            w->notices()->show(message);
}

PaintOptions BasesController::paintOptions(const QFont& font, const QColor& color) const
{
    PaintOptions o;
    o.font = font;
    o.color = color;
    QPointer<const BasesController> self = this;
    o.isResolved = [self](const QString& linktext, const QString& source) { return !self || self->isResolved(linktext, source); };
    o.imagePath = [self](const QString& target) { return self ? self->imagePath(target) : QString(); };
    return o;
}

// ---- toolbar actions ----------------------------------------------------------------------------
void BasesController::toggleSearch()
{
    const bool open = m_searchRow->isHidden();
    m_searchRow->setVisible(open);
    m_searchButton->setActive(open);
    if (open) {
        m_searchInput->setFocus();
        m_searchInput->selectAll();
        m_runner->setSearch(m_searchInput->text());
    } else {
        m_runner->setSearch(QString());
        if (m_layout)
            m_layout->setFocus();
    }
    emit preferredHeightChanged();
}

void BasesController::setSearchText(const QString& text)
{
    if (m_searchRow->isHidden())
        toggleSearch();
    m_searchInput->setText(text);
}

MenuPanel* BasesController::openViewsMenu() { return basesui::openViewsMenu(this, m_viewsButton); }
MenuPanel* BasesController::openResultsMenu() { return basesui::openResultsMenu(this, m_resultsButton); }
MenuPanel* BasesController::openSortMenu() { return basesui::openSortMenu(this, m_sortButton); }
MenuPanel* BasesController::openFilterMenu() { return basesui::openFilterMenu(this, m_filterButton); }
MenuPanel* BasesController::openPropertiesMenu() { return basesui::openPropertiesMenu(this, m_propertiesButton); }
MenuPanel* BasesController::openConfigureView(const QString& name) { return basesui::openConfigureViewMenu(this, m_viewsButton, name); }

void BasesController::promptAddView()
{
    const QString name = m_base.uniqueViewName(tr("View"));
    editBase([&](BaseFile& b) {
        BaseView v;
        v.type = QStringLiteral("table");
        v.name = name;
        b.views << v;
    });
    selectView(name);
    openConfigureView(name);
}

QString BasesController::createNewItem()
{
    Vault* vault = m_session ? m_session->vault() : nullptr;
    FileManager* fm = m_session ? m_session->fileManager() : nullptr;
    if (!vault || !fm || m_readOnly)
        return {};
    const BaseView* v = currentView();
    QString folder = m_base.newItemFolder;
    QList<QPair<QString, QVariant>> frontmatter;
    bool fromTemplate = false;
    if (!m_base.newItemTemplate.isEmpty()) {
        if (vault->file(m_base.newItemTemplate)) {
            if (folder.isEmpty())
                folder = m_base.newItemTemplate.section(u'/', 0, -2);
            if (const md::CachedMetadata* m = m_session->metadata() ? m_session->metadata()->fileCache(m_base.newItemTemplate) : nullptr)
                if (m->frontmatter)
                    for (const md::FrontmatterValue& fv : *m->frontmatter)
                        frontmatter.append({fv.key, fv.value});
            fromTemplate = true;
        }
    }
    const QString ownerPath = m_runner->thisFile();
    PropertyTypeManager* types = PropertyTypeManager::of(m_session);
    const NewItemDefaults defaults = newItemDefaults(m_base, v, m_runner->dataSource(), nullptr, [types](const QString& key) {
        return types ? types->typeInfo(key).expected : QString();
    });
    if (folder.isEmpty())
        folder = defaults.folder;
    if (!fromTemplate)
        frontmatter = defaults.frontmatter;
    TFolder* parent = nullptr;
    if (!folder.isEmpty()) {
        parent = vault->folder(folder);
        if (!parent)
            parent = vault->createFolder(folder);
    } else {
        parent = fm->newFileParent(ownerPath);
    }
    QByteArray content;
    if (!frontmatter.isEmpty())
        content = (QStringLiteral("---\n") + props::yamlDocument(frontmatter) + QStringLiteral("\n---\n")).toUtf8();
    QString error;
    TFile* file = fm->createNewMarkdownFile(parent, QString(), content, &error);
    if (!file) {
        if (!error.isEmpty())
            notice(error);
        return {};
    }
    if (Workspace* ws = m_session->workspace())
        openNewFile(ws, file, Workspace::NewLeaf::Tab);
    return file->path();
}

TableExport BasesController::exportTable() const { return bases::exportTable(m_base, m_runner->data()); }

void BasesController::copyTable()
{
    const TableExport t = exportTable();
    auto* mime = new QMimeData;
    mime->setText(t.toTsv());
    mime->setData(QStringLiteral("text/markdown"), t.toMarkdown().toUtf8());
    QApplication::clipboard()->setMimeData(mime);
    notice(tr("Copied to your clipboard"));
}

QJsonObject BasesController::ephemeralState() const
{
    QJsonObject views;
    for (auto it = m_viewStates.cbegin(); it != m_viewStates.cend(); ++it)
        views.insert(it.key(), it.value());
    if (m_layout)
        views.insert(m_viewName, m_layout->ephemeralState());
    return QJsonObject{{QStringLiteral("views"), views}};
}

void BasesController::setEphemeralState(const QJsonObject& state)
{
    const QJsonObject views = state.value(QStringLiteral("views")).toObject();
    for (auto it = views.begin(); it != views.end(); ++it)
        m_viewStates.insert(it.key(), it.value().toObject());
    if (m_layout && views.contains(m_viewName))
        m_layout->setEphemeralState(views.value(m_viewName).toObject());
}

} // namespace oc::basesui
