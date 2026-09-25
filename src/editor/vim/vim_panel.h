#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QLineEdit;

namespace oc {
class EditorView;
}

namespace oc::vim {

// CodeMirror-vim's bottom panel as Obsidian styles it (.cm-panels-bottom + .cm-vim-panel): docked
// at the bottom of the editor (EditorView::setBottomPanel), one "dialog" at a time — the ":" / "/"
// prompt (prefix, a borderless input, a grey description), a red notification (closes after 5 s
// or on click; "long" ones wait for the next key), or the "(recording)[q]" indicator. Hidden when
// empty.
class VimPanel : public QWidget {
    Q_OBJECT
public:
    // close(value) replaces the input text; close(nullopt) closes the prompt.
    using Close = std::function<void(const std::optional<QString>&)>;
    // A key typed in the prompt (vim key notation). onKeyDown returning true swallows it; else
    // <CR> submits (onEnter + close) and <Esc> closes. onKeyUp runs after the input handled it.
    using KeyFn = std::function<bool(const QString& key, const QString& value, const Close& close)>;

    explicit VimPanel(EditorView* view);
    ~VimPanel() override;

    void openPrompt(const QString& prefix, const QString& desc, const QString& value, bool selectValue, std::function<void(const QString&)> onEnter,
                    KeyFn onKeyDown, KeyFn onKeyUp, bool prefixIsRich = false);
    void closePrompt(); // without submitting
    bool isPromptOpen() const { return m_kind == Kind::Prompt; }
    void showNotification(const QString& text, bool isLong);
    void closeNotification();
    void showRecording(const QString& text);
    void closeRecording();

    QLineEdit* input() const { return m_input; }
    QString displayText() const; // prefix + input text, or the message (tests)
    bool isEmpty() const { return m_kind == Kind::None; }

    QSize sizeHint() const override;

signals:
    void promptClosed();

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    enum class Kind { None, Prompt, Notification, Recording };
    void setKind(Kind k);
    void applyTheme();
    void clearDialog();

    QPointer<EditorView> m_view;
    Kind m_kind = Kind::None;
    QLabel* m_prefix;
    QLineEdit* m_input;
    QLabel* m_desc;
    QLabel* m_message;
    QTimer m_notifyTimer;
    std::function<void(const QString&)> m_onEnter;
    KeyFn m_onKeyDown, m_onKeyUp;
    bool m_closing = false;
};

} // namespace oc::vim
