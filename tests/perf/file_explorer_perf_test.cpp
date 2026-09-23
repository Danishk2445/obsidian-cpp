// Perf gate for the file explorer (label "perf", run with the perf preset): building the tree of a
// 10k-file vault and painting it for the first time must take < 150 ms; so must a repaint with every
// folder expanded, scrolled to the middle. Assertions only with an optimized build (NDEBUG) or
// OC_PERF_ASSERT=1; other runs just report the numbers.
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/views/file_explorer/file_tree.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QScrollBar>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

using namespace oc;

namespace {
bool assertPerf()
{
#ifdef NDEBUG
    return true;
#else
    return qEnvironmentVariableIntValue("OC_PERF_ASSERT") == 1;
#endif
}

bool touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("# note\n") == 7;
}

class ExplorerPerf : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        if (!ThemeManager::instance()) {
            static ThemeManager* tm = new ThemeManager(qApp);
            tm->setMode(ThemeManager::Mode::Dark);
            tm->apply();
        }
        root = tmp.path() + "/vault";
        // 100 folders (10 of them with 9 subfolders) and 10k files with realistic names.
        int files = 0;
        for (int d = 0; d < 10; ++d) {
            for (int s = 0; s < 10; ++s) {
                const QString dir = s == 0 ? root + QStringLiteral("/Area %1").arg(d) : root + QStringLiteral("/Area %1/Topic %2").arg(d).arg(s);
                for (int f = 0; f < 99; ++f, ++files)
                    ASSERT_TRUE(touch(dir + QStringLiteral("/Meeting notes %1 - review %2.md").arg(f).arg(d * 10 + s)));
            }
        }
        for (; files < 10000; ++files)
            ASSERT_TRUE(touch(root + QStringLiteral("/Inbox note %1.md").arg(files)));
        vault = std::make_unique<Vault>(root);
        vault->load();
        ASSERT_EQ(vault->fileCount(), 10000);
    }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<Vault> vault;
};
} // namespace

TEST_F(ExplorerPerf, BuildAndFirstPaint)
{
    qint64 best = 1'000'000, bestExpanded = 1'000'000;
    for (int run = 0; run < 3; ++run) {
        QElapsedTimer t;
        t.start();
        FileTree tree;
        tree.resize(300, 800);
        tree.setVault(vault.get());
        const QPixmap pm = tree.grab();
        best = std::min(best, t.elapsed());
        ASSERT_FALSE(pm.isNull());
        ASSERT_EQ(tree.rowCount(), 100 + 10); // Area folders + root files... (10 areas + 100 inbox notes)

        t.restart();
        tree.setAllCollapsed(false);
        tree.verticalScrollBar()->setValue(tree.verticalScrollBar()->maximum() / 2);
        const QPixmap pm2 = tree.grab();
        bestExpanded = std::min(bestExpanded, t.elapsed());
        ASSERT_EQ(tree.rowCount(), 10000 + 100);
    }
    std::printf("[perf] explorer tree 10k files: build + first paint %lld ms, expand all + scroll + paint %lld ms\n",
                static_cast<long long>(best), static_cast<long long>(bestExpanded));
    if (assertPerf()) {
        EXPECT_LT(best, 150);
        EXPECT_LT(bestExpanded, 150);
    }
}

TEST_F(ExplorerPerf, ViewInTheWorkspace)
{
    VaultSession session(QStringLiteral("0123456789abcdef"), root);
    session.setVault(vault.get());
    ViewRegistry registry;
    registerCoreViews(registry);
    registerFileExplorerView(registry);
    session.setViewRegistry(&registry);
    Workspace ws(&session, &registry, nullptr);
    session.setWorkspace(&ws);
    ws.loadLayout();
    QWidget* w = ws.widget();
    w->resize(1200, 800);
    // the default layout already made one: time a second one (a new leaf in the left sidedock)
    QElapsedTimer t;
    t.start();
    WorkspaceLeaf* leaf = ws.getLeaf(Workspace::NewLeaf::Tab);
    leaf->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("file-explorer")}});
    auto* view = qobject_cast<FileExplorerView*>(leaf->view());
    ASSERT_NE(view, nullptr);
    view->resize(300, 800);
    const QPixmap pm = view->grab();
    const qint64 ms = t.elapsed();
    ASSERT_FALSE(pm.isNull());
    std::printf("[perf] explorer view 10k files: create + first paint %lld ms\n", static_cast<long long>(ms));
    if (assertPerf()) {
        EXPECT_LT(ms, 150);
    }
    session.setWorkspace(nullptr);
}
