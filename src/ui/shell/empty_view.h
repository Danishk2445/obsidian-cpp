#pragma once

#include "ui/workspace/view.h"

#include <QPointer>

#include <functional>

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
    void onOpen() override;

    // The texts of the visible actions, top to bottom (tests / accessibility).
    QStringList actionTexts() const;

signals:
    // Obsidian command ids: "file-explorer:new-file", "switcher:open" (the view also runs them through the
    // session's CommandRegistry). "Close" detaches the leaf itself.
    void actionTriggered(const QString& commandId);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void setTitle(const QString& title);
    void setDescription(const QString& description, const QString& tooltip = {});
    // The action list (.empty-state-action): subclasses may replace the default actions.
    void clearActions();
    QLabel* addAction(const QString& text, std::function<void()> onClick);

private:
    void runCommand(const QString& id);
    void restyle();
    QLabel* m_title;
    QLabel* m_description;
    QVBoxLayout* m_actionList;
    QList<QLabel*> m_actions;
    QPointer<QLabel> m_goToFile;
};

} // namespace oc
