#include "ui/plugins/core_plugins.h"

#include "core/commands/command_registry.h"
#include "core/vault_session.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/settings/settings_modal.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/workspace/workspace.h"

#include <QHash>
#include <QPointer>
#include <QSet>

#include <memory>

using namespace Qt::StringLiterals;

namespace oc {

namespace {
const QHash<QString, CorePluginParts>& table()
{
    static const QHash<QString, CorePluginParts> t = {
        {u"file-explorer"_s, {{u"file-explorer:"_s}, {}, {}, {u"file-explorer"_s}, u"left"_s}},
        {u"global-search"_s, {{u"global-search:"_s}, {}, {}, {u"search"_s}, u"left"_s}},
        {u"switcher"_s, {{u"switcher:"_s}, {u"switcher:open"_s}, {}, {}, {}}},
        {u"graph"_s, {{u"graph:"_s}, {u"graph:open"_s}, {}, {u"graph"_s, u"localgraph"_s}, {}}},
        {u"backlink"_s, {{u"backlink:"_s}, {}, {u"backlinks"_s}, {u"backlink"_s}, u"right"_s}},
        {u"outgoing-link"_s, {{u"outgoing-links:"_s}, {}, {}, {u"outgoing-link"_s}, u"right"_s}},
        {u"tag-pane"_s, {{u"tag-pane:"_s}, {}, {}, {u"tag"_s}, u"right"_s}},
        {u"properties"_s, {{u"properties:"_s}, {}, {}, {u"all-properties"_s}, u"right"_s}},
        {u"command-palette"_s, {{u"command-palette:"_s}, {u"command-palette:open"_s}, {}, {}, {}}},
        {u"editor-status"_s, {{}, {}, {u"editor-status"_s}, {}, {}}},
        {u"bookmarks"_s, {{u"bookmarks:"_s}, {}, {}, {u"bookmarks"_s}, u"left"_s}},
        {u"outline"_s, {{u"outline:"_s}, {}, {}, {u"outline"_s}, u"right"_s}},
        {u"word-count"_s, {{}, {}, {u"word-count"_s}, {}, {}}},
        {u"file-recovery"_s, {{u"file-recovery:"_s}, {}, {}, {}, {}}},
        {u"note-composer"_s, {{u"note-composer:"_s}, {}, {}, {}, {}}},
        {u"canvas"_s, {{u"canvas:"_s}, {u"canvas:new-file"_s}, {}, {u"canvas"_s}, {}}},
        {u"bases"_s, {{u"bases:"_s}, {u"bases:new-file"_s}, {}, {u"bases"_s}, {}}},
        {u"markdown-importer"_s, {{u"markdown-importer:"_s}, {}, {}, {}, {}}},
        {u"workspaces"_s, {{u"workspaces:"_s}, {}, {}, {}, {}}},
        {u"slides"_s, {{u"slides:"_s}, {}, {}, {}, {}}},
        {u"audio-recorder"_s, {{u"audio-recorder:"_s}, {}, {}, {}, {}}},
        {u"footnotes"_s, {{u"footnotes:"_s}, {}, {}, {u"footnotes"_s}, u"right"_s}},
        {u"webviewer"_s, {{u"webviewer:"_s}, {}, {}, {}, {}}},
    };
    return t;
}

// The disabled plugins' command prefixes of one window (a child object found by name), shared with
// the command filter through a weak pointer.
struct SwitchState {
    QSet<QString> disabled;
    QStringList prefixes;
    void rebuild()
    {
        prefixes.clear();
        for (const QString& id : disabled)
            if (const CorePluginParts* p = corePluginParts(id))
                prefixes << p->commandPrefixes;
    }
};

class SwitchHolder : public QObject {
public:
    explicit SwitchHolder(QObject* parent)
        : QObject(parent)
    {
        setObjectName(u"ocCorePluginSwitches"_s);
    }
    std::shared_ptr<SwitchState> state = std::make_shared<SwitchState>();
};

std::shared_ptr<SwitchState> stateOf(MainWindow* w)
{
    QObject* holder = w->findChild<QObject*>(u"ocCorePluginSwitches"_s, Qt::FindDirectChildrenOnly);
    return holder ? static_cast<SwitchHolder*>(holder)->state : nullptr;
}
} // namespace

const CorePluginParts* corePluginParts(const QString& id)
{
    const auto& t = table();
    auto it = t.constFind(id);
    return it == t.cend() ? nullptr : &*it;
}

void applyCorePluginState(MainWindow* window, const QString& id, bool enabled)
{
    const CorePluginParts* parts = corePluginParts(id);
    if (!window || !parts)
        return;
    if (auto st = stateOf(window)) {
        if (enabled)
            st->disabled.remove(id);
        else
            st->disabled.insert(id);
        st->rebuild();
    }
    if (Ribbon* r = window->ribbon())
        for (const QString& a : parts->ribbonActions)
            r->setActionAvailable(a, enabled);
    if (StatusBar* sb = window->statusBar())
        for (const QString& item : parts->statusItems)
            sb->setItemEnabled(item, enabled);
    Workspace* ws = window->workspace();
    if (!ws)
        return;
    if (!enabled) {
        QList<WorkspaceLeaf*> leaves;
        for (const QString& type : parts->viewTypes)
            leaves << ws->leavesOfType(type);
        if (!leaves.isEmpty())
            ws->closeLeaves(leaves);
    } else if (!parts->side.isEmpty() && !parts->viewTypes.isEmpty() && ws->leavesOfType(parts->viewTypes.first()).isEmpty()) {
        ws->ensureSideLeaf(parts->viewTypes.first(), parts->side == u"left" ? Workspace::Side::Left : Workspace::Side::Right);
    }
    if (VaultSession* s = window->session(); s && s->commands())
        emit s->commands()->commandsChanged();
}

void installCorePluginSwitches(MainWindow* window)
{
    VaultSession* s = window ? window->session() : nullptr;
    if (!s || !s->commands() || stateOf(window))
        return;
    std::weak_ptr<SwitchState> weak = (new SwitchHolder(window))->state;
    s->commands()->setAvailabilityFilter([weak](const Command& c) {
        const auto st = weak.lock();
        if (!st)
            return true;
        for (const QString& prefix : st->prefixes)
            if (c.id.startsWith(prefix))
                return false;
        return true;
    });
    // Views restored from the layout of a plugin that is off close too (their type is unknown to
    // Obsidian then); a plugin that is on keeps whatever layout the user left.
    for (auto it = table().cbegin(); it != table().cend(); ++it)
        if (!isCorePluginEnabled(window, it.key().toLatin1().constData()))
            applyCorePluginState(window, it.key(), false);

    QPointer<MainWindow> w = window;
    QObject::connect(SettingsController::of(window), &SettingsController::corePluginToggled, window,
                     [w](const QString& id, bool enabled) { applyCorePluginState(w, id, enabled); });
}

} // namespace oc
