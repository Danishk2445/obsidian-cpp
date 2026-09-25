// Property types: Obsidian's inference, widget validation, the types.json manager (clone config dir,
// never the vault) and the vault-wide rename / delete of a property.
#include "core/config/config_store.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/property_types.h"
#include "core/properties/vault_properties.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;
using namespace oc::props;

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

// ---- inference and validation -----------------------------------------------------------------------

TEST(PropertyTypes, InferenceLikeObsidian)
{
    EXPECT_EQ(inferPropertyType(QVariant()), "text");
    EXPECT_EQ(inferPropertyType(QStringLiteral("hello")), "text");
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-15")), "date");
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-15T10:30")), "datetime");
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-15T10:30:00")), "text"); // Obsidian's regex has no seconds
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-19-39")), "date");          // the regex only checks digit ranges
    EXPECT_EQ(inferPropertyType(QStringLiteral("2024-01-45")), "text");
    EXPECT_EQ(inferPropertyType(3.5), "number");
    EXPECT_EQ(inferPropertyType(true), "checkbox");
    EXPECT_EQ(inferPropertyType(QVariantList{QStringLiteral("a"), QStringLiteral("b")}), "multitext");
    EXPECT_EQ(inferPropertyType(QVariantList{}), "multitext");
    EXPECT_EQ(inferPropertyType(QVariantList{QStringLiteral("a"), 1}), "unknown");
    EXPECT_EQ(inferPropertyType(QVariantMap{{QStringLiteral("a"), 1}}), "unknown");
}

TEST(PropertyTypes, Validation)
{
    EXPECT_TRUE(validatePropertyValue("text", QStringLiteral("x")));
    EXPECT_FALSE(validatePropertyValue("text", 1));
    EXPECT_TRUE(validatePropertyValue("multitext", QStringLiteral("x")));
    EXPECT_TRUE(validatePropertyValue("multitext", QVariantList{QStringLiteral("x"), QVariant()}));
    EXPECT_FALSE(validatePropertyValue("multitext", QVariantList{1}));
    EXPECT_TRUE(validatePropertyValue("tags", QVariantList{QStringLiteral("a")}));
    EXPECT_TRUE(validatePropertyValue("number", 2));
    EXPECT_FALSE(validatePropertyValue("number", QStringLiteral("2")));
    EXPECT_TRUE(validatePropertyValue("checkbox", false));
    EXPECT_FALSE(validatePropertyValue("checkbox", QStringLiteral("true")));
    EXPECT_TRUE(validatePropertyValue("date", QStringLiteral("2024-02-29")));
    EXPECT_TRUE(validatePropertyValue("date", QStringLiteral("")));
    EXPECT_TRUE(validatePropertyValue("date", QVariant()));
    EXPECT_FALSE(validatePropertyValue("date", QStringLiteral("hello")));
    EXPECT_FALSE(validatePropertyValue("date", QStringLiteral("2023-02-29")));
    EXPECT_TRUE(validatePropertyValue("datetime", QStringLiteral("2024-01-15T10:30:00")));
    EXPECT_TRUE(validatePropertyValue("unknown", QVariantMap{}));
}

TEST(PropertyTypes, WidgetsAndMenus)
{
    EXPECT_EQ(propertyType("multitext").name, "List");
    EXPECT_EQ(propertyType("datetime").icon, "clock");
    EXPECT_EQ(propertyType("nonsense").id, "unknown");
    QStringList ids;
    for (const PropertyType& t : menuPropertyTypes("status"))
        ids << t.id;
    EXPECT_EQ(ids, QStringList({"checkbox", "date", "datetime", "multitext", "number", "text"}));
    ids.clear();
    for (const PropertyType& t : menuPropertyTypes("tags"))
        ids << t.id;
    EXPECT_TRUE(ids.contains("tags"));
    EXPECT_FALSE(ids.contains("aliases"));
}

TEST(PropertyTypes, ValueHelpers)
{
    EXPECT_EQ(propertyValueList(QVariant()), QStringList());
    EXPECT_EQ(propertyValueList(QStringLiteral("")), QStringList());
    EXPECT_EQ(propertyValueList(QStringLiteral("a")), QStringList({"a"}));
    EXPECT_EQ(propertyValueList(QVariantList{QStringLiteral("a"), QVariant(), 2}), QStringList({"a", "2"}));
    EXPECT_FALSE(normalizePropertyValue(QStringLiteral("")).isValid());
    EXPECT_FALSE(normalizePropertyValue(QVariantList{}).isValid());
    EXPECT_EQ(normalizePropertyValue(QStringLiteral("x")).toString(), "x");
    EXPECT_EQ(propertyValueText(2.0), "2");
    EXPECT_EQ(propertyValueText(QVariantList{QStringLiteral("a"), QStringLiteral("b")}), "a,b");
    EXPECT_EQ(parseLenientDate(QStringLiteral("2024-1-5")), QDate(2024, 1, 5));
    EXPECT_FALSE(parseLenientDate(QStringLiteral("nope")).has_value());
    EXPECT_EQ(parseLenientDateTime(QStringLiteral("2024-01-05T07:08")), QDateTime(QDate(2024, 1, 5), QTime(7, 8)));
}

// ---- the manager ------------------------------------------------------------------------------------

TEST(PropertyTypeManagerTest, TypesJsonInTheConfigDir)
{
    QTemporaryDir tmp;
    ConfigStore config(tmp.path() + "/cfg");
    PropertyTypeManager m(&config, nullptr);
    EXPECT_EQ(m.assignedType("tags"), "tags");
    EXPECT_EQ(m.assignedType("Aliases"), "aliases");
    EXPECT_EQ(m.assignedType("cssclasses"), "multitext");
    EXPECT_EQ(m.assignedType("status"), "");

    QSignalSpy spy(&m, &PropertyTypeManager::changed);
    m.setType("Status", "multitext");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "status");
    EXPECT_EQ(m.assignedType("status"), "multitext");
    m.setType("tags", "text"); // reserved: ignored
    EXPECT_EQ(m.assignedType("tags"), "tags");
    config.flush();
    const QJsonObject saved = QJsonDocument::fromJson(readFile(tmp.path() + "/cfg/types.json")).object();
    EXPECT_EQ(saved.value("types").toObject().value("Status").toString(), "multitext");
    EXPECT_EQ(saved.value("types").toObject().value("tags").toString(), "tags");

    PropertyTypeManager again(&config, nullptr);
    EXPECT_EQ(again.assignedType("STATUS"), "multitext");
    m.unsetType("status");
    EXPECT_EQ(m.assignedType("status"), "");
    EXPECT_EQ(spy.count(), 2);
}

TEST(PropertyTypeManagerTest, TypeInfoLikeObsidian)
{
    QTemporaryDir tmp;
    ConfigStore config(tmp.path() + "/cfg");
    PropertyTypeManager m(&config, nullptr);
    auto info = m.typeInfo("count", 3);
    EXPECT_EQ(info.expected, "number");
    EXPECT_EQ(info.inferred, "number");
    m.setType("count", "text");
    info = m.typeInfo("count", 3); // assigned text, value is a number: mismatch warning
    EXPECT_EQ(info.expected, "text");
    EXPECT_EQ(info.inferred, "number");
    info = m.typeInfo("count", QVariant());
    EXPECT_EQ(info.expected, "text");
    EXPECT_EQ(info.inferred, "text");
    info = m.typeInfo("tags", QStringLiteral("single"));
    EXPECT_EQ(info.expected, "tags");
    EXPECT_EQ(info.inferred, "tags");
}

// ---- vault-wide operations -----------------------------------------------------------------------

class VaultPropertiesTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        root = tmp.path() + "/vault";
        writeFile(root + "/A.md", "---\r\nstatus: draft # c\r\ntags: [x]\r\n---\r\nA\r\n");
        writeFile(root + "/B.md", "---\nStatus: done\nstate:\n  - old\n---\nB\n");
        writeFile(root + "/C.md", "---\nstate:\n  - keep\nstatus:\n  - one\n---\n");
        writeFile(root + "/D.md", "No frontmatter\n");
        writeFile(root + "/E.md", "---\nstatus: only\n---\nE\n");
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = std::make_unique<Vault>(root);
        docs = std::make_unique<DocumentManager>(vault.get());
        meta = std::make_unique<MetadataCache>();
        session->setVault(vault.get());
        session->setDocuments(docs.get());
        session->setMetadata(meta.get());
        fm = std::make_unique<FileManager>(session.get());
        session->setFileManager(fm.get());
        bindVaultServices(session.get());
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 10000));
    }
    void TearDown() override
    {
        delete PropertyTypeManager::of(session.get());
        docs.reset();
        fm.reset();
        meta.reset();
        vault.reset();
        session.reset();
    }
    QByteArray read(const QString& path) { return readFile(root + "/" + path); }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    std::unique_ptr<Vault> vault;
    std::unique_ptr<DocumentManager> docs;
    std::unique_ptr<MetadataCache> meta;
    std::unique_ptr<FileManager> fm;
};

TEST_F(VaultPropertiesTest, IndexAndValueSuggestions)
{
    PropertyTypeManager* m = PropertyTypeManager::of(session.get());
    ASSERT_TRUE(m);
    EXPECT_EQ(m, PropertyTypeManager::of(session.get()));
    const auto& all = m->allProperties();
    ASSERT_TRUE(all.contains("status"));
    EXPECT_EQ(all.value("status").occurrences, 4);
    EXPECT_EQ(all.value("state").type, "multitext");
    EXPECT_EQ(m->valuesForKey("status"), QStringList({"draft", "one", "only"}));
    EXPECT_EQ(m->valuesForKey("state"), QStringList({"keep", "old"}));
}

TEST_F(VaultPropertiesTest, RenameInPlaceAndMerge)
{
    PropertyTypeManager::of(session.get())->setType("status", "text");
    const QByteArray d = read("D.md");
    EXPECT_EQ(renameProperty(session.get(), "status", "state"), 3); // A, C, E (B has "Status")
    EXPECT_EQ(read("A.md"), "---\r\nstate: draft # c\r\ntags: [x]\r\n---\r\nA\r\n");
    EXPECT_EQ(read("B.md"), "---\nStatus: done\nstate:\n  - old\n---\nB\n");
    EXPECT_EQ(read("C.md"), "---\nstate:\n  - keep\n  - one\n---\n");
    EXPECT_EQ(read("E.md"), "---\nstate: only\n---\nE\n");
    EXPECT_EQ(read("D.md"), d);
    EXPECT_EQ(PropertyTypeManager::of(session.get())->assignedType("state"), "text");
    EXPECT_EQ(PropertyTypeManager::of(session.get())->assignedType("status"), "");
}

TEST_F(VaultPropertiesTest, RenameGoesThroughOpenDocuments)
{
    Document* doc = docs->acquire("E.md");
    ASSERT_TRUE(doc);
    EXPECT_EQ(renameProperty(session.get(), "status", "phase"), 3);
    EXPECT_EQ(doc->text().toString(), "---\nphase: only\n---\nE\n");
    EXPECT_TRUE(doc->history().canUndo());
    EXPECT_EQ(read("E.md"), "---\nphase: only\n---\nE\n");
    docs->release(doc);
}

TEST_F(VaultPropertiesTest, DeleteIsCaseInsensitive)
{
    EXPECT_EQ(deleteProperty(session.get(), "status"), 4);
    EXPECT_EQ(read("A.md"), "---\r\ntags: [x]\r\n---\r\nA\r\n");
    EXPECT_EQ(read("B.md"), "---\nstate:\n  - old\n---\nB\n");
    EXPECT_EQ(read("C.md"), "---\nstate:\n  - keep\n---\n");
    EXPECT_EQ(read("E.md"), "E\n"); // the last property: the block goes
    EXPECT_EQ(read("D.md"), "No frontmatter\n");
}
