#pragma once

#include <QCheckBox>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include <functional>

class QLabel;
class QVBoxLayout;
class QHBoxLayout;

namespace oc {

class IconButton;
class OverlayLayer;

// Obsidian's Modal (.modal-container.mod-dim > .modal-bg + .modal), shown in the window's
// OverlayLayer: a 560 px dialog centered over the dimmed window with a close button (top right), a
// title, content (paragraphs or any widget) and a right-aligned button row that may start with a
// checkbox (".modal-button-container > label.mod-checkbox"), like Obsidian's confirmation modals.
//
// Clicking the dimmed background, the close button or pressing Escape dismisses it (result -1);
// clicking a button closes it with that button's index (unless added with closes = false). Enter
// clicks the focused button, else the default button (the last Cta/Warning button unless set). Tab
// cycles the focus within the modal. The modal deletes itself (later) once closed.
//
// Asynchronous use: connect buttonClicked()/closed() (or use ConfirmModal::ask). Synchronous callers
// (e.g. FileManager::setConfirmLinkUpdate) can use exec(), which runs a local event loop.
class Modal : public QWidget {
    Q_OBJECT
public:
    // Obsidian's button classes: plain, mod-cta, mod-warning (= mod-cta.mod-destructive: red with white
    // text, e.g. "Delete") and mod-destructive alone (red text on a faint red background).
    enum class ButtonStyle { Normal, Cta, Warning, Destructive };

    explicit Modal(OverlayLayer* layer);
    ~Modal() override;

    void setTitle(const QString& title);
    QString title() const;

    // Content: Obsidian's <p> elements; warning paragraphs use --text-warning (p.mod-warning).
    void addParagraph(const QString& text, bool warning = false);
    // Replaces the content with paragraphs split at blank lines ("\n\n").
    void setText(const QString& text);
    QStringList paragraphs() const;
    // Any widget as (additional) content, below the paragraphs; the modal takes ownership.
    void addContentWidget(QWidget* widget);
    QWidget* contentArea() const { return m_content; }

    int addButton(const QString& text, ButtonStyle style = ButtonStyle::Normal, bool closes = true);
    // A button painted like the modal's (Obsidian's <button>), for content that has its own buttons.
    static QPushButton* createButton(const QString& text, ButtonStyle style, QWidget* parent);
    QPushButton* button(int index) const;
    int buttonCount() const { return int(m_buttons.size()); }
    void setDefaultButton(int index); // Enter target and initial focus
    int defaultButton() const;

    // A checkbox left of the buttons ("Don't ask again").
    void setCheckbox(const QString& text, bool checked = false);
    QCheckBox* checkbox() const { return m_checkbox; }
    bool isChecked() const;

    void setDialogWidth(int px); // default --dialog-width (560), at most 80% of the window
    // Obsidian's .modal.mod-sidebar-layout (settings, file recovery): no padding, the frame is
    // --modal-width x --modal-height (90vw x 85vh, at most 1100 x 1000), the title and button row are
    // hidden and the content area fills the frame; the close button stays in the top-right corner.
    void setSidebarLayout(bool on);
    bool isSidebarLayout() const { return m_sidebarLayout; }

    void open();
    void close(int result = -1);
    bool isOpen() const { return m_open; }
    int result() const { return m_result; }
    // open() + a local event loop until closed; returns result().
    int exec();

    QRect dialogGeometry() const; // the .modal box, in overlay coordinates
    IconButton* closeButton() const { return m_close; }

signals:
    void buttonClicked(int index);
    void closed(int result);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool focusNextPrevChild(bool next) override; // Tab stays inside the modal

private:
    void relayout();
    void focusInitial();
    bool handleKey(QKeyEvent* e);
    QList<QWidget*> focusChain() const;

    QPointer<OverlayLayer> m_layer;
    QWidget* m_frame = nullptr;
    IconButton* m_close = nullptr;
    QLabel* m_title = nullptr;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QWidget* m_buttonRow = nullptr;
    QHBoxLayout* m_buttonLayout = nullptr;
    QCheckBox* m_checkbox = nullptr;
    QList<QPushButton*> m_buttons;
    QList<bool> m_closes;
    int m_default = -1;
    int m_width = 560;
    int m_result = -1;
    bool m_open = false;
    bool m_sidebarLayout = false;
};

// One button of a ConfirmModal.
struct ModalButton {
    QString text;
    Modal::ButtonStyle style = Modal::ButtonStyle::Normal;
};

// Obsidian's confirmation prompt in one call: title, text (paragraphs split at blank lines),
// buttons (left to right) and an optional checkbox. The callback receives the clicked button's index
// (-1 when dismissed) and the checkbox state.
class ConfirmModal {
public:
    using Callback = std::function<void(int button, bool checked)>;
    static Modal* ask(OverlayLayer* layer, const QString& title, const QString& text, const QList<ModalButton>& buttons,
                      const QString& checkboxText = {}, Callback callback = {});
    // Synchronous variant (local event loop); returns the button index or -1.
    static int exec(OverlayLayer* layer, const QString& title, const QString& text, const QList<ModalButton>& buttons,
                    const QString& checkboxText = {}, bool* checked = nullptr);
};

} // namespace oc
