#include "ui/overlay/text_prompt_modal.h"

#include "core/files/file_manager.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_tree.h"

#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextBlock>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace oc {

TextPromptModal::TextPromptModal(OverlayLayer* layer, const QString& title, const QString& text)
    : Modal(layer)
{
    setObjectName(u"ocTextPromptModal"_s);
    setTitle(title);
    m_input = new QPlainTextEdit;
    m_input->setObjectName(u"ocPromptInput"_s);
    m_input->setProperty("ocInput", true);
    m_input->setPlainText(text);
    m_input->setTabChangesFocus(true);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_input->installEventFilter(this);
    addContentWidget(m_input);
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
        fitHeight();
        showError(validate ? validate(m_input->toPlainText().trimmed()) : QString());
    });
    const int save = addButton(tr("Save"), ButtonStyle::Cta, false);
    addButton(tr("Cancel"));
    setDefaultButton(save);
    connect(this, &Modal::buttonClicked, this, [this, save](int index) {
        if (index == save)
            submit();
    });
    // Obsidian selects the text and focuses the field when the modal opens.
    QTimer::singleShot(0, this, [this] {
        m_input->selectAll();
        m_input->setFocus(Qt::OtherFocusReason);
        fitHeight();
    });
}

bool TextPromptModal::submit()
{
    const QString text = m_input->toPlainText().trimmed();
    const QString err = validate ? validate(text) : QString();
    if (!err.isEmpty()) {
        showError(err);
        return false;
    }
    showError({});
    auto done = onSubmit;
    close(0);
    if (done)
        done(text);
    return true;
}

bool TextPromptModal::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(ev);
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(k->modifiers() & Qt::ShiftModifier)) {
            submit(); // a textarea, but Enter never inserts a line break
            return true;
        }
    }
    if (obj == m_input && ev->type() == QEvent::Resize)
        QTimer::singleShot(0, this, [this] { fitHeight(); });
    return Modal::eventFilter(obj, ev);
}

void TextPromptModal::fitHeight()
{
    // rows = 1, height = scrollHeight: grow with wrapped lines.
    const QFontMetrics fm(m_input->font());
    int lines = 0;
    for (QTextBlock b = m_input->document()->begin(); b.isValid(); b = b.next())
        lines += b.layout() ? std::max(1, b.layout()->lineCount()) : 1;
    const QMargins m = m_input->contentsMargins();
    const int h = std::max(1, lines) * fm.lineSpacing() + int(m_input->document()->documentMargin() * 2) + m.top() + m.bottom();
    m_input->setFixedHeight(std::max(h, 30));
}

void TextPromptModal::showError(const QString& message)
{
    m_error = message;
    if (message.isEmpty()) {
        if (m_tooltip)
            m_tooltip->dismiss();
        return;
    }
    if (!m_tooltip)
        m_tooltip = new ErrorTooltip(window());
    const QRect anchor(m_input->mapToGlobal(QPoint(0, 0)), m_input->size());
    m_tooltip->showAt(message, anchor);
}

TextPromptModal* promptForFileRename(MainWindow* window, TAbstractFile* item)
{
    VaultSession* session = window ? window->session() : nullptr;
    if (!session || !session->vault() || !item || !item->parent())
        return nullptr;
    const bool markdown = !item->isFolder() && static_cast<TFile*>(item)->extension() == u"md";
    const QString current = item->isFolder() ? item->name() : static_cast<TFile*>(item)->basename();
    auto* m = new TextPromptModal(window->overlay(), markdown ? QObject::tr("Note title") : QObject::tr("File name"), current);
    // TAbstractFile isn't a QObject: look the item up again by path (it may have moved meanwhile).
    QPointer<MainWindow> w = window;
    const QString originalPath = item->path();
    auto itemOf = [w, originalPath]() -> TAbstractFile* {
        return w && w->session() && w->session()->vault() ? w->session()->vault()->item(originalPath) : nullptr;
    };
    m->validate = [w, itemOf](const QString& name) -> QString {
        TAbstractFile* target = itemOf();
        return target ? validateFileName(w->session()->vault(), target, name, true) : QString();
    };
    m->onSubmit = [w, itemOf](const QString& name) {
        TAbstractFile* target = itemOf();
        if (!target)
            return;
        const QString path = pathAfterRename(target, name);
        if (path == target->path())
            return;
        QString err;
        VaultSession* s = w->session();
        FileManager* fm = s->fileManager();
        const bool ok = fm ? fm->renameFile(target, path, &err) : s->vault()->rename(target, path, &err);
        if (w->notices())
            w->notices()->show(ok ? QObject::tr("Successfully renamed file.") : err);
    };
    m->open();
    return m;
}

} // namespace oc
