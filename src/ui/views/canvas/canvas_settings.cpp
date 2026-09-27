#include "ui/views/canvas/canvas_settings.h"

#include "ui/settings/settings_tab.h"

#include <QCoreApplication>
#include <QJsonObject>

namespace oc {

using namespace Qt::StringLiterals;

namespace {

// Obsidian merges the plugin's data over {snapToObjects: true, snapToGrid: true}; the other options
// default when missing.
const QJsonObject& defaults()
{
    static const QJsonObject d{
        {u"newFileLocation"_s, u"root"_s}, {u"newFileFolderPath"_s, QString()},   {u"defaultWheelBehavior"_s, u"pan"_s},
        {u"defaultModDragBehavior"_s, u"menu"_s}, {u"cardLabelVisibility"_s, u"always"_s}, {u"snapToGrid"_s, true},
        {u"snapToObjects"_s, true},               {u"zoomBreakpoint"_s, 0.0},
    };
    return d;
}

class CanvasSettingsTab : public PluginSettingsTab {
public:
    explicit CanvasSettingsTab(SettingsModal* modal)
        : PluginSettingsTab(modal, u"canvas"_s, u"canvas"_s, tr("Canvas"), u"layout-dashboard"_s)
    {
        build = [](PluginSettingsTab& tab) { return definitionsFor(tab); };
    }
    QJsonValue controlValue(const QString& key) const override
    {
        const QJsonValue v = PluginSettingsTab::controlValue(key);
        return v.isUndefined() || v.isNull() ? defaults().value(key) : v;
    }

private:
    static QList<SettingDef> definitionsFor(PluginSettingsTab& tab)
    {
        QPointer<PluginSettingsTab> self = &tab;
        QList<SettingDef> out;
        out << SettingDef::dropdown(tr("Default location for new canvas files"), tr("Where newly created canvas files are placed."),
                                    u"newFileLocation"_s,
                                    {{u"root"_s, tr("Vault folder")},
                                     {u"current"_s, tr("Same folder as current file")},
                                     {u"folder"_s, tr("In the folder specified below")}});
        out.last().control.afterChange = [self](const QJsonValue&) {
            if (self)
                self->refreshState();
        };
        SettingDef folder = SettingDef::folder(tr("Folder to create new canvas files in"), tr("Newly created canvas files will appear under this folder."),
                                               u"newFileFolderPath"_s, tr("Example: folder 1/folder 2"), true);
        folder.visible = [self] { return self && self->controlValue(u"newFileLocation"_s).toString() == u"folder"; };
        out << folder;
        out << SettingDef::dropdown(tr("Default mouse wheel behavior"), QString(), u"defaultWheelBehavior"_s,
                                    {{u"pan"_s, tr("Pan")}, {u"zoom"_s, tr("Zoom")}});
        out << SettingDef::dropdown(tr("Default Ctrl + Drag behavior"), QString(), u"defaultModDragBehavior"_s,
                                    {{u"menu"_s, tr("Show menu")},
                                     {u"card"_s, tr("Add card")},
                                     {u"note"_s, tr("Add note from vault")},
                                     {u"media"_s, tr("Add media from vault")},
                                     {u"webpage"_s, tr("Add web page")},
                                     {u"group"_s, tr("Create group")}});
        out << SettingDef::dropdown(tr("Show card names"), QString(), u"cardLabelVisibility"_s,
                                    {{u"always"_s, tr("Always")}, {u"hover"_s, tr("On hover")}, {u"never"_s, tr("Never")}});
        out << SettingDef::toggle(tr("Snap to grid"), tr("Snap cards to the background grid when moving and resizing."), u"snapToGrid"_s);
        out << SettingDef::toggle(tr("Snap to objects"), tr("Snap cards to nearby objects when moving and resizing."), u"snapToObjects"_s);
        out << SettingDef::slider(tr("Zoom threshold for hiding card content"),
                                  tr("Lower values will increase performance but hide card content sooner when zooming out."),
                                  u"zoomBreakpoint"_s, -0.7, 2.4, 0.01, 0);
        return out;
    }
};

} // namespace

PluginSettingsTab* createCanvasSettingsTab(SettingsModal& modal) { return new CanvasSettingsTab(&modal); }

} // namespace oc
