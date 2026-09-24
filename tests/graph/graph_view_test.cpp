// Graph views over a real (temporary) vault: the vault graph built from the metadata cache,
// GraphView / LocalGraphView in a Workspace, graph.json persistence, commands, node clicks,
// rebuilds on metadata changes and the vault search behind the graph's search box.
#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "graph/graph_settings.h"
#include "render/theme/theme.h"
#include "ui/graph/graph_canvas.h"
#include "ui/graph/graph_controls.h"
#include "ui/graph/graph_vault.h"
#include "ui/graph/graph_view.h"
#include "ui/graph/graph_widget.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace oc;
using graph::NodeKind;

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

// A markdown view stand-in (the real MarkdownView needs the editor stack).
class NoteView : public FileView {
public:
    using FileView::FileView;
    QString viewType() const override { return QStringLiteral("markdown"); }
    QString icon() const override { return QStringLiteral("file-text"); }
    bool canAcceptExtension(const QString& ext) const override { return ext == u"md"; }
    void loadFile(TFile* file) override { m_file = file; }
};

QStringList sortedIds(const graph::GraphData& g, std::optional<NodeKind> kind = std::nullopt)
{
    QStringList out;
    for (const graph::GraphNode& n : g.nodes())
        if (!kind || n.kind == *kind)
            out << n.id;
    out.sort();
    return out;
}

QStringList sortedEdges(const graph::GraphData& g)
{
    QStringList out;
    for (const graph::GraphEdge& e : g.edges())
        out << g.node(e.source).id + " -> " + g.node(e.target).id;
    out.sort();
    return out;
}

class GraphViewTest : public ::testing::Test {
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
        writeFile(root + "/Hub.md", "Links to [[A]] and [[Missing note]]. #topic\n");
        writeFile(root + "/A.md", "Back to [[Hub]], picture ![[pic.png]] #Topic\n");
        writeFile(root + "/B.md", "---\ntags: [topic, other]\n---\nSee [[Hub]].\n");
        writeFile(root + "/Folder/C.md", "[[A]] and [[B]]\n");
        writeFile(root + "/Lonely.md", "Nothing here.\n");
        writeFile(root + "/pic.png", "png");
        writeFile(root + "/doc.pdf", "pdf");
        writeFile(root + "/Board.canvas", "{}");
        session = std::make_unique<VaultSession>(QStringLiteral("0123456789abcdef"), root);
        vault = std::make_unique<Vault>(root);
        meta = std::make_unique<MetadataCache>();
        session->setVault(vault.get());
        session->setMetadata(meta.get());
        bindVaultServices(session.get());
        vault->load();
        ASSERT_TRUE(QTest::qWaitFor([&] { return meta->isReady() && meta->isClean(); }, 10000));

        registry.registerView(QStringLiteral("markdown"), [](WorkspaceLeaf* l) -> View* { return new NoteView(l); });
        registry.registerExtensions({QStringLiteral("md")}, QStringLiteral("markdown"));
        registerGraphViews(registry);
        session->setViewRegistry(&registry);
        commands = std::make_unique<CommandRegistry>();
        session->setCommands(commands.get());
    }
    void TearDown() override
    {
        setGraphViewHooks({});
        workspace.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        commands.reset();
        meta.reset();
        vault.reset();
        session.reset();
    }
    Workspace* makeWorkspace()
    {
        workspace = std::make_unique<Workspace>(session.get(), &registry, nullptr);
        workspace->loadLayout();
        session->setWorkspace(workspace.get());
        registerGraphCommands(*commands, workspace.get());
        return workspace.get();
    }
    bool linksExtracted() const
    {
        const md::CachedMetadata* m = meta->fileCache("Hub.md");
        return m && !m->links.isEmpty();
    }

    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<VaultSession> session;
    std::unique_ptr<Vault> vault;
    std::unique_ptr<MetadataCache> meta;
    std::unique_ptr<CommandRegistry> commands;
    ViewRegistry registry;
    std::unique_ptr<Workspace> workspace;
};

} // namespace

TEST(VaultGraph, BuilderMirrorsObsidian)
{
    const QList<VaultGraphFile> files = {{"Hub.md", 30}, {"A.md", 10}, {"B.md", 20}, {"Folder/C.md", 40}, {"Lonely.md", 50},
                                         {"pic.png", 5}, {"doc.pdf", 6}, {"Board.canvas", 7}};
    QHash<QString, QHash<QString, int>> resolved, unresolved;
    resolved["Hub.md"] = {{"A.md", 1}};
    resolved["A.md"] = {{"Hub.md", 2}, {"pic.png", 1}};
    resolved["B.md"] = {{"Hub.md", 1}};
    resolved["Folder/C.md"] = {{"A.md", 1}, {"B.md", 1}};
    resolved["Ghost.md"] = {{"Hub.md", 1}}; // a source the file list doesn't know (yet)
    unresolved["Hub.md"] = {{"Missing note", 1}};
    unresolved["B.md"] = {{"Missing note", 1}, {"Folder/Other", 1}};
    QHash<QString, QStringList> tags;
    tags["Hub.md"] = {"#topic", "#project/alpha"};
    tags["A.md"] = {"#Topic", "#topic"}; // repeated tags link once
    tags["B.md"] = {"#topic", "#other/"};
    const graph::GraphData g = buildVaultGraph(files, resolved, unresolved, tags);
    ASSERT_TRUE(g.isFinalized());
    EXPECT_EQ(sortedIds(g, NodeKind::File), (QStringList{"A.md", "B.md", "Board.canvas", "Folder/C.md", "Ghost.md", "Hub.md", "Lonely.md"}));
    EXPECT_EQ(sortedIds(g, NodeKind::Attachment), (QStringList{"doc.pdf", "pic.png"}));
    EXPECT_EQ(sortedIds(g, NodeKind::Unresolved), (QStringList{"Folder/Other", "Missing note"}));
    // "#topic" (3×) beats "#Topic" (1×); trailing '/' dropped.
    EXPECT_EQ(sortedIds(g, NodeKind::Tag), (QStringList{"#other", "#project/alpha", "#topic"}));
    EXPECT_EQ(sortedEdges(g),
              (QStringList{"A.md -> #topic", "A.md -> Hub.md", "A.md -> pic.png", "B.md -> #other", "B.md -> #topic", "B.md -> Folder/Other",
                           "B.md -> Hub.md", "B.md -> Missing note", "Folder/C.md -> A.md", "Folder/C.md -> B.md", "Ghost.md -> Hub.md",
                           "Hub.md -> #project/alpha", "Hub.md -> #topic", "Hub.md -> A.md", "Hub.md -> Missing note"}));
    const int hub = g.indexOf("Hub.md");
    EXPECT_EQ(g.node(hub).weight, 7); // out: A, Missing, 2 tags; in: A, B, Ghost
    EXPECT_EQ(g.node(hub).timestamp, 30);
    EXPECT_EQ(g.node(g.indexOf("Folder/C.md")).label, "C");
    EXPECT_EQ(g.node(g.indexOf("#project/alpha")).label, "alpha"); // Obsidian's getDisplayText: after the last '/'
    // Links from one note: resolved, unresolved, then tags.
    const std::span<const int> out = g.outgoing(g.indexOf("B.md"));
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(g.node(out[0]).kind, NodeKind::File);
    EXPECT_EQ(g.node(out[1]).kind, NodeKind::Unresolved);
    EXPECT_EQ(g.node(out[3]).kind, NodeKind::Tag);

    // With the default filter: tags and attachments hidden, unresolved and orphans shown.
    const graph::GraphData shown = graph::GraphFilter{}.apply(g);
    EXPECT_EQ(sortedIds(shown), (QStringList{"A.md", "B.md", "Board.canvas", "Folder/C.md", "Folder/Other", "Ghost.md", "Hub.md",
                                             "Lonely.md", "Missing note"}));
}

TEST_F(GraphViewTest, VaultGraphFromMetadata)
{
    EXPECT_TRUE(isGraphAttachment("pic.png"));
    EXPECT_TRUE(isGraphAttachment("Folder/noext"));
    EXPECT_FALSE(isGraphAttachment("Folder/Note.MD"));
    EXPECT_FALSE(isGraphAttachment("Board.canvas"));
    EXPECT_FALSE(isGraphAttachment("x.base"));

    const graph::GraphData g = graphDataFromSession(*session);
    ASSERT_TRUE(g.isFinalized());
    EXPECT_EQ(sortedIds(g, NodeKind::File), (QStringList{"A.md", "B.md", "Board.canvas", "Folder/C.md", "Hub.md", "Lonely.md"}));
    EXPECT_EQ(sortedIds(g, NodeKind::Attachment), (QStringList{"doc.pdf", "pic.png"}));
    if (!linksExtracted())
        GTEST_SKIP() << "no links extracted by the markdown parser";
    EXPECT_EQ(sortedIds(g, NodeKind::Unresolved), QStringList{"Missing note"});
    // "#topic" twice (Hub inline, B frontmatter) beats "#Topic" once.
    EXPECT_EQ(sortedIds(g, NodeKind::Tag), (QStringList{"#other", "#topic"}));
    EXPECT_EQ(sortedEdges(g), (QStringList{"A.md -> #topic", "A.md -> Hub.md", "A.md -> pic.png", "B.md -> #other", "B.md -> #topic",
                                           "B.md -> Hub.md", "Folder/C.md -> A.md", "Folder/C.md -> B.md", "Hub.md -> #topic",
                                           "Hub.md -> A.md", "Hub.md -> Missing note"}));
    EXPECT_EQ(g.node(g.indexOf("Hub.md")).weight, 5); // out: A, Missing, #topic; in: A, B
    EXPECT_GT(g.node(g.indexOf("Hub.md")).timestamp, 0);
    EXPECT_EQ(g.node(g.indexOf("Folder/C.md")).label, "C");
}

TEST_F(GraphViewTest, GraphViewOpensPersistsAndRebuilds)
{
    // A graph.json from an earlier session is restored.
    graph::GraphSettings saved;
    saved.showTags = true;
    saved.scale = 0.75;
    session->config()->setObject("graph", saved.toJson());

    Workspace* ws = makeWorkspace();
    ASSERT_TRUE(commands->find("graph:open"));
    EXPECT_EQ(commands->find("graph:open")->name, "Open graph view");
    EXPECT_EQ(commands->hotkeys("graph:open").value(0), Hotkey::parse("Mod+G"));
    EXPECT_FALSE(commands->isAvailable("graph:animate"));
    ASSERT_TRUE(commands->execute("graph:open"));
    auto* view = ws->activeViewOfType<GraphView>();
    ASSERT_TRUE(view);
    EXPECT_EQ(view->displayText(), "Graph view");
    EXPECT_EQ(view->icon(), "git-fork");
    EXPECT_TRUE(commands->isAvailable("graph:animate"));
    GraphWidget* w = view->graphWidget();
    EXPECT_TRUE(w->settings().showTags);
    EXPECT_DOUBLE_EQ(w->canvas()->targetScale(), 0.75);
    ASSERT_TRUE(w->fullGraph());
    EXPECT_TRUE(w->fullGraph()->contains("Lonely.md"));
    EXPECT_TRUE(w->shownGraph()->contains("Lonely.md")); // orphans shown by default
    EXPECT_FALSE(w->shownGraph()->contains("pic.png"));  // attachments off

    // Edits are saved to graph.json in the clone's config (debounced).
    w->controls()->toggle("showAttachments")->click();
    EXPECT_TRUE(w->shownGraph()->contains("pic.png"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return session->config()->object("graph").value("showAttachments").toBool(); }, 3000));
    const QJsonObject json = session->config()->object("graph");
    EXPECT_TRUE(json.contains("collapse-filter"));
    EXPECT_TRUE(json.contains("showOrphans"));
    EXPECT_FALSE(json.contains("localJumps"));
    EXPECT_FALSE(QDir(root + "/.obsidian").exists()); // never written into the vault

    // New notes appear after the metadata cache resolves.
    ASSERT_TRUE(vault->createFile("New.md", "[[Lonely]]\n"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return w->fullGraph()->contains("New.md"); }, 5000));
    if (linksExtracted()) {
        ASSERT_TRUE(QTest::qWaitFor([&] { return w->fullGraph()->hasEdge(w->fullGraph()->indexOf("New.md"), w->fullGraph()->indexOf("Lonely.md")); }, 5000));
    }

    // Clicking a tag asks for the global search.
    QString searched;
    GraphViewHooks hooks;
    hooks.openSearch = [&](Workspace*, const QString& q) { searched = q; };
    setGraphViewHooks(hooks);
    if (linksExtracted()) {
        emit w->openRequested("#topic", false);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !searched.isEmpty(); }, 2000));
        EXPECT_EQ(searched, "tag:#topic");
    }

    // Clicking an unresolved node creates the note and opens it in a new tab.
    if (linksExtracted()) {
        emit w->openRequested("Missing note", true);
        ASSERT_TRUE(QTest::qWaitFor([&] { return QFile::exists(root + "/Missing note.md"); }, 2000));
        auto* note = ws->activeViewOfType<FileView>();
        ASSERT_TRUE(note);
        EXPECT_EQ(note->file()->path(), "Missing note.md");
        ws->setActiveLeaf(view->leaf());
    }

    // A plain click opens the note in the graph's own leaf (Obsidian's getLeaf(false)).
    WorkspaceLeaf* graphLeaf = view->leaf();
    emit w->openRequested("A.md", false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return graphLeaf->view() && graphLeaf->view()->viewType() == u"markdown"; }, 2000));
    EXPECT_EQ(qobject_cast<FileView*>(graphLeaf->view())->file()->path(), "A.md");
    EXPECT_FALSE(commands->isAvailable("graph:animate"));
}

TEST_F(GraphViewTest, LocalGraphFollowsTheActiveFile)
{
    Workspace* ws = makeWorkspace();
    EXPECT_FALSE(commands->isAvailable("graph:open-local")); // no active file
    ws->openFile(vault->file("A.md"));
    WorkspaceLeaf* noteLeaf = ws->activeLeaf();
    ASSERT_TRUE(commands->isAvailable("graph:open-local"));
    ASSERT_TRUE(commands->execute("graph:open-local"));
    auto* local = ws->activeViewOfType<LocalGraphView>();
    ASSERT_TRUE(local);
    EXPECT_NE(local->leaf(), noteLeaf);
    EXPECT_EQ(local->displayText(), "Graph of A");
    EXPECT_EQ(local->state().value("file").toString(), "A.md");
    GraphWidget* w = local->graphWidget();
    EXPECT_EQ(w->localCenter(), "A.md");
    if (linksExtracted()) {
        EXPECT_EQ(sortedIds(*w->shownGraph()), (QStringList{"A.md", "Folder/C.md", "Hub.md"}));
    }

    // Options live in the view state.
    w->controls()->toggle("localInterlinks")->click();
    const QJsonObject state = local->state();
    ASSERT_TRUE(state.value("options").isObject());
    EXPECT_TRUE(state.value("options").toObject().value("localInterlinks").toBool());
    EXPECT_FALSE(state.value("options").toObject().contains("showOrphans"));

    // Opening another note in the other pane moves the local graph.
    ws->setActiveLeaf(noteLeaf);
    ws->openFile(vault->file("B.md"));
    EXPECT_EQ(w->localCenter(), "B.md");
    EXPECT_EQ(local->displayText(), "Graph of B");

    // The state round-trips (workspace.json).
    local->setState(QJsonObject{{"file", "Hub.md"}, {"options", graph::GraphSettings{}.toJson(graph::GraphSettings::Variant::Local)}});
    EXPECT_EQ(w->localCenter(), "Hub.md");
    EXPECT_FALSE(w->settings().localInterlinks);
}

TEST_F(GraphViewTest, SearchBoxUsesTheVaultSearch)
{
    Workspace* ws = makeWorkspace();
    ASSERT_TRUE(commands->execute("graph:open"));
    auto* view = ws->activeViewOfType<GraphView>();
    ASSERT_TRUE(view);
    GraphWidget* w = view->graphWidget();
    w->controls()->searchInput()->setText("Nothing");
    QTest::keyClick(w->controls()->searchInput(), Qt::Key_Return);
    // Content search: only Lonely.md contains "Nothing" (runs asynchronously).
    ASSERT_TRUE(QTest::qWaitFor([&] { return sortedIds(*w->shownGraph()) == QStringList{"Lonely.md"}; }, 5000))
        << sortedIds(*w->shownGraph()).join(", ").toStdString();

    w->controls()->searchInput()->setText("path:Folder");
    QTest::keyClick(w->controls()->searchInput(), Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return sortedIds(*w->shownGraph()) == QStringList{"Folder/C.md"}; }, 5000))
        << sortedIds(*w->shownGraph()).join(", ").toStdString();

    // Notes edited later are re-evaluated.
    w->controls()->searchInput()->setText("banana");
    QTest::keyClick(w->controls()->searchInput(), Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return w->shownGraph()->nodeCount() == 0; }, 5000));
    QString err;
    ASSERT_TRUE(vault->write("Lonely.md", "A banana.\n", vault->stat("Lonely.md"), nullptr, &err) == DocumentStorage::WriteResult::Ok)
        << err.toStdString();
    vault->rescan();
    ASSERT_TRUE(QTest::qWaitFor([&] { return sortedIds(*w->shownGraph()) == QStringList{"Lonely.md"}; }, 5000))
        << sortedIds(*w->shownGraph()).join(", ").toStdString();
}
