#pragma once

#include <QWidget>

namespace oc {

// Obsidian's SliderComponent: an optional value label (".slider-value", 13px tabular numbers,
// two decimals for fractional steps) followed by a range input drawn with the --slider-* tokens:
// 4px track, accent fill up to the thumb centre, 18px white thumb with a soft shadow.
// Values are doubles; step 0 means continuous (HTML step="any").
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    explicit ValueSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setStep(double step);
    double minimum() const { return m_min; }
    double maximum() const { return m_max; }
    double step() const { return m_step; }

    // Programmatic update: clamps/snaps, never emits valueChanged.
    void setValue(double value);
    double value() const { return m_value; }

    void setShowValue(bool show);
    QString valueText() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // User edits only (drag, click, keyboard), emitted continuously while dragging.
    void valueChanged(double value);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    double snap(double v) const;
    QRectF trackRect() const;
    double valueLabelWidth() const;
    void setFromX(double x);
    void userSet(double v);

    double m_min = 0, m_max = 1, m_step = 0, m_value = 0;
    bool m_showValue = true;
    bool m_hover = false;
    bool m_dragging = false;
};

} // namespace oc
