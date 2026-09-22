// Page preview popovers: hovering a link in Reading view (no Mod), Mod in the editor, section and
// block embeds, nested popovers, hiding when the mouse leaves, Escape.
#include "core/config/config_store.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/subpath.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "editor/live_preview_decorator.h"
#include "markdown/metadata.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/markdown/page_preview.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDir>
#include <QFile>
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
} // namespace

TEST(Subpath, Resolve)
{
    md::CachedMetadata m;
    auto heading = [&](const char* text, int level, int line) {
        md::HeadingCache h;
        h.heading = QString::fromUtf8(text);
        h.level = level;
        h.position.start.line = h.position.end.line = line;
        m.headings.append(h);
    };
    heading("Intro", 1, 0);
    heading("Part: one", 2, 3);
    heading("Detail", 3, 6);
    heading("Part two", 2, 9);
    heading("Detail", 3, 12);
    auto r = resolveSubpath(m, "#Part: one");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->startLine, 3);
    EXPECT_EQ(r->endLine, 8); // up to the next heading of the same level
    r = resolveSubpath(m, "#part  one"); // normalized, case-insensitive
    ASSERT_TRUE(r);
    EXPECT_EQ(r->startLine, 3);
    r = resolveSubpath(m, "#Part two#Detail"); // nested
    ASSERT_TRUE(r);
    EXPECT_EQ(r->startLine, 12);
    EXPECT_EQ(r->endLine, -1);
    r = resolveSubpath(m, "#Intro");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->endLine, -1);
    EXPECT_FALSE(resolveSubpath(m, "#Nope"));
    md::BlockCache b;
    b.id = "Abc";
    b.position.start.line = 4;
    b.position.end.line = 5;
    m.blocks.insert("abc", b);
    r = resolveSubpath(m, "#^ABC");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->type, SubpathRange::Type::Block);
    EXPECT_EQ(r->startLine, 4);
    EXPECT_EQ(r->endLine, 5);
}

class PagePreviewTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        root = tmp.path() + "/vault";
        writeFile(root + "/Welcome.md", "Read [[Other#Sub]] now.\n\nAnd [[Other]] too.\n");
        writeFile(root + "/Other.md", "# Head\ntext\n## Sub\nsub text [[Third]]\nmore\n# Next\nafter\n");
        writeFile(root + "/Third.md", "third note\n");
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = new Vault(root);
        docs = new DocumentManager(vault);
        meta = new MetadataCache;
        session->setVault(vault);
        session->setDocuments(docs);
        session->setMetadata(meta);
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
        preview = PagePreview::of(window);
        preview->setDelays(0, 50);
    }
    void TearDown() override
    {
        delete window;
        delete docs;
        delete meta;
        delete vault;
        session.reset();
    }
    MarkdownView* open(const QString& path, EditorView::Mode mode)
    {
        Workspace* ws = window->workspace();
        ws->openFile(vault->file(path), Workspace::NewLeaf::No);
        auto* mv = qobject_cast<MarkdownView*>(ws->activeRootLeaf()->view());
        if (mv)
            mv->setMode(mode);
        QTest::qWait(20);
        return mv;
    }
    // viewport point in the middle of the first occurrence of `text` (+ offset chars)
    static QPoint pointAt(EditorView* e, const QString& text, int offset = 3)
    {
        const qsizetype at = e->document()->text().toString().indexOf(text);
        const QRectF r = e->caretRectAt(at + offset);
        return r.center().toPoint();
    }
    static bool linksParsed(EditorView* e, const QString& text)
    {
        const qsizetype at = e->document()->text().toString().indexOf(text);
        return e->livePreview() && e->livePreview()->linkAt(at + 3).has_value();
    }
    static void hover(EditorView* e, const QPoint& vp)
    {
        QCursor::setPos(e->viewport()->mapToGlobal(vp));
        QTest::mouseMove(e->viewport(), vp);
    }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    Vault* vault = nullptr;
    DocumentManager* docs = nullptr;
    MetadataCache* meta = nullptr;
    std::unique_ptr<ViewRegistry> registry;
    MainWindow* window = nullptr;
    PagePreview* preview = nullptr;
};

TEST_F(PagePreviewTest, ReadingViewHoverShowsSection)
{
    MarkdownView* mv = open("Welcome.md", EditorView::Mode::Reading);
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    if (!linksParsed(e, "[[Other"))
        GTEST_SKIP() << "the interim inline parser finds no links";
    EXPECT_FALSE(preview->requiresMod("preview"));
    EXPECT_TRUE(preview->requiresMod("editor"));
    hover(e, pointAt(e, "Other#Sub"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().size() == 1; }, 2000));
    HoverPopover* p = preview->popovers().first();
    EXPECT_EQ(p->file()->path(), "Other.md");
    EXPECT_EQ(p->subpath(), "#Sub");
    ASSERT_TRUE(p->editor());
    EXPECT_EQ(p->editor()->mode(), EditorView::Mode::Reading);
    EXPECT_EQ(p->editor()->visibleLineRange(), qMakePair(2, 4)); // "## Sub" .. "more"
    EXPECT_TRUE(p->isVisible());
    EXPECT_LE(p->height(), 402);
    EXPECT_EQ(p->width(), 452);
    if (const QByteArray dir = qgetenv("OC_SHOT_DIR"); !dir.isEmpty())
        window->grab().save(QString::fromLocal8Bit(dir) + "/page_preview.png");
    // leaving the link and the popover hides it
    hover(e, QPoint(5, e->viewport()->height() - 5));
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().isEmpty(); }, 2000));
}

TEST_F(PagePreviewTest, EditorNeedsMod)
{
    MarkdownView* mv = open("Welcome.md", EditorView::Mode::Source);
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    if (!linksParsed(e, "[[Other"))
        GTEST_SKIP() << "the interim inline parser finds no links";
    hover(e, pointAt(e, "[[Other]]", 4));
    QTest::qWait(60);
    EXPECT_TRUE(preview->popovers().isEmpty()); // no Mod: nothing
    QTest::keyPress(e, Qt::Key_Control);
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().size() == 1; }, 2000));
    QTest::keyRelease(e, Qt::Key_Control);
    HoverPopover* p = preview->popovers().first();
    EXPECT_EQ(p->file()->path(), "Other.md");
    EXPECT_EQ(p->editor()->visibleLineRange(), qMakePair(0, -1)); // the whole note
    // Escape closes
    QTest::keyClick(e, Qt::Key_Escape);
    EXPECT_TRUE(preview->popovers().isEmpty());
}

TEST_F(PagePreviewTest, NestedPopovers)
{
    MarkdownView* mv = open("Welcome.md", EditorView::Mode::Reading);
    ASSERT_TRUE(mv);
    EditorView* e = mv->editor();
    if (!linksParsed(e, "[[Other"))
        GTEST_SKIP() << "the interim inline parser finds no links";
    hover(e, pointAt(e, "Other#Sub"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().size() == 1; }, 2000));
    HoverPopover* outer = preview->popovers().first();
    EditorView* pe = outer->editor();
    ASSERT_TRUE(QTest::qWaitFor([&] { return pe->isVisible(); }, 1000));
    QTest::qWait(20);
    hover(pe, pointAt(pe, "[[Third]]", 4));
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->popovers().size() == 2; }, 2000));
    EXPECT_EQ(preview->popovers().last()->file()->path(), "Third.md");
    EXPECT_EQ(preview->popovers().first(), outer); // the outer one stays
    // clicking elsewhere closes everything
    QTest::mouseClick(e->viewport(), Qt::LeftButton, {}, QPoint(5, 5));
    EXPECT_TRUE(preview->popovers().isEmpty());
}
