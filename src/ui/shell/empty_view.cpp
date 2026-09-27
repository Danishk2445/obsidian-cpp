#include "ui/shell/empty_view.h"

#include "core/commands/command_registry.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/workspace/workspace.h"

#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

namespace oc {

namespace {
// .empty-state-action: 16px, line-height 36px, --text-accent (hover --text-accent-hover).
class ActionLabel : public QLabel {
public:
    ActionLabel(const QString& text, QWidget* parent)
        : QLabel(text, parent)
    {
        setAlignment(Qt::AlignCenter);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    std::function<void()> onClick;
    void restyle()
    {
        const ThemePtr t = currentTheme();
        setFont(t->interfaceFont(t->textSize())); // --font-text-size, UI font family
        setFixedHeight(36);
        const QColor c = t->color(m_hover ? QStringLiteral("--text-accent-hover") : QStringLiteral("--text-accent"));
        setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(c.name(QColor::HexArgb)));
    }

protected:
    void enterEvent(QEnterEvent* e) override
    {
        m_hover = true;
        restyle();
        QLabel::enterEvent(e);
    }
    void leaveEvent(QEvent* e) override
    {
        m_hover = false;
        restyle();
        QLabel::leaveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && onClick)
            onClick();
    }

private:
    bool m_hover = false;
};
} // namespace

EmptyView::EmptyView(WorkspaceLeaf* leaf, QWidget* parent)
    : View(leaf, parent)
{
    setObjectName(QStringLiteral("ocEmptyView"));
    // .empty-state-container: centered, max-width 480px, margin 20px; .empty-state-title: margin 20px 0;
    // the description follows the title; .empty-state-action-list: margin-top 20px.
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 20, 20, 20);
    v->setSpacing(0);
    v->addStretch(1);
    m_title = new QLabel(tr("No file is open"), this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    m_title->setContentsMargins(0, 20, 0, 0);
    v->addWidget(m_title);
    v->addSpacing(20);
    m_description = new QLabel(this);
    m_description->setAlignment(Qt::AlignCenter);
    m_description->setWordWrap(true);
    m_description->setContentsMargins(0, 0, 0, 20);
    m_description->hide();
    v->addWidget(m_description);
    m_actionList = new QVBoxLayout;
    m_actionList->setContentsMargins(0, 0, 0, 0);
    m_actionList->setSpacing(0);
    v->addLayout(m_actionList);
    // Obsidian 1.13: "Create new note", "Go to file" (non-empty vaults) and "Close".
    addAction(tr("Create new note (%1)").arg(QStringLiteral("Ctrl + N")), [this] { runCommand(QStringLiteral("file-explorer:new-file")); });
    m_goToFile = addAction(tr("Go to file (%1)").arg(QStringLiteral("Ctrl + O")), [this] { runCommand(QStringLiteral("switcher:open")); });
    addAction(tr("Close"), [this] {
        if (WorkspaceLeaf* l = this->leaf())
            l->detach();
    });
    v->addStretch(1);
    restyle();
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, &EmptyView::restyle);
}

QString EmptyView::displayText() const { return tr("New tab"); }

void EmptyView::onOpen()
{
    // "Go to file" only when the vault has something in it (Obsidian's vault.isEmpty(): nothing but the root).
    const Vault* vault = session() ? session()->vault() : nullptr;
    if (m_goToFile && vault)
        m_goToFile->setHidden(vault->fileCount() == 0 && vault->folders().isEmpty());
}

void EmptyView::runCommand(const QString& id)
{
    emit actionTriggered(id);
    // Like Obsidian's executeCommandById. Queued: the command may replace this view (the new note opens in
    // this empty tab) while the clicked label is still in its mouse handler.
    VaultSession* s = session();
    QPointer<CommandRegistry> commands = s ? s->commands() : nullptr;
    if (commands)
        QTimer::singleShot(0, commands, [commands, id] { commands->execute(id); });
}

void EmptyView::resizeEvent(QResizeEvent* e)
{
    // The content column is at most 480px wide (plus the container's 20px margins), centered.
    const int side = std::max(20, (width() - 480) / 2);
    layout()->setContentsMargins(side, 20, side, 20);
    View::resizeEvent(e);
}

QStringList EmptyView::actionTexts() const
{
    QStringList out;
    for (const QLabel* l : m_actions)
        if (!l->isHidden())
            out.push_back(l->text());
    return out;
}

void EmptyView::setTitle(const QString& title) { m_title->setText(title); }

void EmptyView::setDescription(const QString& description, const QString& tooltip)
{
    m_description->setText(description);
    m_description->setToolTip(tooltip);
    m_description->setVisible(!description.isEmpty());
}

void EmptyView::clearActions()
{
    qDeleteAll(m_actions);
    m_actions.clear();
}

QLabel* EmptyView::addAction(const QString& text, std::function<void()> onClick)
{
    auto* l = new ActionLabel(text, this);
    l->onClick = std::move(onClick);
    l->restyle();
    m_actionList->addWidget(l);
    m_actions.push_back(l);
    return l;
}

void EmptyView::restyle()
{
    const ThemePtr t = currentTheme();
    // .empty-state-title: --h2-size (1.462em of the 15px UI font), --h2-weight, line-height 1.3.
    const double uiPx = t->px(QStringLiteral("--font-ui-medium"), -1, 15);
    QFont tf = t->interfaceFont(t->px(QStringLiteral("--h2-size"), uiPx, uiPx * 1.462));
    const double weight = t->number(QStringLiteral("--h2-weight"), 600);
    tf.setWeight(QFont::Weight(std::clamp(int(weight), 100, 900)));
    m_title->setFont(tf);
    const QString normal = t->color(QStringLiteral("--text-normal")).name(QColor::HexArgb);
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(normal));
    m_description->setFont(t->interfaceFont(uiPx));
    m_description->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(normal));
    for (QLabel* l : std::as_const(m_actions))
        static_cast<ActionLabel*>(l)->restyle();
}

} // namespace oc
