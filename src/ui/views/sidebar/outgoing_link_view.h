#pragma once

#include "ui/views/sidebar/sidebar_common.h"
#include "ui/widgets/tree_list.h"

#include <QTimer>

#include <memory>

namespace oc {

// Obsidian's "Outgoing links" view (view type "outgoing-link", core plugin "outgoing-link"): the links
// of the active note — one row per destination (resolved notes with their folder as sub-text, headings
// and blocks with their note, unresolved links with a "file-plus" icon, "Not created yet", dimmed) —
// and, in the collapsed-by-default "Unlinked mentions" section, other notes' names and aliases written
// as plain text in it, each with chips that turn the mention into a link to that note.
//
// View state: {"file", "linksCollapsed", "unlinkedCollapsed"}.
class OutgoingLinkView : public LinkedFileView {
    Q_OBJECT
public:
    explicit OutgoingLinkView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);

    QString viewType() const override { return QStringLiteral("outgoing-link"); }
    QString icon() const override { return QStringLiteral("links-going-out"); }
    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    void focusContent() override;

    TreeList* tree() const { return m_tree; }
    bool linksCollapsed() const { return m_linksCollapsed; }
    void setLinksCollapsed(bool collapsed);
    bool unlinkedCollapsed() const { return m_unlinkedCollapsed; }
    void setUnlinkedCollapsed(bool collapsed);

    struct LinkRow {
        QString linktext; // as written (target + subpath)
        QString title;
        QString subtext;
        QString icon;
        QString file;     // resolved vault path ("" = unresolved)
        bool unresolved = false;
    };
    QList<LinkRow> links() const { return m_links; }
    int unlinkedCount() const;
    // The chip action: link the mention (index into the unlinked matches) to target `chip`.
    bool linkMention(int matchIndex, int chip);

protected:
    QString baseTitle() const override;
    QString boundTitle() const override;
    void fileChanged() override;
    void leafActiveChanged(bool active) override;

private:
    void recompute();
    void rebuild();
    void showContextMenu(TreeList::Node* node, const QPoint& globalPos);

    TreeList* m_tree;
    bool m_linksCollapsed = false;
    bool m_unlinkedCollapsed = true;
    QList<LinkRow> m_links;
    struct Mention {
        MatchRange range;
        QStringList targets;
    };
    QList<Mention> m_mentions;
    std::shared_ptr<const QString> m_content;
    QTimer m_update;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace oc
