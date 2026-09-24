#pragma once

#include "core/search/search_query.h"
#include "core/util/match_range.h"
#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"

#include <QHash>
#include <QTimer>

#include <memory>

namespace oc {

namespace search {
class SearchEngine;
class SearchJob;
} // namespace search

class NavActionButton;

// Obsidian's backlink pane (.backlink-pane): "Linked mentions" (notes whose links, embeds or property
// links resolve to the file) and "Unlinked mentions" (its name or aliases written as plain text,
// whole words, case-insensitive — found with the global search engine, then checked against the
// notes' links and frontmatter), each a list of search results with snippets. The nav header holds
// Collapse results, Show more context, Change sort order and Show search filter. The unlinked section
// starts collapsed and is only computed while expanded; each of its snippets has a "Link" button that
// turns the mention into a link (FileManager::generateMarkdownLink with the mention as display text).
//
// View state (Obsidian's keys): {"collapseAll", "extraContext", "sortOrder", "showSearch",
// "searchQuery", "backlinkCollapsed", "unlinkedCollapsed"}.
class BacklinkPane : public QWidget {
    Q_OBJECT
public:
    // `owner` provides the session / workspace (and is the page-preview source's view).
    BacklinkPane(View* owner, QWidget* parent = nullptr);
    ~BacklinkPane() override;

    void setFile(const QString& path);
    QString file() const { return m_path; }

    QJsonObject state() const;
    void setState(const QJsonObject& state);

    bool collapseAll() const { return m_collapseAll; }
    void setCollapseAll(bool on);
    bool extraContext() const { return m_extraContext; }
    void setExtraContext(bool on);
    QString sortOrder() const { return m_sortOrder; }
    void setSortOrder(const QString& order);
    bool backlinkCollapsed() const { return m_backlinkCollapsed; }
    void setBacklinkCollapsed(bool collapsed);
    bool unlinkedCollapsed() const { return m_unlinkedCollapsed; }
    void setUnlinkedCollapsed(bool collapsed);
    void setSearchQuery(const QString& query);

    TreeList* tree() const { return m_tree; }
    NavHeader* header() const { return m_header; }
    NavActionButton* collapseButton() const { return m_collapseButton; }
    NavActionButton* contextButton() const { return m_contextButton; }

    // Results (for tests and the status of the headers).
    QStringList linkedFiles() const;   // in display order
    QStringList unlinkedFiles() const;
    int linkedCount() const;           // matches (the header's flair)
    int unlinkedCount() const;
    bool isSearchingUnlinked() const;
    // The "Link" action of an unlinked match (index into the file's matches).
    bool linkUnlinkedMention(const QString& sourcePath, int matchIndex);

    void setLeafActive(bool active);

signals:
    void stateChanged();
    void resultsChanged();

private:
    struct Result {
        QString path;
        std::shared_ptr<const QString> content;
        QList<MatchRange> matches;
    };
    VaultSession* session() const;
    void recomputeLinked();
    void recomputeUnlinked();
    void cancelUnlinked();
    std::optional<Result> linkedResult(const QString& source) const;
    std::optional<Result> unlinkedResult(const QString& source, const QString* text = nullptr) const;
    QStringList mentionNames() const;
    bool passesFilter(const QString& path, const QString& content) const;
    void onMetadataChanged(const QString& path);
    void scheduleRebuild();
    void rebuild();
    QList<const Result*> sorted(const QHash<QString, Result>& results) const;
    void addSection(std::vector<TreeList::NodePtr>& roots, bool unlinked);
    void openMatch(TreeList::Node* node, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void showContextMenu(TreeList::Node* node, const QPoint& globalPos);

    QPointer<View> m_owner;
    NavHeader* m_header;
    TreeList* m_tree;
    NavActionButton* m_collapseButton = nullptr;
    NavActionButton* m_contextButton = nullptr;
    QString m_path;
    bool m_collapseAll = false;
    bool m_extraContext = false;
    QString m_sortOrder = QStringLiteral("alphabetical");
    bool m_backlinkCollapsed = false;
    bool m_unlinkedCollapsed = true;
    search::SearchQuery m_filter;
    QHash<QString, Result> m_linked;
    QHash<QString, Result> m_unlinked;
    QString m_unlinkedAliases; // the aliases the unlinked results were computed for
    std::unique_ptr<search::SearchEngine> m_engine;
    std::shared_ptr<search::SearchJob> m_job;
    QTimer m_rebuild;
    QTimer m_linkedTimer;
    QList<QMetaObject::Connection> m_connections;
};

// Obsidian's "Backlinks" view (view type "backlink", core plugin "backlink"): the backlink pane of the
// active file (or, when pinned / opened for a note, of that note: "Backlinks for <note>").
class BacklinkView : public LinkedFileView {
    Q_OBJECT
public:
    explicit BacklinkView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);

    QString viewType() const override { return QStringLiteral("backlink"); }
    QString icon() const override { return QStringLiteral("links-coming-in"); }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void focusContent() override;

    BacklinkPane* pane() const { return m_pane; }

protected:
    QString baseTitle() const override;
    QString boundTitle() const override;
    void fileChanged() override;
    void leafActiveChanged(bool active) override;

private:
    BacklinkPane* m_pane;
};

} // namespace oc
