#pragma once

#include <QLineEdit>

namespace oc {

// Obsidian's .search-input-container input: a 30px input with a magnifier on the left, an optional
// circular clear button and an optional right-aligned decorator text (e.g. "2 / 7" match counter).
class SearchInput : public QLineEdit {
    Q_OBJECT
public:
    explicit SearchInput(QWidget* parent = nullptr);

    void setClearButtonEnabled(bool on);
    void setDecorator(const QString& text); // shown inside, right-aligned, muted
    QString decorator() const { return m_decorator; }
    void setNoMatch(bool noMatch);          // tinted background (Obsidian .mod-no-match)
    bool noMatch() const { return m_noMatch; }
    void setShowIcon(bool on);

    QSize sizeHint() const override;

signals:
    void cleared(); // the clear button was clicked

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QRect clearRect() const;
    void updateMargins();
    void updateStyle();
    bool m_clear = true;
    bool m_icon = true;
    bool m_noMatch = false;
    QString m_decorator;
};

} // namespace oc
