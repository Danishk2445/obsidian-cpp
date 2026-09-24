#include "ui/settings/path_suggest.h"

#include "core/fuzzy/fuzzy.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/widgets/search_input.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace oc {

namespace {
constexpr int kMaxSuggestions = 100; // AbstractInputSuggest.MAX_SUGGESTIONS

double tokPx(const char* name, double fallback) { return currentTheme()->px(QString::fromLatin1(name), -1, fallback); }
QColor tok(const char* name, const QColor& fallback = {}) { return currentTheme()->color(QString::fromLatin1(name), fallback); }

struct Scored {
    double score;
    QString path;
    QList<QPair<int, int>> matches;
};

// Fuzzy-matches `paths` against `query` (all of them, unsorted, for an empty query), best first.
QList<Scored> rank(const QStringList& paths, const QString& query)
{
    QList<Scored> out;
    const FuzzyQuery q(query);
    for (const QString& p : paths) {
        if (q.isEmpty()) {
            out.push_back({0, p, {}});
            continue;
        }
        if (const auto m = q.match(p)) {
            Scored s{m->score, p, {}};
            for (const MatchRange& r : matchIndicesToRanges(m->matches))
                s.matches.append({r.from, r.to});
            out.push_back(std::move(s));
        }
        if (out.size() >= kMaxSuggestions && q.isEmpty())
            break;
    }
    if (!q.isEmpty())
        std::stable_sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    if (out.size() > kMaxSuggestions)
        out.resize(kMaxSuggestions);
    return out;
}

QStringList folderPaths(VaultSession* session, bool includeRoot)
{
    QStringList out;
    Vault* v = session ? session->vault() : nullptr;
    if (!v)
        return out;
    if (includeRoot)
        out << QStringLiteral("/");
    QStringList folders;
    for (TFolder* f : v->folders())
        if (f && !f->isRoot())
            folders << f->path();
    std::sort(folders.begin(), folders.end(), [](const QString& a, const QString& b) { return a.localeAwareCompare(b) < 0; });
    out += folders;
    return out;
}

QRect rectInLayer(QWidget* w, OverlayLayer* layer)
{
    return QRect(layer->mapFrom(layer->window(), w->mapTo(w->window(), QPoint(0, 0))), w->size());
}
} // namespace

// ---------------------------------------------------------------------------------------------
// FolderSuggest
// ---------------------------------------------------------------------------------------------

FolderSuggest::FolderSuggest(QLineEdit* input, VaultSession* session, OverlayLayer* layer, bool includeRoot)
    : QObject(input)
    , m_input(input)
    , m_session(session)
    , m_layer(layer)
    , m_includeRoot(includeRoot)
{
    input->installEventFilter(this);
    connect(input, &QLineEdit::textEdited, this, [this] {
        if (m_input && m_input->hasFocus())
            open();
        refresh();
    });
}

FolderSuggest::~FolderSuggest()
{
    qApp->removeEventFilter(this);
    delete m_popup;
}

QStringList FolderSuggest::suggestions(const QString& query) const
{
    QStringList out;
    for (const Scored& s : rank(folderPaths(m_session, m_includeRoot), query))
        out << s.path;
    return out;
}

bool FolderSuggest::isOpen() const { return m_popup && m_popup->isVisible(); }

void FolderSuggest::open()
{
    if (!m_layer || !m_input)
        return;
    if (!m_popup) {
        m_popup = new SuggestPopup(m_layer);
        connect(m_popup->list(), &SuggestionList::activated, this, [this](int index, Qt::KeyboardModifiers) { choose(index); });
    }
    qApp->installEventFilter(this);
    refresh();
}

void FolderSuggest::close()
{
    qApp->removeEventFilter(this);
    if (m_popup)
        m_popup->hide();
    if (m_layer)
        m_layer->refresh();
}

void FolderSuggest::refresh()
{
    if (!m_popup || !m_input)
        return;
    const QList<Scored> ranked = rank(folderPaths(m_session, m_includeRoot), m_input->text());
    QList<Suggestion> items;
    m_current.clear();
    for (const Scored& s : ranked) {
        Suggestion sug;
        sug.title = s.path;
        sug.titleMatches = s.matches;
        sug.data = s.path;
        items << sug;
        m_current << s.path;
    }
    if (items.isEmpty()) {
        m_popup->hide();
        if (m_layer)
            m_layer->refresh();
        return;
    }
    m_popup->list()->setSuggestions(items);
    m_popup->list()->setSelectedIndex(0);
    reposition();
}

void FolderSuggest::reposition()
{
    if (!m_popup || !m_input || !m_layer)
        return;
    m_popup->showAt(rectInLayer(m_input, m_layer));
    m_popup->raise();
}

void FolderSuggest::choose(int index)
{
    if (index < 0 || index >= m_current.size() || !m_input)
        return;
    const QString path = m_current[index];
    m_input->setText(path);
    close();
    emit chosen(path);
}

bool FolderSuggest::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_input) {
        switch (ev->type()) {
        case QEvent::FocusIn:
            open();
            break;
        case QEvent::FocusOut:
            close();
            break;
        case QEvent::KeyPress: {
            if (!isOpen())
                break;
            auto* ke = static_cast<QKeyEvent*>(ev);
            SuggestionList* list = m_popup->list();
            switch (ke->key()) {
            case Qt::Key_Down:
                list->setSelectedIndex(list->selectedIndex() + 1);
                return true;
            case Qt::Key_Up:
                list->setSelectedIndex(list->selectedIndex() - 1);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
                choose(list->selectedIndex());
                return true;
            case Qt::Key_Escape:
                close();
                return true;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        return false;
    }
    if (ev->type() == QEvent::MouseButtonPress && isOpen()) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w != m_input && !m_popup->isAncestorOf(w) && w != m_popup)
            close();
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// FilePicker
// ---------------------------------------------------------------------------------------------

namespace {
// The combobox's popup (.suggestion-container.combobox): a borderless search input over the list.
class ComboPopup : public QWidget {
public:
    explicit ComboPopup(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setObjectName(QStringLiteral("ocComboboxPopup"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double radius = tokPx("--radius-m", 8);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        p.fillPath(path, tok("--suggestion-background", tok("--background-primary")));
        p.setPen(QPen(tok("--background-modifier-border"), 1));
        p.drawPath(path);
    }
};
} // namespace

FilePicker::FilePicker(VaultSession* session, OverlayLayer* layer, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_layer(layer)
    , m_placeholder(tr("No file selected"))
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

FilePicker::~FilePicker()
{
    qApp->removeEventFilter(this);
    delete m_popup;
}

void FilePicker::setPlaceholder(const QString& text)
{
    m_placeholder = text;
    update();
}

void FilePicker::setValue(const QString& path)
{
    m_value = path;
    updateGeometry();
    update();
}

void FilePicker::choose(const QString& path)
{
    const bool changed = path != m_value;
    setValue(path);
    close();
    if (changed)
        emit valueChanged(path);
}

QStringList FilePicker::suggestions(const QString& query) const
{
    QStringList paths;
    if (Vault* v = m_session ? m_session->vault() : nullptr) {
        for (TFile* f : v->files())
            if (!m_filter || m_filter(f->path()))
                paths << f->path();
    }
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a.localeAwareCompare(b) < 0; });
    QStringList out;
    for (const Scored& s : rank(paths, query))
        out << s.path;
    return out;
}

bool FilePicker::isOpen() const { return m_popup && m_popup->isVisible(); }
SearchInput* FilePicker::searchInput() const { return m_search; }
SuggestionList* FilePicker::list() const { return m_list; }

void FilePicker::open()
{
    if (!m_layer || !isEnabled())
        return;
    if (!m_popup) {
        m_popup = new ComboPopup(m_layer);
        auto* v = new QVBoxLayout(m_popup);
        v->setContentsMargins(1, 3, 1, 1);
        v->setSpacing(0);
        m_search = new SearchInput(m_popup);
        m_search->setClearButtonEnabled(false);
        m_search->setPlaceholderText(tr("Start typing..."));
        m_search->setStyleSheet(QStringLiteral("QLineEdit { border: none; background: transparent; }"));
        v->addWidget(m_search);
        m_list = new SuggestionList(m_popup);
        m_list->setPadding(4);
        m_list->setFocusPolicy(Qt::NoFocus);
        v->addWidget(m_list, 1);
        m_search->installEventFilter(this);
        connect(m_search, &QLineEdit::textChanged, this, [this] { refresh(); });
        connect(m_list, &SuggestionList::activated, this, [this](int index, Qt::KeyboardModifiers) {
            if (index >= 0 && index < m_current.size())
                choose(m_current[index]);
        });
    }
    m_search->clear();
    refresh();
    m_popup->show();
    m_popup->raise();
    m_search->setFocus(Qt::PopupFocusReason);
    qApp->installEventFilter(this);
    m_layer->refresh();
}

void FilePicker::close()
{
    qApp->removeEventFilter(this);
    if (!m_popup || m_popup->isHidden())
        return;
    const bool hadFocus = m_popup->isAncestorOf(QApplication::focusWidget());
    m_popup->hide();
    if (m_layer)
        m_layer->refresh();
    if (hadFocus)
        setFocus(Qt::OtherFocusReason);
}

void FilePicker::refresh()
{
    if (!m_popup)
        return;
    QStringList paths;
    if (Vault* v = m_session ? m_session->vault() : nullptr) {
        for (TFile* f : v->files())
            if (!m_filter || m_filter(f->path()))
                paths << f->path();
    }
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a.localeAwareCompare(b) < 0; });
    QList<Suggestion> items;
    m_current.clear();
    for (const Scored& s : rank(paths, m_search->text())) {
        Suggestion sug;
        sug.title = s.path;
        sug.titleMatches = s.matches;
        items << sug;
        m_current << s.path;
    }
    m_list->setSuggestions(items, tr("No results found."));
    m_list->setSelectedIndex(std::max(0, int(m_current.indexOf(m_value))));
    // Below the button, as wide as the button (at least 300 px), at most 300 px of rows.
    const QRect anchor = rectInLayer(this, m_layer);
    const QRect bounds = m_layer->rect().adjusted(8, 8, -8, -8);
    const int w = std::clamp(std::max(anchor.width(), 300), 100, bounds.width());
    const int listH = std::min(m_list->contentHeightForWidth(w - 2, 300), 300);
    m_list->setFixedHeight(std::max(listH, 28));
    const int h = m_list->height() + int(tokPx("--input-height", 30)) + 5;
    int x = std::clamp(anchor.right() - w + 1, bounds.left(), std::max(bounds.left(), bounds.right() - w));
    int y = anchor.bottom() + 4;
    if (y + h > bounds.bottom() && anchor.top() - h - 4 >= bounds.top())
        y = anchor.top() - h - 4;
    m_popup->setGeometry(x, y, w, h);
}

bool FilePicker::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_search && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        switch (ke->key()) {
        case Qt::Key_Down:
            m_list->setSelectedIndex(m_list->selectedIndex() + 1);
            return true;
        case Qt::Key_Up:
            m_list->setSelectedIndex(m_list->selectedIndex() - 1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            const int i = m_list->selectedIndex();
            if (i >= 0 && i < m_current.size())
                choose(m_current[i]);
            return true;
        }
        case Qt::Key_Escape:
        case Qt::Key_Tab:
            close();
            return true;
        default:
            return false;
        }
    }
    if (ev->type() == QEvent::MouseButtonPress && isOpen()) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w != this && w != m_popup && !m_popup->isAncestorOf(w))
            close();
    }
    return false;
}

QRect FilePicker::clearRect() const
{
    // .combobox-clear-button left of the chevron (only when a file is set)
    const int em = int(tokPx("--font-ui-small", 13));
    return QRect(width() - int(0.65 * em) - em - 6 - 16, (height() - 16) / 2, 16, 16);
}

QSize FilePicker::sizeHint() const
{
    const QFont f = currentTheme()->interfaceFont(tokPx("--font-ui-small", 13));
    const QFontMetricsF fm(f);
    const QString text = m_value.isEmpty() ? m_placeholder : m_value;
    const int w = int(std::ceil(fm.horizontalAdvance(text))) + 8 + 6 + 16 + 4 + 16 + 12;
    return {std::clamp(w, 120, 250), int(tokPx("--input-height", 30))}; // max-width: 250px
}

QSize FilePicker::minimumSizeHint() const { return {120, int(tokPx("--input-height", 30))}; }

void FilePicker::mousePressEvent(QMouseEvent* e)
{
    if (!isEnabled())
        return;
    if (!m_value.isEmpty() && clearRect().adjusted(-3, -3, 3, 3).contains(e->position().toPoint())) {
        choose(QString());
        return;
    }
    if (isOpen())
        close();
    else
        open();
}

void FilePicker::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Down
        || e->key() == Qt::Key_Up) {
        open();
        return;
    }
    QWidget::keyPressEvent(e);
}

void FilePicker::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void FilePicker::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

void FilePicker::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        p.setOpacity(0.5);
    const double radius = tokPx("--input-radius", 5);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    QPainterPath shadow;
    shadow.addRoundedRect(r.translated(0, 1), radius, radius);
    p.fillPath(shadow, QColor(0, 0, 0, t->isDark() ? (m_hover ? 110 : 70) : (m_hover ? 30 : 22)));
    p.fillPath(path, t->color(m_hover ? QStringLiteral("--interactive-hover") : QStringLiteral("--interactive-normal")));
    p.setPen(QPen(t->isDark() ? QColor(255, 255, 255, m_hover ? 40 : 23) : QColor(0, 0, 0, m_hover ? 43 : 31), 1));
    p.drawPath(path);
    if (hasFocus() && !isOpen()) {
        p.setPen(QPen(t->color(QStringLiteral("--background-modifier-border-focus")), 2));
        p.drawRoundedRect(r, radius, radius);
    }

    const QFont f = t->interfaceFont(tokPx("--font-ui-small", 13));
    const double em = f.pixelSize();
    const int iconPx = int(std::lround(em));
    const QColor iconColor = t->isDark() ? QColor(255, 255, 255, 153) : QColor(0, 0, 0, 190);
    // chevrons-up-down at the end (padding-end --size-2-3)
    const double cx = width() - 6 - iconPx;
    p.drawPixmap(QPointF(cx, (height() - iconPx) / 2.0),
                 IconProvider::instance().pixmap(QStringLiteral("chevrons-up-down"), iconPx, iconColor, 2.0, devicePixelRatioF()));
    double right = cx - 4;
    if (!m_value.isEmpty()) {
        const QRect cr = clearRect();
        p.drawPixmap(cr.topLeft(), IconProvider::instance().pixmap(QStringLiteral("x"), cr.width(), t->color(QStringLiteral("--text-faint")),
                                                                    2.0, devicePixelRatioF()));
        right = cr.left() - 4;
    }
    // Label: "dir/ " parts muted, the name (without extension) normal; placeholder faint.
    p.setFont(f);
    const QFontMetricsF fm(f);
    double x = 8; // --size-4-2
    const double avail = right - x;
    if (m_value.isEmpty()) {
        p.setPen(t->color(QStringLiteral("--input-placeholder-color"), t->color(QStringLiteral("--text-faint"))));
        p.drawText(QRectF(x, 0, avail, height()), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(m_placeholder, Qt::ElideRight, avail));
        return;
    }
    QString display = m_value;
    const qsizetype slash = display.lastIndexOf(QLatin1Char('/'));
    const qsizetype dot = display.lastIndexOf(QLatin1Char('.'));
    if (dot > slash + 1)
        display.truncate(dot);
    QStringList parts = display.split(QLatin1Char('/'));
    const QString name = parts.takeLast();
    const QString dirs = parts.isEmpty() ? QString() : parts.join(QStringLiteral("/")) + QStringLiteral("/ ");
    const double nameW = fm.horizontalAdvance(name);
    const double dirW = std::min(fm.horizontalAdvance(dirs), std::max(0.0, avail - nameW));
    if (!dirs.isEmpty()) {
        p.setPen(t->color(QStringLiteral("--text-muted")));
        p.drawText(QRectF(x, 0, dirW, height()), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(dirs, Qt::ElideLeft, dirW));
        x += dirW;
    }
    p.setPen(t->color(QStringLiteral("--text-normal")));
    p.drawText(QRectF(x, 0, right - x, height()), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(name, Qt::ElideRight, right - x));
}

} // namespace oc
