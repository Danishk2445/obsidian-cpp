#include "editor/vim/vim_panel.h"

#include "editor/vim/vim_keys.h"

#include "editor/editor_view.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QTextDocumentFragment>

namespace oc::vim {

namespace {
// CSS "font-family: monospace" (Chromium resolves the generic family through fontconfig)
QFont monospaceFont(double px)
{
    QFont f(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    f.setPixelSize(std::max(1, int(std::lround(px))));
    return f;
}
} // namespace

VimPanel::VimPanel(EditorView* view)
    : QWidget(nullptr)
    , m_view(view)
    , m_prefix(new QLabel(this))
    , m_input(new QLineEdit(this))
    , m_desc(new QLabel(this))
    , m_message(new QLabel(this))
{
    setObjectName(QStringLiteral("ocVimPanel"));
    setAttribute(Qt::WA_StyledBackground, false);
    auto* row = new QHBoxLayout(this);
    // .cm-vim-panel { padding: 0 10px }
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(0);
    m_prefix->setObjectName(QStringLiteral("ocVimPanelPrefix"));
    m_prefix->setTextFormat(Qt::PlainText);
    m_input->setObjectName(QStringLiteral("ocVimPanelInput"));
    m_input->setFrame(false);
    m_input->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_input->installEventFilter(this);
    m_desc->setObjectName(QStringLiteral("ocVimPanelDesc"));
    m_message->setObjectName(QStringLiteral("ocVimPanelMessage"));
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(false);
    row->addWidget(m_prefix);
    row->addWidget(m_input, 1);
    row->addWidget(m_desc);
    row->addWidget(m_message, 1);
    m_notifyTimer.setSingleShot(true);
    connect(&m_notifyTimer, &QTimer::timeout, this, &VimPanel::closeNotification);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, &VimPanel::applyTheme);
    applyTheme();
    setKind(Kind::None);
    if (view)
        view->setBottomPanel(this);
}

VimPanel::~VimPanel()
{
    if (m_view && m_view->bottomPanel() == this) {
        hide();
        m_view->setBottomPanel(nullptr);
    }
}

void VimPanel::applyTheme()
{
    const ThemePtr t = currentTheme();
    const double textPx = t ? t->textSize() : 16;
    const QFont mono = monospaceFont(textPx);
    m_prefix->setFont(mono);
    m_message->setFont(mono);
    m_desc->setFont(mono);
    // the input keeps Obsidian's input font size (--font-ui-small) in the inherited monospace
    const double smallPx = t ? t->px(QStringLiteral("--font-ui-small"), -1, 13) : 13;
    m_input->setFont(monospaceFont(smallPx));
    const QColor text = t ? t->color(QStringLiteral("--text-normal")) : QColor(Qt::black);
    const QColor accent = t ? t->color(QStringLiteral("--text-selection")) : QColor(Qt::blue);
    // .cm-vim-panel input { background: transparent; border: none; outline: none }, with the
    // input padding (--input-padding: 4px 8px)
    m_input->setStyleSheet(QStringLiteral("QLineEdit#ocVimPanelInput { background: transparent; border: none; padding: 4px 8px; color: %1;"
                                          " selection-background-color: %2; }")
                               .arg(text.name(QColor::HexArgb), accent.name(QColor::HexArgb)));
    m_prefix->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(text.name(QColor::HexArgb)));
    m_desc->setStyleSheet(QStringLiteral("color: #888888; background: transparent;"));
    m_message->setStyleSheet(QStringLiteral("color: red; background: transparent;"));
    update();
}

QSize VimPanel::sizeHint() const
{
    // .cm-vim-panel { min-height: 1.3em } plus the top border
    const ThemePtr t = currentTheme();
    const double textPx = t ? t->textSize() : 16;
    int h = int(std::ceil(textPx * 1.3));
    if (m_kind == Kind::Prompt)
        h = std::max(h, m_input->sizeHint().height());
    else if (m_kind != Kind::None)
        h = std::max(h, m_message->sizeHint().height());
    return QSize(200, h + 1);
}

void VimPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const ThemePtr t = currentTheme();
    // .cm-panels { background: inherit } (the editor) + .cm-panels-bottom { border-top }
    p.fillRect(rect(), t ? t->color(QStringLiteral("--background-primary")) : palette().window().color());
    p.fillRect(QRect(0, 0, width(), 1), t ? t->color(QStringLiteral("--background-modifier-border")) : palette().mid().color());
}

void VimPanel::setKind(Kind k)
{
    m_kind = k;
    const bool prompt = k == Kind::Prompt;
    m_prefix->setVisible(prompt);
    m_input->setVisible(prompt);
    m_desc->setVisible(prompt && !m_desc->text().isEmpty());
    m_message->setVisible(k == Kind::Notification || k == Kind::Recording);
    if (k != Kind::Notification)
        m_notifyTimer.stop();
    setVisible(k != Kind::None);
    updateGeometry();
    if (m_view)
        m_view->updateBottomPanel();
}

void VimPanel::clearDialog()
{
    m_onEnter = {};
    m_onKeyDown = {};
    m_onKeyUp = {};
}

void VimPanel::openPrompt(const QString& prefix, const QString& desc, const QString& value, bool selectValue,
                          std::function<void(const QString&)> onEnter, KeyFn onKeyDown, KeyFn onKeyUp, bool prefixIsRich)
{
    m_onEnter = std::move(onEnter);
    m_onKeyDown = std::move(onKeyDown);
    m_onKeyUp = std::move(onKeyUp);
    m_prefix->setTextFormat(prefixIsRich ? Qt::RichText : Qt::PlainText);
    m_prefix->setText(prefix);
    m_desc->setText(desc);
    m_input->setText(value);
    // the replace confirmation (":s///c") has no input to type in
    m_input->setReadOnly(prefixIsRich);
    setKind(Kind::Prompt);
    if (selectValue && !value.isEmpty())
        m_input->selectAll();
    else
        m_input->setCursorPosition(int(value.size()));
    m_input->setFocus(Qt::OtherFocusReason);
}

void VimPanel::closePrompt()
{
    if (m_kind != Kind::Prompt)
        return;
    const bool hadFocus = m_input->hasFocus();
    clearDialog();
    setKind(Kind::None);
    if (m_view && hadFocus)
        m_view->setFocus(Qt::OtherFocusReason);
    emit promptClosed();
}

void VimPanel::showNotification(const QString& text, bool isLong)
{
    const bool hadFocus = m_input->hasFocus();
    clearDialog();
    // .cm-vim-message { color: red; white-space: pre }
    m_message->setStyleSheet(QStringLiteral("color: red; background: transparent;"));
    m_message->setText(isLong ? text + QStringLiteral("\nPress ENTER or type command to continue") : text);
    setKind(Kind::Notification);
    if (!isLong)
        m_notifyTimer.start(5000);
    if (hadFocus && m_view)
        m_view->setFocus(Qt::OtherFocusReason);
}

void VimPanel::closeNotification()
{
    if (m_kind == Kind::Notification)
        setKind(Kind::None);
}

void VimPanel::showRecording(const QString& text)
{
    clearDialog();
    const ThemePtr t = currentTheme();
    m_message->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg((t ? t->color(QStringLiteral("--text-normal")) : QColor(Qt::black)).name(QColor::HexArgb)));
    m_message->setText(text);
    setKind(Kind::Recording);
}

void VimPanel::closeRecording()
{
    if (m_kind == Kind::Recording)
        setKind(Kind::None);
}

QString VimPanel::displayText() const
{
    switch (m_kind) {
    case Kind::Prompt:
        return (m_prefix->textFormat() == Qt::RichText ? QTextDocumentFragment::fromHtml(m_prefix->text()).toPlainText() : m_prefix->text())
               + m_input->text();
    case Kind::Notification:
    case Kind::Recording:
        return m_message->text();
    case Kind::None:
        break;
    }
    return {};
}

void VimPanel::mousePressEvent(QMouseEvent* e)
{
    // a click closes a notification
    if (m_kind == Kind::Notification)
        closeNotification();
    QWidget::mousePressEvent(e);
}

bool VimPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_input || m_kind != Kind::Prompt)
        return QWidget::eventFilter(obj, ev);
    if (ev->type() == QEvent::FocusOut) {
        // closeOnBlur (not for window switches or popups)
        const auto reason = static_cast<QFocusEvent*>(ev)->reason();
        if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
            QTimer::singleShot(0, this, [this] {
                if (m_kind == Kind::Prompt && !m_input->hasFocus() && !m_closing) {
                    clearDialog();
                    setKind(Kind::None);
                    emit promptClosed();
                }
            });
        }
        return false;
    }
    if (ev->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const QString key = keyFromEvent(ke);
        // the prompt keeps the keys it handles (Esc, Enter, history, Ctrl+U ...)
        if (key == u"<Esc>" || key == u"<CR>" || key == u"<C-u>" || key == u"<C-c>" || key == u"<C-[>" || key == u"<Up>" || key == u"<Down>") {
            ke->accept();
            return true;
        }
        return false;
    }
    if (ev->type() != QEvent::KeyPress)
        return QWidget::eventFilter(obj, ev);
    auto* ke = static_cast<QKeyEvent*>(ev);
    const QString key = keyFromEvent(ke);
    if (key.isEmpty())
        return false;
    // Every key typed in the prompt stays in the prompt: an ignored key event would propagate to
    // the editor (the parent) and run as a vim key.
    struct AcceptOnExit {
        QKeyEvent* e;
        ~AcceptOnExit() { e->accept(); }
    } acceptOnExit{ke};
    // the handlers may close or replace the prompt: keep them alive while they run
    const KeyFn onKeyDown = m_onKeyDown;
    const KeyFn onKeyUp = m_onKeyUp;
    const auto onEnter = m_onEnter;
    bool closed = false;
    Close close = [this, &closed](const std::optional<QString>& v) {
        if (v) {
            m_input->setText(*v);
            m_input->setCursorPosition(int(v->size()));
        } else if (m_kind == Kind::Prompt) {
            closed = true;
            m_closing = true;
            closePrompt();
            m_closing = false;
        }
    };
    if (onKeyDown && onKeyDown(key, m_input->text(), close))
        return true;
    if (closed || m_kind != Kind::Prompt)
        return true;
    if (key == u"<CR>") {
        const QString value = m_input->text();
        m_closing = true;
        closePrompt(); // before the callback: it may open another dialog
        m_closing = false;
        if (onEnter)
            onEnter(value);
        return true;
    }
    if (key == u"<Esc>") {
        m_closing = true;
        closePrompt();
        m_closing = false;
        return true;
    }
    // let the input edit its text, then report the key
    static_cast<QObject*>(m_input)->event(ev);
    if (onKeyUp && m_kind == Kind::Prompt)
        onKeyUp(key, m_input->text(), close);
    return true;
}

} // namespace oc::vim
