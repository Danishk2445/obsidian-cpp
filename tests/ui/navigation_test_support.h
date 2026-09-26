#pragma once

// A complete vault session for the navigation tests, wired like Application::createServices() and
// populateWindow(): Vault + MetadataCache + DocumentManager + FileManager + CommandRegistry over a scratch copy
// of tests/fixtures/vault-basic, a MainWindow with the workspace (core views, MarkdownView, SearchView) and the
// app, markdown and navigation commands. XDG dirs point into the temp dir; nothing touches a real vault.

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/navigation/navigation.h"
#include "ui/navigation/search_results_list.h"
#include "ui/navigation/search_view_widgets.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/app_commands.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_commands.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <memory>

namespace oc::test {

inline bool copyTree(const QString& from, const QString& to)
{
    QDir().mkpath(to);
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString rel = QDir(from).relativeFilePath(src);
        const QString dst = to + QLatin1Char('/') + rel;
        if (it.fileInfo().isDir()) {
            QDir().mkpath(dst);
        } else {
            QDir().mkpath(QFileInfo(dst).absolutePath());
            if (!QFile::copy(src, dst))
                return false;
        }
    }
    return true;
}

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

// The theme used by all navigation tests (one ThemeManager per process).
inline ThemeManager* testThemes()
{
    if (!ThemeManager::instance()) {
        static ThemeManager* tm = [] {
            auto* m = new ThemeManager(qApp);
            FontSettings fonts; // like --deterministic: Inter when installed
            fonts.interfaceFamily = Theme::firstInstalled({QStringLiteral("Inter")}, QString());
            fonts.textFamily = fonts.interfaceFamily;
            m->setFonts(fonts);
            m->setMode(ThemeManager::Mode::Dark);
            m->apply();
            return m;
        }();
        return tm;
    }
    return ThemeManager::instance();
}

class NavigationEnv {
public:
    // `prepare` may add/modify files in the scratch vault before it is loaded.
    explicit NavigationEnv(const std::function<void(const QString& root)>& prepare = {}, bool copyFixture = true, bool frameless = false)
    {
        EXPECT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        testThemes();
        root = tmp.path() + "/vault";
        if (copyFixture)
            EXPECT_TRUE(copyTree(QStringLiteral(OC_FIXTURES_DIR "/vault-basic"), root));
        else
            QDir().mkpath(root);
        if (prepare)
            prepare(root);

        session = new VaultSession(QStringLiteral("0123456789abcdef"), root);
        vault = new Vault(root);
        files = new FileManager(session, session);
        commands = new CommandRegistry(session);
        meta = new MetadataCache(session);
        docs = new DocumentManager(vault, session);
        vault->setParent(session);
        session->setVault(vault);
        session->setDocuments(docs);
        session->setMetadata(meta);
        session->setFileManager(files);
        session->setCommands(commands);
        bindVaultServices(session);
        vault->load();

        window = new MainWindow(session, frameless); // frameless: the default "hidden" frame (window buttons in the tab row)
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        registry = new ViewRegistry;
        registerCoreViews(*registry);
        registerMarkdownView(*registry);
        registerNavigationViews(*registry);
        session->setViewRegistry(registry);
        workspace = new Workspace(session, registry, window);
        session->setWorkspace(workspace);
        window->setWorkspace(workspace);
        registerAppCommands(*commands, window);
        registerMarkdownCommands(*commands, workspace);
        registerNavigationCommands(*commands, session, window->overlay());
        workspace->loadLayout();
        window->resize(1200, 800);
        window->show();
        QApplication::processEvents();
        EXPECT_TRUE(QTest::qWaitFor([&] { return meta->isReady(); }, 15000));
        QApplication::processEvents();
    }

    ~NavigationEnv()
    {
        for (SuggestModal* m : window->overlay()->findChildren<SuggestModal*>())
            m->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (docs)
            docs->saveAll();
        delete window; // destroys the workspace (views close while the session is alive)
        delete registry;
        delete session;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    OverlayLayer* layer() const { return window->overlay(); }
    TFile* file(const QString& path) const { return vault->file(path); }

    // The path of the file in the active root leaf ("" if none).
    QString activePath() const
    {
        WorkspaceLeaf* l = workspace->activeRootLeaf();
        auto* fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
        return fv && fv->file() ? fv->file()->path() : QString();
    }
    int rootLeafCount() const
    {
        int n = 0;
        workspace->iterateRootLeaves([&](WorkspaceLeaf*) { ++n; });
        return n;
    }
    MarkdownView* activeMarkdown() const
    {
        WorkspaceLeaf* l = workspace->activeRootLeaf();
        return l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
    }
    void openNote(const QString& path, Workspace::NewLeaf mode = Workspace::NewLeaf::No)
    {
        TFile* f = vault->file(path);
        ASSERT_NE(f, nullptr) << qPrintable(path);
        workspace->openFile(f, mode);
        QApplication::processEvents();
    }
    bool waitIdle(SearchView* v, int timeoutMs = 15000) const
    {
        return QTest::qWaitFor([v] { return v->isIdle(); }, timeoutMs);
    }

    QTemporaryDir tmp;
    QString root;
    VaultSession* session = nullptr;
    Vault* vault = nullptr;
    FileManager* files = nullptr;
    CommandRegistry* commands = nullptr;
    MetadataCache* meta = nullptr;
    DocumentManager* docs = nullptr;
    MainWindow* window = nullptr;
    ViewRegistry* registry = nullptr;
    Workspace* workspace = nullptr;
};

} // namespace oc::test
