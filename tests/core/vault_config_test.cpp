// VaultConfig: Obsidian's getConfig/setConfig over app.json + appearance.json with defaults.
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/config/json_file.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

TEST(VaultConfig, DefaultsSplitFilesAndDefaultRemoval)
{
    QTemporaryDir dir;
    ConfigStore store(dir.path());
    VaultConfig cfg(&store);
    EXPECT_FALSE(cfg.alwaysUpdateLinks());
    EXPECT_TRUE(cfg.flag("livePreview"));
    EXPECT_EQ(cfg.string("defaultViewMode"), "source");
    EXPECT_EQ(cfg.tabSize(), 4);
    EXPECT_EQ(cfg.newLinkFormat(), LinkResolver::LinkFormat::Shortest);
    EXPECT_EQ(cfg.get("pdfExportSettings").toObject().value("pageSize").toString(), "Letter");
    EXPECT_EQ(cfg.number("baseFontSize"), 16);

    QSignalSpy spy(&cfg, &VaultConfig::configChanged);
    cfg.set("alwaysUpdateLinks", true);
    cfg.set("newLinkFormat", "relative");
    cfg.set("accentColor", "#ff0000");
    EXPECT_EQ(spy.count(), 3);
    EXPECT_TRUE(cfg.alwaysUpdateLinks());
    EXPECT_EQ(cfg.newLinkFormat(), LinkResolver::LinkFormat::Relative);
    store.flush();
    const QJsonObject app = JsonFile::read(dir.filePath("app.json"));
    const QJsonObject appearance = JsonFile::read(dir.filePath("appearance.json"));
    EXPECT_TRUE(app.value("alwaysUpdateLinks").toBool());
    EXPECT_FALSE(app.contains("accentColor"));
    EXPECT_EQ(appearance.value("accentColor").toString(), "#ff0000");
    // Back to the default: the key is removed again (Obsidian only stores changed options).
    cfg.set("alwaysUpdateLinks", false);
    store.flush();
    EXPECT_FALSE(JsonFile::read(dir.filePath("app.json")).contains("alwaysUpdateLinks"));
}
