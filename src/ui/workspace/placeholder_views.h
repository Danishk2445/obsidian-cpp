#pragma once

#include "ui/shell/empty_view.h"
#include "ui/workspace/view.h"

namespace oc {

class ViewRegistry;

// Shown for a leaf whose view type isn't registered (Obsidian: "Plugin no longer active", titled with
// the type, ghost icon; actions "Close" and "Close all “type” tabs (N)" when N > 1). Keeps the
// original view state (state, icon, title) so the layout round-trips unchanged.
class PlaceholderView : public EmptyView {
    Q_OBJECT
public:
    PlaceholderView(WorkspaceLeaf* leaf, QString type, QWidget* parent = nullptr);
    void onOpen() override;

    QString viewType() const override { return m_type; }
    QString displayText() const override { return m_type; }
    QString icon() const override { return QStringLiteral("ghost"); }
    QJsonObject state() const override { return m_state; }
    void setState(const QJsonObject& state) override { m_state = state; }

    void setOriginalViewState(const QJsonObject& viewState);
    // The view state as it was restored (icon/title included), with the current state and pin flag.
    QJsonObject originalViewState(bool pinned) const;

private:
    void updateCloseAll();
    QString m_type;
    QJsonObject m_state;
    QJsonObject m_original;
    QLabel* m_closeAll = nullptr;
};

// Temporary sidebar views until the real ones land (file explorer, search, bookmarks, backlinks,
// outgoing links, tags, outline): an icon tab with a faint caption; the state round-trips.
class SidebarPlaceholderView : public View {
    Q_OBJECT
public:
    SidebarPlaceholderView(WorkspaceLeaf* leaf, QString type, QString title, QString icon, QWidget* parent = nullptr);

    QString viewType() const override { return m_type; }
    QString displayText() const override { return m_title; }
    QString icon() const override { return m_icon; }
    QJsonObject state() const override { return m_state; }
    void setState(const QJsonObject& state) override { m_state = state; }
    bool navigation() const override { return false; }
    bool showsViewHeader() const override { return false; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_type, m_title, m_icon;
    QJsonObject m_state;
};

// Registers the views provided by the workspace module: the sidebar placeholders listed above.
// ("empty" is built in: every leaf can show it without a registration.)
void registerCoreViews(ViewRegistry& registry);

} // namespace oc
