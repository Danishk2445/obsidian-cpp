#pragma once

#include <QTextEdit>

namespace oc {

// Obsidian's inline title: the note's name shown as an editable H1 above the text (h1 size/weight,
// line-height 1.2, letter-spacing -0.015em, wraps). Enter or leaving it renames the file; Escape
// reverts. Newlines can't be typed or pasted.
class InlineTitle : public QTextEdit {
    Q_OBJECT
public:
    explicit InlineTitle(QWidget* parent = nullptr);

    void setTitle(const QString& title); // current name (no signal)
    QString title() const { return m_title; }
    QString editedText() const;
    void setEditable(bool editable);
    void beginRename(bool selectAll); // eState {rename: "all" | "end"}
    int bottomMargin() const;         // .inline-title margin-bottom: 0.5em

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

signals:
    void renameRequested(const QString& newName); // only when the text differs from the title
    void leaveToEditor();                          // Enter / Escape / Down at the end: focus the text

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void insertFromMimeData(const QMimeData* source) override;

private:
    void applyStyle();
    void commit();
    QString m_title;
    bool m_editable = true;
    int m_marginBottom = 13;
};

} // namespace oc
