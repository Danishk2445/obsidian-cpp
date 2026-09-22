#include "core/config/vault_config.h"

#include "core/config/config_store.h"

#include <QJsonArray>
#include <QSet>

namespace oc {

namespace {
QJsonObject makeDefaults()
{
    // Obsidian 1.13 vault config defaults (mobile-only options omitted).
    return QJsonObject{
        {"alwaysUpdateLinks", false},
        {"spellcheck", true},
        {"spellcheckLanguages", QJsonValue::Null},
        {"readableLineLength", true},
        {"strictLineBreaks", false},
        {"propertiesInDocument", "visible"},
        {"showInlineTitle", true},
        {"showViewHeader", true},
        {"showRibbon", true},
        {"useMarkdownLinks", false},
        {"showUnsupportedFiles", false},
        {"autoPairBrackets", true},
        {"autoPairMarkdown", true},
        {"smartIndentList", true},
        {"foldHeading", true},
        {"foldIndent", true},
        {"showLineNumber", false},
        {"showIndentGuide", true},
        {"useTab", true},
        {"tabSize", 4},
        {"rightToLeft", false},
        {"autoConvertHtml", true},
        {"vimMode", false},
        {"livePreview", true},
        {"nativeMenus", QJsonValue::Null},
        {"attachmentFolderPath", "/"},
        {"newFileLocation", "root"},
        {"newFileFolderPath", "/"},
        {"newLinkFormat", "shortest"},
        {"userIgnoreFilters", QJsonValue::Null},
        {"focusNewTab", true},
        {"defaultViewMode", "source"},
        {"promptDelete", true},
        {"trashOption", "system"},
        {"deleteUnlinkedAttachments", "ask"},
        {"openBehavior", ""},
        {"pdfExportSettings", QJsonObject{{"pageSize", "Letter"}, {"landscape", false}, {"margin", "0"}, {"downscalePercent", 100}}},
        {"uriCallbacks", false},
        {"settingsPopoutWindow", true},
        // appearance.json
        {"theme", "system"},
        {"accentColor", ""},
        {"cssTheme", ""},
        {"enabledCssSnippets", QJsonArray()},
        {"translucency", false},
        {"textFontFamily", ""},
        {"interfaceFontFamily", ""},
        {"monospaceFontFamily", ""},
        {"baseFontSize", 16},
        {"baseFontSizeAction", false},
    };
}
} // namespace

VaultConfig::VaultConfig(ConfigStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    connect(store, &ConfigStore::changed, this, [this](const QString& file, const QString& key) {
        if (file == u"app" || file == u"appearance")
            emit configChanged(key);
    });
}

const QJsonObject& VaultConfig::defaults()
{
    static const QJsonObject d = makeDefaults();
    return d;
}

bool VaultConfig::isAppearanceKey(const QString& key)
{
    static const QSet<QString> keys{"accentColor", "theme", "cssTheme", "enabledCssSnippets", "showViewHeader", "showRibbon",
                                    "nativeMenus", "translucency", "textFontFamily", "interfaceFontFamily",
                                    "monospaceFontFamily", "baseFontSize", "baseFontSizeAction", "slidingSidebar",
                                    "floatingNavigation", "autoFullScreen"};
    return keys.contains(key);
}

QJsonValue VaultConfig::get(const QString& key) const
{
    const QString file = isAppearanceKey(key) ? QStringLiteral("appearance") : QStringLiteral("app");
    const QJsonObject obj = m_store->object(file);
    if (auto it = obj.constFind(key); it != obj.constEnd())
        return *it;
    return defaults().value(key);
}

void VaultConfig::set(const QString& key, const QJsonValue& value)
{
    const QString file = isAppearanceKey(key) ? QStringLiteral("appearance") : QStringLiteral("app");
    const bool isDefault = defaults().contains(key) && defaults().value(key) == value;
    m_store->setValue(file, key, isDefault ? QJsonValue(QJsonValue::Undefined) : value);
}

LinkResolver::LinkFormat VaultConfig::newLinkFormat() const
{
    const QString f = string(QStringLiteral("newLinkFormat"));
    if (f == u"relative")
        return LinkResolver::LinkFormat::Relative;
    if (f == u"absolute")
        return LinkResolver::LinkFormat::Absolute;
    return LinkResolver::LinkFormat::Shortest;
}

} // namespace oc
