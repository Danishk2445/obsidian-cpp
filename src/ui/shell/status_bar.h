#pragma once

#include <QLabel>
#include <QSet>
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
    // A disabled item (its core plugin is off) stays hidden whatever setItemVisible() says.
    void setItemEnabled(const QString& id, bool enabled);

signals:
    void itemClicked(const QString& id);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void anchor(); // fit to contents, bottom-right corner of the parent
    QHBoxLayout* m_layout;
    QList<QPair<QString, QLabel*>> m_items;
    QSet<QString> m_disabled;
    QSet<QString> m_wanted; // setItemVisible(true) while disabled
};

} // namespace oc
