// FileManager over a real Vault (temp dir): rename with link updates, new-file placement, trash
// options, link generation. Also exercises bindVaultServices (metadata/document wiring).
#include "core/config/vault_config.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;

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

class FileManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        root = tmp.path() + "/vault";
        writeFile(root + "/Welcome.md", "See [[Other#Head|alias]] and [[Other]].\n[md](Other.md)\n");
        writeFile(root + "/Other.md", "# Head\n");
        writeFile(root + "/Folder/Note.md", "[[Other]]\n");
        writeFile(root + "/img.png", "png");
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = std::make_unique<Vault>(root);
        docs = std::make_unique<DocumentManager>(vault.get());
        meta = std::make_unique<MetadataCache>();
        session->setVault(vault.get());
        session->setDocuments(docs.get());
        session->setMetadata(meta.get());
        bindVaultServices(session.get());
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 10000));
        fm = std::make_unique<FileManager>(session.get());
    }
    void TearDown() override
    {
        fm.reset();
        docs.reset();
        meta.reset();
        vault.reset();
        session.reset();
    }
    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    std::unique_ptr<Vault> vault;
    std::unique_ptr<DocumentManager> docs;
    std::unique_ptr<MetadataCache> meta;
    std::unique_ptr<FileManager> fm;
};

TEST_F(FileManagerTest, IndexesTheVaultThroughTheBindings)
{
    const md::CachedMetadata* welcome = meta->fileCache("Welcome.md");
    ASSERT_TRUE(welcome != nullptr);
    ASSERT_EQ(welcome->links.size(), 3); // [[Other#Head|alias]], [[Other]], [md](Other.md)
    EXPECT_EQ(welcome->links[0].link, "Other#Head");
    EXPECT_EQ(welcome->links[0].displayText, "alias");
    EXPECT_TRUE(welcome->links[2].markdown);
    EXPECT_TRUE(meta->fileCache("Folder/Note.md") != nullptr);
    EXPECT_TRUE(meta->resolver().contains("img.png"));
}

TEST_F(FileManagerTest, RenameUpdatesLinksWhenEnabled)
{
    session->vaultConfig()->set("alwaysUpdateLinks", true);
    QSignalSpy updated(fm.get(), &FileManager::linksUpdated);
    QString err;
    ASSERT_TRUE(fm->renameFile(vault->file("Other.md"), "Renamed.md", &err)) << err.toStdString();
    EXPECT_TRUE(QFile::exists(root + "/Renamed.md"));
    EXPECT_FALSE(QFile::exists(root + "/Other.md"));
    EXPECT_EQ(readFile(root + "/Welcome.md"), QByteArray("See [[Renamed#Head|alias]] and [[Renamed]].\n[md](Renamed.md)\n"));
    EXPECT_EQ(readFile(root + "/Folder/Note.md"), QByteArray("[[Renamed]]\n"));
    ASSERT_EQ(updated.count(), 1);
    EXPECT_EQ(updated.at(0).at(0).toInt(), 4);
    EXPECT_EQ(updated.at(0).at(1).toInt(), 2);
}

TEST_F(FileManagerTest, RenameAsksWhenNotAlwaysUpdating)
{
    int asked = 0;
    fm->setConfirmLinkUpdate([&](int, int) {
        ++asked;
        return FileManager::LinkUpdateChoice::DoNotUpdate;
    });
    ASSERT_TRUE(fm->renameFile(vault->file("Other.md"), "Renamed.md"));
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(readFile(root + "/Welcome.md"), QByteArray("See [[Other#Head|alias]] and [[Other]].\n[md](Other.md)\n"));
    // renaming it back needs no edits (the links resolve again): no question
    ASSERT_TRUE(fm->renameFile(vault->file("Renamed.md"), "Other.md"));
    EXPECT_EQ(asked, 1);
    // "Always update" flips the setting and updates this time
    fm->setConfirmLinkUpdate([&](int, int) {
        ++asked;
        return FileManager::LinkUpdateChoice::AlwaysUpdate;
    });
    ASSERT_TRUE(fm->renameFile(vault->file("Other.md"), "Third.md"));
    EXPECT_EQ(asked, 2);
    EXPECT_TRUE(session->vaultConfig()->alwaysUpdateLinks());
    EXPECT_EQ(readFile(root + "/Folder/Note.md"), QByteArray("[[Third]]\n"));
}

TEST_F(FileManagerTest, OpenDocumentsGetUndoableLinkEdits)
{
    session->vaultConfig()->set("alwaysUpdateLinks", true);
    Document* doc = docs->acquire("Welcome.md");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(fm->renameFile(vault->file("Other.md"), "Renamed.md"));
    EXPECT_EQ(doc->text().toString(), "See [[Renamed#Head|alias]] and [[Renamed]].\n[md](Renamed.md)\n");
    EXPECT_FALSE(doc->isDirty()); // saved right away
    EXPECT_TRUE(doc->history().canUndo());
    docs->release(doc);
}

TEST_F(FileManagerTest, FolderRenameMovesOpenDocuments)
{
    Document* doc = docs->acquire("Folder/Note.md");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(fm->renameFile(vault->folder("Folder"), "Moved"));
    EXPECT_EQ(doc->path(), "Moved/Note.md");
    EXPECT_TRUE(meta->fileCache("Moved/Note.md") != nullptr);
    EXPECT_TRUE(meta->fileCache("Folder/Note.md") == nullptr);
    docs->release(doc);
}

TEST_F(FileManagerTest, NewFilePlacementAndCreation)
{
    EXPECT_EQ(fm->newFileParent("Folder/Note.md"), vault->rootFolder());
    session->vaultConfig()->set("newFileLocation", "current");
    EXPECT_EQ(fm->newFileParent("Folder/Note.md"), vault->folder("Folder"));
    session->vaultConfig()->set("newFileLocation", "folder");
    session->vaultConfig()->set("newFileFolderPath", "Inbox");
    EXPECT_EQ(fm->newFileParent("Folder/Note.md"), vault->rootFolder()); // missing folder -> root
    ASSERT_NE(vault->createFolder("Inbox"), nullptr);
    EXPECT_EQ(fm->newFileParent("x.md"), vault->folder("Inbox"));
    TFile* a = fm->createNewMarkdownFile(vault->folder("Inbox"));
    TFile* b = fm->createNewMarkdownFile(vault->folder("Inbox"));
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->path(), "Inbox/Untitled.md");
    EXPECT_EQ(b->path(), "Inbox/Untitled 1.md");
}

TEST_F(FileManagerTest, TrashOptions)
{
    session->vaultConfig()->set("trashOption", "local");
    ASSERT_TRUE(fm->trashFile(vault->file("img.png")));
    EXPECT_TRUE(QFile::exists(root + "/.trash/img.png"));
    session->vaultConfig()->set("trashOption", "none");
    ASSERT_TRUE(fm->trashFile(vault->file("Other.md")));
    EXPECT_FALSE(QFile::exists(root + "/Other.md"));
    EXPECT_FALSE(QFile::exists(root + "/.trash/Other.md"));
}

TEST_F(FileManagerTest, GenerateMarkdownLink)
{
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Welcome.md"), "[[Other]]");
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Welcome.md", "#Head"), "[[Other#Head]]");
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Welcome.md", {}, "Alias"), "[[Other|Alias]]");
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Welcome.md", {}, "other"), "[[other]]"); // alias == link text
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Other.md", "#Head"), "[[#Head]]");
    EXPECT_EQ(fm->generateMarkdownLink("img.png", "Welcome.md"), "[[img.png]]");
    session->vaultConfig()->set("useMarkdownLinks", true);
    EXPECT_EQ(fm->generateMarkdownLink("Other.md", "Welcome.md"), "[Other](Other.md)");
    EXPECT_EQ(fm->generateMarkdownLink("Folder/Note.md", "Welcome.md", "#A B"), "[Note](Note.md#A%20B)");
    EXPECT_EQ(fm->generateMarkdownLink("img.png", "Welcome.md"), "[](img.png)");
}
