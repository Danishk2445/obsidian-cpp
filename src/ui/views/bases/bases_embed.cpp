#include "ui/views/bases/bases_embed.h"

#include "core/bases/base_runner.h"
#include "core/documents/document.h"
#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "text/codec.h"
#include "ui/views/bases/bases_controller.h"
#include "ui/views/bases/bases_layout.h"
#include "ui/views/bases/bases_render.h"
#include "ui/views/bases/bases_style.h"
#include "ui/views/bases/bases_widgets.h"
#include "ui/workspace/view.h"

#include <QDir>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QVBoxLayout>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

namespace {

QList<QPointer<VaultSession>>& sessions()
{
    static QList<QPointer<VaultSession>> list;
    return list;
}

// Heights reported by live widgets (request key -> px).
QHash<QString, int>& reportedHeights()
{
    static QHash<QString, int> h;
    return h;
}

quint64& metadataGeneration()
{
    static quint64 g = 1;
    return g;
}

// Obsidian's code block post-processor: leading tabs count as 4 spaces.
QString codeBlockText(const QString& source)
{
    static const QRegularExpression tabs(QStringLiteral("^(\\t+)"), QRegularExpression::MultilineOption);
    QString out;
    qsizetype last = 0;
    auto it = tabs.globalMatch(source);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(source).mid(last, m.capturedStart() - last);
        out += QString(m.capturedLength() * 4, u' ');
        last = m.capturedEnd();
    }
    out += QStringView(source).mid(last);
    return out;
}

QString vaultPathOf(VaultSession* s, const QString& absPath)
{
    if (!s || !s->vault() || absPath.isEmpty())
        return {};
    const std::optional<QString> p = s->vault()->vaultPath(absPath);
    return p ? *p : QString();
}

QString readText(VaultSession* s, const QString& vaultPath, QString* error)
{
    if (!s || !s->vault()) {
        if (error)
            *error = QStringLiteral("No vault");
        return {};
    }
    const std::optional<QByteArray> bytes = s->vault()->read(vaultPath, nullptr, error);
    return bytes ? decodeFile(*bytes).text.toString() : QString();
}

QString viewNameOf(const EmbedRequest& r)
{
    QString sub = r.subpath;
    if (sub.startsWith(u'#'))
        sub.remove(0, 1);
    return sub;
}

// A synchronous evaluation of an embed for sizing / static painting before its live widget exists.
struct Snapshot {
    quint64 generation = 0;
    int height = 0;
    QString title;
    QStringList headers;
    QList<QStringList> rows;
    QString error;
};

QHash<QString, Snapshot>& snapshots()
{
    static QHash<QString, Snapshot> s;
    return s;
}

const Snapshot& snapshot(const EmbedRequest& r, qreal width)
{
    Snapshot& snap = snapshots()[r.key()];
    if (snap.generation == metadataGeneration() && snap.height > 0)
        return snap;
    snap = Snapshot();
    snap.generation = metadataGeneration();
    const Style& s = basesStyle(true);
    VaultSession* session = embedSessionFor(r.absPath);
    QString text;
    QString error;
    if (r.language.isEmpty())
        text = readText(session, vaultPathOf(session, r.absPath), &error);
    else
        text = codeBlockText(r.source);
    std::optional<BaseFile> base;
    if (error.isEmpty())
        base = BaseFile::fromString(text, &error);
    if (!base) {
        snap.error = error;
        snap.height = s.headerHeight + 60;
        return snap;
    }
    QString viewName = viewNameOf(r);
    if (!base->view(viewName))
        viewName = base->views.isEmpty() ? QString() : base->views.first().name;
    const BaseView* view = base->view(viewName);
    auto ds = std::make_shared<VaultDataSource>(session);
    QueryRunner runner(ds);
    runner.setSynchronous(true);
    runner.setQuery(*base, viewName);
    const ViewData& d = runner.data();
    snap.title = viewName;
    for (const QString& c : d.columns)
        snap.headers << base->displayName(c);
    for (const EntryPtr& e : d.rows) {
        QStringList row;
        for (const QString& c : d.columns) {
            const Value v = c == u"file.name" ? e->valueOrError(c) : e->valueOrError(c);
            row << (v.isNull() ? QString() : v.toString());
        }
        snap.rows << row;
    }
    int content = 0;
    const QString type = view ? view->type : QStringLiteral("table");
    const bool grouped = view && view->groupBy;
    if (type == u"list") {
        const int line = int(std::ceil(QFontMetricsF(s.listFont).height() * 1.5));
        const bool indent = view && view->option(QStringLiteral("indentProperties")).toBool();
        content = int(d.rows.size()) * line * (indent ? std::max<int>(1, int(d.columns.size())) : 1) + (grouped ? int(d.groups.size()) * s.rowHeight : 0);
    } else if (type == u"cards") {
        const YamlNode size = view->option(QStringLiteral("cardSize"));
        const double cardSize = size.isNumber() && size.toNumber() > 0 ? size.toNumber() : 200;
        const int perRow = std::max(1, int(std::floor((width - 2) / cardSize)));
        const double cardWidth = (width - 2 - s.cardGap * (perRow - 1)) / perRow;
        double cardHeight = view->option(QStringLiteral("image")).isString() ? cardWidth * 1 : 0;
        if (!d.columns.isEmpty())
            cardHeight += s.cardPadTop + s.cardPadBottom + s.cardLineHeight;
        if (d.columns.size() > 1)
            cardHeight += (QFontMetricsF(s.cardLabelFont).height() + s.cardLineHeight + s.cardPropGap) * (d.columns.size() - 1);
        for (const ViewGroup& g : d.groups) {
            const int rows = int(std::ceil(double(g.entries.size()) / perRow));
            content += int(rows * cardHeight + std::max(0, rows - 1) * s.cardGap) + (grouped ? s.rowHeight + 12 + 16 : 0);
        }
        content += 8;
    } else {
        int mult = 1;
        const QString rh = view ? view->option(QStringLiteral("rowHeight")).toString() : QString();
        mult = rh == u"medium" ? 2 : rh == u"tall" ? 4 : rh == u"extra" ? 8 : 1;
        content = s.rowHeight; // header row
        for (int g = 0; g < d.groups.size(); ++g) {
            content += int(d.groups[g].entries.size()) * s.rowHeight * mult + (grouped ? s.rowHeight : 0);
            if (g < d.groups.size() - 1)
                content += s.groupGap;
        }
        if (view && view->summaries && !view->summaries->isEmpty())
            content += s.rowHeight * (d.groups.size() > 1 ? int(d.groups.size()) + 1 : 1);
        content += 2;
    }
    snap.height = s.headerHeight + (d.rows.isEmpty() ? 0 : content);
    return snap;
}

int requestHeight(const EmbedRequest& r, qreal width)
{
    if (auto it = reportedHeights().constFind(r.key()); it != reportedHeights().cend())
        return *it;
    return snapshot(r, width).height;
}

} // namespace

void addEmbedSession(VaultSession* session)
{
    if (!session)
        return;
    sessions().removeAll(nullptr);
    for (const QPointer<VaultSession>& s : sessions())
        if (s == session)
            return;
    sessions() << session;
    if (MetadataCache* mc = session->metadata()) {
        // stale snapshots are recomputed on their next layout
        QObject::connect(mc, &MetadataCache::resolved, session, [] { ++metadataGeneration(); });
    }
}

VaultSession* embedSessionFor(const QString& absPath)
{
    sessions().removeAll(nullptr);
    for (const QPointer<VaultSession>& s : sessions())
        if (s && !absPath.isEmpty() && (absPath.startsWith(s->root() + u'/')))
            return s;
    return sessions().isEmpty() ? nullptr : sessions().first().data();
}

// ---------------------------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------------------------
QSizeF BaseEmbedProvider::size(const EmbedRequest& request, qreal maxWidth) const
{
    return QSizeF(maxWidth, requestHeight(request, maxWidth));
}

void BaseEmbedProvider::paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const
{
    const Style& s = basesStyle(true);
    const Snapshot& snap = snapshot(request, rect.width());
    p->save();
    p->fillRect(rect, s.bgPrimary);
    p->setFont(s.uiSmall);
    p->setPen(s.text);
    const QRectF header(rect.left(), rect.top(), rect.width(), s.headerHeight);
    if (!snap.error.isEmpty()) {
        p->setPen(s.warning);
        p->drawText(rect, Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("Unable to parse your base file:\n") + snap.error);
        p->restore();
        return;
    }
    drawIcon(p, QRectF(header.left() + 4, header.top(), s.iconS, header.height()), QStringLiteral("table"), s.iconS, s.muted);
    p->drawText(header.adjusted(4 + s.iconS + 6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, snap.title);
    // a static table: the header row and the rows that fit
    const qreal top = header.bottom();
    const int cols = int(snap.headers.size());
    if (cols > 0 && !snap.rows.isEmpty()) {
        const qreal colW = rect.width() / cols;
        p->setPen(QPen(s.tableBorder, 1));
        p->drawRect(QRectF(rect.left(), top, rect.width() - 1, rect.bottom() - top - 1));
        p->setFont(s.cellFont);
        for (int c = 0; c < cols; ++c) {
            p->setPen(s.headerText);
            p->drawText(QRectF(rect.left() + c * colW + 8, top, colW - 16, s.rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetricsF(s.cellFont).elidedText(snap.headers[c], Qt::ElideRight, colW - 16));
        }
        for (int r = 0; r < snap.rows.size(); ++r) {
            const qreal y = top + (r + 1) * s.rowHeight;
            if (y > rect.bottom())
                break;
            p->fillRect(QRectF(rect.left(), y, rect.width(), 1), s.tableBorder);
            p->setPen(s.text);
            for (int c = 0; c < cols && c < snap.rows[r].size(); ++c)
                p->drawText(QRectF(rect.left() + c * colW + 8, y, colW - 16, s.rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                            QFontMetricsF(s.cellFont).elidedText(snap.rows[r][c], Qt::ElideRight, colW - 16));
        }
    }
    p->restore();
}

QWidget* BaseEmbedProvider::createWidget(const EmbedContext& context, QWidget* parent) const { return new BaseEmbedWidget(context, parent); }

quint64 BaseEmbedProvider::signature(const EmbedRequest& request) const
{
    return qHashMulti(0xba5e, requestHeight(request, 700), metadataGeneration() & 0xff);
}

// ---------------------------------------------------------------------------------------------
// Live widget
// ---------------------------------------------------------------------------------------------
BaseEmbedWidget::BaseEmbedWidget(const EmbedContext& context, QWidget* parent)
    : QWidget(parent)
    , m_ctx(context)
{
    setObjectName(QStringLiteral("ocBasesEmbed"));
    m_session = embedSessionFor(context.request.absPath);
    m_controller = new BasesController(m_session, BasesController::Mode::Embed, this);
    m_vaultPath = vaultPathOf(m_session, context.request.absPath);
    m_writeTimer.setSingleShot(true);
    m_writeTimer.setInterval(300);
    connect(&m_writeTimer, &QTimer::timeout, this, &BaseEmbedWidget::writeBack);
    // the embed action: edit the block / open the base
    m_action = new TextIconButton(isCodeBlock() ? QStringLiteral("code-2") : QStringLiteral("maximize-2"), QString(), this);
    m_action->setObjectName(QStringLiteral("ocBasesEmbedAction"));
    m_action->setToolTip(isCodeBlock() ? tr("Edit this block") : tr("Open link"));
    connect(m_action, &QAbstractButton::clicked, this, [this] {
        if (isCodeBlock()) {
            auto* ev = qobject_cast<EditorView*>(m_ctx.view.data());
            Document* doc = ev ? ev->document() : nullptr;
            if (!doc)
                return;
            // put the cursor on the fence (Live Preview shows the source)
            const md::SyntaxTree& tree = doc->syntax();
            for (int id = 0; id < tree.nodeCount(); ++id) {
                const md::BlockNode& n = tree.node(id);
                if (n.type != md::NodeType::FencedCode || tree.fence(id).language != u"base")
                    continue;
                QString body;
                for (int l = n.fromLine + 1; l < n.toLine - (tree.fence(id).closed ? 1 : 0); ++l)
                    body += doc->text().lineView(l).mid(std::min(tree.contentColumn(l), doc->text().lineLength(l))) + u'\n';
                if (body != m_ctx.request.source)
                    continue;
                ev->setFocus(Qt::OtherFocusReason);
                ev->setSelection(Selection::cursor(doc->text().lineStart(n.fromLine + (n.toLine - n.fromLine > 1 ? 1 : 0))), true);
                return;
            }
        } else {
            EmbedRegistry::instance().open(this, m_ctx.request, QString());
        }
    });
    connect(m_controller, &BasesController::preferredHeightChanged, this, &BaseEmbedWidget::reportHeight);
    connect(m_controller, &BasesController::dataUpdated, this, &BaseEmbedWidget::reportHeight);
    connect(m_controller, &BasesController::baseEdited, this, [this](const BaseFile& base) {
        m_pending = base.toString();
        m_writeTimer.start();
    });
    const QString note = notePath();
    if (!note.isEmpty())
        m_controller->setThisFile(note);
    if (isCodeBlock()) {
        m_source = m_ctx.request.source;
        QString error;
        const std::optional<BaseFile> base = BaseFile::fromString(codeBlockText(m_source), &error);
        if (base)
            m_controller->setBase(*base);
        else
            m_controller->setParseError(error);
        if (auto* ev = qobject_cast<EditorView*>(m_ctx.view.data()))
            m_controller->setReadOnly(ev->isReadOnly() || ev->mode() == EditorView::Mode::Reading);
    } else {
        loadFile();
        if (m_session && m_session->vault())
            m_modifiedConn = connect(m_session->vault(), &Vault::modified, this, [this](TFile* f, bool) {
                if (f && f->path() == m_vaultPath)
                    loadFile();
            });
    }
}

BaseEmbedWidget::~BaseEmbedWidget()
{
    if (m_writeTimer.isActive())
        writeBack();
}

QString BaseEmbedWidget::notePath() const
{
    for (QWidget* w = m_ctx.view.data(); w; w = w->parentWidget())
        if (auto* fv = qobject_cast<FileView*>(w))
            return fv->file() ? fv->file()->path() : QString();
    return {};
}

void BaseEmbedWidget::loadFile()
{
    QString error;
    const QString text = readText(m_session, m_vaultPath, &error);
    if (!error.isEmpty()) {
        m_controller->setParseError(error);
        return;
    }
    if (text == m_source && m_controller->base().views.size() > 0)
        return;
    m_source = text;
    const std::optional<BaseFile> base = BaseFile::fromString(text, &error);
    if (!base) {
        m_controller->setParseError(error);
        return;
    }
    m_controller->setBase(*base);
    const QString view = viewNameOf(m_ctx.request);
    if (!view.isEmpty())
        m_controller->selectView(view);
}

void BaseEmbedWidget::flush()
{
    if (m_writeTimer.isActive())
        writeBack();
}

void BaseEmbedWidget::writeBack()
{
    m_writeTimer.stop();
    if (m_pending.isEmpty() || m_pending == (isCodeBlock() ? codeBlockText(m_source) : m_source))
        return;
    const QString text = m_pending;
    if (isCodeBlock()) {
        replaceCode(text);
        return;
    }
    FileManager* fm = m_session ? m_session->fileManager() : nullptr;
    if (!fm || m_vaultPath.isEmpty())
        return;
    QString error;
    if (fm->editFile(m_vaultPath, [&](const Text& t) -> QList<ChangeSpec> {
            if (t.toString() == text)
                return {};
            return {ChangeSpec{0, t.length(), text}};
        }, &error))
        m_source = text;
}

bool BaseEmbedWidget::replaceCode(const QString& text)
{
    auto* ev = qobject_cast<EditorView*>(m_ctx.view.data());
    Document* doc = ev ? ev->document() : nullptr;
    if (!doc || doc->isReadOnly())
        return false;
    const md::SyntaxTree& tree = doc->syntax();
    const Text& t = doc->text();
    for (int id = 0; id < tree.nodeCount(); ++id) {
        const md::BlockNode& n = tree.node(id);
        if (n.type != md::NodeType::FencedCode)
            continue;
        const md::FenceInfo fi = tree.fence(id);
        if (fi.language != u"base")
            continue;
        const int bodyFrom = n.fromLine + 1;
        const int bodyTo = n.toLine - (fi.closed ? 1 : 0); // exclusive
        QString body;
        for (int l = bodyFrom; l < bodyTo; ++l)
            body += t.lineView(l).mid(std::min(tree.contentColumn(l), t.lineLength(l))) + u'\n';
        if (body != m_source)
            continue;
        // the container prefix of the block's lines ("> " in quotes, list indentation)
        const int prefixLen = std::min(tree.contentColumn(n.fromLine), t.lineLength(n.fromLine));
        const QString prefix = t.lineView(n.fromLine).left(prefixLen).toString();
        QString insert;
        const QStringList lines = text.endsWith(u'\n') ? text.chopped(1).split(u'\n') : text.split(u'\n');
        for (const QString& line : lines)
            insert += prefix + line + u'\n';
        qsizetype from, to;
        if (bodyTo > bodyFrom) {
            from = t.lineStart(bodyFrom);
            to = bodyTo < t.lineCount() ? t.lineStart(bodyTo) : t.length();
            if (bodyTo >= t.lineCount() && !insert.isEmpty())
                insert.chop(1); // an unclosed fence at the end of the note
        } else {
            from = to = fi.closed ? t.lineStart(n.toLine - 1) : t.length();
            if (!fi.closed)
                insert.prepend(u'\n');
        }
        Transaction tr;
        tr.changes = ChangeSet::of({ChangeSpec{from, to, insert}}, t.length());
        tr.userEvent = QStringLiteral("input.plugin");
        tr.scrollIntoView = false;
        ev->dispatch(tr);
        m_source = text;
        return true;
    }
    return false;
}

void BaseEmbedWidget::reportHeight()
{
    const int h = std::max(m_controller->header()->height(), m_controller->preferredHeight()) + 2;
    const QString key = m_ctx.request.key();
    if (reportedHeights().value(key, -1) == h)
        return;
    reportedHeights().insert(key, h);
    EmbedRegistry::instance().notifyChanged(m_ctx.request.absPath);
}

void BaseEmbedWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    m_controller->setGeometry(0, 0, width(), height());
    const Style& s = basesStyle(true);
    const QSize bs = m_action->sizeHint();
    m_action->setGeometry(width() - bs.width() - 4, (s.headerHeight - bs.height()) / 2, bs.width(), bs.height());
    m_action->raise();
    if (e->oldSize().width() != e->size().width())
        QTimer::singleShot(0, this, &BaseEmbedWidget::reportHeight);
}

void BaseEmbedWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), basesStyle(true).bgPrimary);
}

void registerBaseEmbeds()
{
    static const std::shared_ptr<BaseEmbedProvider> provider = std::make_shared<BaseEmbedProvider>();
    EmbedRegistry& r = EmbedRegistry::instance();
    if (r.provider(QStringLiteral("base")) != provider)
        r.registerExtensions({QStringLiteral("base")}, provider);
    if (r.codeBlockProvider(QStringLiteral("base")) != provider)
        r.registerCodeBlockLanguage(QStringLiteral("base"), provider);
}

} // namespace oc::basesui
