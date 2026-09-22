#include "core/config/clone_paths.h"
#include "core/config/config_store.h"
#include "core/config/vault_registry.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace oc;

class RegistryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (m_tmp.path() + "/cache").toUtf8());
    }
    QTemporaryDir m_tmp;
};

TEST_F(RegistryTest, PathsLiveUnderXdgDirs)
{
    EXPECT_TRUE(ClonePaths::registryFile().startsWith(m_tmp.path() + "/config/obsidian-cpp/"));
    EXPECT_EQ(ClonePaths::vaultConfigDir("abc"), m_tmp.path() + "/config/obsidian-cpp/vaults/abc");
    EXPECT_EQ(ClonePaths::vaultDataDir("abc"), m_tmp.path() + "/data/obsidian-cpp/abc");
}

TEST_F(RegistryTest, TouchSaveLoadRoundTrip)
{
    const QString vaultDir = m_tmp.path() + "/My Vault";
    QDir().mkpath(vaultDir);
    QString id;
    {
        VaultRegistry reg;
        reg.load();
        id = reg.touch(vaultDir + "/");
        EXPECT_EQ(id.size(), 16);
        reg.setOption("frame", "native");
        ASSERT_TRUE(reg.save());
        EXPECT_EQ(reg.touch(vaultDir), id); // same folder -> same id
    }
    VaultRegistry reg2;
    reg2.load();
    ASSERT_EQ(reg2.vaults().size(), 1);
    EXPECT_EQ(reg2.vaults().first().id, id);
    EXPECT_EQ(reg2.vaults().first().path, vaultDir);
    EXPECT_TRUE(reg2.vaults().first().open);
    EXPECT_EQ(reg2.option("frame").toString(), "native");
    reg2.remove(id);
    EXPECT_TRUE(reg2.vaults().isEmpty());
}

TEST_F(RegistryTest, ObsidianRegistryIsReadOnlySuggestion)
{
    QDir().mkpath(m_tmp.path() + "/config/obsidian");
    QFile f(m_tmp.path() + "/config/obsidian/obsidian.json");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({"vaults":{"28a10709c601a79e":{"path":"/some/where/notes","ts":1763731873015,"open":true}}})");
    f.close();
    const QByteArray before = [&] { QFile r(f.fileName()); return r.open(QIODevice::ReadOnly) ? r.readAll() : QByteArray(); }();
    const auto v = VaultRegistry::obsidianVaults();
    ASSERT_EQ(v.size(), 1);
    EXPECT_EQ(v.first().path, "/some/where/notes");
    EXPECT_EQ(v.first().name(), "notes");
    QFile r(f.fileName());
    ASSERT_TRUE(r.open(QIODevice::ReadOnly));
    EXPECT_EQ(r.readAll(), before);
}

TEST_F(RegistryTest, ConfigStoreWritesOnlyChangesAndTwoSpaceJson)
{
    const QString dir = ClonePaths::vaultConfigDir("0123456789abcdef");
    {
        ConfigStore cs(dir);
        cs.setValue("app", "alwaysUpdateLinks", true);
        cs.setValue("app", "tabSize", 4);
        cs.flush();
    }
    QFile f(dir + "/app.json");
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray text = f.readAll();
    EXPECT_TRUE(text.contains("\n  \"alwaysUpdateLinks\": true"));
    ConfigStore cs2(dir);
    EXPECT_TRUE(cs2.value("app", "alwaysUpdateLinks").toBool());
    EXPECT_EQ(cs2.value("app", "tabSize").toInt(), 4);
    EXPECT_EQ(cs2.value("app", "missing", 7).toInt(), 7);
}
