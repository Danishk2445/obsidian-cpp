#pragma once
// A real vault session + main window + workspace (the recipe of Application::createServices and
// populateWindow, minus commands/hotkeys), on a temp vault with isolated XDG dirs.
#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <memory>

namespace oc::test {

inline bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

inline QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

class AppFixture : public ::testing::Test {
protected:
    // Subclasses write their notes in files() before SetUp runs the session.
    virtual QList<QPair<QString, QByteArray>> files() const = 0;

    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        root = tmp.path() + "/vault";
        QDir().mkpath(root);
        for (const auto& [path, bytes] : files())
            writeFile(root + "/" + path, bytes);
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = new Vault(root);
        docs = new DocumentManager(vault);
        meta = new MetadataCache;
        session->setVault(vault);
        session->setDocuments(docs);
        session->setMetadata(meta);
        fileManager = new FileManager(session.get());
        session->setFileManager(fileManager);
        bindVaultServices(session.get());
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 10000));
        window = new MainWindow(session.get(), false);
        registry = std::make_unique<ViewRegistry>();
        registerCoreViews(*registry);
        registerMarkdownView(*registry);
        session->setViewRegistry(registry.get());
        auto* ws = new Workspace(session.get(), registry.get(), window);
        session->setWorkspace(ws);
        window->setWorkspace(ws);
        window->resize(1200, 800);
        window->show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
    }
    void TearDown() override
    {
        delete window;
        delete fileManager;
        delete docs;
        delete meta;
        delete vault;
        session.reset();
    }
    MarkdownView* open(const QString& path, EditorView::Mode mode = EditorView::Mode::LivePreview)
    {
        Workspace* ws = window->workspace();
        ws->openFile(vault->file(path), Workspace::NewLeaf::No);
        auto* mv = qobject_cast<MarkdownView*>(ws->activeRootLeaf()->view());
        if (mv) {
            mv->setMode(mode);
            mv->editor()->setFocus();
        }
        return mv;
    }
    static QString lastLine(EditorView* e)
    {
        const Text& t = e->document()->text();
        return t.line(t.lineCount() - 1);
    }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    Vault* vault = nullptr;
    DocumentManager* docs = nullptr;
    MetadataCache* meta = nullptr;
    FileManager* fileManager = nullptr;
    std::unique_ptr<ViewRegistry> registry;
    MainWindow* window = nullptr;
};

} // namespace oc::test
