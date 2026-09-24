#include "ui/navigation/navigation.h"

#include "core/commands/command_registry.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/overlay/overlay_layer.h"

#include <QCoreApplication>
#include <QPointer>

namespace oc {

namespace {
QString tr(const char* s) { return QCoreApplication::translate("oc::Navigation", s); }
} // namespace

void registerNavigationViews(ViewRegistry& registry) { registerSearchView(registry); }

void registerNavigationCommands(CommandRegistry& registry, VaultSession* session, OverlayLayer* layer)
{
    QPointer<VaultSession> sess = session;
    QPointer<OverlayLayer> lay = layer;
    QPointer<CommandRegistry> reg = &registry;

    Command switcher;
    switcher.id = QStringLiteral("switcher:open");
    switcher.name = tr("Quick switcher: Open quick switcher");
    switcher.icon = QStringLiteral("navigation");
    switcher.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+O"))};
    switcher.checkCallback = [sess, lay](bool checking) {
        if (!sess || !lay)
            return false;
        if (!checking)
            openQuickSwitcher(sess, lay);
        return true;
    };
    registry.add(switcher);

    Command palette;
    palette.id = QStringLiteral("command-palette:open");
    palette.name = tr("Command palette: Open command palette");
    palette.icon = QStringLiteral("terminal-square");
    palette.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+P"))};
    palette.checkCallback = [sess, lay](bool checking) {
        if (!sess || !lay)
            return false;
        if (!checking)
            openCommandPalette(sess, lay);
        return true;
    };
    registry.add(palette);

    Command search;
    search.id = QStringLiteral("global-search:open");
    search.name = tr("Search: Search in all files");
    search.icon = QStringLiteral("search");
    search.defaultHotkeys = {Hotkey::parse(QStringLiteral("Mod+Shift+F"))};
    search.checkCallback = [sess, reg](bool checking) {
        if (!sess)
            return false;
        if (!checking) {
            // Obsidian searches for the editor's selection (when it fits on one line).
            QString query;
            if (reg)
                if (auto* editor = qobject_cast<EditorView*>(reg->activeEditor().editor)) {
                    const QString sel = editor->selectedText();
                    if (!sel.trimmed().isEmpty() && !sel.contains(QLatin1Char('\n')))
                        query = sel;
                }
            openGlobalSearch(sess, query, true);
        }
        return true;
    };
    registry.add(search);

    persistRecentCommands(registry, session);
}

SearchView* openGlobalSearchForTag(VaultSession* session, const QString& tag)
{
    QString t = tag.trimmed();
    while (t.startsWith(QLatin1Char('#')))
        t.remove(0, 1);
    if (t.isEmpty())
        return openGlobalSearch(session);
    return openGlobalSearch(session, QStringLiteral("tag:#") + t, true);
}

} // namespace oc
