#pragma once

// The view layouts of a base (Obsidian's registered Bases views): a painted, virtualized scroll area
// showing the controller's ViewData. In a .base tab the layout scrolls; in an embed it has no
// vertical scroll bar and reports its whole height (contentHeight()).

#include "core/bases/base_query.h"

#include <QAbstractScrollArea>
#include <QJsonObject>
#include <QPointer>

namespace oc::basesui {

class BasesController;

class BaseLayout : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit BaseLayout(BasesController* controller, QWidget* parent = nullptr);

    virtual QString type() const = 0;
    // New data (results, sort, columns, options changed).
    virtual void setData(const bases::ViewData& data) = 0;
    // The whole content's height (embeds size to it).
    virtual int contentHeight() const = 0;
    virtual QJsonObject ephemeralState() const;
    virtual void setEphemeralState(const QJsonObject& state);

    BasesController* controller() const { return m_controller; }
    const bases::ViewData& data() const { return m_data; }
    bool isEmbedded() const;

signals:
    void contentHeightChanged();

protected:
    void storeData(const bases::ViewData& data) { m_data = data; }
    int scrollX() const;
    int scrollY() const;
    void updateScrollBars(int contentWidth, int contentHeight);

    QPointer<BasesController> m_controller;
    bases::ViewData m_data;
};

// The layout for a view type ("table", "cards", "list"), nullptr for unknown types.
BaseLayout* createLayout(const QString& type, BasesController* controller, QWidget* parent);

} // namespace oc::basesui
