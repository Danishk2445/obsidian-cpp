#pragma once

#include <QWidget>

namespace oc {

class EmptyView;

// Temporary root area for M0 (replaced by the real workspace in M1): a header row (drag region,
// new-tab and tab-list buttons) above the empty state.
class RootPlaceholder : public QWidget {
    Q_OBJECT
public:
    explicit RootPlaceholder(QWidget* parent = nullptr);
    void setRightReserve(int px);
    EmptyView* emptyView() const { return m_empty; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QWidget* m_header;
    QWidget* m_reserve;
    EmptyView* m_empty;
};

} // namespace oc
