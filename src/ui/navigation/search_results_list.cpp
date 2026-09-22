#include "ui/navigation/search_results_list.h"

#include "render/theme/icons.h"
#include "render/theme/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>

#include <cmath>

namespace oc {

namespace {
// .search-result-container: padding 12px 12px 16px
constexpr int kPadX = 12;
constexpr int kPadTop = 12;
constexpr int kPadBottom = 16;
// .tree-item-self (--nav-item-padding 4px 8px 4px 24px, --nav-item-margin-bottom 2px), 13px, line-height 1.3
constexpr int kTitlePadY = 4;
constexpr int kTitlePadLeft = 24;
constexpr int kTitlePadRight = 8;
constexpr int kTitleGap = 2;
constexpr double kTitleFont = 13;
// .search-result-file-matches: margin 4px 0 8px; .search-result-file-match: padding 8px 20px 8px 12px, 12px
constexpr int kBoxMarginTop = 4;
constexpr int kBoxMarginBottom = 8;
constexpr int kMatchPadY = 8;
constexpr int kMatchPadLeft = 12;
constexpr int kMatchPadRight = 20;
constexpr double kMatchFont = 12;
constexpr double kLineHeight = 1.3; // --line-height-tight
constexpr int kFlairGap = 4;       // .tree-item-flair-outer padding-inline-start

QColor tok(const ThemePtr& t, const char* name, const QColor& fallback = {})
{
    return t->color(QString::fromLatin1(name), fallback);
}

// Lays out `text` (pre-wrap: '\n' breaks lines, long lines wrap) with highlight ranges; returns the height.
qreal layoutRich(QTextLayout& layout, const QString& text, const QFont& font, const QList<QTextLayout::FormatRange>& formats,
                 qreal width, qreal lineHeight)
{
    QString t = text;
    t.replace(QLatin1Char('\n'), QChar::LineSeparator);
    layout.setText(t);
    layout.setFont(font);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
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

// Height of `text` without building formats; single short lines skip the text layout.
int measureText(const QString& text, const QFont& font, qreal width, qreal lineHeight)
{
    if (!text.contains(QLatin1Char('\n')) && QFontMetricsF(font).horizontalAdvance(text) <= width)
        return int(std::ceil(lineHeight));
    QTextLayout tl;
    return int(std::ceil(layoutRich(tl, text, font, {}, width, lineHeight)));
}

QList<QTextLayout::FormatRange> highlightFormats(const QList<MatchRange>& ranges, int shift, int length, const ThemePtr& t)
{
    QList<QTextLayout::FormatRange> out;
    const QColor fg = tok(t, "--text-normal");
    const QColor bg = tok(t, "--text-highlight-bg", QColor(255, 208, 0, 102));
    for (const MatchRange& m : ranges) {
        const int a = std::clamp(m.from + shift, 0, length), b = std::clamp(m.to + shift, 0, length);
        if (b <= a)
            continue;
        QTextLayout::FormatRange fr;
        fr.start = a;
        fr.length = b - a;
        fr.format.setForeground(fg);
        fr.format.setBackground(bg);
        out.append(fr);
    }
    return out;
}

QString flairText(const SearchResultGroup& g)
{
    return g.contentMatches.isEmpty() ? QString() : QString::number(g.contentMatches.size());
}
} // namespace

SearchResultsList::SearchResultsList(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("ocSearchResults"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::NoFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    m_offsets.assign(1, kPadTop);
    m_loadingTimer.setInterval(16);
    connect(&m_loadingTimer, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            for (SearchResultGroup& g : m_groups)
                g.layoutWidth = -1; // fonts may have changed
            groupsChanged();
        });
}

QString SearchResultsList::snippetDisplayText(const search::Snippet& s)
{
    return (s.truncatedStart ? QStringLiteral("...") : QString()) + s.text + (s.truncatedEnd ? QStringLiteral("...") : QString());
}

QString SearchResultsList::moreMatchesText(int count)
{
    return count == 1 ? tr("... and %1 more match.").arg(count) : tr("... and %1 more matches.").arg(count);
}

int SearchResultsList::innerWidth() const { return std::max(40, viewport()->width() - 2 * kPadX); }

void SearchResultsList::measure(SearchResultGroup& g, int width) const
{
    if (g.layoutWidth == width)
        return;
    const ThemePtr t = currentTheme();
    const QFont titleFont = t->interfaceFont(kTitleFont);
    const QFont matchFont = t->interfaceFont(kMatchFont);
    const QString flair = flairText(g);
    const int flairW = flair.isEmpty() ? 0 : int(std::ceil(QFontMetricsF(t->interfaceFont(kMatchFont)).horizontalAdvance(flair))) + kFlairGap;
    const qreal titleW = width - kTitlePadLeft - kTitlePadRight - flairW;
    g.titleHeight = kTitlePadY * 2 + measureText(g.title, titleFont, titleW, kTitleFont * kLineHeight);
    g.snippetHeights.clear();
    const qreal textW = width - kMatchPadLeft - kMatchPadRight;
    for (const search::Snippet& s : std::as_const(g.snippets))
        g.snippetHeights.append(kMatchPadY * 2 + measureText(snippetDisplayText(s), matchFont, textW, kMatchFont * kLineHeight));
    g.moreHeight = g.moreMatches > 0 ? kMatchPadY * 2 + measureText(moreMatchesText(g.moreMatches), matchFont, textW, kMatchFont * kLineHeight) : 0;
    g.layoutWidth = width;
}

int SearchResultsList::totalHeight(const SearchResultGroup& g)
{
    int h = g.titleHeight + kTitleGap;
    if (!g.collapsed && (!g.snippets.isEmpty() || g.moreMatches > 0)) {
        int box = 0;
        for (int sh : std::as_const(g.snippetHeights))
            box += sh + 1;
        box += g.moreHeight > 0 ? g.moreHeight : -1; // separators between rows only
        h += kBoxMarginTop + box + kBoxMarginBottom;
    }
    return h;
}

void SearchResultsList::groupsChanged()
{
    const int width = innerWidth();
    m_offsets.resize(size_t(m_groups.size()) + 1);
    int y = kPadTop;
    for (int i = 0; i < m_groups.size(); ++i) {
        measure(m_groups[i], width);
        m_groups[i].height = totalHeight(m_groups[i]);
        m_offsets[size_t(i)] = y;
        y += m_groups[i].height;
    }
    m_offsets[size_t(m_groups.size())] = y;
    if (m_hover.group >= m_groups.size())
        m_hover = {};
    updateScrollRange();
    viewport()->update();
}

void SearchResultsList::clear()
{
    m_groups.clear();
    m_hover = {};
    m_pressed = {};
    verticalScrollBar()->setValue(0);
    groupsChanged();
}

void SearchResultsList::setEmptyText(const QString& text)
{
    if (m_emptyText == text)
        return;
    m_emptyText = text;
    updateScrollRange();
    viewport()->update();
}

void SearchResultsList::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    if (loading) {
        m_loadingClock.start();
        m_loadingTimer.start();
    } else {
        m_loadingTimer.stop();
    }
    viewport()->update();
}

int SearchResultsList::contentHeight() const
{
    if (m_groups.isEmpty())
        return m_emptyText.isEmpty() ? 0 : kPadTop + int(std::ceil(13 * 1.5)) + 12 + kPadBottom;
    return m_offsets.back() + kPadBottom;
}

void SearchResultsList::updateScrollRange()
{
    QScrollBar* sb = verticalScrollBar();
    sb->setRange(0, std::max(0, contentHeight() - viewport()->height()));
    sb->setPageStep(viewport()->height());
    sb->setSingleStep(24);
}

void SearchResultsList::scrollToGroup(int group)
{
    if (group < 0 || group >= m_groups.size())
        return;
    QScrollBar* sb = verticalScrollBar();
    const int top = groupTop(group), bottom = top + m_groups[group].height;
    if (top < sb->value())
        sb->setValue(top - kPadTop);
    else if (bottom > sb->value() + viewport()->height())
        sb->setValue(bottom - viewport()->height());
}

QRect SearchResultsList::itemRect(int group, int snippet) const
{
    if (group < 0 || group >= m_groups.size())
        return {};
    const SearchResultGroup& g = m_groups[group];
    const int x = kPadX, w = innerWidth();
    int y = groupTop(group) - verticalScrollBar()->value();
    if (snippet < 0)
        return QRect(x, y, w, g.titleHeight);
    if (g.collapsed || snippet > g.snippets.size() || (snippet == g.snippets.size() && g.moreMatches <= 0))
        return {};
    y += g.titleHeight + kTitleGap + kBoxMarginTop;
    for (int i = 0; i < snippet; ++i)
        y += g.snippetHeights[i] + 1;
    const int h = snippet < g.snippets.size() ? g.snippetHeights[snippet] : g.moreHeight;
    return QRect(x, y, w, h);
}

SearchResultsList::Hit SearchResultsList::hitTest(const QPoint& pos) const
{
    Hit hit;
    const int y = pos.y() + verticalScrollBar()->value();
    if (m_groups.isEmpty() || y < m_offsets.front() || y >= m_offsets.back() || pos.x() < kPadX || pos.x() >= kPadX + innerWidth())
        return hit;
    const auto it = std::upper_bound(m_offsets.begin(), m_offsets.end(), y);
    const int gi = int(it - m_offsets.begin()) - 1;
    if (gi < 0 || gi >= m_groups.size())
        return hit;
    const SearchResultGroup& g = m_groups[gi];
    int top = m_offsets[size_t(gi)];
    if (y < top + g.titleHeight) {
        hit.group = gi;
        hit.part = pos.x() < kPadX + kTitlePadLeft ? Part::Collapse : Part::Title;
        return hit;
    }
    if (g.collapsed)
        return hit;
    top += g.titleHeight + kTitleGap + kBoxMarginTop;
    for (int i = 0; i < g.snippets.size(); ++i) {
        if (y >= top && y < top + g.snippetHeights[i]) {
            hit.group = gi;
            hit.snippet = i;
            hit.part = Part::Snippet;
            return hit;
        }
        top += g.snippetHeights[i] + 1;
    }
    if (g.moreMatches > 0 && y >= top && y < top + g.moreHeight) {
        hit.group = gi;
        hit.snippet = int(g.snippets.size());
        hit.part = Part::More;
    }
    return hit;
}

void SearchResultsList::setHover(const Hit& hit)
{
    if (hit.group == m_hover.group && hit.snippet == m_hover.snippet && hit.part == m_hover.part)
        return;
    m_hover = hit;
    viewport()->update();
}

void SearchResultsList::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    groupsChanged();
}

void SearchResultsList::mouseMoveEvent(QMouseEvent* e)
{
    setHover(hitTest(e->position().toPoint()));
    QAbstractScrollArea::mouseMoveEvent(e);
}

void SearchResultsList::leaveEvent(QEvent* e)
{
    setHover({});
    QAbstractScrollArea::leaveEvent(e);
}

bool SearchResultsList::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave)
        setHover({});
    return QAbstractScrollArea::viewportEvent(e);
}

void SearchResultsList::mousePressEvent(QMouseEvent* e)
{
    m_pressed = hitTest(e->position().toPoint());
    e->accept();
}

void SearchResultsList::mouseReleaseEvent(QMouseEvent* e)
{
    const Hit hit = hitTest(e->position().toPoint());
    const Hit pressed = m_pressed;
    m_pressed = {};
    if (hit.group < 0 || hit.group != pressed.group || hit.snippet != pressed.snippet || hit.part != pressed.part)
        return;
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton)
        return;
    Qt::KeyboardModifiers mods = e->modifiers();
    if (e->button() == Qt::MiddleButton)
        mods |= Qt::ControlModifier; // auxclick: new tab
    switch (hit.part) {
    case Part::Collapse:
        if (e->button() == Qt::LeftButton)
            emit collapseToggled(hit.group);
        break;
    case Part::Title:
        emit titleClicked(hit.group, mods);
        break;
    case Part::Snippet:
        emit snippetClicked(hit.group, hit.snippet, mods);
        break;
    case Part::More:
    case Part::None:
        break;
    }
}

void SearchResultsList::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePtr t = currentTheme();
    const int scroll = verticalScrollBar()->value();
    const int vh = viewport()->height();
    const int width = innerWidth();

    if (m_loading && m_loadingClock.elapsed() > 300) {
        // .search-result-container.is-loading:before — a 3px accent bar sweeping across (1s, after 300ms)
        const qreal phase = std::fmod((m_loadingClock.elapsed() - 300) / 1000.0, 1.0);
        const qreal vw = viewport()->width();
        const qreal from = phase < 0.5 ? 0 : (phase - 0.5) * 2 * vw;
        const qreal to = phase < 0.5 ? phase * 2 * vw : vw;
        p.fillRect(QRectF(from, 0, to - from, 3), tok(t, "--interactive-accent"));
    }

    if (m_groups.isEmpty()) {
        if (!m_emptyText.isEmpty()) {
            // .search-empty-state: faint, --font-ui-small, padding-inline-start 8px
            p.setFont(t->interfaceFont(t->px(QStringLiteral("--font-ui-small"), -1, 13)));
            p.setPen(tok(t, "--text-faint"));
            p.drawText(QRect(kPadX + 8, kPadTop - scroll, width - 8, int(std::ceil(13 * 1.5))), Qt::AlignLeft | Qt::AlignVCenter, m_emptyText);
        }
        return;
    }

    const QFont titleFont = t->interfaceFont(kTitleFont);
    const QFont matchFont = t->interfaceFont(kMatchFont);
    const QColor muted = tok(t, "--text-muted");
    const QColor faint = tok(t, "--text-faint");
    const QColor normal = tok(t, "--text-normal");
    const QColor hoverBg = tok(t, "--nav-item-background-hover", tok(t, "--background-modifier-hover"));
    const QColor border = tok(t, "--background-modifier-border");
    const QColor boxBg = tok(t, "--search-result-background", tok(t, "--background-primary"));
    const QColor selection = tok(t, "--text-selection");
    const QColor collapseColor = tok(t, "--nav-collapse-icon-color", faint);
    const qreal iconOpacity = t->number(QStringLiteral("--icon-opacity"), 0.85);

    auto first = std::upper_bound(m_offsets.begin(), m_offsets.end() - 1, scroll) - m_offsets.begin() - 1;
    for (qsizetype gi = std::max<qsizetype>(0, first); gi < m_groups.size(); ++gi) {
        const SearchResultGroup& g = m_groups[gi];
        const int top = m_offsets[size_t(gi)] - scroll;
        if (top > vh)
            break;
        if (top + g.height < 0)
            continue;
        // --- title row
        const QRect row(kPadX, top, width, g.titleHeight);
        const bool rowHover = m_hover.group == gi && (m_hover.part == Part::Title || m_hover.part == Part::Collapse);
        if (rowHover) {
            QPainterPath path;
            path.addRoundedRect(QRectF(row), 4, 4);
            p.fillPath(path, hoverBg);
        }
        const qreal titleLine = kTitleFont * kLineHeight;
        {
            // collapse arrow: 10px chevron (stroke 4 on the 24 grid), rotated -90° when collapsed
            const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("chevron-down"), 10, collapseColor, 4.0, devicePixelRatioF());
            const QPointF c(row.left() + kTitlePadLeft - 20 + 8, row.top() + kTitlePadY + titleLine / 2);
            p.save();
            p.setOpacity(iconOpacity);
            p.translate(c);
            if (g.collapsed)
                p.rotate(-90);
            p.drawPixmap(QPointF(-5, -5), pm);
            p.restore();
        }
        const QString flair = flairText(g);
        int flairW = 0;
        if (!flair.isEmpty()) {
            p.setFont(matchFont);
            p.setPen(rowHover ? muted : faint);
            flairW = int(std::ceil(QFontMetricsF(matchFont).horizontalAdvance(flair)));
            p.drawText(QRectF(row.right() + 1 - kTitlePadRight - flairW, row.top() + kTitlePadY, flairW, titleLine), Qt::AlignRight | Qt::AlignVCenter, flair);
            flairW += kFlairGap;
        }
        {
            QTextLayout tl;
            const qreal w = width - kTitlePadLeft - kTitlePadRight - flairW;
            layoutRich(tl, g.title, titleFont, highlightFormats(g.titleMatches, 0, int(g.title.size()), t), w, titleLine);
            // .search-result:not(.is-collapsed) .search-result-file-title: --nav-item-color-active
            p.setPen(rowHover ? tok(t, "--nav-item-color-hover", normal)
                              : (g.collapsed ? tok(t, "--nav-item-color", muted) : tok(t, "--nav-item-color-active", normal)));
            tl.draw(&p, QPointF(row.left() + kTitlePadLeft, row.top() + kTitlePadY));
        }
        if (g.collapsed || (g.snippets.isEmpty() && g.moreMatches <= 0))
            continue;
        // --- matches box
        int boxH = 0;
        for (int sh : std::as_const(g.snippetHeights))
            boxH += sh + 1;
        boxH += g.moreHeight > 0 ? g.moreHeight : -1;
        const QRectF box(kPadX + 0.5, top + g.titleHeight + kTitleGap + kBoxMarginTop + 0.5, width - 1, boxH - 1);
        QPainterPath boxPath;
        boxPath.addRoundedRect(box, 4, 4);
        p.fillPath(boxPath, boxBg);
        p.save();
        p.setClipPath(boxPath);
        int y = int(box.top() - 0.5);
        for (int si = 0; si < g.snippets.size(); ++si) {
            const int h = g.snippetHeights[si];
            if (y + h >= 0 && y <= vh) {
                const search::Snippet& s = g.snippets[si];
                const bool hover = m_hover.group == gi && m_hover.snippet == si && m_hover.part == Part::Snippet;
                if (hover)
                    p.fillRect(QRect(kPadX, y, width, h), selection);
                const QString text = snippetDisplayText(s);
                QTextLayout tl;
                layoutRich(tl, text, matchFont, highlightFormats(s.matches, s.truncatedStart ? 3 : 0, int(text.size()), t),
                           width - kMatchPadLeft - kMatchPadRight, kMatchFont * kLineHeight);
                p.setPen(hover ? normal : muted);
                tl.draw(&p, QPointF(kPadX + kMatchPadLeft, y + kMatchPadY));
            }
            y += h;
            if (si + 1 < g.snippets.size() || g.moreHeight > 0) {
                p.fillRect(QRect(kPadX, y, width, 1), border);
                y += 1;
            }
        }
        if (g.moreHeight > 0) {
            p.setFont(matchFont);
            p.setPen(faint);
            p.drawText(QRect(kPadX + kMatchPadLeft, y + kMatchPadY, width - kMatchPadLeft - kMatchPadRight, g.moreHeight - 2 * kMatchPadY),
                       Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, moreMatchesText(g.moreMatches));
        }
        p.restore();
        p.setPen(QPen(border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(boxPath);
    }
}

} // namespace oc
