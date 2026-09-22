#include "editor/editor_view.h"

#include "editor/source_decorator.h"
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
#include <QTextBoundaryFinder>

#include <cmath>

namespace oc {

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
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalScrollBar()->setSingleStep(24);
    m_layout.setDecorator(m_decorator.get());
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, &EditorView::applyTheme);
    applyTheme();
}

EditorView::~EditorView()
{
    if (m_doc) {
        disconnect(m_docConn);
        disconnect(m_syntaxConn);
        disconnect(m_roConn);
    }
}

void EditorView::applyTheme()
{
    m_styles = StyleSheet::build(*currentTheme());
    m_decorator->setStyleSheet(m_styles.get());
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
    m_decorator->setDocument(m_doc.get());
    m_sel = Selection::cursor(0);
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
    m_marginTop = top;
    m_marginBottom = bottom;
    updateGeometryAndLayout();
}

void EditorView::setLineWrapping(bool) {}

void EditorView::setExtraDecorations(std::function<void(int, LineDecoration&)> fn)
{
    m_extra = std::move(fn);
    m_decorator->extra = m_extra;
    refreshDecorations();
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
    m_layout.setContentWidth(lineWidth);
    ensureVisibleLayout();
    updateScrollbar();
}

double EditorView::scrollTop() const { return m_scrollTop - m_marginTop; }

void EditorView::setScrollTop(double y)
{
    m_scrollTop = y + m_marginTop;
    updateScrollbar();
    ensureVisibleLayout();
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
        m_sel = Selection::single(toPos(c.value(QStringLiteral("from")).toObject()), toPos(c.value(QStringLiteral("to")).toObject()));
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
    p.fillRect(viewport()->rect(), t->color(QStringLiteral("--background-primary")));
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

    if (hasFocus() && m_caretOn && !isReadOnly()) {
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

bool EditorView::event(QEvent* e)
{
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

    if (u.origin == this && u.tr.selection)
        m_sel = u.tr.selection->clamped(u.after.length());
    else
        m_sel = m_sel.map(u.tr.changes.desc()).clamped(u.after.length());

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
    invalidateSelectionLines(oldSel, m_sel);
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

void EditorView::invalidateSelectionLines(const Selection&, const Selection&)
{
    // Source mode decorations don't depend on the selection. Live preview overrides this (M2).
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
    m_sel = sel.clamped(m_doc->text().length());
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
    if (md && md->hasText())
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
            insertNewline();
            return;
        }
        break;
    case Qt::Key_Tab:
        if (!ctrl && !alt) {
            indentSelection(true);
            return;
        }
        break;
    case Qt::Key_Backtab:
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
    // Show the composition inline at the caret (a zero-width replacement with underline).
    const qsizetype head = m_sel.main().head;
    const int caretLine = m_doc->text().lineAt(head);
    const int col = int(head - m_doc->text().lineStart(caretLine));
    auto userExtra = m_extra;
    const QString pre = m_preedit;
    m_decorator->extra = [userExtra, pre, caretLine, col](int l, LineDecoration& d) {
        if (userExtra)
            userExtra(l, d);
        if (l == caretLine && !pre.isEmpty()) {
            Replace r;
            r.from = col;
            r.to = col;
            r.kind = Replace::Kind::Text;
            r.text = pre;
            r.style = StyleId::Preedit;
            d.replaces.push_back(r);
            std::sort(d.replaces.begin(), d.replaces.end(), [](const Replace& a, const Replace& b) { return a.from < b.from; });
        }
    };
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

void EditorView::mouseMoveEvent(QMouseEvent* e)
{
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
    m_drag = DragUnit::None;
    m_autoScroll.stop();
    updatePrimarySelection();
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
    menu.exec(e->globalPos());
}

} // namespace oc
