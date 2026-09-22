// Perf gate (label "perf", run with the perf preset): the initial scan of a 10k-file vault,
// including inotify watch setup, must take < 300 ms.

#include "core/vault/vault.h"
#include "core/vault/vault_private.h"
#include "vault_test_util.h"

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

using namespace oc;

TEST(VaultPerf, LoadTenThousandFiles)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path() + QStringLiteral("/perf vault");
    // 500 folders up to 7 levels deep, 20 files each; plus ignored noise that must be skipped.
    int files = 0;
    for (int top = 0; top < 25; ++top) {
        QString dir;
        for (int depth = 0; depth < 20; ++depth) {
            if (depth % 7 == 0)
                dir = root + QStringLiteral("/Area %1").arg(top);
            dir += QStringLiteral("/level %1").arg(depth);
            for (int f = 0; f < 20; ++f, ++files)
                ASSERT_TRUE(vt::writeFile(dir + QStringLiteral("/note %1.md").arg(f), "# note\n"));
        }
    }
    ASSERT_TRUE(vt::writeFile(root + QStringLiteral("/.git/objects/pack/p.pack"), "x"));
    ASSERT_EQ(files, 10000);

    for (const char* backend : {"inotify", "none"}) {
        qint64 best = 1'000'000;
        for (int run = 0; run < 3; ++run) {
            Vault v(root);
            VaultImplAccess::tuning(v).watcher = QString::fromUtf8(backend);
            QElapsedTimer t;
            t.start();
            v.load();
            best = std::min(best, t.elapsed());
            ASSERT_EQ(v.fileCount(), 10000);
        }
        std::printf("[perf] load 10k files (%s): %lld ms\n", backend, static_cast<long long>(best));
        EXPECT_LT(best, 300) << backend;
    }
}
