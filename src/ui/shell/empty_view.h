#pragma once

#include "ui/workspace/view.h"

class QLabel;
class QVBoxLayout;

namespace oc {

// The "empty" view of a new tab (Obsidian's EmptyView, view type "empty"): "No file is open" plus
// actions. Also the base of PlaceholderView ("Plugin no longer active").
class EmptyView : public View {
    Q_OBJECT
public:
    explicit EmptyView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);

    QString viewType() const override { return QStringLiteral("empty"); }
    QString displayText() const override;
    QString icon() const override { return QStringLiteral("file"); }

signals:
    // Obsidian command ids: "file-explorer:new-file", "switcher:open". "Close" detaches the leaf itself.
    void actionTriggered(const QString& commandId);

protected:
    void setTitle(const QString& title);
    void setDescription(const QString& description);

private:
    void restyle();
    QLabel* m_title;
    QLabel* m_description;
    QList<QLabel*> m_actions;
};

} // namespace oc
