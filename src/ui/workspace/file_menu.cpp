#include "ui/workspace/file_menu.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QAction>
#include <QMenu>

#include <map>

namespace oc {

namespace {

QIcon menuIcon(const QString& name, bool warning)
{
    if (name.isEmpty())
        return {};
    if (!warning)
        return IconProvider::instance().icon(name, 16, 2.0);
    const QColor red = currentTheme()->color(QStringLiteral("--text-error"), QColor(0xfb, 0x46, 0x4c));
    QIcon ic;
    for (qreal dpr : {1.0, 2.0})
        ic.addPixmap(IconProvider::instance().pixmap(name, 16, red, 2.0, dpr));
    return ic;
}

template <typename Provider>
struct Registry {
    std::map<int, Provider> providers;
    int next = 1;
};

Registry<FileMenuHooks::Provider>& fileProviders()
{
    static Registry<FileMenuHooks::Provider> r;
    return r;
}

Registry<EditorMenuHooks::Provider>& editorProviders()
{
    static Registry<EditorMenuHooks::Provider> r;
    return r;
}

} // namespace

const QStringList& menuSectionOrder()
{
    static const QStringList order{QStringLiteral("title"),  QStringLiteral("open"),      QStringLiteral("selection"),
                                   QStringLiteral("clipboard"), QStringLiteral("action-primary"), QStringLiteral("action"),
                                   QStringLiteral("info"),   QStringLiteral("info.copy"), QStringLiteral("view"),
                                   QStringLiteral("view.linked"), QStringLiteral("system"), QString(),
                                   QStringLiteral("danger")};
    return order;
}

void buildSectionedMenu(QMenu* menu, const QList<MenuEntry>& items)
{
    QStringList sections = menuSectionOrder();
    for (const MenuEntry& e : items)
        if (!sections.contains(e.section))
            sections.insert(sections.size() - 1, e.section); // unknown sections go before "danger"
    bool first = menu->actions().isEmpty();
    for (const QString& section : std::as_const(sections)) {
        bool any = false;
        for (const MenuEntry& e : items) {
            if (e.section != section)
                continue;
            if (!any && !first)
                menu->addSeparator();
            any = true;
            QAction* a = menu->addAction(menuIcon(e.icon, e.warning), e.title);
            a->setEnabled(e.enabled);
            a->setProperty("ocWarning", e.warning);
            auto run = e.run;
            QObject::connect(a, &QAction::triggered, menu, [run] {
                if (run)
                    run();
            });
        }
        if (any)
            first = false;
    }
}

int FileMenuHooks::add(Provider provider)
{
    auto& r = fileProviders();
    const int id = r.next++;
    r.providers.emplace(id, std::move(provider));
    return id;
}

void FileMenuHooks::remove(int id)
{
    fileProviders().providers.erase(id);
}

void FileMenuHooks::collect(QList<MenuEntry>& items, TAbstractFile* file, const QString& source, WorkspaceLeaf* leaf, QWidget* origin)
{
    const auto providers = fileProviders().providers; // a provider may add/remove hooks
    for (const auto& [id, p] : providers)
        if (p)
            p(items, file, source, leaf, origin);
}

int EditorMenuHooks::add(Provider provider)
{
    auto& r = editorProviders();
    const int id = r.next++;
    r.providers.emplace(id, std::move(provider));
    return id;
}

void EditorMenuHooks::remove(int id)
{
    editorProviders().providers.erase(id);
}

void EditorMenuHooks::collect(QList<MenuEntry>& items, MarkdownView* view, qsizetype docPos)
{
    const auto providers = editorProviders().providers;
    for (const auto& [id, p] : providers)
        if (p)
            p(items, view, docPos);
}

} // namespace oc
