#include "ui/views/markdown/editor_suggest.h"

#include "core/commands/command_registry.h"
#include "core/commands/hotkey.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/link_suggest.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "render/theme/theme.h"
#include "ui/navigation/command_palette.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/workspace.h"

#include <QFile>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <tuple>

namespace oc {

using namespace Qt::StringLiterals;

namespace {
constexpr int kMaxHeight = 300; // .suggestion-container max-height
constexpr int kMaxWidth = 500;
constexpr int kMinWidth = 200;
constexpr int kLimit = 100; // EditorSuggest.limit

QList<QPair<int, int>> pairs(const QList<MatchRange>& ranges, int shift = 0, int clampTo = -1)
{
    QList<QPair<int, int>> out;
    for (const MatchRange& r : ranges) {
        int a = r.from + shift, b = r.to + shift;
        if (b <= 0)
            continue;
        a = std::max(a, 0);
        if (clampTo >= 0) {
            if (a >= clampTo)
                break;
            b = std::min(b, clampTo);
        }
        out.append({a, b});
    }
    return out;
}

QString withoutMd(const QString& path) { return path.endsWith(u".md"_s, Qt::CaseInsensitive) ? path.left(path.size() - 3) : path; }

bool isImage(const QString& path)
{
    static const QStringList exts = {u"bmp"_s, u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"svg"_s, u"webp"_s, u"avif"_s};
    return exts.contains(path.mid(path.lastIndexOf(u'.') + 1).toLower());
}

// Obsidian's tag Tab-completion: up to the "/" after the last matched character.
QString completeTagPrefix(const QString& tag, const QList<MatchRange>& matches)
{
    if (!matches.isEmpty()) {
        const qsizetype slash = tag.indexOf(u'/', matches.last().to);
        if (slash >= 0)
            return tag.left(slash + 1);
    }
    return tag;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// SuggestPopup
// ---------------------------------------------------------------------------------------------
SuggestPopup::SuggestPopup(OverlayLayer* layer)
    : QWidget(layer)
    , m_layer(layer)
{
    setObjectName(u"ocSuggestPopup"_s);
    setAttribute(Qt::WA_TranslucentBackground);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(1, 1, 1, 1);
    v->setSpacing(0);
    m_list = new SuggestionList(this);
    m_list->setPadding(4);
    m_list->setFocusPolicy(Qt::NoFocus); // the editor keeps the focus
    v->addWidget(m_list, 1);
    m_instructions = new PromptInstructions(this);
    m_instructions->hide();
    v->addWidget(m_instructions);
    hide();
}

void SuggestPopup::setInstructions(const QList<SuggestModal::Instruction>& items)
{
    m_instructions->setItems(items);
    m_instructions->setVisible(!items.isEmpty());
}

void SuggestPopup::showAt(const QRect& anchor)
{
    if (!m_layer)
        return;
    const QRect bounds = m_layer->rect().adjusted(8, 8, -8, -8);
    // content-sized like the CSS box: the widest row or the one-line instructions, capped
    const bool instr = m_instructions->isVisibleTo(this);
    const int natural = std::max(m_list->preferredWidth(), instr ? m_instructions->naturalWidth() : 0) + 2;
    const int w = std::clamp(natural, kMinWidth, std::min(kMaxWidth, bounds.width()));
    const int instrH = instr ? m_instructions->heightForWidth(w - 2) : 0;
    m_instructions->setFixedHeight(instrH);
    const int h = std::min(m_list->contentHeightForWidth(w - 2, kMaxHeight) + instrH + 2, kMaxHeight);
    m_list->setFixedHeight(h - instrH - 2); // (a scroll area's minimum size hint would pad short lists)
    int x = std::clamp(anchor.left(), bounds.left(), std::max(bounds.left(), bounds.right() - w));
    int y = anchor.bottom() + 1;
    if (y + h > bounds.bottom() && anchor.top() - h >= bounds.top())
        y = anchor.top() - h; // no room below: above the text
    setGeometry(x, std::max(bounds.top(), y), w, h);
    show();
    raise();
    m_layer->refresh();
}

void SuggestPopup::paintEvent(QPaintEvent*)
{
    const ThemePtr t = currentTheme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal radius = t->px(u"--radius-m"_s, -1, 8);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.fillPath(path, t->color(u"--suggestion-background"_s, t->color(u"--background-primary"_s)));
    p.setPen(QPen(t->color(u"--background-modifier-border"_s), 1));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------
// EditorSuggest
// ---------------------------------------------------------------------------------------------
EditorSuggest::EditorSuggest(MarkdownView* view, EditorView* editor)
    : QObject(view)
    , m_view(view)
    , m_editor(editor)
    , m_editorObject(editor)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(50);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        const bool force = m_force;
        m_force = false;
        trigger(force);
    });
    editor->installEventFilter(this);
    connect(editor, &EditorView::selectionChanged, this, [this] { m_debounce.start(); });
    connect(editor, &EditorView::focusChangedIn, this, [this](bool in) {
        if (!in)
            close();
    });
    connect(editor, &EditorView::modeChanged, this, [this] { close(); });
    connect(editor, &EditorView::scrolled, this, [this] {
        if (m_open)
            reposition();
    });
}

EditorSuggest::~EditorSuggest()
{
    if (m_editor)
        m_editor->setProperty("ocSuggestActive", false);
    delete m_popup;
}

void EditorSuggest::documentChanged()
{
    close();
    disconnect(m_docConn);
    if (Document* d = m_editor ? m_editor->document() : nullptr)
        m_docConn = connect(d, &Document::changed, this, &EditorSuggest::onDocChanged);
}

void EditorSuggest::onDocChanged(const DocUpdate& u)
{
    if (u.origin != m_editor)
        return;
    if (u.tr.isUserEvent(u"input"_s) || u.tr.isUserEvent(u"delete"_s)) {
        m_force = true;
        m_debounce.start();
    }
}

bool EditorSuggest::isOpen() const { return m_open; }

void EditorSuggest::setOpen(bool open)
{
    m_open = open;
    if (m_editor)
        m_editor->setProperty("ocSuggestActive", open); // HotkeyDispatcher leaves the suggest keys alone
}

void EditorSuggest::close()
{
    if (m_popup && m_popup->isVisible()) {
        m_popup->hide();
        if (auto* layer = qobject_cast<OverlayLayer*>(m_popup->parentWidget()))
            layer->refresh();
    }
    if (m_links)
        m_links->reset();
    m_ctx = {};
    setOpen(false);
}

QString EditorSuggest::sourcePath() const
{
    TFile* f = m_view ? m_view->file() : nullptr;
    return f ? f->path() : QString();
}

void EditorSuggest::trigger(bool force)
{
    if (!m_view || !m_editor || !m_editor->document() || m_editor->mode() == EditorView::Mode::Reading
        || (m_requireFocus && !m_editor->hasFocus())) {
        close();
        return;
    }
    const Selection& sel = m_editor->selection();
    if (sel.ranges().size() != 1 || !sel.main().empty()) {
        close();
        return;
    }
    const Text& text = m_editor->document()->text();
    const qsizetype head = sel.main().head;
    const int line = text.lineAt(head);
    const int col = int(head - text.lineStart(line));
    const QString lineText = text.line(line);
    Context ctx;
    ctx.line = line;
    if (auto t = linkSuggestTrigger(lineText, col)) {
        ctx.kind = Kind::Link;
        ctx.start = t->start;
        ctx.end = t->end;
        ctx.query = t->query;
    } else if (auto t2 = tagSuggestTrigger(lineText, col)) {
        ctx.kind = Kind::Tag;
        ctx.start = t2->start;
        ctx.end = t2->end;
        ctx.query = t2->query;
    } else if (auto t3 = footnoteSuggestTrigger(lineText, col)) {
        ctx.kind = Kind::Footnote;
        ctx.start = t3->start;
        ctx.end = t3->end;
        ctx.query = t3->query;
    } else if (slashCommandsEnabled() && m_view->file()) {
        // Slash commands: "/query" at the start of the line or after whitespace, up to the cursor.
        static const QRegularExpression slash(u"(^|\\s)\\/([^\\s\\/]*)$"_s);
        const QRegularExpressionMatch m = slash.match(QStringView(lineText).left(col));
        if (m.hasMatch()) {
            ctx.kind = Kind::Command;
            ctx.start = int(m.capturedStart() + m.capturedLength(1));
            ctx.end = col;
            ctx.query = m.captured(2);
        }
    }
    if (ctx.kind == Kind::None) {
        close();
        return;
    }
    if (m_open && ctx.kind != m_ctx.kind)
        close(); // another suggester takes over
    if (!force && !m_open) {
        m_ctx = {};
        return;
    }
    show(ctx);
}

Suggestion EditorSuggest::renderLink(const LinkSuggestion& s) const
{
    using T = LinkSuggestion::Type;
    Suggestion out;
    out.downranked = s.downranked;
    const bool global = m_links && m_links->isGlobal();
    if (global && (s.type == T::Heading || s.type == T::Block))
        out.note = withoutMd(s.file);
    switch (s.type) {
    case T::None:
        out.title = tr("No match found");
        break;
    case T::File: {
        const qsizetype slash = s.path.lastIndexOf(u'/');
        const QString folder = slash < 0 ? QString() : s.path.left(slash + 1);
        out.title = s.path.mid(folder.size());
        out.titleMatches = pairs(s.matches, -int(folder.size()));
        out.note = folder;
        out.noteMatches = pairs(s.matches, 0, int(folder.size()));
        break;
    }
    case T::Alias: {
        out.note = s.path;
        out.auxIcon = u"forward"_s;
        static const QRegularExpression size(u"^\\d+(x\\d+)?$"_s);
        if (isImage(s.path) && size.match(s.alias).hasMatch()) {
            out.auxIcon = u"scaling"_s;
            out.title = tr("Image size: %1px wide").arg(s.alias);
        } else if (!s.alias.isEmpty()) {
            out.title = s.alias;
            out.titleMatches = pairs(s.matches);
        } else {
            out.title = tr("Display text");
            out.downranked = true; // .suggestion-empty-suggestion
        }
        break;
    }
    case T::Linktext:
        out.title = s.path;
        out.titleMatches = pairs(s.matches);
        break;
    case T::Heading:
        if (!s.heading.isEmpty()) {
            out.title = s.heading;
            out.titleMatches = pairs(s.matches);
        } else {
            out.title = tr("Heading");
            out.downranked = true;
        }
        if (s.level > 0)
            out.auxText = u"H"_s + QString::number(s.level);
        break;
    case T::Block: {
        QStringList lines = s.display.split(u'\n');
        if (lines.size() > 4) {
            lines = lines.mid(0, 4);
            lines.last() += u"…"_s;
        }
        out.title = lines.join(QChar(QChar::LineSeparator));
        out.titleMatches = pairs(s.matches);
        if (!s.blockId.isEmpty()) {
            const int shift = int(out.note.trimmed().isEmpty() ? 0 : out.note.size() + 3);
            if (shift)
                out.note += u" > "_s;
            out.note += s.blockId;
            out.noteMatches = pairs(s.idMatches, shift);
        }
        break;
    }
    }
    return out;
}

void EditorSuggest::show(const Context& ctx)
{
    VaultSession* session = m_view->session();
    m_ctx = ctx;
    QList<Suggestion> rows;
    QList<SuggestModal::Instruction> instructions;
    bool selectNone = false;
    switch (ctx.kind) {
    case Kind::Link: {
        if (!session || !session->metadata()) {
            close();
            return;
        }
        if (!m_links) {
            m_links = std::make_unique<LinkSuggestProvider>(session->vault(), session->metadata());
            m_links->setContentReader([session](const QString& path) -> std::optional<QString> {
                if (DocumentManager* dm = session->documents())
                    if (Document* d = dm->find(path))
                        return d->text().toString();
                Vault* v = session->vault();
                if (!v)
                    return std::nullopt;
                QFile f(v->absolutePath(path));
                if (!f.open(QIODevice::ReadOnly))
                    return std::nullopt;
                QString s = QString::fromUtf8(f.readAll());
                if (s.startsWith(QChar(0xFEFF)))
                    s.remove(0, 1);
                return s;
            });
        }
        m_linkItems = m_links->suggestions(ctx.query.trimmed(), sourcePath());
        if (m_linkItems.size() > kLimit)
            m_linkItems.resize(kLimit);
        for (const LinkSuggestion& s : m_linkItems)
            rows.append(renderLink(s));
        for (const auto& [cmd, purpose] : m_links->instructions())
            instructions.append({cmd, purpose});
        const QString q = ctx.query;
        selectNone = q.isEmpty() || q == u"^" || q == u"#";
        break;
    }
    case Kind::Tag: {
        if (!session || !session->metadata()) {
            close();
            return;
        }
        m_tagItems = tagSuggestions(*session->metadata(), ctx.query);
        if (m_tagItems.size() > kLimit)
            m_tagItems.resize(kLimit);
        for (const TagSuggestion& t : m_tagItems) {
            Suggestion s;
            s.title = t.tag;
            s.titleMatches = pairs(t.matches);
            rows.append(s);
        }
        break;
    }
    case Kind::Footnote: {
        static const QRegularExpression def(u"^\\[\\^([^\\]]+)\\]:"_s);
        m_footnoteItems.clear();
        const Text& text = m_editor->document()->text();
        const FuzzyQuery q(ctx.query);
        int maxNum = 0;
        QList<QPair<double, FootnoteItem>> scored;
        for (int l = 0; l < text.lineCount(); ++l) {
            const QStringView lv = text.lineView(l);
            if (!lv.startsWith(u"[^"))
                continue;
            const QRegularExpressionMatch m = def.matchView(lv);
            if (!m.hasMatch())
                continue;
            const QString id = m.captured(1);
            bool ok = false;
            const int n = id.toInt(&ok);
            if (ok)
                maxNum = std::max(maxNum, n);
            const QString full = lv.toString();
            if (const auto r = q.match(full); r || q.isEmpty())
                scored.append({r ? r->score : 0.0, FootnoteItem{id, full, false}});
        }
        if (ctx.query.isEmpty())
            scored.append({1.0, FootnoteItem{QString::number(maxNum + 1), u"[^%1]: "_s.arg(maxNum + 1) + tr("New footnote..."), true}});
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [score, item] : scored) {
            m_footnoteItems.append(item);
            Suggestion s;
            s.title = item.text;
            rows.append(s);
        }
        break;
    }
    case Kind::Command: {
        CommandRegistry* registry = session ? session->commands() : nullptr;
        if (!registry) {
            close();
            return;
        }
        // The command palette's commands, fuzzy-matched by name (all of them for "/"), best first.
        const CommandPaletteOptions palette =
            CommandPaletteOptions::fromJson(session->config() ? session->config()->object(u"command-palette"_s) : QJsonObject());
        const FuzzyQuery q(ctx.query);
        QList<std::tuple<double, const Command*, QList<QPair<int, int>>>> scored;
        for (const Command* c : registry->all()) {
            if (c->name.isEmpty() || !registry->isAvailable(c->id))
                continue;
            if (ctx.query.isEmpty()) {
                scored.append({0.0, c, {}});
            } else if (const std::optional<FuzzyResult> r = q.match(c->name)) {
                QList<QPair<int, int>> ranges;
                for (const MatchRange& m : matchIndicesToRanges(r->matches))
                    ranges.append({m.from, m.to});
                scored.append({r->score, c, ranges});
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        m_commandItems.clear();
        for (const auto& [score, c, ranges] : scored) {
            if (m_commandItems.size() >= kLimit)
                break;
            m_commandItems.append(c->id);
            Suggestion s;
            const qsizetype colon = c->name.indexOf(u": "_s);
            const int shift = colon > 0 ? int(colon) + 2 : 0;
            s.prefix = colon > 0 ? c->name.left(colon) : QString();
            s.title = c->name.mid(shift);
            for (const auto& [from, to] : ranges)
                s.titleMatches.append({from - shift, to - shift});
            for (const Hotkey& hk : registry->hotkeys(c->id))
                if (hk.isValid())
                    s.hotkeys << hk.toString();
            if (palette.pinned.contains(c->id))
                s.auxIcon = u"pin"_s;
            rows.append(s);
        }
        break;
    }
    case Kind::None:
        break;
    }
    if (rows.isEmpty()) {
        close();
        return;
    }
    if (!m_popup) {
        Workspace* ws = m_view->workspace();
        MainWindow* w = ws ? ws->window() : nullptr;
        if (!w || !w->overlay())
            return;
        m_popup = new SuggestPopup(w->overlay());
        connect(m_popup->list(), &SuggestionList::activated, this, [this](int index, Qt::KeyboardModifiers) { choose(index, QString()); });
    }
    m_popup->list()->setSuggestions(rows);
    if (selectNone)
        m_popup->list()->clearSelection();
    m_popup->setInstructions(instructions);
    setOpen(true);
    reposition();
}

void EditorSuggest::reposition()
{
    if (!m_popup || !m_editor || !m_editor->document() || m_ctx.kind == Kind::None)
        return;
    const Text& text = m_editor->document()->text();
    const qsizetype base = text.lineStart(m_ctx.line);
    const QRectF a = m_editor->caretRectAt(base + m_ctx.start);
    const QRectF b = m_editor->caretRectAt(base + m_ctx.end);
    QRectF r(QPointF(std::min(a.left(), b.left()), a.top()), QPointF(std::max(a.right(), b.right()), b.bottom()));
    QWidget* layer = m_popup->parentWidget();
    const QPoint tl = m_editor->viewport()->mapTo(layer->window(), r.topLeft().toPoint());
    const QPoint origin = layer->mapTo(layer->window(), QPoint(0, 0));
    m_popup->showAt(QRect(tl - origin, r.size().toSize()));
}

bool EditorSuggest::choose(int index, const QString& key)
{
    if (!m_open || index < 0)
        return false;
    switch (m_ctx.kind) {
    case Kind::Link:
        if (index >= m_linkItems.size())
            return false;
        chooseLink(m_linkItems[index], key);
        return true;
    case Kind::Tag:
        if (index >= m_tagItems.size())
            return false;
        chooseTag(m_tagItems[index], key == u"Tab");
        return true;
    case Kind::Footnote:
        if (index >= m_footnoteItems.size())
            return false;
        chooseFootnote(m_footnoteItems[index]);
        return true;
    case Kind::Command:
        if (index >= m_commandItems.size())
            return false;
        chooseCommand(m_commandItems[index]);
        return true;
    case Kind::None:
        break;
    }
    return false;
}

bool EditorSuggest::slashCommandsEnabled() const
{
    VaultSession* s = m_view ? m_view->session() : nullptr;
    // Core plugin "slash-command", off by default.
    return s && s->config() && s->config()->object(u"core-plugins"_s).value(u"slash-command"_s).toBool(false);
}

void EditorSuggest::chooseCommand(const QString& id)
{
    const Context ctx = m_ctx;
    close();
    Document* doc = m_editor ? m_editor->document() : nullptr;
    VaultSession* session = m_view ? m_view->session() : nullptr;
    if (!doc || !session || !session->commands())
        return;
    // Remove "/query", then run the command in this editor.
    const qsizetype base = doc->text().lineStart(ctx.line);
    Transaction tr;
    tr.changes = ChangeSet::of(ChangeSpec{base + ctx.start, base + ctx.end, QString()}, doc->text().length());
    tr.selection = Selection::cursor(base + ctx.start);
    tr.userEvent = u"delete"_s;
    m_editor->dispatch(tr);
    session->commands()->execute(id);
}

void EditorSuggest::chooseLink(const LinkSuggestion& s, const QString& key)
{
    const Context ctx = m_ctx;
    close();
    Document* doc = m_editor ? m_editor->document() : nullptr;
    VaultSession* session = m_view ? m_view->session() : nullptr;
    if (!doc || !session)
        return;
    const Text& text = doc->text();
    const QString line = text.line(ctx.line);
    const qsizetype base = text.lineStart(ctx.line);
    VaultConfig* cfg = session->vaultConfig();
    MetadataCache* mc = session->metadata();
    const LinkResolver::LinkFormat format = cfg ? cfg->newLinkFormat() : LinkResolver::LinkFormat::Shortest;
    auto linktext = [mc, format](const QString& target, const QString& source, bool omitMd) {
        return mc ? mc->resolver().linkText(target, source, format, omitMd) : target;
    };
    const QString source = sourcePath();
    const LinkReplacement r = linkSuggestReplacement(s, ctx.query, line, ctx.start - 2, ctx.end, source, key,
                                                     cfg && cfg->useMarkdownLinks(), linktext);
    QList<ChangeSpec> changes{{base + r.start, base + r.end, r.replacement}};
    qsizetype selFrom = base + r.selectionStart, selTo = base + r.selectionEnd;
    if (s.type == LinkSuggestion::Type::Block && !r.newBlockId.isEmpty()) {
        if (s.file != source) {
            insertBlockIdElsewhere(s, r.newBlockId);
        } else {
            // same file: find the block's text nearest to where it was and append the id there
            const BlockIdInsertion ins = blockIdInsertion(s, r.newBlockId);
            const QString blockText = s.content.mid(s.blockFrom, (s.blockParagraphEnd >= 0 && s.blockType == u"listItem" ? s.blockParagraphEnd : s.blockTo) - s.blockFrom);
            const QString all = text.toString();
            qsizetype best = -1;
            if (!blockText.isEmpty()) {
                for (qsizetype at = all.indexOf(blockText); at >= 0; at = all.indexOf(blockText, at + blockText.size())) {
                    if (best < 0 || std::abs(at - s.blockFrom) < std::abs(best - s.blockFrom))
                        best = at;
                }
            }
            if (best >= 0) {
                const qsizetype at = best + blockText.size() + (ins.at - (s.blockFrom + blockText.size()));
                changes.append({at, at, ins.text});
                std::sort(changes.begin(), changes.end(), [](const ChangeSpec& a, const ChangeSpec& b) { return a.from < b.from; });
                if (at <= base + r.start) {
                    selFrom += ins.text.size();
                    selTo += ins.text.size();
                }
            }
        }
    }
    Transaction tr;
    tr.changes = ChangeSet::of(changes, text.length());
    tr.selection = Selection::single(selFrom, selTo);
    tr.userEvent = u"input.autocomplete"_s;
    m_editor->dispatch(tr);
    m_editor->setFocus();
}

bool EditorSuggest::insertBlockIdElsewhere(const LinkSuggestion& s, const QString& id)
{
    VaultSession* session = m_view ? m_view->session() : nullptr;
    DocumentManager* dm = session ? session->documents() : nullptr;
    if (!dm)
        return false;
    Document* doc = dm->acquire(s.file);
    if (!doc)
        return false;
    const BlockIdInsertion ins = blockIdInsertion(s, id);
    const QString all = doc->text().toString();
    const int ownEnd = s.blockType == u"listItem" && s.blockParagraphEnd >= 0 ? s.blockParagraphEnd : s.blockTo;
    const QString blockText = s.content.mid(s.blockFrom, ownEnd - s.blockFrom);
    qsizetype best = -1;
    for (qsizetype at = all.indexOf(blockText); !blockText.isEmpty() && at >= 0; at = all.indexOf(blockText, at + blockText.size())) {
        if (best < 0 || std::abs(at - s.blockFrom) < std::abs(best - s.blockFrom))
            best = at;
    }
    bool ok = false;
    if (best >= 0) {
        const qsizetype at = best + blockText.size() + (ins.at - ownEnd);
        Transaction tr;
        tr.changes = ChangeSet::of(ChangeSpec{at, at, ins.text}, doc->text().length());
        tr.userEvent = u"input.autocomplete"_s;
        doc->dispatch(tr, nullptr, Selection::cursor(0));
        ok = doc->save();
    }
    dm->release(doc);
    return ok;
}

void EditorSuggest::chooseTag(const TagSuggestion& t, bool tab)
{
    const Context ctx = m_ctx;
    close();
    Document* doc = m_editor ? m_editor->document() : nullptr;
    if (!doc)
        return;
    const qsizetype base = doc->text().lineStart(ctx.line);
    const QString insert = tab ? QString(u"#"_s + completeTagPrefix(t.tag, t.matches)) : QString(u"#"_s + t.tag + u' ');
    Transaction tr;
    tr.changes = ChangeSet::of(ChangeSpec{base + ctx.start, base + ctx.end, insert}, doc->text().length());
    tr.selection = Selection::cursor(base + ctx.start + insert.size());
    tr.userEvent = u"input.autocomplete"_s;
    m_editor->dispatch(tr);
    m_editor->setFocus();
}

void EditorSuggest::chooseFootnote(const FootnoteItem& f)
{
    const Context ctx = m_ctx;
    close();
    Document* doc = m_editor ? m_editor->document() : nullptr;
    if (!doc)
        return;
    const Text& text = doc->text();
    const qsizetype base = text.lineStart(ctx.line);
    const QString ref = u"[^"_s + f.id + u']';
    Transaction tr;
    tr.userEvent = u"input.autocomplete"_s;
    if (!f.create) {
        tr.changes = ChangeSet::of(ChangeSpec{base + ctx.start, base + ctx.end, ref}, text.length());
        tr.selection = Selection::cursor(base + ctx.start + ref.size());
    } else {
        // Obsidian's insertFootnote: the reference here, its definition at the end of the note
        const QString last = text.line(text.lineCount() - 1);
        static const QRegularExpression def(u"^\\[\\^[^\\]]+\\]:"_s);
        const QString sep = last.isEmpty() ? QString() : (def.match(last).hasMatch() ? u"\n"_s : u"\n\n"_s);
        const QString definition = sep + ref + u": "_s;
        tr.changes = ChangeSet::of({{base + ctx.start, base + ctx.end, ref}, {text.length(), text.length(), definition}}, text.length());
        tr.selection = Selection::cursor(text.length() - (ctx.end - ctx.start) + ref.size() + definition.size());
    }
    m_editor->dispatch(tr);
    m_editor->setFocus();
}

bool EditorSuggest::eventFilter(QObject* obj, QEvent* ev)
{
    // compare identities only: the editor may be mid-destruction here (no QPointer downcast)
    if (!m_open || obj != m_editorObject || !m_popup)
        return false;
    if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::ShortcutOverride)
        return false;
    auto* ke = static_cast<QKeyEvent*>(ev);
    SuggestionList* list = m_popup->list();
    const int key = ke->key();
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
    const bool press = ev->type() == QEvent::KeyPress;
    auto handled = [&] {
        ke->accept();
        return true;
    };
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods != Qt::NoModifier)
            return false;
        if (press) {
            const int cur = list->selectedIndex();
            list->setSelectedIndex(key == Qt::Key_Up ? (cur < 0 ? -1 : cur - 1) : cur + 1);
        }
        return handled();
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        if (press)
            key == Qt::Key_PageUp ? list->pageUp() : list->pageDown();
        return handled();
    case Qt::Key_Escape:
        if (press)
            close();
        return handled();
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_ctx.kind == Kind::Link && (mods & Qt::ShiftModifier)) {
            // Shift+Enter: link the typed text as is
            if (press) {
                const LinkSuggestion none;
                chooseLink(none, u"Enter"_s);
            }
            return handled();
        }
        if (list->selectedIndex() < 0) {
            if (press)
                close();
            return false; // nothing selected: Enter goes to the editor
        }
        if (press)
            choose(list->selectedIndex(), u"Enter"_s);
        return handled();
    case Qt::Key_Tab:
        if (mods != Qt::NoModifier || list->selectedIndex() < 0)
            return false;
        if (press)
            choose(list->selectedIndex(), u"Tab"_s);
        return handled();
    default:
        break;
    }
    // "#" / "^" accept the selected file (and "#" a heading) and continue with a subpath
    if (m_ctx.kind == Kind::Link && m_links && list->selectedIndex() >= 0 && (ke->text() == u"#" || ke->text() == u"^")) {
        const bool hash = ke->text() == u"#";
        const auto mode = m_links->mode();
        if (mode == LinkSuggestProvider::Mode::File || (hash && mode == LinkSuggestProvider::Mode::Heading)) {
            if (press)
                choose(list->selectedIndex(), ke->text());
            return handled();
        }
    }
    return false;
}

} // namespace oc
