#include "editor/embeds/note_embed.h"

#include "core/documents/document.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/subpath.h"
#include "editor/embeds/hosted_embeds.h"
#include "editor/live_preview_decorator.h"
#include "markdown/doc_source.h"
#include "markdown/md_scan.h"
#include "markdown/metadata.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "text/codec.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPagedPaintDevice>
#include <QRegularExpression>

#include <cmath>

namespace oc {

namespace {
constexpr int kMaxLayouts = 128; // cached layouts (trimmed to 3/4 when exceeded)
constexpr int kMaxSnapshots = 48;
constexpr qreal kIconBox = 24;   // .markdown-embed-link: --icon-s (16) + padding --size-2-2 (4) on each side
constexpr qreal kIconInset = 4;  // top: 4px; inset-inline-end: 4px

QString tr(const char* s) { return QCoreApplication::translate("oc::NoteEmbed", s); }

// The embed box's CSS: .markdown-embed { border-*: var(--embed-border-*) } and the padding of the
// title and of the content (--embed-padding), .inline-embed > .markdown-embed-content { max-height }.
struct Chrome {
    qreal borderLeft = 2, borderRight = 0, borderTop = 0, borderBottom = 0;
    QColor colorLeft, colorRight, colorTop, colorBottom;
    QMarginsF padding{24, 0, 0, 0};
    QColor background; // invalid: inherit (transparent)
    qreal maxHeight = 4000;
};

// "2px solid <color>" / "none" -> width + color (currentColor = --text-normal).
void parseBorder(const Theme& t, const QString& name, const QString& fallbackName, qreal* width, QColor* color)
{
    QString n = name;
    if (!t.has(n) && !fallbackName.isEmpty())
        n = fallbackName;
    *width = 0;
    *color = t.color(QStringLiteral("--text-normal"));
    if (!t.has(n))
        return;
    const CssValue v = t.value(n);
    const QList<CssValue> items = v.kind == CssValue::List ? v.items : QList<CssValue>{v};
    qreal w = 3; // CSS "medium"
    bool styled = false;
    for (const CssValue& item : items) {
        switch (item.kind) {
        case CssValue::Length:
        case CssValue::Number:
            w = item.toPx(t.textSize());
            break;
        case CssValue::Color:
            *color = item.color;
            break;
        case CssValue::Ident:
            if (item.text == u"none" || item.text == u"hidden")
                return;
            styled = true; // solid, dashed...
            break;
        default:
            break;
        }
    }
    if (styled)
        *width = std::max<qreal>(0, w);
}

Chrome chromeOf(const Theme& t)
{
    Chrome c;
    parseBorder(t, QStringLiteral("--embed-border-start"), QStringLiteral("--embed-border-left"), &c.borderLeft, &c.colorLeft);
    parseBorder(t, QStringLiteral("--embed-border-end"), QStringLiteral("--embed-border-right"), &c.borderRight, &c.colorRight);
    parseBorder(t, QStringLiteral("--embed-border-top"), QString(), &c.borderTop, &c.colorTop);
    parseBorder(t, QStringLiteral("--embed-border-bottom"), QString(), &c.borderBottom, &c.colorBottom);
    // --embed-padding: CSS shorthand (1-4 lengths)
    const CssValue pv = t.value(QStringLiteral("--embed-padding"));
    QList<qreal> p;
    for (const CssValue& item : pv.kind == CssValue::List ? pv.items : QList<CssValue>{pv})
        if (item.kind == CssValue::Length || item.kind == CssValue::Number)
            p << item.toPx(t.textSize());
    if (p.size() == 1)
        c.padding = QMarginsF(p[0], p[0], p[0], p[0]);
    else if (p.size() == 2)
        c.padding = QMarginsF(p[1], p[0], p[1], p[0]);
    else if (p.size() == 3)
        c.padding = QMarginsF(p[1], p[0], p[1], p[2]);
    else if (p.size() >= 4)
        c.padding = QMarginsF(p[3], p[0], p[1], p[2]);
    const CssValue bg = t.value(QStringLiteral("--embed-background"));
    if (bg.kind == CssValue::Color && bg.color.alpha() > 0)
        c.background = bg.color;
    c.maxHeight = t.px(QStringLiteral("--embed-max-height"), -1, 4000);
    return c;
}

// .file-embed boxes (generic / not created yet): padding --size-4-2, --font-smaller text.
qreal fileBoxHeight(const Theme& t)
{
    const qreal pad = t.px(QStringLiteral("--size-4-2"), -1, 8);
    const qreal font = t.px(QStringLiteral("--font-smaller"), t.textSize(), 14);
    const qreal lineH = std::round(font * t.number(QStringLiteral("--line-height-normal"), 1.5));
    const qreal icon = t.px(QStringLiteral("--icon-size"), -1, 18);
    return std::round(2 * pad + std::max(lineH, icon));
}

QFont fileBoxFont(const Theme& t, const StyleSheet& ss)
{
    QFont f = ss.line(StyleId::Normal).font;
    f.setPixelSize(std::max(1, int(std::lround(t.px(QStringLiteral("--font-smaller"), t.textSize(), 14)))));
    return f;
}

bool isBlank(QStringView s) { return s.trimmed().isEmpty(); }

// Leading whitespace width in columns (tabs = 4, like Obsidian's un-indent of embedded list items).
int indentWidth(QStringView s)
{
    int w = 0;
    for (QChar c : s) {
        if (c == u'\t')
            w += 4;
        else if (c == u' ')
            ++w;
        else
            break;
    }
    return w;
}

QString removeIndent(const QString& line, int width)
{
    int col = 0;
    qsizetype i = 0;
    while (i < line.size() && col < width && (line[i] == u' ' || line[i] == u'\t')) {
        col += line[i] == u'\t' ? 4 : 1;
        ++i;
    }
    return line.mid(i);
}

quint64 hashText(const Text& t)
{
    size_t h = qHash(t.lineCount());
    for (int i = 0; i < t.lineCount(); ++i)
        h = qHashMulti(h, t.lineView(i));
    return quint64(h) | 1; // never 0 (= unreadable)
}

// The part of the painter's device being repainted, in the painter's logical coordinates: skips
// painting under hosted live widgets and outside the update region.
QRectF paintedArea(QPainter* p, const QRectF& rect)
{
    QRectF r = rect;
    if (p->hasClipping())
        r &= p->clipBoundingRect();
    if (QPaintEngine* e = p->paintEngine(); e && !dynamic_cast<QPagedPaintDevice*>(p->device())) {
        const QRegion sys = e->systemClip();
        if (!sys.isEmpty()) {
            bool invertible = false;
            const QTransform inv = p->deviceTransform().inverted(&invertible);
            if (invertible)
                r &= inv.mapRect(QRectF(sys.boundingRect()));
        }
    }
    return r;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Snapshots: a note's current text (open Document or file) + its metadata, by absolute path
// ---------------------------------------------------------------------------------------------
struct NoteEmbeds::Snapshot {
    Text text;
    quint64 hash = 0;
    const Document* doc = nullptr; // taken from this open document (else from the file)
    qint64 mtime = 0, fileSize = -1;
    quint64 lastUse = 0;
    std::optional<md::CachedMetadata> meta;

    const md::CachedMetadata& metadata()
    {
        if (!meta) {
            md::SyntaxTree tree;
            tree.parse(text);
            meta = md::extractMetadata(tree, text);
        }
        return *meta;
    }
};

struct NoteEmbeds::ChainScope {
    explicit ChainScope(const QStringList& chain) : prev(std::exchange(NoteEmbeds::instance().m_chain, chain)) {}
    ~ChainScope() { NoteEmbeds::instance().m_chain = prev; }
    QStringList prev;
};

NoteEmbeds& NoteEmbeds::instance()
{
    static NoteEmbeds* n = new NoteEmbeds; // (leaked: outlives every widget and layout)
    return *n;
}

void NoteEmbeds::addHost(std::shared_ptr<NoteEmbedHost> host)
{
    if (host && !m_hosts.contains(host))
        m_hosts.append(std::move(host));
    clearCaches();
}

void NoteEmbeds::removeHost(const NoteEmbedHost* host)
{
    m_hosts.removeIf([host](const std::shared_ptr<NoteEmbedHost>& h) { return h.get() == host; });
    clearCaches();
}

NoteEmbedHost* NoteEmbeds::hostFor(const QString& absPath, QString* vaultPath) const
{
    for (const std::shared_ptr<NoteEmbedHost>& h : m_hosts) {
        if (const std::optional<QString> vp = h->vaultPath(absPath)) {
            if (vaultPath)
                *vaultPath = *vp;
            return h.get();
        }
    }
    return nullptr;
}

void NoteEmbeds::clearCaches()
{
    m_snapshots.clear();
    m_layouts.clear();
    ++m_linkGeneration;
}

std::shared_ptr<NoteEmbeds::Snapshot> NoteEmbeds::snapshot(const QString& absPath)
{
    if (absPath.isEmpty())
        return nullptr;
    std::shared_ptr<Snapshot> cached = m_snapshots.value(absPath);
    QString vp;
    NoteEmbedHost* host = hostFor(absPath, &vp);
    const Document* doc = host ? host->openDocument(vp) : nullptr;
    if (cached && cached->doc == doc) {
        // Open documents and files of a vault: the host reports changes (noteChanged drops the
        // snapshot). Files outside every host: compare the file's stamp.
        bool fresh = doc || host;
        if (!fresh) {
            const QFileInfo fi(absPath);
            fresh = fi.lastModified().toMSecsSinceEpoch() == cached->mtime && fi.size() == cached->fileSize;
        }
        if (fresh) {
            cached->lastUse = ++m_clock;
            return cached;
        }
    }
    auto s = std::make_shared<Snapshot>();
    s->doc = doc;
    if (doc) {
        s->text = doc->text();
    } else {
        const QFileInfo fi(absPath);
        s->mtime = fi.lastModified().toMSecsSinceEpoch();
        s->fileSize = fi.exists() ? fi.size() : -1;
        QFile f(absPath);
        if (!f.open(QIODevice::ReadOnly))
            return nullptr;
        s->text = decodeFile(f.readAll()).text;
    }
    s->hash = hashText(s->text);
    s->lastUse = ++m_clock;
    m_snapshots.insert(absPath, s);
    if (m_snapshots.size() > kMaxSnapshots) {
        QList<std::pair<quint64, QString>> order;
        for (auto it = m_snapshots.cbegin(); it != m_snapshots.cend(); ++it)
            order.append({it.value()->lastUse, it.key()});
        std::sort(order.begin(), order.end());
        for (qsizetype i = 0; i < order.size() - kMaxSnapshots * 3 / 4; ++i)
            m_snapshots.remove(order[i].second);
    }
    return s;
}

quint64 NoteEmbeds::contentHash(const QString& absPath)
{
    const std::shared_ptr<Snapshot> s = snapshot(absPath);
    return s ? s->hash : 0;
}

QList<std::shared_ptr<NoteEmbedLayout>> NoteEmbeds::cachedLayoutsOf(const QString& absPath) const
{
    QList<std::shared_ptr<NoteEmbedLayout>> out;
    for (const std::shared_ptr<NoteEmbedLayout>& l : m_layouts)
        if (l->m_request.absPath == absPath)
            out << l;
    return out;
}

void NoteEmbeds::noteChanged(const QString& absPath)
{
    m_snapshots.remove(absPath);
    // every embed showing it, directly or nested, re-lays out
    QSet<QString> roots{absPath};
    for (const std::shared_ptr<NoteEmbedLayout>& l : std::as_const(m_layouts))
        if (l->m_deps.contains(absPath) && !l->m_request.absPath.isEmpty())
            roots.insert(l->m_request.absPath);
    for (const QString& root : std::as_const(roots))
        EmbedRegistry::instance().notifyChanged(root);
    emit changed(absPath);
}

void NoteEmbeds::linksChanged()
{
    ++m_linkGeneration;
    emit changed(QString());
}

StyleSheetPtr NoteEmbeds::currentStyles(std::shared_ptr<const Theme>* theme)
{
    if (EmbedPrintScope::active() && EmbedPrintScope::styles() && EmbedPrintScope::theme()) {
        *theme = EmbedPrintScope::theme();
        return EmbedPrintScope::styles();
    }
    const ThemePtr t = currentTheme();
    if (!t)
        return nullptr;
    if (!m_styles || !m_stylesTheme || m_stylesTheme->generation() != t->generation()) {
        m_styles = StyleSheet::build(*t);
        m_stylesTheme = t;
    }
    *theme = m_stylesTheme;
    return m_styles;
}

bool NoteEmbeds::valid(NoteEmbedLayout& l)
{
    if (l.m_linkGeneration != m_linkGeneration)
        return false;
    std::shared_ptr<const Theme> theme;
    const StyleSheetPtr styles = currentStyles(&theme);
    if (!styles || styles->generation() != l.m_styleKey)
        return false;
    for (auto it = l.m_deps.cbegin(); it != l.m_deps.cend(); ++it)
        if (contentHash(it.key()) != it.value())
            return false;
    return true;
}

void NoteEmbeds::trim()
{
    if (m_layouts.size() <= kMaxLayouts)
        return;
    QList<std::pair<quint64, QString>> order;
    for (auto it = m_layouts.cbegin(); it != m_layouts.cend(); ++it)
        order.append({it.value()->m_lastUse, it.key()});
    std::sort(order.begin(), order.end());
    for (qsizetype i = 0; i < order.size() - kMaxLayouts * 3 / 4; ++i)
        m_layouts.remove(order[i].second);
}

std::shared_ptr<NoteEmbedLayout> NoteEmbeds::layout(const EmbedRequest& request, qreal width)
{
    const QStringList chain = m_chain;
    const int w = std::max(1, int(std::lround(width)));
    const bool print = EmbedPrintScope::active();
    const QString key = request.absPath + u'\n' + (request.absPath.isEmpty() ? request.linktext : request.subpath) + u'\n'
                        + QString::number(w) + u'\n' + chain.join(QChar(0x1f)) + (print ? u"\np" : u"\n");
    std::shared_ptr<NoteEmbedLayout> l = m_layouts.value(key);
    if (!l || !valid(*l)) {
        l = build(request, w, chain);
        m_layouts.insert(key, l);
        trim();
    }
    l->m_lastUse = ++m_clock;
    // the embed being built around this one depends on everything this one shows
    if (!m_collect.isEmpty())
        for (auto it = l->m_deps.cbegin(); it != l->m_deps.cend(); ++it)
            m_collect.last()->insert(it.key(), it.value());
    return l;
}

std::shared_ptr<NoteEmbedLayout> NoteEmbeds::build(const EmbedRequest& request, qreal width, const QStringList& chain)
{
    ++m_builds;
    std::shared_ptr<NoteEmbedLayout> l(new NoteEmbedLayout);
    l->m_request = request;
    l->m_print = EmbedPrintScope::active();
    l->m_depth = int(chain.size()) + 1;
    l->m_linkGeneration = m_linkGeneration;
    l->m_styles = currentStyles(&l->m_theme);
    if (!l->m_styles || !l->m_theme) {
        l->m_size = QSizeF(width, 1);
        return l;
    }
    l->m_styleKey = l->m_styles->generation();
    const Theme& theme = *l->m_theme;
    const StyleSheet& ss = *l->m_styles;
    QString vaultPath;
    NoteEmbedHost* host = hostFor(request.absPath, &vaultPath);
    l->m_vaultPath = vaultPath;

    // Obsidian's embed loader: an unresolved link, too deep (or a cycle), or the note.
    const QString self = request.absPath + u'#' + request.subpath.toLower();
    if (request.absPath.isEmpty())
        l->m_kind = NoteEmbedLayout::Kind::Missing;
    else if (chain.size() >= kMaxDepth || chain.contains(self))
        l->m_kind = NoteEmbedLayout::Kind::Generic;
    if (l->m_kind != NoteEmbedLayout::Kind::Note) {
        l->m_size = QSizeF(width, fileBoxHeight(theme));
        return l;
    }

    const std::shared_ptr<Snapshot> snap = snapshot(request.absPath);
    if (snap)
        l->m_deps.insert(request.absPath, snap->hash);
    const QString basename = QFileInfo(request.absPath).completeBaseName();

    // The markdown shown: the whole note without its frontmatter, or the subpath's part.
    QStringList lines;
    const Text text = snap ? snap->text : Text();
    const QString subpath = request.subpath == u"#" ? QString() : request.subpath;
    if (subpath.isEmpty()) {
        l->m_title = basename;
        int from = 0;
        if (text.lineCount() > 1 && md::scan::isFrontmatterDelim(text.lineView(0))) {
            for (int i = 1; i < text.lineCount(); ++i) {
                if (md::scan::isFrontmatterDelim(text.lineView(i))) {
                    from = i + 1;
                    break;
                }
            }
        }
        for (int i = from; i < text.lineCount(); ++i)
            lines << text.line(i);
    } else {
        const std::optional<SubpathRange> r = snap ? resolveSubpath(snap->metadata(), subpath) : std::nullopt;
        if (!r) {
            l->m_found = false;
            QString shown = subpath.mid(1);
            shown.replace(u'[', QStringLiteral("\\[")).replace(u']', QStringLiteral("\\]"));
            lines << QStringLiteral("##### ") + tr("Unable to find “%1” in %2").arg(shown, basename);
        } else {
            const int last = r->endLine < 0 ? text.lineCount() - 1 : std::min(r->endLine, text.lineCount() - 1);
            for (int i = r->startLine; i <= last; ++i)
                lines << text.line(i);
            bool listItem = false;
            if (r->type == SubpathRange::Type::Block) {
                const QString id = subpath.section(u'#', -1).mid(1).toLower(); // "#^id"
                for (const md::ListItemCache& li : snap->metadata().listItems)
                    listItem |= li.id.toLower() == id && li.position.start.line == r->startLine;
            }
            if (listItem && !lines.isEmpty()) {
                // Obsidian un-indents the item (and its children) by the item's own indentation
                const int indent = indentWidth(lines.first());
                for (QString& s : lines)
                    s = removeIndent(s, indent);
            } else if (r->type == SubpathRange::Type::Footnote && !lines.isEmpty()) {
                // the definition's text: without "[^id]: ", continuation lines one level less indented
                static const QRegularExpression label(QStringLiteral(R"(^\[\^[^\]]+\]:\s*)"));
                lines.first().remove(label);
                for (qsizetype i = 1; i < lines.size(); ++i) {
                    if (lines[i].startsWith(u'\t'))
                        lines[i].remove(0, 1);
                    else if (lines[i].startsWith(QLatin1String("    ")))
                        lines[i].remove(0, 4);
                }
            }
        }
    }
    // the first block's top margin and the last block's bottom margin are 0 (.markdown-rendered)
    while (!lines.isEmpty() && isBlank(lines.first()))
        lines.removeFirst();
    while (!lines.isEmpty() && isBlank(lines.last()))
        lines.removeLast();

    const Chrome chrome = chromeOf(theme);
    const qreal titleH = l->m_title.isEmpty() ? 0 : chrome.padding.top() + ss.line(StyleId::Normal).lineBox + chrome.padding.bottom();
    l->m_contentLeft = chrome.borderLeft + chrome.padding.left();
    l->m_contentTop = chrome.borderTop + titleH;
    l->m_contentWidth = std::max<qreal>(20, width - chrome.borderLeft - chrome.borderRight - chrome.padding.left() - chrome.padding.right());
    l->m_childChain = chain;
    l->m_childChain.append(self);

    qreal contentH = 0;
    if (!lines.isEmpty()) {
        l->m_source = std::make_unique<md::StaticDocSource>(Text::fromString(lines.join(u'\n')), vaultPath);
        l->m_decorator = std::make_unique<LivePreviewDecorator>(l->m_source.get());
        LivePreviewDecorator& deco = *l->m_decorator;
        deco.setStyleSheet(l->m_styles.get());
        deco.setReading(true);
        deco.setNestingDepth(l->m_depth);
        deco.setContentWidth(l->m_contentWidth);
        // links in the embedded note resolve from it
        std::weak_ptr<NoteEmbedHost> weakHost;
        for (const std::shared_ptr<NoteEmbedHost>& h : std::as_const(m_hosts))
            if (h.get() == host)
                weakHost = h;
        if (host) {
            deco.isResolved = [weakHost, vaultPath](const QString& target) {
                const std::shared_ptr<NoteEmbedHost> h = weakHost.lock();
                const QString path = LinkResolver::splitSubpath(target).first;
                return !h || path.isEmpty() || !h->resolveLink(path, vaultPath).isEmpty();
            };
            deco.resolveAttachment = [weakHost, vaultPath](const QString& linkpath) -> QString {
                const std::shared_ptr<NoteEmbedHost> h = weakHost.lock();
                if (!h)
                    return {};
                const QString dest = h->resolveLink(LinkResolver::splitSubpath(linkpath).first, vaultPath);
                return dest.isEmpty() ? QString() : h->absolutePath(dest);
            };
        }
        const int lastLine = l->m_source->text().lineCount() - 1;
        deco.extra = [lastLine](int line, LineDecoration& d) {
            if (line == 0)
                d.paddingTop = 0;
            if (line == lastLine)
                d.paddingBottom = 0;
        };
        l->m_layout = std::make_unique<DocLayout>();
        DocLayout& dl = *l->m_layout;
        dl.setDecorator(&deco);
        dl.setStyleSheet(l->m_styles);
        dl.setContentWidth(l->m_contentWidth);
        // Nested embeds are laid out now (sizes): inside this one's chain, their notes are deps.
        QHash<QString, quint64> nested;
        m_collect.append(&nested);
        {
            const ChainScope scope(l->m_childChain);
            dl.setText(l->m_source->text());
            dl.ensureLaidOut(0, l->m_print ? 1e12 : chrome.maxHeight);
        }
        m_collect.removeLast();
        for (auto it = nested.cbegin(); it != nested.cend(); ++it)
            l->m_deps.insert(it.key(), it.value());
        contentH = chrome.padding.top() + dl.totalHeight() + chrome.padding.bottom();
    }
    const qreal maxH = l->m_print ? 1e12 : chrome.maxHeight;
    l->m_scrollable = contentH > maxH + 0.5;
    l->m_viewportH = std::ceil(std::min(contentH, maxH));
    l->m_size = QSizeF(width, std::max<qreal>(1, std::ceil(l->m_contentTop + l->m_viewportH + chrome.borderBottom)));
    if (!l->m_print)
        l->m_iconRect = QRectF(width - kIconInset - kIconBox, kIconInset, kIconBox, kIconBox);
    return l;
}

// ---------------------------------------------------------------------------------------------
// NoteEmbedLayout
// ---------------------------------------------------------------------------------------------
NoteEmbedLayout::~NoteEmbedLayout()
{
    // the layout refers to the decorator, the decorator to the source
    m_layout.reset();
    m_decorator.reset();
    m_source.reset();
}

QString NoteEmbedLayout::markdown() const { return m_source ? m_source->text().toString() : QString(); }

qreal NoteEmbedLayout::contentHeight() const
{
    if (!m_layout || !m_theme)
        return 0;
    const Chrome c = chromeOf(*m_theme);
    return c.padding.top() + m_layout->totalHeight() + c.padding.bottom();
}

qreal NoteEmbedLayout::maxScroll() const { return m_scrollable ? std::max<qreal>(0, std::ceil(contentHeight() - m_viewportH)) : 0; }

QRectF NoteEmbedLayout::contentRect() const
{
    const qreal right = m_theme ? chromeOf(*m_theme).borderRight : 0;
    const qreal left = m_theme ? chromeOf(*m_theme).borderLeft : 0;
    return QRectF(left, m_contentTop, m_size.width() - left - right, m_viewportH);
}

void NoteEmbedLayout::ensureLaidOut(qreal scroll)
{
    if (!m_layout || !m_theme)
        return;
    const qreal padTop = chromeOf(*m_theme).padding.top();
    const NoteEmbeds::ChainScope scope(m_childChain);
    m_layout->ensureLaidOut(std::max<qreal>(0, scroll - padTop), scroll - padTop + m_viewportH);
}

void NoteEmbedLayout::paintBox(QPainter* p, const QRectF& box, bool hover) const
{
    const Theme& t = *m_theme;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const qreal radius = t.px(QStringLiteral("--radius-m"), -1, 8);
    QPainterPath path;
    path.addRoundedRect(box, radius, radius);
    // .file-embed; :hover of .mod-generic / .mod-empty
    p->fillPath(path, t.color(hover ? QStringLiteral("--background-secondary") : QStringLiteral("--background-primary-alt")));
    const QColor muted = t.color(hover ? QStringLiteral("--text-normal") : QStringLiteral("--text-muted"));
    const QFont font = fileBoxFont(t, *m_styles);
    p->setFont(font);
    p->setPen(muted);
    const QFontMetricsF fm(font);
    const qreal pad = t.px(QStringLiteral("--size-4-2"), -1, 8);
    const QRectF inner = box.adjusted(pad, pad, -pad, -pad);
    if (m_kind == Kind::Generic) {
        // .file-embed-title: the file icon, a gap and the file name, centred
        const int icon = int(std::lround(t.px(QStringLiteral("--icon-size"), -1, 18)));
        const qreal gap = pad;
        const QString name = QFileInfo(m_request.absPath).fileName();
        const QString shown = fm.elidedText(name, Qt::ElideRight, std::max<qreal>(10, inner.width() - icon - gap));
        const qreal w = icon + gap + fm.horizontalAdvance(shown);
        const qreal x = inner.center().x() - w / 2;
        const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("file"), icon, muted,
                                                           t.px(QStringLiteral("--icon-m-stroke-width"), -1, 1.75), p->device()->devicePixelRatioF());
        p->drawPixmap(QRectF(x, inner.center().y() - icon / 2.0, icon, icon), pm, QRectF(QPointF(0, 0), pm.size()));
        p->drawText(QRectF(x + icon + gap, inner.top(), inner.right() - x - icon - gap, inner.height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
    } else {
        const QString label = tr("“%1” is not created yet. Click to create.").arg(m_request.linktext);
        p->drawText(inner, Qt::AlignCenter, fm.elidedText(label, Qt::ElideRight, inner.width()));
    }
    p->restore();
}

void NoteEmbedLayout::paint(QPainter* p, const QPointF& origin, qreal scroll, const QRectF& visible, bool icon, bool hover)
{
    if (!m_theme || !m_styles)
        return;
    const QRectF box(origin, m_size);
    const QRectF area = (visible.isEmpty() ? QRectF(QPointF(0, 0), m_size) : visible).translated(origin) & box;
    if (area.isEmpty())
        return;
    if (m_kind != Kind::Note) {
        paintBox(p, box, hover);
        return;
    }
    const Theme& t = *m_theme;
    const Chrome c = chromeOf(t);
    p->save();
    if (c.background.isValid())
        p->fillRect(box, c.background);
    // borders (.markdown-embed: border-inline-start etc.)
    if (c.borderTop > 0)
        p->fillRect(QRectF(box.left(), box.top(), box.width(), c.borderTop), c.colorTop);
    if (c.borderBottom > 0)
        p->fillRect(QRectF(box.left(), box.bottom() - c.borderBottom, box.width(), c.borderBottom), c.colorBottom);
    if (c.borderLeft > 0)
        p->fillRect(QRectF(box.left(), box.top(), c.borderLeft, box.height()), c.colorLeft);
    if (c.borderRight > 0)
        p->fillRect(QRectF(box.right() - c.borderRight, box.top(), c.borderRight, box.height()), c.colorRight);
    // .markdown-embed-title: the note's name, bold, one line
    if (!m_title.isEmpty()) {
        const LineMetrics& lm = m_styles->line(StyleId::Normal);
        QFont f = lm.font;
        f.setWeight(QFont::Weight(std::clamp(int(t.number(QStringLiteral("--font-weight"), 400) + t.number(QStringLiteral("--bold-modifier"), 200)), 100, 900)));
        p->setFont(f);
        p->setPen(t.color(QStringLiteral("--text-normal"), lm.color));
        const QFontMetricsF fm(f);
        const QRectF titleRect(box.left() + c.borderLeft + c.padding.left(), box.top() + c.borderTop + c.padding.top(), m_contentWidth, lm.lineBox);
        p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(m_title, Qt::ElideRight, titleRect.width()));
    }
    // the content (reading view rendering), clipped to the scroll viewport
    if (m_layout) {
        const QRectF viewport(box.left() + c.borderLeft, box.top() + m_contentTop, box.width() - c.borderLeft - c.borderRight, m_viewportH);
        const QRectF clip = viewport & area;
        if (!clip.isEmpty()) {
            const QPointF contentOrigin(box.left() + m_contentLeft, box.top() + m_contentTop + c.padding.top() - scroll);
            const NoteEmbeds::ChainScope scope(m_childChain);
            if (scroll > 0)
                m_layout->ensureLaidOut(std::max<qreal>(0, scroll - c.padding.top()), scroll - c.padding.top() + m_viewportH);
            p->save();
            p->setClipRect(clip, Qt::IntersectClip);
            p->translate(contentOrigin);
            DocLayout::PaintContext ctx;
            ctx.drawSelection = false;
            ctx.clip = clip.translated(-contentOrigin);
            ctx.rightEdge = m_contentWidth;
            m_layout->paint(p, ctx);
            p->restore();
        }
    }
    // .markdown-embed-link (the live widget has a button instead; print hides it)
    if (icon && !m_iconRect.isEmpty()) {
        const qreal size = t.px(QStringLiteral("--icon-s"), -1, 16);
        QColor color = t.color(QStringLiteral("--icon-color"), t.color(QStringLiteral("--text-muted")));
        color.setAlphaF(color.alphaF() * std::clamp(t.number(QStringLiteral("--icon-opacity"), 0.85), 0.0, 1.0));
        const QPixmap pm = IconProvider::instance().pixmap(QStringLiteral("maximize-2"), int(size), color,
                                                           t.px(QStringLiteral("--icon-s-stroke-width"), -1, 2), p->device()->devicePixelRatioF());
        const QRectF ir = m_iconRect.translated(origin);
        p->drawPixmap(QRectF(ir.center() - QPointF(size / 2, size / 2), QSizeF(size, size)), pm, QRectF(QPointF(0, 0), pm.size()));
    }
    p->restore();
}

NoteEmbedLayout::Hit NoteEmbedLayout::hitTest(const QPointF& pos, qreal scroll)
{
    Hit h;
    if (!QRectF(QPointF(0, 0), m_size).contains(pos))
        return h;
    if (m_kind != Kind::Note) {
        h.type = Hit::Type::Box;
        h.linktext = m_request.linktext;
        h.rect = QRectF(QPointF(0, 0), m_size);
        return h;
    }
    if (m_iconRect.contains(pos)) {
        h.type = Hit::Type::OpenIcon;
        h.linktext = m_vaultPath.isEmpty() ? m_request.linktext : m_vaultPath + m_request.subpath;
        h.rect = m_iconRect;
        return h;
    }
    h.type = Hit::Type::Content;
    if (!m_layout || !m_theme || !contentRect().contains(pos))
        return h;
    const Chrome c = chromeOf(*m_theme);
    const QPointF origin(m_contentLeft, m_contentTop + c.padding.top() - scroll);
    const QPointF pt = pos - origin;
    if (pt.x() < 0 || pt.y() < 0 || pt.y() > m_layout->totalHeight())
        return h;
    const NoteEmbeds::ChainScope scope(m_childChain);
    DocLayout& dl = *m_layout;
    if (const auto bh = dl.blockAt(pt)) {
        // tables, callouts: their own link hit testing
        const QString link = bh->widget->linkAt(pt - bh->rect.topLeft());
        if (!link.isEmpty()) {
            h.type = Hit::Type::Link;
            h.linktext = link;
            h.sourcePath = m_vaultPath;
            h.linkKind = 0;
            h.rect = bh->rect.translated(origin);
        }
        return h;
    }
    if (const auto wh = dl.widgetAt(pt)) {
        if (const auto* ew = dynamic_cast<const EmbedWidget*>(wh->widget.get())) {
            if (dynamic_cast<const NoteEmbedProvider*>(ew->provider().get())) {
                // a nested note embed: hit test inside it
                const std::shared_ptr<NoteEmbedLayout> nested = NoteEmbeds::instance().layout(ew->request(), wh->rect.width());
                Hit n = nested->hitTest(pt - wh->rect.topLeft(), 0);
                if (n.type == Hit::Type::None)
                    n.type = Hit::Type::Content;
                if (n.type == Hit::Type::Box && n.sourcePath.isEmpty())
                    n.sourcePath = m_vaultPath;
                n.rect.translate(wh->rect.topLeft() + origin);
                return n;
            }
            h.type = Hit::Type::Link; // PDF / audio / video: open the file
            h.linktext = ew->request().linktext;
            h.sourcePath = m_vaultPath;
            h.linkKind = 1;
            h.rect = wh->rect.translated(origin);
            return h;
        }
    }
    const qsizetype docPos = dl.hitTest(pt);
    if (const auto link = m_decorator->linkAt(docPos); link && link->to > link->from) {
        for (const QRectF& r : dl.rangeRects(link->from, link->to)) {
            if (r.adjusted(-1, 0, 1, 0).contains(pt)) {
                h.type = Hit::Type::Link;
                h.linktext = link->target;
                h.sourcePath = m_vaultPath;
                h.linkKind = int(link->kind);
                h.rect = r.translated(origin);
                break;
            }
        }
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// NoteEmbedProvider
// ---------------------------------------------------------------------------------------------
QSizeF NoteEmbedProvider::size(const EmbedRequest& request, qreal maxWidth) const
{
    return NoteEmbeds::instance().layout(request, maxWidth)->size();
}

void NoteEmbedProvider::paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const
{
    const QRectF area = paintedArea(p, rect);
    if (area.isEmpty())
        return;
    const std::shared_ptr<NoteEmbedLayout> l = NoteEmbeds::instance().layout(request, rect.width());
    l->paint(p, rect.topLeft(), 0, area.translated(-rect.topLeft()), true);
}

QWidget* NoteEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const
{
    Q_UNUSED(context);
    Q_UNUSED(parent);
    return nullptr; // static only (the UI's provider adds the live widget)
}

quint64 NoteEmbedProvider::signature(const EmbedRequest& request) const
{
    NoteEmbeds& n = NoteEmbeds::instance();
    return qHashMulti(0x6e6f7465, n.contentHash(request.absPath), EmbedPrintScope::active());
}

} // namespace oc
