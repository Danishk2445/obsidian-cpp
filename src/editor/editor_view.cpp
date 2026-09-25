#include "editor/editor_view.h"

#include "editor/editor_hooks.h"
#include "editor/embeds/hosted_embeds.h"
#include "editor/live_preview_decorator.h"
#include "editor/markdown_commands.h"
#include "editor/source_decorator.h"
#include "render/widgets/inline_widgets.h"
#include "render/theme/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QStyleHints>
#include <QFontMetricsF>
#include <QTextBoundaryFinder>

#include <cmath>

namespace oc {

bool isFoldPlaceholder(const InlineWidget* w); // editor_view_fold.cpp

namespace {
enum class CharCat { Word, Space, Other };
CharCat categorize(QChar c)
{
    if (c.isSpace())
        return CharCat::Space;
    if (c.isLetterOrNumber() || c == u'_' || c.category() == QChar::Mark_NonSpacing || c.isSurrogate())
        return CharCat::Word;
    return CharCat::Other;
}
} // namespace

EditorView::EditorView(QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_decorator(std::make_unique<SourceDecorator>(nullptr))
    , m_lp(std::make_unique<LivePreviewDecorator>(nullptr))
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalScrollBar()->setSingleStep(24);
    m_layout.setDecorator(m_decorator.get());
    m_decorator->extra = [this](int line, LineDecoration& d) { applyExtraDecorations(line, d); };
    m_lp->extra = m_decorator->extra;
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, &EditorView::applyTheme);
    applyTheme();
    new HostedEmbeds(this); // live widgets of PDF / audio / video embeds near the viewport
}

EditorView::~EditorView()
{
    if (m_doc) {
        disconnect(m_docConn);
        disconnect(m_syntaxConn);
        disconnect(m_roConn);
    }
}

LineDecorator* EditorView::activeDecorator() const
{
    if (m_mode == Mode::Source)
        return m_decorator.get();
    return m_lp.get();
}

void EditorView::applyTheme()
{
    m_styles = StyleSheet::build(*currentTheme());
    m_decorator->setStyleSheet(m_styles.get());
    m_lp->setStyleSheet(m_styles.get());
    m_layout.setStyleSheet(m_styles);
    updateGeometryAndLayout();
    viewport()->update();
}

void EditorView::setDocument(std::shared_ptr<Document> doc)
{
    if (m_doc) {
        disconnect(m_docConn);
        disconnect(m_syntaxConn);
        disconnect(m_roConn);
    }
    m_doc = std::move(doc);
    const bool hadFolds = !m_folds.isEmpty();
    m_folds.clear();
    m_hoverLine = -1;
    m_clipFrom = 0;
    m_clipTo = -1;
    m_layout.setHiddenLines({});
    if (hadFolds)
        emit foldsChanged();
    m_decorator->setDocument(m_doc.get());
    m_lp->setDocument(m_doc.get());
    m_sel = Selection::cursor(0);
    m_lp->setSelection(m_sel);
    m_revealed = m_sel;
    m_scrollTop = 0;
    if (m_doc) {
        m_docConn = connect(m_doc.get(), &Document::changed, this, &EditorView::onDocumentChanged);
        m_syntaxConn = connect(m_doc.get(), &Document::syntaxProgress, this, &EditorView::onSyntaxProgress);
        m_roConn = connect(m_doc.get(), &Document::readOnlyChanged, this, [this] { viewport()->update(); });
        m_layout.setText(m_doc->text());
    } else {
        m_layout.setText(Text());
    }
    updateGeometryAndLayout();
    viewport()->update();
}

void EditorView::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_lp->setReading(mode == Mode::Reading);
    m_lp->setSelection(m_sel);
    m_revealed = m_sel;
    m_layout.setDecorator(activeDecorator());
    viewport()->setCursor(mode == Mode::Reading ? Qt::ArrowCursor : Qt::IBeamCursor);
    m_layout.invalidateAll();
    updateGeometryAndLayout();
    viewport()->update();
    emit modeChanged(mode);
}

bool EditorView::isReadOnly() const { return m_readOnly || !m_doc || m_doc->isReadOnly() || m_mode == Mode::Reading; }
void EditorView::setReadOnly(bool ro)
{
    m_readOnly = ro;
    viewport()->update();
}

void EditorView::setReadableLineLength(bool on)
{
    m_readable = on;
    updateGeometryAndLayout();
    viewport()->update();
}

void EditorView::setMargins(qreal horizontal, qreal top, qreal bottom)
{
    m_marginX = horizontal;
    m_topBase = top;
    m_marginTop = m_topBase + m_topWidgetH;
    m_marginBottom = bottom;
    updateGeometryAndLayout();
}

void EditorView::setTopWidget(QWidget* widget, int bottomMargin)
{
    if (m_topWidget && m_topWidget != widget)
        m_topWidget->hide();
    m_topWidget = widget;
    m_topWidgetMargin = bottomMargin;
    if (widget) {
        widget->setParent(viewport());
        widget->show();
    }
    updateTopWidget();
}

void EditorView::updateTopWidget()
{
    const double docTop = scrollTop();
    qreal h = 0;
    if (m_topWidget && !m_topWidget->isHidden()) {
        const int w = int(m_layout.contentWidth());
        h = (m_topWidget->hasHeightForWidth() ? m_topWidget->heightForWidth(w) : m_topWidget->sizeHint().height()) + m_topWidgetMargin;
    }
    m_topWidgetH = h;
    m_marginTop = m_topBase + m_topWidgetH;
    if (docTop > 0)
        m_scrollTop = docTop + m_marginTop; // keep the text where it was
    updateScrollbar();
    positionTopWidget();
    viewport()->update();
}

void EditorView::positionTopWidget()
{
    if (!m_topWidget)
        return;
    const int w = int(m_layout.contentWidth());
    const int h = int(std::max<qreal>(0, m_topWidgetH - m_topWidgetMargin));
    m_topWidget->setGeometry(int(m_contentLeft), int(std::round(m_topBase - m_scrollTop)), w, h);
}

void EditorView::setLineWrapping(bool) {}

// ---------------------------------------------------------------------------------------------
// cursor shape & bottom panel
// ---------------------------------------------------------------------------------------------
void EditorView::setCursorShape(CursorShape shape, bool mainOnly)
{
    if (m_cursorShape == shape && m_cursorMainOnly == mainOnly)
        return;
    m_cursorShape = shape;
    m_cursorMainOnly = mainOnly;
    restartBlink();
    viewport()->update();
}

qsizetype EditorView::blockCursorPos(const SelRange& r) const
{
    // Obsidian's vim fat cursor: on the head, or on the last selected character of a forward
    // selection (unless the head sits at a line break).
    const Text& t = m_doc->text();
    qsizetype s = r.head;
    if (r.anchor < r.head && !(s < t.length() && t.charAt(s) == u'\n'))
        --s;
    if (s > 1 && s < t.length() && t.charAt(s).isLowSurrogate())
        --s;
    return std::clamp<qsizetype>(s, 0, t.length());
}

namespace {
struct BlockCursorBox {
    QRectF rect;       // doc coordinates
    int dispFrom = -1; // display range of the character inside it (-1: none)
    int dispTo = -1;
    int line = -1;
};

BlockCursorBox computeBlockCursor(DocLayout& layout, const Text& t, qsizetype s, const StyleSheet* styles, int tabSize)
{
    BlockCursorBox box;
    const QRectF r0 = layout.caretRect(s, 1);
    const int line = t.lineAt(s);
    const qsizetype ls = t.lineStart(line), le = t.lineEnd(line);
    box.line = line;
    const DocLayout::UnitLayout* ul = layout.isInBlock(line) ? nullptr : layout.layoutFor(line);
    QFont font = styles ? styles->line(StyleId::Normal).font : QFont();
    int disp = -1;
    if (ul) {
        font = styles ? styles->line(ul->deco.lineStyle).font : font;
        disp = ul->block.docToDisp(int(s - ls), 1);
        for (const QTextLayout::FormatRange& f : ul->block.formats())
            if (disp >= f.start && disp < f.start + f.length)
                font = f.format.font().resolve(font);
    }
    const qreal spaceW = QFontMetricsF(font).horizontalAdvance(QLatin1Char(' '));
    qreal x = r0.x(), w = 0;
    if (s < le) {
        QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, t.line(line));
        f.setPosition(int(s - ls));
        qsizetype e = f.toNextBoundary();
        e = e < 0 ? le : ls + e;
        const QRectF r1 = layout.caretRect(e, -1);
        if (qFuzzyCompare(r1.top() + 1, r0.top() + 1) && r1.x() > r0.x()) {
            w = r1.x() - r0.x();
            if (t.charAt(s) == u'\t') {
                w = w / std::max(1, tabSize); // one column at the end of the tab
                x = r1.x() - w;
            } else if (ul) {
                box.dispFrom = disp;
                box.dispTo = ul->block.docToDisp(int(e - ls), -1);
            }
        }
    }
    if (w <= 0)
        w = spaceW;
    box.rect = QRectF(x, r0.y(), w, r0.height());
    return box;
}
} // namespace

QRectF EditorView::blockCursorRect(const SelRange& range) const
{
    if (!m_doc || m_cursorShape != CursorShape::Block)
        return {};
    auto* self = const_cast<EditorView*>(this);
    const BlockCursorBox b = computeBlockCursor(self->m_layout, m_doc->text(), blockCursorPos(range), m_styles.get(), m_foldOptions.tabSize);
    return QRectF(docToViewport(b.rect.topLeft()), b.rect.size());
}

void EditorView::paintBlockCursor(QPainter& p, const SelRange& r)
{
    const Text& t = m_doc->text();
    const BlockCursorBox b = computeBlockCursor(m_layout, t, blockCursorPos(r), m_styles.get(), m_foldOptions.tabSize);
    if (b.rect.isEmpty())
        return;
    const ThemePtr th = currentTheme();
    const QColor bg = th->color(QStringLiteral("--interactive-accent"));
    const QColor fg = th->color(QStringLiteral("--text-on-accent"));
    p.fillRect(b.rect, bg);
    if (b.dispTo <= b.dispFrom || b.dispFrom < 0)
        return;
    const DocLayout::UnitLayout* ul = m_layout.layoutFor(b.line);
    if (!ul || !ul->text)
        return;
    QTextLayout::FormatRange fr;
    fr.start = b.dispFrom;
    fr.length = b.dispTo - b.dispFrom;
    fr.format.setForeground(fg);
    fr.format.setBackground(bg);
    p.save();
    p.setClipRect(b.rect);
    ul->text->draw(&p, QPointF(0, m_layout.lineTop(b.line)), {fr});
    p.restore();
}

void EditorView::setBottomPanel(QWidget* panel)
{
    if (m_bottomPanel != panel) {
        if (m_bottomPanel)
            m_bottomPanel->hide();
        m_bottomPanel = panel;
        if (panel)
            panel->setParent(this); // not the viewport: it doesn't scroll
    }
    updateBottomPanel();
}

void EditorView::updateBottomPanel()
{
    const bool shown = m_bottomPanel && !m_bottomPanel->isHidden();
    const int h = shown ? std::max(0, m_bottomPanel->sizeHint().height()) : 0;
    if (viewportMargins().bottom() != h)
        setViewportMargins(0, 0, 0, h);
    if (shown) {
        m_bottomPanel->setGeometry(0, height() - h, width(), h);
        m_bottomPanel->raise();
    }
}

void EditorView::setBackgroundColor(const QColor& color)
{
    m_background = color;
    viewport()->update();
}

int EditorView::addDecorationLayer(DecorationLayer layer)
{
    const int id = m_nextLayerId++;
    m_layers.append({id, std::move(layer)});
    refreshDecorations();
    return id;
}

void EditorView::removeDecorationLayer(int id)
{
    for (qsizetype i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].first == id) {
            m_layers.removeAt(i);
            refreshDecorations();
            return;
        }
    }
}

void EditorView::applyExtraDecorations(int line, LineDecoration& d) const
{
    for (const auto& [id, layer] : m_layers)
        layer(line, d);
    applyFoldPlaceholder(line, d);
    // IME composition: shown inline at the caret as a zero-width replacement with an underline.
    if (!m_preedit.isEmpty() && m_doc) {
        const qsizetype head = m_sel.main().head;
        const Text& t = m_doc->text();
        if (t.lineAt(head) == line) {
            Replace r;
            r.from = r.to = int(head - t.lineStart(line));
            r.kind = Replace::Kind::Text;
            r.text = m_preedit;
            r.style = StyleId::Preedit;
            d.replaces.push_back(r);
            std::stable_sort(d.replaces.begin(), d.replaces.end(), [](const Replace& a, const Replace& b) { return a.from < b.from; });
        }
    }
}

void EditorView::refreshDecorations(int fromLine, int toLine)
{
    if (toLine < 0)
        toLine = m_layout.lineCount();
    m_layout.invalidateLines(fromLine, toLine);
    ensureVisibleLayout();
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// geometry & scrolling
// ---------------------------------------------------------------------------------------------
void EditorView::updateGeometryAndLayout()
{
    const double w = viewport()->width();
    double lineWidth = std::max(80.0, w - 2 * m_marginX);
    if (m_readable) {
        const double fileW = currentTheme()->px(QStringLiteral("--file-line-width"), -1, 700);
        lineWidth = std::min(lineWidth, fileW);
    }
    m_contentLeft = std::max(m_marginX, std::floor((w - lineWidth) / 2));
    m_lp->setContentWidth(lineWidth);
    m_layout.setContentWidth(lineWidth);
    if (m_topWidget) {
        // its height depends on the width (wrapping title)
        const int w = int(lineWidth);
        const qreal h = (m_topWidget->hasHeightForWidth() ? m_topWidget->heightForWidth(w) : m_topWidget->sizeHint().height()) + m_topWidgetMargin;
        if (!qFuzzyCompare(h, m_topWidgetH)) {
            m_topWidgetH = h;
            m_marginTop = m_topBase + m_topWidgetH;
        }
    }
    ensureVisibleLayout();
    updateScrollbar();
    positionTopWidget();
}

double EditorView::scrollTop() const { return m_scrollTop - m_marginTop; }

void EditorView::setScrollTop(double y)
{
    m_scrollTop = y + m_marginTop;
    updateScrollbar();
    ensureVisibleLayout();
    positionTopWidget();
    viewport()->update();
    emit scrolled(scrollTop());
}

void EditorView::updateScrollbar()
{
    const double viewH = viewport()->height();
    const double total = m_marginTop + m_layout.totalHeight() + m_marginBottom + std::max(0.0, viewH * 0.5 - m_marginBottom);
    const int max = std::max(0, int(std::ceil(total - viewH)));
    m_scrollTop = std::clamp(m_scrollTop, 0.0, double(max));
    m_settingScroll = true;
    QScrollBar* sb = verticalScrollBar();
    sb->setRange(0, max);
    sb->setPageStep(int(viewH));
    sb->setSingleStep(std::max(12, int(m_styles ? m_styles->line(StyleId::Normal).lineBox : 24)));
    if (!sb->isSliderDown())
        sb->setValue(int(std::lround(m_scrollTop)));
    m_settingScroll = false;
    positionTopWidget();
}

void EditorView::ensureVisibleLayout()
{
    if (!m_doc || !m_styles)
        return;
    const double viewH = viewport()->height();
    const bool dragging = verticalScrollBar()->isSliderDown();
    for (int pass = 0; pass < 4; ++pass) {
        const double docTop = m_scrollTop - m_marginTop;
        const int anchorLine = m_layout.lineAtY(std::max(0.0, docTop));
        const double anchorOffset = docTop - m_layout.lineTop(anchorLine);
        const bool changed = m_layout.ensureLaidOut(docTop - viewH * 0.5, docTop + viewH * 1.5);
        if (!changed)
            break;
        if (!dragging && docTop > 0)
            m_scrollTop = m_layout.lineTop(anchorLine) + anchorOffset + m_marginTop;
        updateScrollbar();
    }
    // keep memory bounded: drop layouts far away (cheap relative to layout work)
    static int evictCounter = 0;
    if (++evictCounter % 32 == 0) {
        const double docTop = m_scrollTop - m_marginTop;
        m_layout.evictOutside(docTop - viewH * 4, docTop + viewH * 5);
    }
}

void EditorView::scrollContentsBy(int, int)
{
    if (m_settingScroll)
        return;
    m_scrollTop = verticalScrollBar()->value();
    ensureVisibleLayout();
    positionTopWidget();
    viewport()->update();
    emit scrolled(scrollTop());
}

void EditorView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    const qsizetype anchor = m_doc ? m_layout.hitTest(QPointF(0, std::max(0.0, scrollTop()))) : 0;
    updateGeometryAndLayout();
    if (m_doc && scrollTop() > 0)
        m_scrollTop = m_layout.lineTop(m_doc->text().lineAt(anchor)) + m_marginTop;
    updateScrollbar();
}

QPointF EditorView::docToViewport(const QPointF& d) const { return QPointF(d.x() + m_contentLeft, d.y() - (m_scrollTop - m_marginTop)); }
QPointF EditorView::viewportToDoc(const QPointF& v) const { return QPointF(v.x() - m_contentLeft, v.y() + (m_scrollTop - m_marginTop)); }

qsizetype EditorView::positionAt(const QPoint& vp)
{
    if (!m_doc)
        return 0;
    QPointF d = viewportToDoc(vp);
    if (d.y() < 0)
        return 0;
    if (d.y() >= m_layout.totalHeight())
        return m_doc->text().length();
    return m_layout.hitTest(d);
}

QRectF EditorView::caretRectInViewport() const
{
    if (!m_doc)
        return {};
    auto* self = const_cast<EditorView*>(this);
    const QRectF r = self->m_layout.caretRect(m_sel.main().head);
    return QRectF(docToViewport(r.topLeft()), r.size());
}

QRectF EditorView::caretRectAt(qsizetype pos) const
{
    if (!m_doc)
        return {};
    auto* self = const_cast<EditorView*>(this);
    const QRectF r = self->m_layout.caretRect(std::clamp<qsizetype>(pos, 0, m_doc->text().length()));
    return QRectF(docToViewport(r.topLeft()), r.size());
}

void EditorView::scrollToPos(qsizetype pos, bool center)
{
    if (!m_doc)
        return;
    const QRectF r = m_layout.caretRect(pos);
    const double viewH = viewport()->height();
    const double docTop = scrollTop();
    const double margin = std::min(viewH / 4, 2.0 * (m_styles ? m_styles->line(StyleId::Normal).lineBox : 24));
    double newTop = docTop;
    if (center)
        newTop = r.center().y() - viewH / 2;
    else if (r.top() - margin < docTop)
        newTop = r.top() - margin;
    else if (r.bottom() + margin > docTop + viewH)
        newTop = r.bottom() + margin - viewH;
    if (newTop != docTop) {
        m_scrollTop = std::max(0.0, newTop + m_marginTop);
        updateScrollbar();
        ensureVisibleLayout();
        positionTopWidget();
        viewport()->update();
        emit scrolled(scrollTop());
    }
}

QJsonObject EditorView::ephemeralState() const
{
    if (!m_doc)
        return {};
    const Text& t = m_doc->text();
    auto pos = [&](qsizetype p) {
        const int l = t.lineAt(p);
        return QJsonObject{{QStringLiteral("line"), l}, {QStringLiteral("ch"), int(p - t.lineStart(l))}};
    };
    const int topLine = m_layout.lineAtY(std::max(0.0, scrollTop()));
    const double frac = m_layout.lineHeight(topLine) > 0 ? (scrollTop() - m_layout.lineTop(topLine)) / m_layout.lineHeight(topLine) : 0;
    return {{QStringLiteral("cursor"), QJsonObject{{QStringLiteral("from"), pos(m_sel.main().anchor)}, {QStringLiteral("to"), pos(m_sel.main().head)}}},
            {QStringLiteral("scroll"), topLine + std::max(0.0, frac)}};
}

void EditorView::setEphemeralState(const QJsonObject& st)
{
    if (!m_doc)
        return;
    const Text& t = m_doc->text();
    auto toPos = [&](const QJsonObject& o) {
        const int l = std::clamp(o.value(QStringLiteral("line")).toInt(), 0, t.lineCount() - 1);
        return t.lineStart(l) + std::clamp<qsizetype>(o.value(QStringLiteral("ch")).toInt(), 0, t.lineLength(l));
    };
    if (st.contains(QStringLiteral("cursor"))) {
        const QJsonObject c = st.value(QStringLiteral("cursor")).toObject();
        setSelection(Selection::single(toPos(c.value(QStringLiteral("from")).toObject()), toPos(c.value(QStringLiteral("to")).toObject())), false,
                     false);
    }
    if (st.contains(QStringLiteral("scroll"))) {
        const double s = st.value(QStringLiteral("scroll")).toDouble();
        const int line = std::clamp(int(s), 0, std::max(0, m_layout.lineCount() - 1));
        m_layout.layoutFor(line);
        setScrollTop(m_layout.lineTop(line) + (s - line) * m_layout.lineHeight(line));
    } else {
        scrollToPos(m_sel.main().head, true);
    }
    viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------------------------
void EditorView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const ThemePtr t = currentTheme();
    p.fillRect(viewport()->rect(), m_background.isValid() ? m_background : t->color(QStringLiteral("--background-primary")));
    if (!m_doc || !m_styles)
        return;
    const double docTop = scrollTop();
    p.translate(m_contentLeft, -docTop);
    DocLayout::PaintContext ctx;
    ctx.selection = m_sel.ranges();
    ctx.clip = QRectF(-m_contentLeft, docTop, viewport()->width(), viewport()->height());
    ctx.rightEdge = m_layout.contentWidth();
    p.setRenderHint(QPainter::TextAntialiasing);
    m_layout.paint(&p, ctx);

    if (hasFocus() && m_caretOn && !isReadOnly() && m_cursorShape == CursorShape::Block) {
        for (int i = 0; i < m_sel.size(); ++i)
            if (!m_cursorMainOnly || i == m_sel.mainIndex())
                paintBlockCursor(p, m_sel.ranges()[i]);
    } else if (hasFocus() && m_caretOn && !isReadOnly()) {
        const QColor caret = m_styles->caret();
        for (const SelRange& r : m_sel.ranges()) {
            QRectF cr = m_layout.caretRect(r.head);
            if (!m_preedit.isEmpty() && r == m_sel.main()) {
                // caret inside the composition text
                const QRectF end = m_layout.caretRect(r.head + 0);
                cr = end;
            }
            p.fillRect(QRectF(std::round(cr.x()), cr.y(), 1.5, cr.height()), caret);
        }
    }
    p.resetTransform();
    paintFoldIndicators(p);
    if (m_dropPos >= 0) { // where a drag would drop
        const QRectF r = caretRectAt(m_dropPos);
        p.fillRect(QRectF(std::round(r.x()), r.y(), 2, r.height()), t->color(QStringLiteral("--text-accent")));
    }
}

void EditorView::dragEnterEvent(QDragEnterEvent* e)
{
    if (m_doc && !isReadOnly() && (e->mimeData()->hasUrls() || e->mimeData()->hasText() || e->mimeData()->hasImage())) {
        e->acceptProposedAction();
        dragMoveEvent(static_cast<QDragMoveEvent*>(static_cast<QDropEvent*>(e)));
    } else {
        e->ignore();
    }
}

void EditorView::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_doc || isReadOnly()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    const qsizetype pos = positionAt(e->position().toPoint());
    if (pos != m_dropPos) {
        m_dropPos = pos;
        viewport()->update();
    }
}

void EditorView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropPos = -1;
    viewport()->update();
}

void EditorView::dropEvent(QDropEvent* e)
{
    m_dropPos = -1;
    viewport()->update();
    if (!m_doc || isReadOnly()) {
        e->ignore();
        return;
    }
    const qsizetype pos = positionAt(e->position().toPoint());
    setSelection(Selection::cursor(pos), false);
    setFocus(Qt::MouseFocusReason);
    e->acceptProposedAction();
    if (EditorHooks::global().runDrop(*this, *e->mimeData(), pos))
        return;
    if (e->mimeData()->hasText())
        pasteText(e->mimeData()->text(), QStringLiteral("input.drop"));
}

void EditorView::restartBlink()
{
    m_caretOn = true;
    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    if (flash > 0 && hasFocus())
        m_blink.start(std::max(100, flash / 2), this);
    else
        m_blink.stop();
}

void EditorView::timerEvent(QTimerEvent* e)
{
    if (e->timerId() == m_blink.timerId()) {
        m_caretOn = !m_caretOn;
        for (const SelRange& r : m_sel.ranges()) {
            if (m_cursorShape == CursorShape::Block) {
                viewport()->update(blockCursorRect(r).adjusted(-2, -1, 2, 1).toAlignedRect());
                continue;
            }
            const QRectF cr = m_layout.caretRect(r.head);
            viewport()->update(QRectF(docToViewport(cr.topLeft()) - QPointF(2, 1), QSizeF(6, cr.height() + 2)).toAlignedRect());
        }
        return;
    }
    if (e->timerId() == m_autoScroll.timerId()) {
        autoScrollTick();
        return;
    }
    QAbstractScrollArea::timerEvent(e);
}

void EditorView::focusInEvent(QFocusEvent* e)
{
    QAbstractScrollArea::focusInEvent(e);
    restartBlink();
    viewport()->update();
    emit focusChangedIn(true);
}

void EditorView::focusOutEvent(QFocusEvent* e)
{
    QAbstractScrollArea::focusOutEvent(e);
    m_blink.stop();
    if (!m_preedit.isEmpty()) {
        QGuiApplication::inputMethod()->commit();
    }
    viewport()->update();
    emit focusChangedIn(false);
}

bool EditorView::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave) {
        setHoverLine(-1);
        updateHoveredLink(QPoint(-1, -1));
    } else if (e->type() == QEvent::LayoutRequest && m_topWidget) {
        // the top widget's content changed (updateGeometry() posts this to its parent)
        const int w = int(m_layout.contentWidth());
        const qreal h = m_topWidget->isHidden() ? 0
                        : (m_topWidget->hasHeightForWidth() ? m_topWidget->heightForWidth(w) : m_topWidget->sizeHint().height()) + m_topWidgetMargin;
        if (!qFuzzyCompare(h + 1, m_topWidgetH + 1))
            updateTopWidget();
    }
    return QAbstractScrollArea::viewportEvent(e);
}

bool EditorView::event(QEvent* e)
{
    // Key hooks (vim) see keys before anything else the editor does with them — not keys typed into
    // a child widget (the inline title, a bottom panel) that it let through.
    if ((e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) && m_doc) {
        auto* ke = static_cast<QKeyEvent*>(e);
        QWidget* focus = QApplication::focusWidget();
        const bool fromChild = focus && focus != this && isAncestorOf(focus);
        if (!fromChild && EditorHooks::global().runKey(*this, ke)) {
            ke->accept();
            return true;
        }
    }
    if (e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest) {
        const bool r = QAbstractScrollArea::event(e);
        if (m_bottomPanel)
            updateBottomPanel();
        return r;
    }
    // Keep Tab / Shift+Tab for indentation instead of focus traversal.
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QAbstractScrollArea::event(e);
}

// ---------------------------------------------------------------------------------------------
// document updates
// ---------------------------------------------------------------------------------------------
void EditorView::onDocumentChanged(const DocUpdate& u)
{
    const Selection oldSel = m_sel;
    // Scroll anchor: the doc position at the top of the viewport, mapped through the change.
    const double docTop = scrollTop();
    const int topLineOld = m_layout.lineAtY(std::max(0.0, docTop));
    const double topOffset = docTop - m_layout.lineTop(topLineOld);
    const qsizetype anchorOld = u.before.lineStart(std::min(topLineOld, u.before.lineCount() - 1));

    int oldFrom = INT_MAX, oldTo = -1, newFrom = INT_MAX, newTo = -1;
    u.tr.changes.iterChangedRanges([&](qsizetype fa, qsizetype ta, qsizetype fb, qsizetype tb) {
        oldFrom = std::min(oldFrom, u.before.lineAt(fa));
        oldTo = std::max(oldTo, u.before.lineAt(ta));
        newFrom = std::min(newFrom, u.after.lineAt(fb));
        newTo = std::max(newTo, u.after.lineAt(tb));
    });
    if (oldTo >= 0)
        m_layout.replaceLines(u.after, std::min(oldFrom, newFrom), oldTo + 1, newTo + 1);
    else
        m_layout.replaceLines(u.after, 0, 0, 0);
    if (u.syntax.structural || u.syntax.toLine > u.syntax.fromLine)
        m_layout.invalidateLines(u.syntax.fromLine, u.syntax.toLine);
    mapFolds(u);

    if (u.origin == this && u.tr.selection)
        m_sel = u.tr.selection->clamped(u.after.length());
    else
        m_sel = m_sel.map(u.tr.changes.desc()).clamped(u.after.length());
    m_sel = skipFolds(m_sel, oldSel.map(u.tr.changes.desc()).clamped(u.after.length()));

    const bool ownScroll = u.origin == this && u.tr.scrollIntoView;
    if (!ownScroll && docTop > 0) {
        const qsizetype anchorNew = u.tr.changes.mapPos(anchorOld, -1);
        const int line = u.after.lineAt(anchorNew);
        m_scrollTop = m_layout.lineTop(line) + topOffset + m_marginTop;
    }
    updateScrollbar();
    ensureVisibleLayout();
    if (ownScroll)
        scrollToPos(m_sel.main().head);
    if (m_mode == Mode::LivePreview && m_drag == DragUnit::None)
        syncRevealSelection(m_revealed.map(u.tr.changes.desc()));
    viewport()->update();
    restartBlink();
    emit selectionChanged();
    emitCursor();
}

void EditorView::onSyntaxProgress(const md::TreeUpdate& u)
{
    if (u.toLine > u.fromLine) {
        m_layout.invalidateLines(u.fromLine, u.toLine);
        ensureVisibleLayout();
        viewport()->update();
    }
}

void EditorView::invalidateSelectionLines(const Selection& a, const Selection& b)
{
    if (m_mode != Mode::LivePreview || m_drag != DragUnit::None)
        return; // source/reading decorations don't depend on the selection; reveal is frozen while dragging
    syncRevealSelection(a);
    Q_UNUSED(b);
}

void EditorView::syncRevealSelection(const Selection& old)
{
    if (m_mode != Mode::LivePreview)
        return;
    const Selection now = m_sel;
    m_lp->setSelection(now);
    for (const auto& [from, to] : m_lp->affectedLines(old.clamped(m_doc->text().length()), now))
        m_layout.invalidateLines(from, to);
    m_revealed = now;
    ensureVisibleLayout();
}

void EditorView::emitCursor()
{
    if (!m_doc)
        return;
    const qsizetype h = m_sel.main().head;
    const int line = m_doc->text().lineAt(h);
    emit cursorMoved(line, int(h - m_doc->text().lineStart(line)));
    if (QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImAnchorPosition | Qt::ImSurroundingText);
}

void EditorView::setSelection(const Selection& sel, bool scroll, bool userEvent)
{
    if (!m_doc)
        return;
    const Selection old = m_sel;
    m_sel = skipFolds(sel.clamped(m_doc->text().length()), old);
    if (m_sel == old && !scroll)
        return;
    if (userEvent)
        m_doc->noteSelectionChange();
    invalidateSelectionLines(old, m_sel);
    if (scroll)
        scrollToPos(m_sel.main().head);
    restartBlink();
    viewport()->update();
    updatePrimarySelection();
    emit selectionChanged();
    emitCursor();
}

void EditorView::updatePrimarySelection()
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb && cb->supportsSelection() && !m_sel.main().empty())
        cb->setText(selectedText(), QClipboard::Selection);
}

QString EditorView::selectedText() const
{
    if (!m_doc)
        return {};
    QStringList parts;
    for (const SelRange& r : m_sel.ranges())
        if (!r.empty())
            parts << m_doc->text().slice(r.from(), r.to());
    return parts.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------------------------
void EditorView::dispatch(const Transaction& tr)
{
    if (!m_doc || isReadOnly())
        return;
    m_doc->dispatch(tr, this, m_sel);
}

void EditorView::replaceSelections(const QString& text, const QString& userEvent)
{
    if (!m_doc || isReadOnly())
        return;
    QList<ChangeSpec> specs;
    for (const SelRange& r : m_sel.ranges())
        specs.push_back({r.from(), r.to(), text});
    Transaction tr;
    tr.changes = ChangeSet::of(specs, m_doc->text().length());
    QList<SelRange> after;
    for (const SelRange& r : m_sel.ranges()) {
        const qsizetype end = tr.changes.mapPos(r.to(), 1);
        after.push_back(SelRange::cursor(end));
    }
    tr.selection = Selection::create(after, m_sel.mainIndex());
    tr.userEvent = userEvent;
    dispatch(tr);
}

void EditorView::pasteText(const QString& raw, const QString& userEvent)
{
    QString text = raw;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (m_sel.size() > 1) {
        const QStringList lines = text.split(QLatin1Char('\n'));
        if (lines.size() == m_sel.size()) {
            QList<ChangeSpec> specs;
            for (int i = 0; i < m_sel.size(); ++i)
                specs.push_back({m_sel.ranges()[i].from(), m_sel.ranges()[i].to(), lines[i]});
            Transaction tr;
            tr.changes = ChangeSet::of(specs, m_doc->text().length());
            QList<SelRange> after;
            for (const SelRange& r : m_sel.ranges())
                after.push_back(SelRange::cursor(tr.changes.mapPos(r.to(), 1)));
            tr.selection = Selection::create(after, m_sel.mainIndex());
            tr.userEvent = userEvent;
            dispatch(tr);
            return;
        }
    }
    replaceSelections(text, userEvent);
}

bool EditorView::undo()
{
    if (!m_doc || isReadOnly())
        return false;
    auto sel = m_doc->undo(this, m_sel);
    if (!sel)
        return false;
    setSelection(*sel, true, false);
    return true;
}

bool EditorView::redo()
{
    if (!m_doc || isReadOnly())
        return false;
    auto sel = m_doc->redo(this, m_sel);
    if (!sel)
        return false;
    setSelection(*sel, true, false);
    return true;
}

void EditorView::selectAll()
{
    if (m_doc)
        setSelection(Selection::single(0, m_doc->text().length()), false);
}

void EditorView::copy()
{
    if (!m_doc)
        return;
    QString text = selectedText();
    if (text.isEmpty()) {
        // Copy the whole line when nothing is selected (CodeMirror behaviour).
        const Text& t = m_doc->text();
        const int line = t.lineAt(m_sel.main().head);
        text = t.line(line) + QLatin1Char('\n');
    }
    QGuiApplication::clipboard()->setText(text);
}

void EditorView::cut()
{
    if (!m_doc || isReadOnly())
        return;
    if (m_sel.allEmpty()) {
        copy();
        deleteLines();
        return;
    }
    copy();
    replaceSelections(QString(), QStringLiteral("delete.cut"));
}

void EditorView::paste()
{
    if (!m_doc || isReadOnly())
        return;
    const QMimeData* md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return;
    if (EditorHooks::global().runPaste(*this, *md))
        return;
    if (md->hasText())
        pasteText(md->text());
}

qsizetype EditorView::clusterBoundary(qsizetype pos, int dir) const
{
    const Text& t = m_doc->text();
    const int line = t.lineAt(pos);
    const qsizetype ls = t.lineStart(line);
    const QString s = t.line(line);
    const int col = int(pos - ls);
    if (dir < 0) {
        if (col == 0)
            return std::max<qsizetype>(0, pos - 1);
        QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, s);
        f.setPosition(col);
        const qsizetype prev = f.toPreviousBoundary();
        return ls + (prev < 0 ? 0 : prev);
    }
    if (col >= s.size())
        return std::min(t.length(), pos + 1);
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, s);
    f.setPosition(col);
    const qsizetype next = f.toNextBoundary();
    return ls + (next < 0 ? s.size() : next);
}

qsizetype EditorView::wordBoundary(qsizetype pos, int dir) const
{
    // CodeMirror "group" movement: skip spaces, then a run of the same character category.
    const Text& t = m_doc->text();
    const int line = t.lineAt(pos);
    const qsizetype ls = t.lineStart(line);
    const QString s = t.line(line);
    int col = int(pos - ls);
    if (dir < 0) {
        if (col == 0)
            return std::max<qsizetype>(0, pos - 1);
        while (col > 0 && categorize(s[col - 1]) == CharCat::Space)
            --col;
        if (col == 0)
            return ls;
        const CharCat cat = categorize(s[col - 1]);
        while (col > 0 && categorize(s[col - 1]) == cat)
            --col;
        return ls + col;
    }
    if (col >= s.size())
        return std::min(t.length(), pos + 1);
    while (col < s.size() && categorize(s[col]) == CharCat::Space)
        ++col;
    if (col >= s.size())
        return ls + s.size();
    const CharCat cat = categorize(s[col]);
    while (col < s.size() && categorize(s[col]) == cat)
        ++col;
    return ls + col;
}

void EditorView::deleteBackward(bool byWord)
{
    if (!m_doc || isReadOnly())
        return;
    QList<ChangeSpec> specs;
    for (const SelRange& r : m_sel.ranges()) {
        if (!r.empty()) {
            specs.push_back({r.from(), r.to(), QString()});
            continue;
        }
        const qsizetype from = byWord ? wordBoundary(r.head, -1) : clusterBoundary(r.head, -1);
        if (from < r.head)
            specs.push_back({from, r.head, QString()});
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, m_doc->text().length());
    QList<SelRange> after;
    for (const SelRange& r : m_sel.ranges())
        after.push_back(SelRange::cursor(tr.changes.mapPos(r.from(), -1)));
    tr.selection = Selection::create(after, m_sel.mainIndex());
    tr.userEvent = QStringLiteral("delete.backward");
    dispatch(tr);
}

void EditorView::deleteForward(bool byWord)
{
    if (!m_doc || isReadOnly())
        return;
    QList<ChangeSpec> specs;
    for (const SelRange& r : m_sel.ranges()) {
        if (!r.empty()) {
            specs.push_back({r.from(), r.to(), QString()});
            continue;
        }
        const qsizetype to = byWord ? wordBoundary(r.head, 1) : clusterBoundary(r.head, 1);
        if (to > r.head)
            specs.push_back({r.head, to, QString()});
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, m_doc->text().length());
    QList<SelRange> after;
    for (const SelRange& r : m_sel.ranges())
        after.push_back(SelRange::cursor(tr.changes.mapPos(r.from(), -1)));
    tr.selection = Selection::create(after, m_sel.mainIndex());
    tr.userEvent = QStringLiteral("delete.forward");
    dispatch(tr);
}

void EditorView::insertNewline() { replaceSelections(QStringLiteral("\n"), QStringLiteral("input.type")); }

void EditorView::indentSelection(bool more)
{
    if (!m_doc || isReadOnly())
        return;
    const Text& t = m_doc->text();
    if (more && m_sel.allEmpty()) {
        replaceSelections(QStringLiteral("\t"));
        return;
    }
    QList<ChangeSpec> specs;
    int lastLine = -1;
    for (const SelRange& r : m_sel.ranges()) {
        const int la = t.lineAt(r.from());
        int lb = t.lineAt(r.to());
        if (lb > la && r.to() == t.lineStart(lb))
            --lb;
        for (int l = std::max(la, lastLine + 1); l <= lb; ++l) {
            const qsizetype ls = t.lineStart(l);
            if (more) {
                specs.push_back({ls, ls, QStringLiteral("\t")});
            } else {
                const QStringView v = t.lineView(l);
                int n = 0;
                if (!v.isEmpty() && v[0] == u'\t')
                    n = 1;
                else
                    while (n < 4 && n < v.size() && v[n] == u' ')
                        ++n;
                if (n)
                    specs.push_back({ls, ls + n, QString()});
            }
            lastLine = l;
        }
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    tr.selection = m_sel.map(tr.changes.desc(), 1);
    tr.userEvent = more ? QStringLiteral("input.indent") : QStringLiteral("delete.dedent");
    dispatch(tr);
}

void EditorView::moveLines(int direction)
{
    if (!m_doc || isReadOnly())
        return;
    const Text& t = m_doc->text();
    const SelRange r = m_sel.main();
    int la = t.lineAt(r.from());
    int lb = t.lineAt(r.to());
    if (lb > la && r.to() == t.lineStart(lb))
        --lb;
    if ((direction < 0 && la == 0) || (direction > 0 && lb >= t.lineCount() - 1))
        return;
    const qsizetype blockFrom = t.lineStart(la), blockTo = t.lineEnd(lb);
    const QString block = t.slice(blockFrom, blockTo);
    Transaction tr;
    if (direction < 0) {
        const qsizetype prevStart = t.lineStart(la - 1);
        const QString prev = t.line(la - 1);
        tr.changes = ChangeSet::of({{prevStart, blockTo, block + QLatin1Char('\n') + prev}}, t.length());
        const qsizetype shift = -(prev.size() + 1);
        tr.selection = Selection::single(r.anchor + shift, r.head + shift);
    } else {
        const QString next = t.line(lb + 1);
        tr.changes = ChangeSet::of({{blockFrom, t.lineEnd(lb + 1), next + QLatin1Char('\n') + block}}, t.length());
        const qsizetype shift = next.size() + 1;
        tr.selection = Selection::single(r.anchor + shift, r.head + shift);
    }
    tr.userEvent = QStringLiteral("move.line");
    dispatch(tr);
}

void EditorView::copyLines(int direction)
{
    if (!m_doc || isReadOnly())
        return;
    const Text& t = m_doc->text();
    const SelRange r = m_sel.main();
    const int la = t.lineAt(r.from());
    int lb = t.lineAt(r.to());
    if (lb > la && r.to() == t.lineStart(lb))
        --lb;
    const QString block = t.slice(t.lineStart(la), t.lineEnd(lb));
    Transaction tr;
    tr.changes = ChangeSet::of({{t.lineEnd(lb), t.lineEnd(lb), QLatin1Char('\n') + block}}, t.length());
    if (direction > 0) {
        const qsizetype shift = block.size() + 1;
        tr.selection = Selection::single(r.anchor + shift, r.head + shift);
    } else {
        tr.selection = m_sel;
    }
    tr.userEvent = QStringLiteral("input.copyline");
    dispatch(tr);
}

void EditorView::deleteLines()
{
    if (!m_doc || isReadOnly())
        return;
    const Text& t = m_doc->text();
    QList<ChangeSpec> specs;
    int lastLine = -1;
    for (const SelRange& r : m_sel.ranges()) {
        int la = t.lineAt(r.from());
        int lb = t.lineAt(r.to());
        if (lb > la && r.to() == t.lineStart(lb) && !r.empty())
            --lb;
        la = std::max(la, lastLine + 1);
        if (la > lb)
            continue;
        qsizetype from = t.lineStart(la), to = t.lineEnd(lb);
        if (lb + 1 < t.lineCount())
            to += 1;
        else if (la > 0)
            from -= 1;
        specs.push_back({from, to, QString()});
        lastLine = lb;
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    tr.selection = Selection::cursor(tr.changes.mapPos(m_sel.main().from(), -1));
    tr.userEvent = QStringLiteral("delete.line");
    dispatch(tr);
}

void EditorView::addCursorVertical(int direction)
{
    if (!m_doc)
        return;
    const SelRange edge = direction < 0 ? m_sel.ranges().first() : m_sel.ranges().last();
    const QRectF cr = m_layout.caretRect(edge.head);
    const qsizetype np = m_layout.verticalMove(edge.head, cr.x(), direction);
    if (np == edge.head)
        return;
    setSelection(m_sel.addRange(SelRange::cursor(np), true));
}

void EditorView::collapseToMainCursor()
{
    if (m_sel.size() > 1)
        setSelection(m_sel.asSingle());
    else if (!m_sel.main().empty())
        setSelection(Selection::cursor(m_sel.main().head));
}

// ---------------------------------------------------------------------------------------------
// movement
// ---------------------------------------------------------------------------------------------
void EditorView::moveHorizontal(int dir, bool byWord, bool extend)
{
    if (!m_doc)
        return;
    QList<SelRange> out;
    for (const SelRange& r : m_sel.ranges()) {
        if (!extend && !r.empty()) {
            out.push_back(SelRange::cursor(dir < 0 ? r.from() : r.to()));
            continue;
        }
        const qsizetype np = byWord ? wordBoundary(r.head, dir) : clusterBoundary(r.head, dir);
        out.push_back(extend ? r.extend(np) : SelRange::cursor(np));
    }
    setSelection(Selection::create(out, m_sel.mainIndex()));
}

void EditorView::moveVertical(int dir, bool extend, int pages)
{
    if (!m_doc)
        return;
    QList<SelRange> out;
    const double pageH = viewport()->height() * 0.9;
    for (const SelRange& r : m_sel.ranges()) {
        const QRectF cr = m_layout.caretRect(r.head);
        const float goal = r.hasGoal() ? r.goalX : float(cr.x());
        qsizetype np = r.head;
        if (pages) {
            const QPointF target(goal, cr.center().y() + dir * pageH * pages);
            if (target.y() < 0)
                np = 0;
            else if (target.y() >= m_layout.totalHeight())
                np = m_doc->text().length();
            else
                np = m_layout.hitTest(target);
        } else {
            np = m_layout.verticalMove(r.head, goal, dir);
        }
        SelRange nr = extend ? SelRange(r.anchor, np) : SelRange::cursor(np);
        nr.goalX = goal;
        out.push_back(nr);
    }
    Selection s = Selection::create(out, m_sel.mainIndex());
    setSelection(s);
    if (pages) {
        setScrollTop(scrollTop() + dir * pageH * pages);
        scrollToPos(m_sel.main().head);
    }
}

void EditorView::moveLineBoundary(bool forward, bool extend)
{
    if (!m_doc)
        return;
    const Text& t = m_doc->text();
    QList<SelRange> out;
    for (const SelRange& r : m_sel.ranges()) {
        qsizetype np;
        if (forward) {
            np = m_layout.visualLineEnd(r.head);
            if (np == r.head)
                np = t.lineEnd(t.lineAt(r.head));
        } else {
            const int line = t.lineAt(r.head);
            const qsizetype ls = t.lineStart(line);
            const QStringView v = t.lineView(line);
            int indent = 0;
            while (indent < v.size() && (v[indent] == u' ' || v[indent] == u'\t'))
                ++indent;
            const qsizetype vstart = m_layout.visualLineStart(r.head);
            if (vstart > ls && vstart != r.head)
                np = vstart;
            else if (r.head != ls + indent && vstart == ls)
                np = ls + indent;
            else
                np = ls;
        }
        out.push_back(extend ? r.extend(np) : SelRange::cursor(np));
    }
    setSelection(Selection::create(out, m_sel.mainIndex()));
}

void EditorView::moveDocBoundary(bool forward, bool extend)
{
    if (!m_doc)
        return;
    const qsizetype np = forward ? m_doc->text().length() : 0;
    const SelRange r = m_sel.main();
    setSelection(extend ? Selection::single(r.anchor, np) : Selection::cursor(np));
}

// ---------------------------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------------------------
void EditorView::keyPressEvent(QKeyEvent* e)
{
    if (!m_doc) {
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool meta = mods & Qt::MetaModifier;

    if (e == QKeySequence::Undo) {
        undo();
        return;
    }
    if (e == QKeySequence::Redo || (ctrl && shift && e->key() == Qt::Key_Z)) {
        redo();
        return;
    }
    if (e == QKeySequence::Copy) {
        copy();
        return;
    }
    if (e == QKeySequence::Cut) {
        cut();
        return;
    }
    if (e == QKeySequence::Paste) {
        paste();
        return;
    }
    if (e == QKeySequence::SelectAll) {
        selectAll();
        return;
    }

    // Folding (CodeMirror's fold keymap): Ctrl+Shift+[ / ] fold / unfold, Ctrl+Alt+[ / ] all.
    if (ctrl && (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BraceLeft || e->key() == Qt::Key_BracketRight
                 || e->key() == Qt::Key_BraceRight)) {
        const bool open = e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BraceLeft;
        if (alt && !shift) {
            open ? foldAll() : unfoldAll();
            return;
        }
        if (shift && !alt) {
            open ? foldAtSelection() : unfoldAtSelection();
            return;
        }
    }

    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
        moveHorizontal(e->key() == Qt::Key_Left ? -1 : 1, ctrl, shift);
        return;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int dir = e->key() == Qt::Key_Up ? -1 : 1;
        if (ctrl && alt) {
            addCursorVertical(dir);
        } else if (alt && shift) {
            copyLines(dir);
        } else if (alt) {
            moveLines(dir);
        } else if (ctrl && !shift) {
            setScrollTop(scrollTop() + dir * m_styles->line(StyleId::Normal).lineBox);
        } else {
            moveVertical(dir, shift);
        }
        return;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        moveVertical(e->key() == Qt::Key_PageUp ? -1 : 1, shift, 1);
        return;
    case Qt::Key_Home:
    case Qt::Key_End:
        if (ctrl)
            moveDocBoundary(e->key() == Qt::Key_End, shift);
        else
            moveLineBoundary(e->key() == Qt::Key_End, shift);
        return;
    case Qt::Key_Escape:
        collapseToMainCursor();
        return;
    default:
        break;
    }

    if (isReadOnly()) {
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }

    switch (e->key()) {
    case Qt::Key_Backspace:
        if (!ctrl && !alt && mdcmd::deleteMarkupBackward(*this))
            return;
        deleteBackward(ctrl || alt);
        return;
    case Qt::Key_Delete:
        if (shift && !ctrl) {
            cut();
            return;
        }
        deleteForward(ctrl || alt);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!ctrl && !alt) {
            if (!shift && mdcmd::newlineAndContinueMarkup(*this))
                return;
            insertNewline();
            return;
        }
        break;
    case Qt::Key_Tab:
        if (!ctrl && !alt) {
            if (mdcmd::indentListItem(*this, true))
                return;
            indentSelection(true);
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (mdcmd::indentListItem(*this, false))
            return;
        indentSelection(false);
        return;
    case Qt::Key_K:
        if (ctrl && shift) {
            deleteLines();
            return;
        }
        break;
    case Qt::Key_D:
        if (ctrl && !shift && !alt) {
            deleteLines();
            return;
        }
        break;
    default:
        break;
    }

    const QString text = e->text();
    const bool altGr = (mods & (Qt::ControlModifier | Qt::AltModifier)) == (Qt::ControlModifier | Qt::AltModifier);
    if (!text.isEmpty() && (!ctrl || altGr) && !meta && text.at(0).isPrint()) {
        const SelRange main = m_sel.main();
        if (EditorHooks::global().runInput(*this, main.from(), main.to(), text))
            return;
        if (mdcmd::autoPairInput(*this, text))
            return;
        replaceSelections(text, QStringLiteral("input.type"));
        return;
    }
    if (!text.isEmpty() && text == QStringLiteral("\t")) {
        replaceSelections(text);
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

// ---------------------------------------------------------------------------------------------
// IME
// ---------------------------------------------------------------------------------------------
void EditorView::inputMethodEvent(QInputMethodEvent* e)
{
    if (!m_doc || isReadOnly()) {
        e->ignore();
        return;
    }
    const QString commit = e->commitString();
    const int line = m_doc->text().lineAt(m_sel.main().head);
    if (!commit.isEmpty() || e->replacementLength() != 0) {
        if (e->replacementLength() != 0 && m_sel.size() == 1 && m_sel.main().empty()) {
            const qsizetype from = std::max<qsizetype>(0, m_sel.main().head + e->replacementStart());
            const qsizetype to = std::min(m_doc->text().length(), from + e->replacementLength());
            Transaction tr;
            tr.changes = ChangeSet::of({{from, to, commit}}, m_doc->text().length());
            tr.selection = Selection::cursor(from + commit.size());
            tr.userEvent = QStringLiteral("input.type.compose");
            dispatch(tr);
        } else {
            replaceSelections(commit, QStringLiteral("input.type.compose"));
        }
    }
    m_preedit = e->preeditString();
    m_preeditCursor = int(m_preedit.size());
    for (const QInputMethodEvent::Attribute& a : e->attributes())
        if (a.type == QInputMethodEvent::Cursor)
            m_preeditCursor = a.start;
    // The composition is shown inline at the caret by applyExtraDecorations().
    const int caretLine = m_doc->text().lineAt(m_sel.main().head);
    m_layout.invalidateLines(std::min(line, caretLine), std::max(line, caretLine) + 1);
    ensureVisibleLayout();
    viewport()->update();
    e->accept();
}

QVariant EditorView::inputMethodQuery(Qt::InputMethodQuery q) const
{
    if (!m_doc)
        return QAbstractScrollArea::inputMethodQuery(q);
    const Text& t = m_doc->text();
    const qsizetype head = m_sel.main().head;
    const int line = t.lineAt(head);
    const qsizetype ls = t.lineStart(line);
    switch (q) {
    case Qt::ImEnabled:
        return !isReadOnly();
    case Qt::ImCursorRectangle: {
        QRectF r = caretRectInViewport();
        r.moveTopLeft(viewport()->mapTo(const_cast<EditorView*>(this), r.topLeft().toPoint()));
        return r.toAlignedRect();
    }
    case Qt::ImFont:
        return m_styles ? QVariant(m_styles->line(StyleId::Normal).font) : QVariant();
    case Qt::ImCursorPosition:
        return int(head - ls);
    case Qt::ImAnchorPosition: {
        const qsizetype a = m_sel.main().anchor;
        return t.lineAt(a) == line ? int(a - ls) : int(head - ls);
    }
    case Qt::ImSurroundingText:
        return t.line(line);
    case Qt::ImCurrentSelection:
        return selectedText();
    case Qt::ImHints:
        return int(Qt::ImhMultiLine);
    case Qt::ImAbsolutePosition:
        return int(head);
    default:
        return QAbstractScrollArea::inputMethodQuery(q);
    }
}

// ---------------------------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------------------------
void EditorView::selectWordAt(qsizetype pos, bool extend)
{
    const qsizetype a = wordBoundary(std::min(pos + 1, m_doc->text().length()), -1);
    const qsizetype b = wordBoundary(a, 1);
    if (extend)
        setSelection(Selection::single(std::min(m_dragOrigin.from(), a), std::max(m_dragOrigin.to(), b)), false);
    else
        setSelection(Selection::single(a, b), false);
}

void EditorView::selectLineAt(qsizetype pos, bool)
{
    const Text& t = m_doc->text();
    const int line = t.lineAt(pos);
    const qsizetype end = line + 1 < t.lineCount() ? t.lineStart(line + 1) : t.lineEnd(line);
    setSelection(Selection::single(t.lineStart(line), end), false);
}

void EditorView::mousePressEvent(QMouseEvent* e)
{
    if (!m_doc)
        return;
    setFocus(Qt::MouseFocusReason);
    if (QGuiApplication::inputMethod() && !m_preedit.isEmpty())
        QGuiApplication::inputMethod()->commit();
    const QPoint p = e->position().toPoint();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
        if (const int fl = foldLineAtIndicator(p); fl >= 0) {
            toggleFoldLine(fl);
            return;
        }
        if (auto hit = m_layout.widgetAt(viewportToDoc(e->position())); hit && isFoldPlaceholder(hit->widget.get())) {
            unfoldLine(hit->line);
            return;
        }
    }
    const qsizetype pos = positionAt(p);

    if (e->button() == Qt::MiddleButton) {
        QClipboard* cb = QGuiApplication::clipboard();
        if (cb->supportsSelection() && !isReadOnly()) {
            setSelection(Selection::cursor(pos), false);
            pasteText(cb->text(QClipboard::Selection));
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;

    if (m_mode != Mode::Source && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
        const QPointF docPt = viewportToDoc(e->position());
        if (auto bh = m_layout.blockAt(docPt)) {
            const QPointF local = docPt - bh->rect.topLeft();
            if (bh->widget->click(local, e->modifiers())) {
                m_layout.invalidateLines(bh->fromLine, bh->toLine);
                ensureVisibleLayout();
                updateScrollbar();
                viewport()->update();
                return;
            }
            const QString link = bh->widget->linkAt(local);
            if (!link.isEmpty()) {
                emit linkActivated(link, 0, e->modifiers() & Qt::ControlModifier);
                return;
            }
            if (m_mode == Mode::Reading)
                return;
        }
        if (handleWidgetClick(docPt))
            return;
        if (handleLinkClick(m_layout.hitTest(docPt), e->modifiers()))
            return;
    }

    const int dblInterval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    if (m_lastClickTime.isValid() && m_lastClickTime.elapsed() < dblInterval && (p - m_lastClickPos).manhattanLength() < 5)
        m_clickCount = m_clickCount % 3 + 1;
    else
        m_clickCount = 1;
    m_lastClickTime.restart();
    m_lastClickPos = p;

    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (m_clickCount == 1) {
        m_drag = DragUnit::Char;
        if (alt)
            setSelection(m_sel.addRange(SelRange::cursor(pos)), false);
        else if (shift)
            setSelection(Selection::single(m_sel.main().anchor, pos), false);
        else
            setSelection(Selection::cursor(pos), false);
        m_dragOrigin = m_sel.main();
    } else if (m_clickCount == 2) {
        m_drag = DragUnit::Word;
        selectWordAt(pos, false);
        m_dragOrigin = m_sel.main();
    } else {
        m_drag = DragUnit::Line;
        selectLineAt(pos, false);
        m_dragOrigin = m_sel.main();
    }
    m_lastMouse = p;
}

void EditorView::mouseDoubleClickEvent(QMouseEvent* e) { mousePressEvent(e); }

void EditorView::updateHoveredLink(const QPoint& vp)
{
    QString key;
    int kind = 0;
    QString target;
    QRect box;
    if (m_doc && vp.x() >= 0) {
        const qsizetype pos = positionAt(vp);
        if (const auto hit = m_lp->linkAt(pos); hit && hit->to > hit->from) {
            // positionAt() snaps to the nearest character: only count it when the point is on the link's box
            const QRectF a = caretRectAt(hit->from);
            const QRectF b = caretRectAt(hit->to);
            QRectF r;
            if (qFuzzyCompare(a.top() + 1, b.top() + 1))
                r = QRectF(QPointF(a.left(), a.top()), QPointF(b.left(), b.bottom()));
            else // wrapped over several visual lines: from the start line's top to the end line's bottom
                r = QRectF(QPointF(m_contentLeft, a.top()), QPointF(m_contentLeft + m_layout.contentWidth(), b.bottom()));
            if (r.contains(QPointF(vp))) {
                kind = int(hit->kind);
                target = hit->target;
                box = r.toAlignedRect();
                key = QString::number(kind) + u':' + QString::number(hit->from) + u':' + target;
            }
        }
    }
    if (key == m_hoverLink)
        return;
    m_hoverLink = key;
    emit linkHovered(target, kind, box);
}

void EditorView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_doc && e->buttons() == Qt::NoButton) {
        const QPointF docPt = viewportToDoc(e->position());
        setHoverLine(docPt.y() >= 0 && docPt.y() < m_layout.totalHeight() ? m_layout.lineAtY(docPt.y()) : -1);
        const bool overIndicator = foldLineAtIndicator(e->position().toPoint()) >= 0;
        updateHoveredLink(e->position().toPoint());
        const bool overLink = !m_hoverLink.isEmpty() && m_mode == Mode::Reading;
        viewport()->setCursor(overIndicator ? Qt::ArrowCursor
                                            : overLink ? Qt::PointingHandCursor
                                                       : (m_mode == Mode::Reading ? Qt::ArrowCursor : Qt::IBeamCursor));
    }
    if (!m_doc || m_drag == DragUnit::None || !(e->buttons() & Qt::LeftButton))
        return;
    m_lastMouse = e->position().toPoint();
    const qsizetype pos = positionAt(m_lastMouse);
    switch (m_drag) {
    case DragUnit::Char:
        setSelection(m_sel.withMain(SelRange(m_dragOrigin.anchor, pos)), false);
        break;
    case DragUnit::Word: {
        const qsizetype a = wordBoundary(std::min(pos + 1, m_doc->text().length()), -1);
        const qsizetype b = wordBoundary(a, 1);
        if (pos < m_dragOrigin.from())
            setSelection(Selection::single(m_dragOrigin.to(), a), false);
        else
            setSelection(Selection::single(m_dragOrigin.from(), std::max(b, m_dragOrigin.to())), false);
        break;
    }
    case DragUnit::Line: {
        const Text& t = m_doc->text();
        const int line = t.lineAt(pos);
        if (pos < m_dragOrigin.from())
            setSelection(Selection::single(m_dragOrigin.to(), t.lineStart(line)), false);
        else
            setSelection(Selection::single(m_dragOrigin.from(), line + 1 < t.lineCount() ? t.lineStart(line + 1) : t.lineEnd(line)), false);
        break;
    }
    default:
        break;
    }
    if (m_lastMouse.y() < 0 || m_lastMouse.y() > viewport()->height()) {
        if (!m_autoScroll.isActive())
            m_autoScroll.start(30, this);
    } else {
        m_autoScroll.stop();
    }
}

void EditorView::autoScrollTick()
{
    if (m_drag == DragUnit::None) {
        m_autoScroll.stop();
        return;
    }
    const int y = m_lastMouse.y();
    const double delta = y < 0 ? y : y - viewport()->height();
    setScrollTop(scrollTop() + std::clamp(delta, -60.0, 60.0));
    const qsizetype pos = positionAt(QPoint(m_lastMouse.x(), std::clamp(y, 0, viewport()->height() - 1)));
    setSelection(m_sel.withMain(SelRange(m_dragOrigin.anchor, pos)), false);
}

void EditorView::mouseReleaseEvent(QMouseEvent* e)
{
    Q_UNUSED(e);
    const bool wasDragging = m_drag != DragUnit::None;
    m_drag = DragUnit::None;
    m_autoScroll.stop();
    if (wasDragging && m_mode == Mode::LivePreview) {
        // Reveal was frozen during the drag; apply it now for the final selection.
        syncRevealSelection(m_revealed);
        viewport()->update();
    }
    updatePrimarySelection();
}

bool EditorView::handleWidgetClick(const QPointF& docPt)
{
    auto hit = m_layout.widgetAt(docPt);
    if (!hit || !m_doc)
        return false;
    if (auto* cb = dynamic_cast<CheckboxWidget*>(hit->widget.get())) {
        if (isReadOnly() && m_mode != Mode::Reading)
            return true;
        // Toggle the status char inside "[ ]" (Obsidian: any non-space counts as checked -> ' ').
        const qsizetype ls = m_doc->text().lineStart(hit->line);
        const qsizetype pos = ls + hit->docFrom + 1;
        Transaction tr;
        tr.changes = ChangeSet::of({{pos, pos + 1, cb->checked() ? QStringLiteral(" ") : QStringLiteral("x")}}, m_doc->text().length());
        tr.selection = m_sel;
        tr.userEvent = QStringLiteral("input.toggle-checkbox");
        tr.scrollIntoView = false;
        m_doc->dispatch(tr, this, m_sel);
        return true;
    }
    return false;
}

bool EditorView::handleLinkClick(qsizetype pos, Qt::KeyboardModifiers mods)
{
    auto hit = m_lp->linkAt(pos);
    if (!hit)
        return false;
    // In live preview a link is followed only when it's rendered (not being edited).
    if (m_mode == Mode::LivePreview) {
        bool active = false;
        for (const SelRange& r : m_sel.ranges())
            active |= r.from() <= hit->to && r.to() >= hit->from;
        if (active)
            return false;
    }
    const bool newLeaf = mods & Qt::ControlModifier;
    emit linkActivated(hit->target, int(hit->kind), newLeaf);
    return true;
}

void EditorView::contextMenuEvent(QContextMenuEvent* e)
{
    if (!m_doc)
        return;
    const qsizetype pos = positionAt(viewport()->mapFrom(this, e->pos()));
    bool inside = false;
    for (const SelRange& r : m_sel.ranges())
        inside |= pos >= r.from() && pos <= r.to() && !r.empty();
    if (!inside)
        setSelection(Selection::cursor(pos), false);
    QMenu menu(this);
    QAction* cutA = menu.addAction(tr("Cut"), this, &EditorView::cut);
    QAction* copyA = menu.addAction(tr("Copy"), this, &EditorView::copy);
    QAction* pasteA = menu.addAction(tr("Paste"), this, &EditorView::paste);
    menu.addSeparator();
    menu.addAction(tr("Select all"), this, &EditorView::selectAll);
    cutA->setEnabled(!isReadOnly() && !m_sel.allEmpty());
    copyA->setEnabled(!m_sel.allEmpty());
    pasteA->setEnabled(!isReadOnly());
    emit contextMenuRequestedAt(e->globalPos(), pos);
    emit contextMenuAboutToShow(&menu, pos); // owners add their entries (spelling suggestions first)
    if (!menu.isEmpty())
        menu.exec(e->globalPos());
}

} // namespace oc
