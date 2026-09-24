#pragma once

// Obsidian's core "Graph view" plugin as workspace views and commands:
//  - GraphView ("graph"): the whole vault; options persist in the per-vault config store as
//    graph.json (Obsidian's .obsidian/graph.json keys);
//  - LocalGraphView ("localgraph"): the neighbourhood of one file, following the active file;
//    its options live in the view state ({"file", "options": {...}}) like Obsidian's;
//  - registerGraphViews() / registerGraphCommands(): graph:open (Mod+G), graph:open-local,
//    graph:animate.
// Both views rebuild (debounced) when the metadata cache resolves, open nodes through
// Workspace::openFile(), and run search / colour-group queries with the vault search.

#include "ui/workspace/view.h"

#include <functional>

class QMenu;

namespace oc {

class CommandRegistry;
class GraphVaultSearch;
class GraphWidget;
class ViewRegistry;
class Workspace;

// Hooks the app wires to other plugins (all optional, process-wide: they receive the view's
// session/workspace, so one set serves every window).
struct GraphViewHooks {
    // Tag nodes: Obsidian opens the global search with "tag:#tag" (click and right click).
    std::function<void(Workspace* workspace, const QString& query)> openSearch;
    // Extra items for a file node's context menu (Obsidian's "file-menu" event, source
    // "graph-context-menu"), e.g. bookmarks, reveal in navigation, rename.
    std::function<void(QMenu* menu, TFile* file, WorkspaceLeaf* leaf)> fileMenu;
    // "Delete file" in the context menu (Obsidian's fileManager.promptForDeletion); hidden when unset.
    std::function<void(VaultSession* session, TFile* file)> deleteFile;
};
void setGraphViewHooks(GraphViewHooks hooks);

class GraphViewController;

class GraphView : public View {
    Q_OBJECT
public:
    explicit GraphView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~GraphView() override;

    QString viewType() const override { return QStringLiteral("graph"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("git-fork"); }
    void onOpen() override;
    void onClose() override;
    void populatePaneMenu(QMenu* menu) override;
    void focusContent() override;

    GraphWidget* graphWidget() const { return m_widget; }
    void startTimelapse(); // graph:animate
    // Rebuilds the vault graph now (normally debounced after MetadataCache::resolved).
    void refresh();

private:
    GraphWidget* m_widget;
    GraphViewController* m_controller = nullptr;
};

class LocalGraphView : public FileView {
    Q_OBJECT
public:
    explicit LocalGraphView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~LocalGraphView() override;

    QString viewType() const override { return QStringLiteral("localgraph"); }
    QString displayText() const override; // "Graph of <file>" / "Local graph"
    QString icon() const override { return QStringLiteral("git-fork"); }
    bool navigation() const override { return false; }
    bool canAcceptExtension(const QString&) const override { return true; }
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;
    void onDelete(TFile* file) override; // the graph stays open, empty
    QJsonObject state() const override;  // {"file", "options"}
    void setState(const QJsonObject& state) override;
    void onOpen() override;
    void onClose() override;
    void populatePaneMenu(QMenu* menu) override;
    void focusContent() override;

    GraphWidget* graphWidget() const { return m_widget; }
    void refresh();

private:
    void followActiveLeaf(WorkspaceLeaf* leaf);

    GraphWidget* m_widget;
    GraphViewController* m_controller = nullptr;
};

void registerGraphViews(ViewRegistry& registry);
// graph:open (Mod+G), graph:open-local, graph:animate.
void registerGraphCommands(CommandRegistry& registry, Workspace* workspace);

} // namespace oc
