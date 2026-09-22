#include "ui/workspace/placeholder_views.h"

#include "render/theme/theme.h"
#include "ui/workspace/view_registry.h"

#include <QCoreApplication>
#include <QPainter>

namespace oc {

PlaceholderView::PlaceholderView(WorkspaceLeaf* leaf, QString type, QWidget* parent)
    : EmptyView(leaf, parent)
    , m_type(std::move(type))
{
    setObjectName(QStringLiteral("ocPlaceholderView"));
    setTitle(tr("Plugin no longer active"));
    setDescription(tr("The plugin that created this tab (%1) has gone away.").arg(m_type));
    setToolTip(QStringLiteral("This pane doesn't look like anything to me."));
}

void PlaceholderView::setOriginalViewState(const QJsonObject& viewState)
{
    m_original = viewState;
    m_original.remove(QStringLiteral("active")); // transient setViewState() flag
}

QJsonObject PlaceholderView::originalViewState(bool pinned) const
{
    QJsonObject o = m_original;
    o.insert(QStringLiteral("type"), m_type);
    o.insert(QStringLiteral("state"), m_state);
    if (pinned)
        o.insert(QStringLiteral("pinned"), true);
    else
        o.remove(QStringLiteral("pinned"));
    return o;
}

SidebarPlaceholderView::SidebarPlaceholderView(WorkspaceLeaf* leaf, QString type, QString title, QString icon, QWidget* parent)
    : View(leaf, parent)
    , m_type(std::move(type))
    , m_title(std::move(title))
    , m_icon(std::move(icon))
{
    setObjectName(QStringLiteral("ocSidebarPlaceholder"));
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void SidebarPlaceholderView::paintEvent(QPaintEvent*)
{
    // .view-content padding 12px; caption in --text-faint.
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13)));
    p.setPen(t->color(QStringLiteral("--text-faint")));
    p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignHCenter | Qt::AlignTop, m_title);
}

void registerCoreViews(ViewRegistry& registry)
{
    struct Def {
        const char* type;
        const char* title;
        const char* icon;
    };
    static const Def defs[] = {
        {"file-explorer", QT_TRANSLATE_NOOP("oc::Workspace", "Files"), "folder-closed"},
        {"search", QT_TRANSLATE_NOOP("oc::Workspace", "Search"), "search"},
        {"bookmarks", QT_TRANSLATE_NOOP("oc::Workspace", "Bookmarks"), "bookmark"},
        {"backlink", QT_TRANSLATE_NOOP("oc::Workspace", "Backlinks"), "links-coming-in"},
        {"outgoing-link", QT_TRANSLATE_NOOP("oc::Workspace", "Outgoing links"), "links-going-out"},
        {"tag", QT_TRANSLATE_NOOP("oc::Workspace", "Tags"), "tags"},
        {"outline", QT_TRANSLATE_NOOP("oc::Workspace", "Outline"), "list"},
    };
    for (const Def& def : defs) {
        const QString type = QString::fromLatin1(def.type);
        const QString title = QCoreApplication::translate("oc::Workspace", def.title);
        const QString icon = QString::fromLatin1(def.icon);
        registry.registerView(type, [type, title, icon](WorkspaceLeaf* leaf) -> View* {
            return new SidebarPlaceholderView(leaf, type, title, icon);
        });
    }
}

} // namespace oc
