#pragma once

#include <QColor>
#include <QWidget>

namespace oc {

// Obsidian's ColorComponent (<input type="color">): a 22 px round swatch (--swatch-width/-height,
// --swatch-radius) with the inset --swatch-shadow ring, in a 26 × 22 box. Clicking (or Space/Enter)
// opens a color dialog; the chosen color is reported through colorChanged().
class ColorSwatch : public QWidget {
    Q_OBJECT
public:
    explicit ColorSwatch(QWidget* parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor& color); // programmatic: never emits colorChanged
    QString hexValue() const;           // "#rrggbb", like the input's value

    // A user choice (dialog, tests): emits colorChanged when it differs.
    void choose(const QColor& color);
    void openDialog();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void colorChanged(const QColor& color);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QColor m_color = Qt::black;
};

} // namespace oc
