#include "ui/views/markdown/editor_search_bar.h"

#include "editor/editor_view.h"
#include "editor/search/editor_search.h"
#include "render/theme/theme.h"
#include "ui/widgets/icon_button.h"
#include "ui/widgets/search_input.h"

#include <QBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>

namespace oc {

namespace {
constexpr int kMaxRowWidth = 700; // --file-line-width

// A row centered at max 700px with 8px side padding and 8px gaps (.document-search / .document-replace).
class CenteredRow : public QWidget {
public:
    explicit CenteredRow(QWidget* parent)
        : QWidget(parent)
    {
        auto* outer = new QHBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addStretch(1);
        inner = new QWidget(this);
        inner->setMaximumWidth(kMaxRowWidth);
        box = new QHBoxLayout(inner);
        box->setContentsMargins(8, 0, 8, 0);
        box->setSpacing(8);
        outer->addWidget(inner, 100);
        outer->addStretch(1);
    }
    QWidget* inner;
    QHBoxLayout* box;
};

IconButton* button(const QString& icon, const QString& tip, QWidget* parent)
{
    auto* b = new IconButton(icon, tip, parent);
    b->setIconSize(16);
    b->setButtonSize(QSize(26, 26));
    return b;
}
} // namespace

EditorSearchBar::EditorSearchBar(EditorView* editor, QWidget* parent)
    : QWidget(parent)
    , m_editor(editor)
    , m_search(new EditorSearch(editor, this))
{
    setObjectName(QStringLiteral("ocDocumentSearch"));
    auto* v = new QVBoxLayout(this);
    // .document-search-container: padding 8px 0; margin 0 16px; gap 8px
    v->setContentsMargins(16, 8, 16, 8);
    v->setSpacing(8);

    auto* findRow = new CenteredRow(this);
    m_find = new SearchInput(findRow->inner);
    m_find->setPlaceholderText(tr("Find..."));
    m_find->setClearButtonEnabled(false);
    m_find->installEventFilter(this);
    findRow->box->addWidget(m_find, 1);
    auto* buttons = new QWidget(findRow->inner);
    auto* bl = new QHBoxLayout(buttons);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(4);
    m_prev = button(QStringLiteral("arrow-up"), tr("Previous") + QStringLiteral("\nShift + F3"), buttons);
    m_next = button(QStringLiteral("arrow-down"), tr("Next") + QStringLiteral("\nF3"), buttons);
    m_all = button(QStringLiteral("text-select"), tr("Find all") + QStringLiteral("\nAlt + Enter"), buttons);
    bl->addWidget(m_prev);
    bl->addWidget(m_next);
    bl->addWidget(m_all);
    findRow->box->addWidget(buttons);
    m_close = button(QStringLiteral("x"), tr("Exit search"), findRow->inner);
    findRow->box->addWidget(m_close);
    v->addWidget(findRow);

    auto* replaceRow = new CenteredRow(this);
    m_replaceRow = replaceRow;
    m_replaceInput = new QLineEdit(replaceRow->inner);
    m_replaceInput->setPlaceholderText(tr("Replace..."));
    m_replaceInput->setFixedHeight(30);
    m_replaceInput->installEventFilter(this);
    replaceRow->box->addWidget(m_replaceInput, 1);
    m_replaceOne = button(QStringLiteral("replace"), tr("Replace") + QStringLiteral("\nEnter"), replaceRow->inner);
    m_replaceAll = button(QStringLiteral("replace-all"), tr("Replace all") + QStringLiteral("\nCtrl + Alt + Enter"), replaceRow->inner);
    replaceRow->box->addWidget(m_replaceOne);
    replaceRow->box->addWidget(m_replaceAll);
    v->addWidget(replaceRow);
    m_replaceRow->hide();

    connect(m_find, &QLineEdit::textChanged, this, &EditorSearchBar::onQueryChanged);
    connect(m_prev, &QAbstractButton::clicked, this, &EditorSearchBar::findPrevious);
    connect(m_next, &QAbstractButton::clicked, this, &EditorSearchBar::findNext);
    connect(m_all, &QAbstractButton::clicked, this, [this] { m_search->findAll(); });
    connect(m_close, &QAbstractButton::clicked, this, &EditorSearchBar::close);
    connect(m_replaceOne, &QAbstractButton::clicked, this, [this] { m_search->replaceCurrent(m_replaceInput->text()); });
    connect(m_replaceAll, &QAbstractButton::clicked, this, [this] { m_search->replaceAll(m_replaceInput->text()); });
    m_countTimer.setSingleShot(true);
    m_countTimer.setInterval(20); // Obsidian debounces the counter (20 ms)
    connect(&m_countTimer, &QTimer::timeout, this, &EditorSearchBar::updateCount);
    connect(m_search, &EditorSearch::resultsChanged, this, [this] { m_countTimer.start(); });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    hide();
}

EditorSearchBar::~EditorSearchBar() = default;

void EditorSearchBar::paintEvent(QPaintEvent*)
{
    // .mod-active .document-search-container { background-color: --background-primary }
    QPainter p(this);
    p.fillRect(rect(), currentTheme()->color(QStringLiteral("--background-primary")));
}

void EditorSearchBar::open(bool replace)
{
    m_replace = replace;
    m_replaceRow->setVisible(replace);
    show();
    if (m_editor) {
        QString sel = m_editor->selectedText();
        sel.remove(QLatin1Char('\n')).remove(QLatin1Char('\r')); // like an <input type=text>
        if (!sel.isEmpty()) {
            const QSignalBlocker b(m_find);
            m_find->setText(sel);
        }
    }
    m_find->setFocus();
    m_find->selectAll();
    onQueryChanged();
}

void EditorSearchBar::close()
{
    if (isHidden())
        return;
    // Escape from an input leaves the current match selected (Obsidian).
    const bool inputFocused = m_find->hasFocus() || m_replaceInput->hasFocus();
    if (inputFocused && m_search->current())
        m_search->selectCurrent();
    m_search->clear();
    {
        const QSignalBlocker b(m_find);
        m_find->clear();
    }
    m_find->setNoMatch(false);
    m_find->setDecorator({});
    hide();
    if (m_editor)
        m_editor->setFocus();
    emit closed();
}

void EditorSearchBar::onQueryChanged()
{
    const QString q = m_find->text();
    const bool found = m_search->setQuery(SearchQuery{q});
    m_find->setNoMatch(!q.isEmpty() && !found);
    m_countTimer.start();
}

void EditorSearchBar::findNext()
{
    m_search->findNext();
}

void EditorSearchBar::findPrevious()
{
    m_search->findPrevious();
}

QString EditorSearchBar::countText() const { return m_find->decorator(); }

void EditorSearchBar::updateCount()
{
    if (m_find->text().isEmpty()) {
        m_find->setDecorator({});
        return;
    }
    const auto [index, count] = m_search->indexAndCount();
    m_find->setDecorator(QStringLiteral("%1 / %2").arg(index).arg(count));
}

bool EditorSearchBar::handleKey(QKeyEvent* e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    const bool inReplace = m_replaceInput->hasFocus();
    switch (e->key()) {
    case Qt::Key_F3:
        shift ? findPrevious() : findNext();
        return true;
    case Qt::Key_G:
        if (ctrl && !alt) {
            shift ? findPrevious() : findNext();
            return true;
        }
        return false;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (ctrl && alt) {
            if (m_replace && inReplace)
                m_search->replaceAll(m_replaceInput->text());
            return true;
        }
        if (alt) {
            m_search->findAll();
            return true;
        }
        if (shift) {
            findPrevious();
            return true;
        }
        if (inReplace && !m_replaceInput->text().isEmpty())
            m_search->replaceCurrent(m_replaceInput->text()); // replaces, then moves on
        else
            findNext();
        return true;
    case Qt::Key_Escape:
        close();
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        if (m_replace) {
            (m_find->hasFocus() ? static_cast<QWidget*>(m_replaceInput) : m_find)->setFocus();
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool EditorSearchBar::eventFilter(QObject* obj, QEvent* ev)
{
    if ((obj == m_find || obj == m_replaceInput) && (ev->type() == QEvent::KeyPress || ev->type() == QEvent::ShortcutOverride)) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ev->type() == QEvent::ShortcutOverride) {
            // keep our keys away from global hotkeys while typing in the bar
            switch (ke->key()) {
            case Qt::Key_F3:
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Escape:
                ke->accept();
                return true;
            default:
                return false;
            }
        }
        if (handleKey(ke))
            return true;
    }
    return QWidget::eventFilter(obj, ev);
}

} // namespace oc
