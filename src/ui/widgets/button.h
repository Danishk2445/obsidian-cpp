#pragma once

#include <QPushButton>

namespace oc {

// Obsidian's ButtonComponent (<button>): 30 px high (--input-height), 12 px side padding,
// --button-radius, 13 px text, --interactive-normal with --input-shadow. Styles: plain, mod-cta
// (accent), mod-warning (= mod-destructive.mod-cta: red with white text) and mod-destructive alone
// (red text on a faint red background). Disabled buttons are drawn at 70 % opacity; a loading button
// hides its text behind a spinner-less placeholder (Obsidian's .mod-loading).
class Button : public QPushButton {
    Q_OBJECT
public:
    enum class Style { Normal, Cta, Warning, Destructive };

    explicit Button(const QString& text, Style style = Style::Normal, QWidget* parent = nullptr);

    Style buttonStyle() const { return m_style; }
    void setButtonStyle(Style style);
    void setLoading(bool loading);
    bool isLoading() const { return m_loading; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void focusInEvent(QFocusEvent*) override;

private:
    Style m_style;
    bool m_loading = false;
    bool m_focusVisible = false;
};

} // namespace oc
