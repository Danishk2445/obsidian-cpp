#pragma once

// A real session for the file explorer tests: a scratch copy of a vault (never a real one), the
// session services wired like the Application does, a MainWindow with the default workspace layout
// (the explorer in the left sidedock), isolated XDG dirs and stubbed desktop integration.

#include "core/commands/command_registry.h"
#include "core/config/vault_config.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/file_explorer/file_tree.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <functional>
#include <ostream>

// Readable QString / QStringList values in gtest failure messages.
inline void PrintTo(const QString& s, std::ostream* os) { *os << '"' << s.toStdString() << '"'; }

namespace oc::test {

inline bool copyTree(const QString& from, const QString& to)
{
    QDir().mkpath(to);
    QDirIterator it(from, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
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

inline void sendMouse(QWidget* w, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                      Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, mods);
    QApplication::sendEvent(w, &ev);
}

inline void clickAt(QWidget* w, QPoint pos, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    sendMouse(w, QEvent::MouseButtonPress, pos, button, button, mods);
    sendMouse(w, QEvent::MouseButtonRelease, pos, button, Qt::NoButton, mods);
}

template <typename T> T* findOpen(QWidget* root)
{
    for (T* m : root->findChildren<T*>())
        if (m->isVisible())
            return m;
    return nullptr;
}

class ExplorerFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (m_tmp.path() + "/cache").toUtf8());
        if (!ThemeManager::instance()) {
            static ThemeManager* tm = new ThemeManager(qApp);
            tm->setMode(ThemeManager::Mode::Dark);
            tm->apply();
        }
        m_root = m_tmp.path() + "/vault";
        ASSERT_TRUE(copyTree(QStringLiteral(OC_FIXTURES_DIR "/vault-basic"), m_root));
        prepareVault(m_root);
        createSession();
        opened.clear();
        shown.clear();
        desktopHandlers().openUrl = [this](const QUrl& u) { opened << u; };
        desktopHandlers().showInFolder = [this](const QString& p) { shown << p; };
    }
    void TearDown() override { destroySession(); }

    // Extra files before the vault is scanned.
    virtual void prepareVault(const QString& root) { Q_UNUSED(root); }

    void createSession()
    {
        m_session = new VaultSession(QStringLiteral("0123456789abcdef"), m_root);
        m_session->vaultConfig()->set(QStringLiteral("trashOption"), QStringLiteral("local")); // never the system trash
        m_vault = new Vault(m_root);
        m_docs = new DocumentManager(m_vault);
        m_meta = new MetadataCache;
        m_fm = new FileManager(m_session);
        m_commands = new CommandRegistry;
        m_session->setVault(m_vault);
        m_session->setDocuments(m_docs);
        m_session->setMetadata(m_meta);
        m_session->setFileManager(m_fm);
        m_session->setCommands(m_commands);
        bindVaultServices(m_session);
        m_vault->load();
        m_registry = new ViewRegistry;
        registerCoreViews(*m_registry);
        registerMarkdownView(*m_registry);
        registerFileExplorerView(*m_registry);
        m_session->setViewRegistry(m_registry);
        m_window = new MainWindow(m_session, false);
        m_window->setAttribute(Qt::WA_DeleteOnClose, false);
        m_ws = new Workspace(m_session, m_registry, m_window);
        m_session->setWorkspace(m_ws);
        m_window->setWorkspace(m_ws);
        m_ws->loadLayout();
        registerFileExplorerCommands(*m_commands, m_session);
        m_window->resize(1200, 800);
        m_window->show();
        m_window->activateWindow();
        (void)QTest::qWaitForWindowActive(m_window, 2000);
        QApplication::processEvents();
    }

    void destroySession()
    {
        delete m_window; // closes the views first
        m_window = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        delete m_commands;
        delete m_fm;
        delete m_meta;
        delete m_docs;
        delete m_vault;
        delete m_registry;
        delete m_session;
        m_commands = nullptr;
        m_fm = nullptr;
        m_meta = nullptr;
        m_docs = nullptr;
        m_vault = nullptr;
        m_registry = nullptr;
        m_session = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    FileExplorerView* explorer() const
    {
        WorkspaceLeaf* l = m_ws->leavesOfType(QStringLiteral("file-explorer")).value(0);
        return l ? qobject_cast<FileExplorerView*>(l->view()) : nullptr;
    }
    FileTree* tree() const { return explorer()->tree(); }
    QWidget* vp() const { return tree()->viewport(); }
    QPoint rowCenter(const QString& path) const
    {
        const int row = tree()->rowOf(m_vault->item(path));
        EXPECT_GE(row, 0) << qPrintable(path);
        return tree()->rowRect(row).center();
    }
    void click(const QString& path, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        clickAt(vp(), rowCenter(path), b, mods);
        QApplication::processEvents();
    }
    void key(int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QWidget* target = QApplication::focusWidget() ? QApplication::focusWidget() : tree();
        QTest::keyClick(target, Qt::Key(k), mods);
        QApplication::processEvents();
    }
    bool exists(const QString& rel) const { return QFileInfo::exists(m_root + "/" + rel); }
    Modal* openModal() const { return findOpen<Modal>(m_window->overlay()); }
    SuggestModal* openSuggest() const { return findOpen<SuggestModal>(m_window->overlay()); }
    TFile* activeRootFile() const
    {
        WorkspaceLeaf* l = m_ws->activeRootLeaf();
        auto* fv = l ? qobject_cast<FileView*>(l->view()) : nullptr;
        return fv ? fv->file() : nullptr;
    }
    int rootLeafCount() const
    {
        int n = 0;
        m_ws->iterateRootLeaves([&](WorkspaceLeaf*) { ++n; });
        return n;
    }

    QTemporaryDir m_tmp;
    QString m_root;
    VaultSession* m_session = nullptr;
    Vault* m_vault = nullptr;
    DocumentManager* m_docs = nullptr;
    MetadataCache* m_meta = nullptr;
    FileManager* m_fm = nullptr;
    CommandRegistry* m_commands = nullptr;
    ViewRegistry* m_registry = nullptr;
    MainWindow* m_window = nullptr;
    Workspace* m_ws = nullptr;
    QList<QUrl> opened;
    QStringList shown;
};

} // namespace oc::test
