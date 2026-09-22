#pragma once

#include <QLabel>
#include <QWidget>

class QHBoxLayout;

namespace oc {

// Floating status bar in the bottom-right corner (Obsidian: fixed, 12px, rounded top-left corner).
class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget* parent = nullptr);

    // Items keep insertion order; plugins own the returned label.
    QLabel* addItem(const QString& id, const QString& text = {});
    QLabel* item(const QString& id) const;
    void setItemText(const QString& id, const QString& text);
    void setItemVisible(const QString& id, bool visible);

signals:
    void itemClicked(const QString& id);

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QHBoxLayout* m_layout;
    QList<QPair<QString, QLabel*>> m_items;
};

} // namespace oc
