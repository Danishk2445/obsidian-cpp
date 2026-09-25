#pragma once

#include "ui/overlay/modal.h"

#include <functional>

class QPlainTextEdit;

namespace oc {

class ErrorTooltip;
class MainWindow;
class OverlayLayer;
class TAbstractFile;

// Obsidian's one-field prompt (.modal.mod-file-rename): a title, a one-row auto-growing text area
// (Enter submits, the text starts selected) and Save / Cancel. Every edit is validated; a failing
// validation shows its message in an error tooltip under the field, and a submit with an error keeps
// the modal open. Used for "Rename file", "Rename heading" and "Rename block ID".
class TextPromptModal : public Modal {
    Q_OBJECT
public:
    TextPromptModal(OverlayLayer* layer, const QString& title, const QString& text);

    // "" = valid. Runs on each edit and on submit (with the trimmed text).
    std::function<QString(const QString& text)> validate;
    // Runs after a successful validation, once the modal has closed.
    std::function<void(const QString& text)> onSubmit;

    QPlainTextEdit* input() const { return m_input; }
    QString errorMessage() const { return m_error; }
    // Save: validates the trimmed text, then closes and calls onSubmit. False when it stays open.
    bool submit();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void showError(const QString& message);
    void fitHeight();

    QPlainTextEdit* m_input;
    ErrorTooltip* m_tooltip = nullptr;
    QString m_error;
};

// Obsidian's fileManager.promptForFileRename(): "Note title" (Markdown) / "File name" with the base
// name, validated like the file explorer (characters, dot files, duplicates), then FileManager's
// rename (links updated per "alwaysUpdateLinks") and "Successfully renamed file.".
TextPromptModal* promptForFileRename(MainWindow* window, TAbstractFile* item);

} // namespace oc
