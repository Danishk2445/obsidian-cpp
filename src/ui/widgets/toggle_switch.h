#pragma once

#include <QAbstractButton>

class QVariantAnimation;

namespace oc {

// Obsidian's toggle (".checkbox-container"): a pill that is --interactive-accent when on and
// --background-modifier-border-hover when off, with a white thumb that slides over 150 ms and
// grows while pressed. Small = the default --toggle-s-* size (34×19, graph controls, settings
// lists); Normal = --toggle-* (40×22, modals).
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
public:
    enum class Size { Small, Normal };

    explicit ToggleSwitch(QWidget* parent = nullptr);

    void setSize(Size size);
    Size switchSize() const { return m_size; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void checkStateSet() override;
    void nextCheckState() override;
    bool hitButton(const QPoint& pos) const override;

private:
    void animateTo(bool on);

    Size m_size = Size::Small;
    double m_pos = 0; // thumb position 0 (off) .. 1 (on)
    QVariantAnimation* m_anim = nullptr;
};

} // namespace oc
