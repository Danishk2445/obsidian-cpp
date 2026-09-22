#include "ui/overlay/suggest_modal.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace oc {

namespace {
constexpr int kPromptTop = 80;
constexpr int kPromptWidth = 700;
constexpr int kInputHeight = 40;

QColor tok(const char* name, const QColor& fallback = {})
{
    return currentTheme()->color(QString::fromLatin1(name), fallback);
}

// Lays out `text` with bold ranges into `layout` at `width`; returns the height.
qreal layoutText(QTextLayout& layout, const QString& text, const QFont& font, const QList<QPair<int, int>>& bold, qreal width,
                 qreal lineHeight)
{
    layout.setText(text);
    layout.setFont(font);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
    QList<QTextLayout::FormatRange> formats;
    for (const auto& [s, e] : bold) {
        if (e <= s)
            continue;
        QTextLayout::FormatRange fr;
        fr.start = s;
        fr.length = e - s;
        fr.format.setFontWeight(QFont::Bold);
        formats.append(fr);
    }
    layout.setFormats(formats);
    layout.beginLayout();
    qreal y = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(std::max<qreal>(10, width));
        const qreal pad = (lineHeight - (line.ascent() + line.descent())) / 2;
        line.setPosition(QPointF(0, y + pad));
        y += lineHeight;
    }
    layout.endLayout();
    return std::max(y, lineHeight);
}

// The prompt box: rounded, bordered, with Obsidian's large shadow.
class PromptFrame : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, 12, 12);
        p.fillPath(path, tok("--prompt-background", tok("--background-primary")));
        QColor border = tok("--prompt-border-color");
        if (!border.isValid())
            border = tok("--background-modifier-border-focus");
        p.setPen(QPen(border, 1));
        p.drawPath(path);
    }
};

// The circular "x" clear button of search inputs.
class ClearButton : public QWidget {
public:
    explicit ClearButton(QWidget* parent)
        : QWidget(parent)
    {
        setCursor(Qt::ArrowCursor);
        setAttribute(Qt::WA_Hover);
        setFixedSize(28, kInputHeight);
    }
    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal d = 17;
        const QRectF c((width() - d) / 2, (height() - d) / 2, d, d);
        QPainterPath circle;
        circle.addEllipse(c);
        QPainterPath cross;
        const qreal k = d / 12.0; // the icon is drawn on a 12x12 grid
        QPen pen;
        QPainterPathStroker st;
        st.setWidth(1.1 * k);
        QPainterPath lines;
        lines.moveTo(c.left() + 3.5 * k, c.top() + 3.5 * k);
        lines.lineTo(c.left() + 8.5 * k, c.top() + 8.5 * k);
        lines.moveTo(c.left() + 8.5 * k, c.top() + 3.5 * k);
        lines.lineTo(c.left() + 3.5 * k, c.top() + 8.5 * k);
        cross = st.createStroke(lines);
        const QColor col = underMouse() ? tok("--text-normal") : tok("--search-clear-button-color", tok("--text-muted"));
        p.fillPath(circle.subtracted(cross), col);
    }
    void mousePressEvent(QMouseEvent* e) override { e->accept(); }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (rect().contains(e->position().toPoint()) && onClick)
            onClick();
    }
};

// Footer with "command purpose" pairs.
class InstructionsBar : public QWidget {
public:
    explicit InstructionsBar(QWidget* parent)
        : QWidget(parent)
    {
    }
    QList<SuggestModal::Instruction> items;
    QSize sizeHint() const override { return {100, heightForWidth(width())}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return int(layoutRows(w).size()) * 18 + 16; }

protected:
    QList<QList<int>> layoutRows(int width) const
    {
        const ThemePtr t = currentTheme();
        QFont f = t->interfaceFont(12);
        QFont b = f;
        b.setWeight(QFont::Bold);
        const QFontMetricsF fm(f), bm(b);
        QList<QList<int>> rows{QList<int>()};
        qreal x = 0;
        for (int i = 0; i < items.size(); ++i) {
            const qreal w = bm.horizontalAdvance(items[i].command) + 4 + fm.horizontalAdvance(items[i].purpose);
            if (!rows.last().isEmpty() && x + 12 + w > width - 16) {
                rows.append(QList<int>());
                x = 0;
            }
            x += (rows.last().isEmpty() ? 0 : 12) + w;
            rows.last().append(i);
        }
        return rows;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const ThemePtr t = currentTheme();
        QFont f = t->interfaceFont(12);
        QFont b = f;
        b.setWeight(QFont::Bold);
        const QFontMetricsF fm(f), bm(b);
        const QColor muted = t->color(QStringLiteral("--text-muted"));
        p.setPen(muted);
        int y = 8;
        for (const QList<int>& row : layoutRows(width())) {
            qreal total = 0;
            for (int k = 0; k < row.size(); ++k) {
                const auto& it = items[row[k]];
                total += (k ? 12 : 0) + bm.horizontalAdvance(it.command) + 4 + fm.horizontalAdvance(it.purpose);
            }
            qreal x = (width() - total) / 2;
            for (int k = 0; k < row.size(); ++k) {
                const auto& it = items[row[k]];
                if (k)
                    x += 12;
                p.setFont(b);
                p.drawText(QPointF(x, y + bm.ascent() + (18 - bm.height()) / 2), it.command);
                x += bm.horizontalAdvance(it.command) + 4;
                p.setFont(f);
                p.drawText(QPointF(x, y + fm.ascent() + (18 - fm.height()) / 2), it.purpose);
                x += fm.horizontalAdvance(it.purpose);
            }
            y += 18;
        }
    }
};
} // namespace

// ---------------------------------------------------------------------------------------------
// SuggestModal
// ---------------------------------------------------------------------------------------------
SuggestModal::SuggestModal(OverlayLayer* layer)
    : QWidget(layer)
    , m_layer(layer)
    , m_emptyText(tr("No results found."))
{
    setProperty("ocCapturesKeys", true); // global hotkeys don't fire while a modal is open
    setAttribute(Qt::WA_TranslucentBackground);
    setGeometry(layer->rect());

    auto* prompt = new PromptFrame(this);
    m_prompt = prompt;
    auto* shadow = new QGraphicsDropShadowEffect(prompt);
    shadow->setBlurRadius(60);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, 90));
    prompt->setGraphicsEffect(shadow);

    auto* v = new QVBoxLayout(prompt);
    v->setContentsMargins(1, 1, 1, 1);
    v->setSpacing(0);

    auto* inputRow = new QWidget(prompt);
    inputRow->setFixedHeight(kInputHeight + 1);
    m_input = new QLineEdit(inputRow);
    m_input->setProperty("ocFlat", true);
    m_input->setProperty("ocCapturesKeys", true);
    m_input->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_input->installEventFilter(this);
    auto* clear = new ClearButton(inputRow);
    m_clear = clear;
    clear->onClick = [this] {
        if (!m_input->text().isEmpty()) {
            m_input->clear();
            m_input->setFocus();
        } else {
            close();
        }
    };
    inputRow->installEventFilter(this);
    v->addWidget(inputRow);

    m_list = new SuggestionList(prompt);
    v->addWidget(m_list, 1);
    auto* instr = new InstructionsBar(prompt);
    m_instructions = instr;
    instr->hide();
    v->addWidget(instr);

    connect(m_input, &QLineEdit::textChanged, this, [this] { updateSuggestions(); });
    connect(m_list, &SuggestionList::activated, this, [this](int, Qt::KeyboardModifiers mods) { chooseSelected(mods); });
    connect(m_list, &SuggestionList::selectedChanged, this, &SuggestModal::selectionChanged);
    layer->installEventFilter(this);
    hide();
}

SuggestModal::~SuggestModal() = default;

void SuggestModal::setPlaceholder(const QString& text) { m_input->setPlaceholderText(text); }

void SuggestModal::setInstructions(const QList<Instruction>& instructions)
{
    m_instructionList = instructions;
    auto* bar = static_cast<InstructionsBar*>(m_instructions);
    bar->items = instructions;
    bar->setVisible(!instructions.isEmpty());
    relayout();
}

void SuggestModal::open()
{
    m_open = true;
    setGeometry(m_layer->rect());
    show();
    raise();
    m_layer->setBlocking(this, true);
    {
        const QSignalBlocker b(m_input);
        m_input->clear();
    }
    updateSuggestions();
    m_input->setFocus();
}

void SuggestModal::close()
{
    if (!m_open)
        return;
    m_open = false;
    hide();
    if (m_layer)
        m_layer->setBlocking(this, false);
    emit closed();
    deleteLater();
}

QString SuggestModal::query() const { return m_input->text(); }

void SuggestModal::setQuery(const QString& q)
{
    if (m_input->text() == q)
        updateSuggestions();
    else
        m_input->setText(q); // textChanged -> updateSuggestions
}

void SuggestModal::refresh() { updateSuggestions(); }

const QList<Suggestion>& SuggestModal::suggestions() const { return m_list->suggestions(); }
int SuggestModal::selectedIndex() const { return m_list->selectedIndex(); }
void SuggestModal::setSelectedIndex(int index) { m_list->setSelectedIndex(index); }

void SuggestModal::chooseSelected(Qt::KeyboardModifiers mods)
{
    const QList<Suggestion>& items = m_list->suggestions();
    const int i = m_list->selectedIndex();
    if (i < 0 || i >= items.size())
        return;
    const Suggestion chosenItem = items[i];
    QPointer<SuggestModal> self(this);
    close(); // Obsidian closes first, then runs the choice
    emit chosen(chosenItem, mods);
}

void SuggestModal::updateSuggestions()
{
    QList<Suggestion> items = m_provider ? m_provider(m_input->text()) : QList<Suggestion>();
    if (m_limit > 0 && items.size() > m_limit)
        items.resize(m_limit);
    m_list->setSuggestions(std::move(items), m_emptyText);
    m_clear->setVisible(!m_input->text().isEmpty());
    relayout();
}

QRect SuggestModal::promptGeometry() const { return m_prompt->geometry(); }

void SuggestModal::relayout()
{
    if (!m_layer)
        return;
    const QRect area = rect();
    const int w = std::min(kPromptWidth, int(area.width() * 0.8));
    const int maxH = int(area.height() * 0.7);
    const int instrH = m_instructions->isVisible() ? m_instructions->heightForWidth(w) : 0;
    const int listH = std::min(m_list->contentHeight(), std::max(60, maxH - kInputHeight - 3 - instrH));
    m_list->setFixedHeight(listH);
    const int h = std::min(maxH, kInputHeight + 3 + listH + instrH);
    m_prompt->setGeometry((area.width() - w) / 2, kPromptTop, w, h);
    // input row children
    QWidget* row = m_input->parentWidget();
    m_input->setGeometry(0, 0, row->width(), kInputHeight);
    m_input->setTextMargins(24 - 2, 0, 48 - 2, 0);
    m_clear->move(row->width() - 16 - m_clear->width() + 6, 0);
}

bool SuggestModal::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_layer && ev->type() == QEvent::Resize) {
        setGeometry(m_layer->rect());
        relayout();
    } else if (obj == m_input->parentWidget() && ev->type() == QEvent::Resize) {
        relayout();
    } else if (obj == m_input->parentWidget() && ev->type() == QEvent::Paint) {
        // bottom border of the input: 1px --background-secondary
        QWidget* row = m_input->parentWidget();
        QPainter p(row);
        p.fillRect(QRect(0, kInputHeight, row->width(), 1), tok("--background-secondary"));
    } else if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (m_keyHandler && m_keyHandler(ke))
            return true;
        const bool ctrl = ke->modifiers() & Qt::ControlModifier;
        switch (ke->key()) {
        case Qt::Key_Up:
            m_list->setSelectedIndex(m_list->selectedIndex() - 1);
            return true;
        case Qt::Key_Down:
            m_list->setSelectedIndex(m_list->selectedIndex() + 1);
            return true;
        case Qt::Key_P:
        case Qt::Key_N:
            if (ctrl && !(ke->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
                m_list->setSelectedIndex(m_list->selectedIndex() + (ke->key() == Qt::Key_P ? -1 : 1));
                return true;
            }
            break;
        case Qt::Key_PageUp:
            m_list->pageUp();
            return true;
        case Qt::Key_PageDown:
            m_list->pageDown();
            return true;
        case Qt::Key_Home:
            m_list->setSelectedIndex(0);
            return true;
        case Qt::Key_End:
            m_list->setSelectedIndex(int(m_list->suggestions().size()) - 1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            chooseSelected(ke->modifiers());
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void SuggestModal::mousePressEvent(QMouseEvent* e)
{
    if (!m_prompt->geometry().contains(e->position().toPoint()))
        close(); // click on the dimmed background
    e->accept();
}

void SuggestModal::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    relayout();
}

// ---------------------------------------------------------------------------------------------
// SuggestionList
// ---------------------------------------------------------------------------------------------
SuggestionList::SuggestionList(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::NoFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}

void SuggestionList::setSuggestions(QList<Suggestion> items, const QString& emptyText)
{
    m_items = std::move(items);
    m_empty = emptyText;
    m_selected = 0;
    relayoutRows();
    verticalScrollBar()->setValue(0);
    viewport()->update();
    emit selectedChanged(m_items.isEmpty() ? -1 : 0);
}

int SuggestionList::rowHeight(const Suggestion& s, int width) const
{
    const ThemePtr t = currentTheme();
    const QFont font = t->interfaceFont(15);
    int textW = width - 24; // padding 6px 12px
    if (!s.icon.isEmpty())
        textW -= 16 + 4 + 12;
    const QFontMetricsF fm(font);
    int auxW = 0;
    for (const QString& k : s.hotkeys)
        auxW += int(QFontMetricsF(t->monospaceFont(12)).horizontalAdvance(k)) + 12 + 10;
    if (!s.auxText.isEmpty())
        auxW += int(QFontMetricsF(t->interfaceFont(12)).horizontalAdvance(s.auxText)) + 12;
    if (!s.auxIcon.isEmpty())
        auxW += 16 + 16;
    textW -= auxW;
    QTextLayout tl;
    const QString title = s.prefix.isEmpty() ? s.title : s.prefix + QStringLiteral(": ") + s.title;
    qreal h = layoutText(tl, title, font, {}, textW, 22.5);
    if (!s.note.isEmpty()) {
        QTextLayout nl;
        h += layoutText(nl, s.note, t->interfaceFont(12), {}, textW, 18);
    }
    return int(std::ceil(h)) + 12 + 1; // vertical padding + 1px gap
}

void SuggestionList::relayoutRows()
{
    m_rows.clear();
    const int width = viewport()->width() - 2 * m_pad;
    int y = m_pad;
    for (const Suggestion& s : std::as_const(m_items)) {
        const int h = rowHeight(s, width);
        m_rows.append(Row{y, h});
        y += h;
    }
    const int total = m_items.isEmpty() ? (m_empty.isEmpty() ? 0 : 8 + 12 + 23 + 8) : y + m_pad;
    verticalScrollBar()->setRange(0, std::max(0, total - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(24);
}

int SuggestionList::contentHeight() const
{
    if (m_items.isEmpty())
        return m_empty.isEmpty() ? 0 : 8 + 12 + 23 + 8;
    return m_rows.isEmpty() ? 0 : m_rows.last().y + m_rows.last().height + m_pad;
}

QRect SuggestionList::rowRect(int index) const
{
    if (index < 0 || index >= m_rows.size())
        return {};
    const Row& r = m_rows[index];
    return QRect(m_pad, r.y - verticalScrollBar()->value(), viewport()->width() - 2 * m_pad, r.height - 1);
}

int SuggestionList::indexAt(const QPoint& pos) const
{
    const int y = pos.y() + verticalScrollBar()->value();
    for (int i = 0; i < m_rows.size(); ++i)
        if (y >= m_rows[i].y && y < m_rows[i].y + m_rows[i].height)
            return i;
    return -1;
}

void SuggestionList::setSelectedIndex(int index, bool scroll)
{
    const int n = int(m_items.size());
    if (n == 0)
        return;
    if (index < 0)
        index = n - 1;
    else if (index >= n)
        index = 0;
    if (index != m_selected) {
        m_selected = index;
        viewport()->update();
        emit selectedChanged(index);
    }
    if (scroll)
        ensureVisible(index);
}

void SuggestionList::ensureVisible(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    const Row& r = m_rows[index];
    QScrollBar* sb = verticalScrollBar();
    const int top = sb->value(), h = viewport()->height();
    if (r.y < top + (index == 0 ? m_pad : 0))
        sb->setValue(index == 0 ? 0 : r.y);
    else if (r.y + r.height > top + h)
        sb->setValue(r.y + r.height - h + (index == m_items.size() - 1 ? m_pad : 0));
}

void SuggestionList::pageUp()
{
    if (m_rows.isEmpty())
        return;
    const int rowH = m_rows[std::clamp(m_selected, 0, int(m_rows.size()) - 1)].height;
    const int visible = std::max(1, viewport()->height() / std::max(1, rowH));
    const int firstVisible = std::max(0, indexAt(QPoint(0, m_pad)));
    int target = firstVisible;
    if (m_selected <= firstVisible)
        target -= visible;
    setSelectedIndex(std::max(0, target));
}

void SuggestionList::pageDown()
{
    if (m_rows.isEmpty())
        return;
    const int rowH = m_rows[std::clamp(m_selected, 0, int(m_rows.size()) - 1)].height;
    const int visible = std::max(1, viewport()->height() / std::max(1, rowH));
    const int firstVisible = std::max(0, indexAt(QPoint(0, m_pad)));
    int target = firstVisible + visible - 1;
    if (m_selected >= target)
        target += visible;
    setSelectedIndex(std::min(int(m_items.size()) - 1, target));
}

void SuggestionList::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayoutRows();
}

void SuggestionList::mouseMoveEvent(QMouseEvent* e)
{
    // Only real pointer movement selects (not a list scrolling under a resting pointer).
    const QPoint p = e->position().toPoint();
    if (p != m_lastMouse) {
        m_lastMouse = p;
        const int i = indexAt(p);
        if (i >= 0)
            setSelectedIndex(i, false);
    }
    QAbstractScrollArea::mouseMoveEvent(e);
}

void SuggestionList::mouseReleaseEvent(QMouseEvent* e)
{
    const int i = indexAt(e->position().toPoint());
    if (i >= 0 && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
        setSelectedIndex(i, false);
        Qt::KeyboardModifiers mods = e->modifiers();
        if (e->button() == Qt::MiddleButton)
            mods |= Qt::ControlModifier; // auxclick: open in a new tab
        emit activated(i, mods);
    }
}

void SuggestionList::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePtr t = currentTheme();
    const QColor normal = t->color(QStringLiteral("--text-normal"));
    const QColor muted = t->color(QStringLiteral("--text-muted"));
    const QColor faint = t->color(QStringLiteral("--text-faint"));
    const QColor hover = t->color(QStringLiteral("--background-modifier-hover"));
    const QFont font = t->interfaceFont(15);
    const QFont noteFont = t->interfaceFont(12);
    const QFont keyFont = t->monospaceFont(12);

    if (m_items.isEmpty()) {
        if (m_empty.isEmpty())
            return;
        p.setFont(font);
        p.setPen(muted);
        p.drawText(QRect(0, 8 + 12, viewport()->width(), 23), Qt::AlignHCenter | Qt::AlignVCenter, m_empty);
        return;
    }
    const int scroll = verticalScrollBar()->value();
    const int vh = viewport()->height();
    const IconProvider& icons = IconProvider::instance();
    for (int i = 0; i < m_items.size(); ++i) {
        const Row& row = m_rows[i];
        if (row.y + row.height < scroll || row.y > scroll + vh)
            continue;
        const Suggestion& s = m_items[i];
        const QRect r(m_pad, row.y - scroll, viewport()->width() - 2 * m_pad, row.height - 1);
        if (i == m_selected) {
            QPainterPath path;
            path.addRoundedRect(QRectF(r), 4, 4);
            p.fillPath(path, hover);
        }
        int x = r.left() + 12;
        int right = r.right() - 12;
        const QColor textColor = s.downranked ? muted : normal;
        // left flair icon
        if (!s.icon.isEmpty()) {
            const QPixmap pm = icons.pixmap(s.icon, 16, muted, 2.0, devicePixelRatioF());
            p.setOpacity(0.85);
            p.drawPixmap(QPointF(x + 4, r.center().y() - 8), pm);
            p.setOpacity(1);
            x += 4 + 16 + 12;
        }
        // right side: aux icon, aux text, hotkeys (right to left)
        if (!s.auxIcon.isEmpty()) {
            const QPixmap pm = icons.pixmap(s.auxIcon, 16, muted, 2.0, devicePixelRatioF());
            p.setOpacity(0.85);
            p.drawPixmap(QPointF(right - 4 - 16, r.center().y() - 8), pm);
            p.setOpacity(1);
            right -= 16 + 16;
        }
        if (!s.auxText.isEmpty()) {
            p.setFont(noteFont);
            p.setPen(faint);
            const int w = int(QFontMetricsF(noteFont).horizontalAdvance(s.auxText)) + 1;
            p.drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignRight, s.auxText);
            right -= w + 12;
        }
        for (qsizetype k = s.hotkeys.size() - 1; k >= 0; --k) {
            const QString& key = s.hotkeys[k];
            const QFontMetricsF km(keyFont);
            const qreal w = km.horizontalAdvance(key) + 12;
            const qreal h = km.height() + 4;
            const QRectF chip(right - w, r.center().y() - h / 2, w, h);
            QPainterPath cp;
            cp.addRoundedRect(chip, 4, 4);
            p.fillPath(cp, t->color(QStringLiteral("--code-background")));
            p.setFont(keyFont);
            p.setPen(t->color(QStringLiteral("--code-normal"), muted));
            p.drawText(chip, Qt::AlignCenter, key);
            right = int(chip.left()) - 10;
        }
        // title (+ prefix) and note
        const int textW = right - x;
        const QString title = s.prefix.isEmpty() ? s.title : s.prefix + QStringLiteral(": ") + s.title;
        QList<QPair<int, int>> bold;
        const int offset = s.prefix.isEmpty() ? 0 : int(s.prefix.size()) + 2;
        for (const auto& [a, b] : s.titleMatches)
            bold.append({a + offset, b + offset});
        QTextLayout tl;
        const qreal th = layoutText(tl, title, font, bold, textW, 22.5);
        p.setPen(textColor);
        tl.draw(&p, QPointF(x, r.top() + 6));
        if (!s.note.isEmpty()) {
            QTextLayout nl;
            layoutText(nl, s.note, noteFont, s.noteMatches, textW, 18);
            p.setPen(muted);
            nl.draw(&p, QPointF(x, r.top() + 6 + th));
        }
    }
}

} // namespace oc
