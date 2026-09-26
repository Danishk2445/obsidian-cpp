#include "ui/plugins/file_recovery_ui.h"

#include "core/commands/command_registry.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/recovery/file_recovery.h"
#include "core/time/moment_format.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "text/codec.h"
#include "text/diff.h"
#include "ui/overlay/modal.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/widgets/search_input.h"
#include "ui/widgets/toggle_switch.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStackedLayout>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <optional>

namespace oc {

using namespace Qt::StringLiterals;

namespace {

QColor tok(const char* name, const QColor& fallback = {})
{
    return currentTheme()->color(QString::fromLatin1(name), fallback);
}

double tokPx(const char* name, double fallback)
{
    return currentTheme()->px(QString::fromLatin1(name), -1, fallback);
}

QColor withAlpha(QColor c, double a)
{
    c.setAlphaF(c.alphaF() * a);
    return c;
}

// Obsidian shows markdown paths without their extension.
QString displayPath(const QString& path)
{
    return path.endsWith(u".md"_s) ? path.chopped(3) : path;
}

QString snapshotText(const QByteArray& bytes)
{
    return decodeFile(bytes).text.toString();
}

// ---------------------------------------------------------------------------------------------
// Diff (Obsidian's file-recovery / sync-history diff view)
// ---------------------------------------------------------------------------------------------

enum class Op { Equal, Delete, Insert };
struct Hunk {
    Op op;
    QStringList lines;
};

QStringList diffLines(QString s)
{
    if (!s.endsWith(u'\n'))
        s += u'\n';
    QStringList lines = s.split(u'\n');
    if (lines.size() > 1 && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

int internId(QHash<QString, int>& ids, const QString& s)
{
    auto it = ids.constFind(s);
    if (it != ids.cend())
        return *it;
    const int id = int(ids.size());
    ids.insert(s, id);
    return id;
}

qsizetype charLength(const QStringList& lines)
{
    qsizetype n = 0;
    for (const QString& l : lines)
        n += l.size() + 1;
    return n;
}

// Merges runs of edits into one deletion followed by one insertion, and adjacent equalities.
QList<Hunk> mergeHunks(const QList<Hunk>& in)
{
    QList<Hunk> out;
    QStringList del, ins;
    const auto flush = [&] {
        if (!del.isEmpty())
            out.push_back({Op::Delete, std::exchange(del, {})});
        if (!ins.isEmpty())
            out.push_back({Op::Insert, std::exchange(ins, {})});
    };
    for (const Hunk& h : in) {
        if (h.lines.isEmpty())
            continue;
        if (h.op == Op::Delete) {
            del += h.lines;
        } else if (h.op == Op::Insert) {
            ins += h.lines;
        } else {
            flush();
            if (!out.isEmpty() && out.last().op == Op::Equal)
                out.last().lines += h.lines;
            else
                out.push_back(h);
        }
    }
    flush();
    return out;
}

// Line diff (diff-match-patch line mode) followed by the first phase of diff_cleanupSemantic: an
// equality no longer than the edits on both of its sides is turned into a deletion + insertion.
QList<Hunk> lineDiff(const QStringList& a, const QStringList& b)
{
    QHash<QString, int> ids;
    const auto encode = [&](const QStringList& lines) {
        std::vector<int> out;
        out.reserve(size_t(lines.size()));
        for (const QString& l : lines)
            out.push_back(internId(ids, l));
        return out;
    };
    const std::vector<int> ia = encode(a), ib = encode(b);
    QList<Hunk> hunks;
    qsizetype i = 0, j = 0;
    const auto push = [&](Op op, const QString& line) {
        if (!hunks.isEmpty() && hunks.last().op == op)
            hunks.last().lines << line;
        else
            hunks.push_back({op, {line}});
    };
    for (const auto& [pi, pj] : diffSequences(ia, ib)) {
        while (i < pi)
            push(Op::Delete, a[i++]);
        while (j < pj)
            push(Op::Insert, b[j++]);
        push(Op::Equal, a[i]);
        ++i;
        ++j;
    }
    while (i < a.size())
        push(Op::Delete, a[i++]);
    while (j < b.size())
        push(Op::Insert, b[j++]);
    hunks = mergeHunks(hunks);

    for (bool changed = true; changed;) {
        changed = false;
        for (qsizetype k = 1; k + 1 < hunks.size(); ++k) {
            if (hunks[k].op != Op::Equal)
                continue;
            qsizetype del1 = 0, ins1 = 0, del2 = 0, ins2 = 0;
            for (qsizetype p = k - 1; p >= 0 && hunks[p].op != Op::Equal; --p)
                (hunks[p].op == Op::Delete ? del1 : ins1) += charLength(hunks[p].lines);
            for (qsizetype p = k + 1; p < hunks.size() && hunks[p].op != Op::Equal; ++p)
                (hunks[p].op == Op::Delete ? del2 : ins2) += charLength(hunks[p].lines);
            const qsizetype len = charLength(hunks[k].lines);
            if ((del1 || ins1) && (del2 || ins2) && len <= std::max(del1, ins1) && len <= std::max(del2, ins2)) {
                const QStringList lines = hunks[k].lines;
                hunks[k] = {Op::Delete, lines};
                hunks.insert(k + 1, {Op::Insert, lines});
                hunks = mergeHunks(hunks);
                changed = true;
                break;
            }
        }
    }
    return hunks;
}

// diff_wordMode: tokens are runs of [A-Za-z0-9_] and single other characters (/\W/).
QStringList wordTokens(const QString& s)
{
    QStringList out;
    qsizetype start = 0;
    const auto isWord = [](QChar c) { return c.unicode() < 128 && (c.isLetterOrNumber() || c == u'_'); };
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (isWord(s[i]))
            continue;
        if (i > start)
            out << s.mid(start, i - start);
        out << s.mid(i, 1);
        start = i + 1;
    }
    if (start < s.size())
        out << s.mid(start);
    return out;
}

void addRange(QList<QPair<int, int>>& ranges, int from, int to)
{
    if (from >= to)
        return;
    if (!ranges.isEmpty() && ranges.last().second == from)
        ranges.last().second = to;
    else
        ranges.push_back({from, to});
}

// Word diff of two changed lines; returns false when they are too different (Levenshtein distance
// at least half the longer line), then the lines are shown whole.
bool wordDiff(const QString& left, const QString& right, QList<QPair<int, int>>& leftRanges, QList<QPair<int, int>>& rightRanges)
{
    const QStringList a = wordTokens(left), b = wordTokens(right);
    QHash<QString, int> ids;
    const auto encode = [&](const QStringList& tokens) {
        std::vector<int> out;
        out.reserve(size_t(tokens.size()));
        for (const QString& t : tokens)
            out.push_back(internId(ids, t));
        return out;
    };
    const std::vector<int> ia = encode(a), ib = encode(b);
    qsizetype i = 0, j = 0;
    int posA = 0, posB = 0;
    qsizetype levenshtein = 0, ins = 0, del = 0;
    const auto gap = [&](qsizetype toI, qsizetype toJ) {
        while (i < toI) {
            addRange(leftRanges, posA, posA + int(a[i].size()));
            del += a[i].size();
            posA += int(a[i++].size());
        }
        while (j < toJ) {
            addRange(rightRanges, posB, posB + int(b[j].size()));
            ins += b[j].size();
            posB += int(b[j++].size());
        }
    };
    for (const auto& [pi, pj] : diffSequences(ia, ib)) {
        gap(pi, pj);
        levenshtein += std::max(ins, del);
        ins = del = 0;
        posA += int(a[i++].size());
        posB += int(b[j++].size());
    }
    gap(a.size(), b.size());
    levenshtein += std::max(ins, del);
    return levenshtein < std::max(left.size(), right.size()) / 2.0;
}

// ---------------------------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------------------------

// .modal-sidebar.mod-history > .modal-sidebar-inner: 250 px, --background-secondary, 1 px end border;
// its left corners follow the modal's radius.
class Sidebar : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double r = std::max(0.0, tokPx("--modal-radius", 12) - 1);
        const QRectF box = QRectF(rect()).adjusted(0, 0, -1, 0);
        QPainterPath path;
        path.moveTo(box.right(), box.top());
        path.lineTo(box.left() + r, box.top());
        path.arcTo(QRectF(box.left(), box.top(), 2 * r, 2 * r), 90, 90);
        path.lineTo(box.left(), box.bottom() - r);
        path.arcTo(QRectF(box.left(), box.bottom() - 2 * r, 2 * r, 2 * r), 180, 90);
        path.lineTo(box.right(), box.bottom());
        path.closeSubpath();
        p.fillPath(path, tok("--background-secondary"));
        p.fillRect(QRectF(width() - 1, 0, 1, height()), tok("--background-modifier-border"));
    }
};

} // namespace

// .modal-setting-titlebar: the file name (ellipsized) with a bottom border.
class RecoveryTitle : public QWidget {
public:
    using QWidget::QWidget;
    void setText(const QString& text)
    {
        m_text = text;
        update();
    }
    const QString& text() const { return m_text; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(QRectF(0, height() - 1, width(), 1), tok("--background-modifier-border"));
        QFont f = currentTheme()->interfaceFont(tokPx("--font-ui-medium", 15));
        f.setWeight(QFont::Weight(int(currentTheme()->number(u"--file-header-font-weight"_s, 400))));
        p.setFont(f);
        p.setPen(tok("--text-normal"));
        const QRect r = contentsRect();
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(m_text, Qt::ElideRight, r.width()));
    }

private:
    QString m_text;
};

// .modal-sidebar-list: one .modal-sidebar-list-item per snapshot (time + muted size), the selected one
// with --interactive-accent. ↑/↓ move the keyboard focus, Enter or a click selects.
class RecoveryVersionList : public QAbstractScrollArea {
public:
    std::function<void(int)> onSelect;
    std::function<void()> onFocusInput;

    explicit RecoveryVersionList(QWidget* parent)
        : QAbstractScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        viewport()->setAttribute(Qt::WA_Hover);
        viewport()->setMouseTracking(true);
        viewport()->setAutoFillBackground(false);
        setAutoFillBackground(false);
    }

    void setItems(const QList<SnapshotStore::Info>& items)
    {
        m_items = items;
        m_active = m_focus = m_hover = -1;
        updateScroll();
        verticalScrollBar()->setValue(0);
        viewport()->update();
    }
    int count() const { return int(m_items.size()); }
    int active() const { return m_active; }
    void setActive(int index)
    {
        m_active = index;
        viewport()->update();
    }
    void focusAt(int index, bool visible)
    {
        if (m_items.isEmpty())
            return;
        m_focus = std::clamp(index, 0, count() - 1);
        m_focusVisible = visible;
        setFocus(Qt::OtherFocusReason);
        ensureVisible(m_focus);
        viewport()->update();
    }
    int focusIndex() const { return m_focus; }

protected:
    static int pad() { return int(tokPx("--size-4-2", 8)); }
    static int gap() { return int(tokPx("--size-4-1", 4)); }
    static QFont timeFont() { return currentTheme()->interfaceFont(tokPx("--font-ui-small", 13)); }
    static QFont sizeFont() { return currentTheme()->interfaceFont(tokPx("--font-ui-small", 13) * 0.8); }
    static int timeLine() { return int(std::round(tokPx("--font-ui-small", 13) * 1.5)); }
    static int sizeLine() { return int(std::round(tokPx("--font-ui-small", 13) * 0.8 * 1.5)); }
    static int rowHeight() { return 2 * pad() + timeLine() + sizeLine(); }
    QRect rowRect(int i) const
    {
        const int y = pad() + i * (rowHeight() + gap()) - verticalScrollBar()->value();
        return {pad(), y, viewport()->width() - 2 * pad(), rowHeight()};
    }
    int indexAt(const QPoint& pos) const
    {
        for (int i = 0; i < count(); ++i)
            if (rowRect(i).contains(pos))
                return i;
        return -1;
    }
    void updateScroll()
    {
        const int total = m_items.isEmpty() ? 0 : 2 * pad() + count() * rowHeight() + (count() - 1) * gap();
        verticalScrollBar()->setRange(0, std::max(0, total - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
        verticalScrollBar()->setSingleStep(rowHeight());
    }
    void ensureVisible(int i)
    {
        const QRect r = rowRect(i);
        if (r.top() < 0)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top() - pad());
        else if (r.bottom() > viewport()->height())
            verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height() + pad());
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        const double radius = tokPx("--radius-s", 4);
        for (int i = 0; i < count(); ++i) {
            const QRect r = rowRect(i);
            if (r.bottom() < 0 || r.top() > viewport()->height())
                continue;
            QPainterPath path;
            path.addRoundedRect(QRectF(r), radius, radius);
            const bool active = i == m_active;
            if (active)
                p.fillPath(path, tok("--interactive-accent"));
            else if (i == m_hover)
                p.fillPath(path, tok("--background-modifier-hover"));
            if (hasFocus() && m_focusVisible && i == m_focus) {
                p.setPen(QPen(tok("--background-modifier-border-focus"), 2));
                p.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), radius, radius);
            }
            const SnapshotStore::Info& info = m_items[i];
            const QRect text = r.adjusted(pad(), pad(), -pad(), -pad());
            p.setFont(timeFont());
            p.setPen(active ? tok("--text-on-accent", Qt::white) : tok("--text-normal"));
            const QString when = formatMoment(QDateTime::fromMSecsSinceEpoch(info.ts), u"llll"_s);
            p.drawText(QRect(text.left(), text.top(), text.width(), timeLine()), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(timeFont()).elidedText(when, Qt::ElideRight, text.width()));
            p.setFont(sizeFont());
            QColor muted = active ? tok("--text-on-accent", Qt::white) : tok("--text-muted");
            if (active)
                muted.setAlphaF(0.8);
            p.setPen(muted);
            p.drawText(QRect(text.left(), text.top() + timeLine(), text.width(), sizeLine()), Qt::AlignLeft | Qt::AlignVCenter,
                       formatByteSize(info.size));
        }
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QAbstractScrollArea::resizeEvent(e);
        updateScroll();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int h = indexAt(e->position().toPoint());
        if (h != m_hover) {
            m_hover = h;
            viewport()->update();
        }
    }
    void leaveEvent(QEvent* e) override
    {
        m_hover = -1;
        viewport()->update();
        QAbstractScrollArea::leaveEvent(e);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int i = indexAt(e->position().toPoint());
        if (i < 0 || e->button() != Qt::LeftButton)
            return;
        if (onSelect)
            onSelect(i);
        focusAt(i, false);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        switch (e->key()) {
        case Qt::Key_Up:
            if (m_focus <= 0 && onFocusInput) {
                m_focus = -1;
                onFocusInput();
            } else {
                focusAt(m_focus < 0 ? std::max(0, m_active) : m_focus - 1, true);
            }
            return;
        case Qt::Key_Down:
            focusAt(m_focus < 0 ? std::max(0, m_active) : m_focus + 1, true);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_focus >= 0 && onSelect) {
                onSelect(m_focus);
                return;
            }
            break;
        default:
            break;
        }
        e->ignore(); // Escape etc. reach the modal
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QAbstractScrollArea::focusOutEvent(e);
        viewport()->update();
    }
    void scrollContentsBy(int, int) override { viewport()->update(); }

private:
    QList<SnapshotStore::Info> m_items;
    int m_active = -1;
    int m_focus = -1;
    int m_hover = -1;
    bool m_focusVisible = false;
};

// .file-recovery-diff > .diff-view: removed lines tinted red, added lines green (changed words
// stronger), folded runs of equal lines as a centered "N lines folded" that expands on click.
class RecoveryDiffView : public QTextBrowser {
public:
    explicit RecoveryDiffView(QWidget* parent)
        : QTextBrowser(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setUndoRedoEnabled(false);
        document()->setDocumentMargin(tokPx("--size-4-6", 24));
        connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
            if (url.scheme() != u"fold"_s)
                return;
            const int index = url.path().toInt();
            if (index < 0 || index >= m_rows.size() || m_rows[index].kind != RecoveryDiffRow::Folded)
                return;
            const QList<RecoveryDiffRow> inner = m_rows[index].folded;
            m_rows.remove(index);
            for (qsizetype k = 0; k < inner.size(); ++k)
                m_rows.insert(index + k, inner[k]);
            rebuild();
        });
    }
    void setRows(QList<RecoveryDiffRow> rows)
    {
        m_rows = std::move(rows);
        verticalScrollBar()->setValue(0);
        rebuild();
    }
    const QList<RecoveryDiffRow>& rows() const { return m_rows; }

private:
    void rebuild()
    {
        const int scroll = verticalScrollBar()->value();
        const ThemePtr t = currentTheme();
        QPalette pal = palette();
        pal.setColor(QPalette::Base, tok("--background-primary"));
        pal.setColor(QPalette::Text, tok("--text-normal"));
        setPalette(pal);
        QTextDocument* doc = document();
        doc->clear();
        const QFont font = t->interfaceFont(tokPx("--font-ui-medium", 15));
        doc->setDefaultFont(font);
        QTextCursor c(doc);
        const QColor error = tok("--background-modifier-error", QColor(0xfb464c));
        const QColor success = tok("--background-modifier-success", QColor(0x44cf6e));
        const double sidePad = tokPx("--size-4-2", 8);
        bool first = true;
        for (qsizetype i = 0; i < m_rows.size(); ++i) {
            const RecoveryDiffRow& row = m_rows[i];
            QTextBlockFormat bf;
            bf.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
            QTextCharFormat cf;
            cf.setFont(font);
            cf.setForeground(tok("--text-normal"));
            if (row.kind == RecoveryDiffRow::Folded) {
                bf.setAlignment(Qt::AlignHCenter);
                bf.setTopMargin(sidePad);
                bf.setBottomMargin(sidePad);
                cf.setFont(t->interfaceFont(tokPx("--font-ui-small", 13)));
                cf.setForeground(tok("--text-muted"));
                cf.setAnchor(true);
                cf.setAnchorHref(u"fold:%1"_s.arg(i));
            } else {
                bf.setLeftMargin(sidePad);
                bf.setRightMargin(sidePad);
                if (row.kind == RecoveryDiffRow::Left)
                    bf.setBackground(withAlpha(error, 0.2));
                else if (row.kind == RecoveryDiffRow::Right)
                    bf.setBackground(withAlpha(success, 0.2));
            }
            if (first)
                c.setBlockFormat(bf);
            else
                c.insertBlock(bf);
            first = false;
            if (row.kind == RecoveryDiffRow::Folded) {
                c.insertText(u"%1 lines folded"_s.arg(row.folded.size()), cf);
                continue;
            }
            QTextCharFormat strong = cf;
            strong.setBackground(withAlpha(row.kind == RecoveryDiffRow::Left ? error : success, 0.4));
            int pos = 0;
            for (const auto& [from, to] : row.changed) {
                if (from > pos)
                    c.insertText(row.text.mid(pos, from - pos), cf);
                c.insertText(row.text.mid(from, to - from), strong);
                pos = to;
            }
            if (pos < row.text.size())
                c.insertText(row.text.mid(pos), cf);
        }
        verticalScrollBar()->setValue(scroll);
    }

    QList<RecoveryDiffRow> m_rows;
};

// ---------------------------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------------------------

QString formatByteSize(qint64 bytes)
{
    if (bytes <= 0)
        return u"0 B"_s;
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int n = 5;
    for (int i = 0; i < 6; ++i) {
        if (double(bytes) < std::pow(1024.0, i + 1)) {
            n = i;
            break;
        }
    }
    const double value = double(bytes) / std::pow(1024.0, n);
    QString num = QString::number(value, 'f', n == 0 ? 0 : 2);
    const qsizetype dot = num.indexOf(u'.');
    for (qsizetype p = (dot < 0 ? num.size() : dot) - 3; p > 0; p -= 3)
        num.insert(p, u',');
    return num + u' ' + QString::fromLatin1(units[n]);
}

QList<RecoveryDiffRow> recoveryDiff(const QString& before, const QString& after)
{
    const QList<Hunk> hunks = lineDiff(diffLines(before), diffLines(after));
    QList<RecoveryDiffRow> out, equal, left, right;
    const auto flushEqual = [&](bool atEnd) {
        if (equal.isEmpty())
            return;
        const bool atStart = out.isEmpty();
        const qsizetype limit = atStart || atEnd ? 5 : 10;
        if (equal.size() > limit) {
            RecoveryDiffRow fold;
            fold.kind = RecoveryDiffRow::Folded;
            if (atStart) {
                fold.folded = equal.mid(0, equal.size() - 3);
                out << fold << equal.mid(equal.size() - 3);
            } else if (atEnd) {
                fold.folded = equal.mid(3);
                out << equal.mid(0, 3) << fold;
            } else {
                fold.folded = equal.mid(3, equal.size() - 6);
                out << equal.mid(0, 3) << fold << equal.mid(equal.size() - 3);
            }
        } else {
            out << equal;
        }
        equal.clear();
    };
    const auto flushChanged = [&] {
        out << left << right;
        left.clear();
        right.clear();
    };
    // One line of each side (nullopt = none), like Obsidian's pairing of deleted and inserted lines.
    const auto line = [&](const std::optional<QString>& l, const std::optional<QString>& r) {
        const bool differ = l.has_value() != r.has_value() || (l && *l != *r);
        if (l && r && !l->isEmpty() && !r->isEmpty() && differ) {
            RecoveryDiffRow lr{RecoveryDiffRow::Left, *l, {}, {}};
            RecoveryDiffRow rr{RecoveryDiffRow::Right, *r, {}, {}};
            if (wordDiff(*l, *r, lr.changed, rr.changed)) {
                flushEqual(false);
                left << lr;
                right << rr;
                return;
            }
        }
        if (differ) {
            flushEqual(false);
            left << RecoveryDiffRow{RecoveryDiffRow::Left, l.value_or(QString()), {}, {}};
            right << RecoveryDiffRow{RecoveryDiffRow::Right, r.value_or(QString()), {}, {}};
        } else {
            flushChanged();
            equal << RecoveryDiffRow{RecoveryDiffRow::Equal, *l, {}, {}};
        }
    };
    for (qsizetype p = 0; p < hunks.size(); ++p) {
        const Hunk& h = hunks[p];
        if (h.op == Op::Delete && p + 1 < hunks.size() && hunks[p + 1].op == Op::Insert) {
            const QStringList& ins = hunks[p + 1].lines;
            const qsizetype n = std::max(h.lines.size(), ins.size());
            for (qsizetype y = 0; y < n; ++y)
                line(y < h.lines.size() ? std::optional(h.lines[y]) : std::nullopt,
                     y < ins.size() ? std::optional(ins[y]) : std::nullopt);
            ++p;
            continue;
        }
        for (const QString& l : h.lines) {
            if (h.op == Op::Insert)
                line(std::nullopt, l);
            else if (h.op == Op::Delete)
                line(l, std::nullopt);
            else
                line(l, l);
        }
    }
    flushChanged();
    flushEqual(true);
    return out;
}

FileRecovery* fileRecoveryOf(VaultSession* session)
{
    return session ? session->findChild<FileRecovery*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// FileRecoveryPanel
// ---------------------------------------------------------------------------------------------

FileRecoveryPanel::FileRecoveryPanel(MainWindow* window, FileRecovery* recovery, Modal* modal)
    : m_window(window)
    , m_recovery(recovery)
    , m_modal(modal)
{
    setObjectName(u"ocFileRecovery"_s);
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Sidebar: search input + snapshot list.
    m_sidebar = new Sidebar(this);
    m_sidebar->setFixedWidth(250);
    auto* sv = new QVBoxLayout(m_sidebar);
    const int inset = int(tokPx("--size-4-2", 8));
    sv->setContentsMargins(0, 0, 1, 0);
    sv->setSpacing(0);
    m_input = new SearchInput(m_sidebar);
    m_input->setPlaceholderText(tr("Choose a file..."));
    m_input->installEventFilter(this);
    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(inset, inset, inset, 0);
    inputRow->addWidget(m_input);
    sv->addLayout(inputRow);
    m_list = new RecoveryVersionList(m_sidebar);
    sv->addWidget(m_list, 1);
    h->addWidget(m_sidebar);

    // Content: empty text, or title bar + text / diff.
    auto* container = new QWidget(this);
    auto* pages = new QStackedLayout(container);
    auto* emptyPage = new QWidget(container);
    auto* cv = new QVBoxLayout(emptyPage);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    pages->addWidget(emptyPage);
    const QFont uiFont = currentTheme()->interfaceFont(tokPx("--font-ui-medium", 15));
    m_empty = new QLabel(tr("Please select a file on the left to view snapshots."), emptyPage);
    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_empty->setWordWrap(true);
    m_empty->setFont(uiFont);
    m_empty->setContentsMargins(24, 24, 24, 24);
    cv->addWidget(m_empty);
    m_noHistory = new QLabel(tr("No snapshots found."), emptyPage);
    m_noHistory->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_noHistory->setFont(uiFont);
    m_noHistory->setContentsMargins(24, 24, 24, 24);
    m_noHistory->hide();
    cv->addWidget(m_noHistory);
    cv->addStretch(1);

    m_content = new QWidget(container);
    auto* content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    m_title = new RecoveryTitle(m_content);
    auto* tb = new QHBoxLayout(m_title);
    // padding: 8px 40px 8px 16px (room for the modal's close button)
    tb->setContentsMargins(int(tokPx("--size-4-4", 16)), inset, int(tokPx("--size-4-10", 40)), inset + 1);
    tb->setSpacing(int(tokPx("--size-4-1", 4)));
    tb->addStretch(1);
    auto* toggleLabel = new QLabel(tr("Show changes"), m_title);
    toggleLabel->setFont(uiFont);
    tb->addSpacing(int(tokPx("--size-4-3", 12)));
    tb->addWidget(toggleLabel);
    tb->addSpacing(int(tokPx("--size-4-2", 8)) - tb->spacing());
    m_diffToggle = new ToggleSwitch(m_title);
    m_diffToggle->setObjectName(u"ocShowChanges"_s);
    m_diffToggle->setFocusPolicy(Qt::NoFocus);
    tb->addWidget(m_diffToggle, 0, Qt::AlignVCenter);
    tb->addSpacing(int(tokPx("--size-4-3", 12)) - tb->spacing());
    m_copyButton = Modal::createButton(tr("Copy"), Modal::ButtonStyle::Normal, m_title);
    m_restoreButton = Modal::createButton(tr("Restore"), Modal::ButtonStyle::Normal, m_title);
    tb->addWidget(m_copyButton);
    tb->addWidget(m_restoreButton);
    m_title->setContentsMargins(tb->contentsMargins());
    content->addWidget(m_title);

    auto* stackHost = new QWidget(m_content);
    auto* stack = new QStackedLayout(stackHost);
    m_textView = new QPlainTextEdit(stackHost);
    m_textView->setReadOnly(true);
    m_textView->setFrameShape(QFrame::NoFrame);
    m_textView->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_textView->document()->setDocumentMargin(tokPx("--size-4-6", 24));
    QPalette pal = m_textView->palette();
    pal.setColor(QPalette::Base, tok("--background-primary"));
    pal.setColor(QPalette::Text, tok("--text-normal"));
    pal.setColor(QPalette::Highlight, tok("--text-selection"));
    pal.setColor(QPalette::HighlightedText, tok("--text-normal"));
    m_textView->setPalette(pal);
    stack->addWidget(m_textView);
    m_diffView = new RecoveryDiffView(stackHost);
    stack->addWidget(m_diffView);
    content->addWidget(stackHost, 1);
    pages->addWidget(m_content);
    h->addWidget(container, 1);

    m_list->onSelect = [this](int i) { selectVersion(i); };
    m_list->onFocusInput = [this] { m_input->setFocus(Qt::OtherFocusReason); };
    connect(m_input, &QLineEdit::textEdited, this, [this] { updateSuggestions(); });
    connect(m_diffToggle, &QAbstractButton::toggled, this, [this] { applyToggle(); });
    connect(m_copyButton, &QPushButton::clicked, this, [this] { copy(); });
    connect(m_restoreButton, &QPushButton::clicked, this, [this] { restore(); });

    if (SnapshotStore* store = m_recovery ? m_recovery->store() : nullptr) {
        m_paths = store->paths();
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(m_paths.begin(), m_paths.end(), [&](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
    }
    if (m_paths.isEmpty()) {
        m_empty->hide();
        m_noHistory->show();
    }
}

FileRecoveryPanel::~FileRecoveryPanel()
{
    delete m_popup;
}

SuggestPopup* FileRecoveryPanel::suggestPopup() const
{
    return m_popup;
}

QString FileRecoveryPanel::title() const
{
    return m_title->text();
}

void FileRecoveryPanel::focusInput()
{
    m_input->setFocus(Qt::OtherFocusReason);
    updateSuggestions();
}

void FileRecoveryPanel::updateSuggestions()
{
    // The popup goes in the layer of the window this panel is in (the main window or a pop-out).
    OverlayLayer* layer = OverlayLayer::of(m_input);
    if (m_paths.isEmpty() || !m_window || !layer) {
        hidePopup();
        return;
    }
    const QString q = m_input->text().toLower();
    m_suggested.clear();
    QList<Suggestion> items;
    for (const QString& p : m_paths) {
        if (!p.toLower().contains(q))
            continue;
        m_suggested << p;
        Suggestion s;
        s.title = displayPath(p);
        items << s;
        if (m_suggested.size() == 100)
            break;
    }
    if (items.isEmpty()) {
        hidePopup();
        return;
    }
    if (!m_popup) {
        m_popup = new SuggestPopup(layer);
        m_popup->list()->setSuggestions({});
        connect(m_popup->list(), &SuggestionList::activated, this, [this](int i, Qt::KeyboardModifiers) { chooseSuggestion(i); });
    }
    m_popup->list()->setSuggestions(items);
    m_popup->list()->setSelectedIndex(0);
    const QRect anchor(m_input->mapTo(m_popup->parentWidget(), QPoint(0, 0)), m_input->size());
    m_popup->showAt(anchor.adjusted(0, 0, 0, 4));
}

void FileRecoveryPanel::hidePopup()
{
    if (m_popup)
        m_popup->hide();
}

void FileRecoveryPanel::chooseSuggestion(int index)
{
    if (index < 0 || index >= m_suggested.size())
        return;
    const QString path = m_suggested[index];
    hidePopup();
    switchToPath(path);
}

void FileRecoveryPanel::switchToPath(const QString& path)
{
    hidePopup();
    m_input->setText(displayPath(path));
    m_input->clearFocus();
    m_title->setText(displayPath(path));
    if (m_modal)
        m_modal->setTitle(displayPath(path));
    loadVersions(path);
    if (!m_versions.isEmpty())
        m_list->focusAt(0, false);
}

void FileRecoveryPanel::loadVersions(const QString& path)
{
    m_path = path;
    SnapshotStore* store = m_recovery ? m_recovery->store() : nullptr;
    m_versions = store ? store->history(path) : QList<SnapshotStore::Info>();
    m_list->setItems(m_versions);
    m_selected = -1;
    if (!m_versions.isEmpty())
        selectVersion(0);
}

void FileRecoveryPanel::selectVersion(int index)
{
    if (index < 0 || index >= m_versions.size())
        return;
    SnapshotStore* store = m_recovery ? m_recovery->store() : nullptr;
    if (!store)
        return;
    m_selected = index;
    m_list->setActive(index);
    m_bytes = store->bytes(m_versions[index].id).value_or(QByteArray());
    m_text = snapshotText(m_bytes);
    static_cast<QStackedLayout*>(m_content->parentWidget()->layout())->setCurrentWidget(m_content);
    openHistory();
}

void FileRecoveryPanel::openHistory()
{
    const bool md = m_path.endsWith(u".md"_s);
    const ThemePtr t = currentTheme();
    const QFont font = md ? t->textFont(t->px(u"--font-text-size"_s, -1, 16)) : t->monospaceFont(tokPx("--font-ui-medium", 15));
    m_textView->setFont(font);
    m_textView->document()->setDefaultFont(font);
    m_textView->setTabStopDistance(QFontMetricsF(font).horizontalAdvance(u' ') * 4);
    m_textView->setPlainText(m_text);
    m_diffBuilt = false;
    applyToggle();
}

void FileRecoveryPanel::applyToggle()
{
    const bool on = m_diffToggle->isChecked();
    auto* stack = static_cast<QStackedLayout*>(m_textView->parentWidget()->layout());
    stack->setCurrentWidget(on ? static_cast<QWidget*>(m_diffView) : m_textView);
    if (on && !m_diffBuilt && m_selected >= 0) {
        m_diffBuilt = true;
        QString previous;
        if (m_selected + 1 < m_versions.size())
            if (SnapshotStore* store = m_recovery ? m_recovery->store() : nullptr)
                previous = snapshotText(store->bytes(m_versions[m_selected + 1].id).value_or(QByteArray()));
        m_diffView->setRows(recoveryDiff(previous, m_text));
    }
}

void FileRecoveryPanel::setShowChanges(bool on)
{
    m_diffToggle->setChecked(on);
}

bool FileRecoveryPanel::showChanges() const
{
    return m_diffToggle->isChecked();
}

QList<RecoveryDiffRow> FileRecoveryPanel::diffRows() const
{
    return m_diffView->rows();
}

void FileRecoveryPanel::notice(const QString& text)
{
    if (m_window && m_window->notices())
        m_window->notices()->show(text);
}

void FileRecoveryPanel::copy()
{
    if (m_selected < 0)
        return;
    QApplication::clipboard()->setText(m_text);
    notice(tr("Copied to your clipboard"));
}

bool FileRecoveryPanel::restore(QString* error)
{
    if (m_selected < 0 || !m_window || !m_recovery)
        return false;
    VaultSession* s = m_window->session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!vault)
        return false;
    const QString path = m_path;
    const QByteArray bytes = m_bytes;
    const QString when = momentFromNow(QDateTime::fromMSecsSinceEpoch(m_versions[m_selected].ts));
    // Unsaved edits in an open editor are saved first (and so snapshotted), then replaced.
    DocumentManager* docs = s->documents();
    if (Document* doc = docs ? docs->find(path) : nullptr; doc && doc->isDirty())
        doc->save();
    QString err;
    bool ok = false;
    if (vault->file(path)) {
        ok = vault->write(path, bytes, vault->stat(path), nullptr, &err) == DocumentStorage::WriteResult::Ok;
    } else {
        const qsizetype slash = path.lastIndexOf(u'/');
        if (slash > 0 && !vault->folder(path.left(slash)))
            vault->createFolder(path.left(slash), &err);
        ok = vault->createFile(path, bytes, &err) != nullptr;
    }
    if (!ok) {
        if (error)
            *error = err;
        notice(tr("Failed to restore %1: %2").arg(displayPath(path), err));
        return false;
    }
    if (docs)
        docs->fileModified(path); // an open editor adopts the restored content
    m_recovery->forceAdd(path, bytes);
    notice(tr("Successfully restored version from %1").arg(when));
    loadVersions(path);
    return true;
}

bool FileRecoveryPanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_input) {
        if (ev->type() == QEvent::FocusIn) {
            updateSuggestions();
        } else if (ev->type() == QEvent::FocusOut) {
            hidePopup();
        } else if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            const bool open = m_popup && m_popup->isVisible();
            SuggestionList* list = open ? m_popup->list() : nullptr;
            switch (ke->key()) {
            case Qt::Key_Down:
                if (list) {
                    list->setSelectedIndex(list->selectedIndex() + 1);
                } else if (m_list->count() > 0 && m_input->cursorPosition() == m_input->text().size()) {
                    m_list->focusAt(0, true);
                }
                return true;
            case Qt::Key_Up:
                if (list) {
                    list->setSelectedIndex(list->selectedIndex() - 1);
                    return true;
                }
                break;
            case Qt::Key_PageDown:
                if (list) {
                    list->pageDown();
                    return true;
                }
                break;
            case Qt::Key_PageUp:
                if (list) {
                    list->pageUp();
                    return true;
                }
                break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (list) {
                    chooseSuggestion(list->selectedIndex());
                    return true;
                }
                break;
            case Qt::Key_Escape:
                if (list) {
                    hidePopup();
                    return true;
                }
                break;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void FileRecoveryPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (m_popup && m_popup->isVisible())
        updateSuggestions();
}

Modal* openFileRecoveryModal(MainWindow* window, const QString& path)
{
    if (!window || !window->overlay())
        return nullptr;
    auto* modal = new Modal(window->overlay());
    modal->setObjectName(u"ocFileRecoveryModal"_s);
    modal->setSidebarLayout(true);
    modal->setTitle(QObject::tr("File recovery"));
    auto* panel = new FileRecoveryPanel(window, fileRecoveryOf(window->session()), modal);
    modal->addContentWidget(panel);
    modal->open();
    if (!path.isEmpty())
        panel->switchToPath(path);
    else if (!panel->paths().isEmpty())
        panel->focusInput();
    return modal;
}

void registerFileRecoveryCommands(CommandRegistry& registry, MainWindow* window)
{
    // Unavailable while the core plugin is off (installCorePluginSwitches' command filter).
    QPointer<MainWindow> w = window;
    Command c;
    c.id = u"file-recovery:open"_s;
    c.name = QObject::tr("Open local history");
    c.icon = u"rotate-ccw"_s;
    c.callback = [w] {
        if (!w)
            return;
        QString path;
        if (Workspace* ws = w->workspace())
            if (WorkspaceLeaf* leaf = ws->activeRootLeaf())
                if (auto* fv = qobject_cast<FileView*>(leaf->view()); fv && fv->file() && fv->file()->extension() == u"md"_s)
                    path = fv->file()->path();
        openFileRecoveryModal(w, path);
    };
    registry.add(c);
}

} // namespace oc
