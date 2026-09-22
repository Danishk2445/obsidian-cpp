#pragma once

#include <QWidget>

namespace oc {

// "No file is open" empty state shown in an empty tab (Obsidian's .empty-state).
class EmptyView : public QWidget {
    Q_OBJECT
public:
    explicit EmptyView(QWidget* parent = nullptr);

signals:
    void actionTriggered(const QString& action); // "new-file" | "open-switcher" | "recent" | "close"

protected:
    void paintEvent(QPaintEvent*) override;
};

} // namespace oc
