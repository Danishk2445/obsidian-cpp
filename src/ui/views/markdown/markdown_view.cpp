#include "ui/views/markdown/markdown_view.h"

#include "ui/views/markdown/attachments.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/views/markdown/page_preview.h"

#include "editor/live_preview_decorator.h"
#include "editor/spell/spell_checker.h"
#include "editor/vim/vim_controller.h"

#include "core/commands/command_registry.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/documents/document_manager.h"
#include "core/documents/fold_store.h"
#include "core/files/file_manager.h"
#include "core/metadata/link_resolver.h"
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/plugins/pdf_export_ui.h"
#include "ui/shell/main_window.h"
#include "ui/views/markdown/editor_search_bar.h"
#include "ui/views/markdown/inline_title.h"
#include "ui/views/markdown/properties/properties_style.h"
#include "ui/views/markdown/properties/properties_widget.h"
#include "ui/views/sidebar/backlink_view.h"
#include "core/properties/frontmatter_editor.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

namespace oc {

namespace {
std::function<void(const QString&, MarkdownView*)> g_tagHandler;

// Obsidian's heading normalization for subpath matching: special characters become spaces,
// whitespace collapses, case-insensitive.
QString normalizeHeading(QString s)
{
    static const QRegularExpression special(QStringLiteral(R"([!"#$%&()*+,.:;<=>?@^`{|}~/\[\]\\\r\n])"));
    s.replace(special, QStringLiteral(" "));
    return s.simplified().toLower();
}

NoticeManager* noticesFor(const View* v)
{
    Workspace* ws = v->workspace();
    MainWindow* w = ws ? ws->window() : nullptr;
    return w ? w->notices() : nullptr;
}

// The editor's top widget: the inline title with the properties below it (Obsidian's .inline-title and
// .metadata-container at the top of the sizer). It reaches kHeaderOverhang px into the gutter left of
// the text column, where the properties' collapse indicator sits; the properties are shifted 4px left
// of the text (.metadata-container: translateX(-4px)) and followed by a 2rem margin.
constexpr int kHeaderOverhang = 28;

class DocumentHeader : public QWidget {
public:
    DocumentHeader(InlineTitle* title, PropertiesWidget* properties)
        : m_title(title)
        , m_properties(properties)
    {
        setObjectName(QStringLiteral("ocDocumentHeader"));
        setAttribute(Qt::WA_TranslucentBackground);
        title->setParent(this);
        properties->setParent(this);
        properties->setLeftInset(kHeaderOverhang - 4);
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return layoutChildren(w, false); }
    QSize sizeHint() const override { return {700, heightForWidth(width() > 0 ? width() : 700)}; }
    void relayout()
    {
        layoutChildren(width(), true);
        updateGeometry();
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        layoutChildren(width(), true);
    }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::LayoutRequest)
            layoutChildren(width(), true);
        return QWidget::event(e);
    }

private:
    int layoutChildren(int width, bool apply) const
    {
        const int w = std::max(50, width - kHeaderOverhang);
        int y = 0;
        if (!m_title->isHidden()) {
            const int h = m_title->heightForWidth(w);
            if (apply)
                m_title->setGeometry(kHeaderOverhang, y, w, h);
            y += h + m_title->bottomMargin();
        }
        const bool props = m_properties->shouldShow();
        if (apply && m_properties->isHidden() == props)
            m_properties->setVisible(props);
        if (props) {
            const int pw = std::max(50, width - 4);
            const int h = m_properties->heightForWidth(pw);
            if (apply)
                m_properties->setGeometry(0, y, pw, h);
            y += h + propui::style().marginBottom;
        }
        return y;
    }

    InlineTitle* m_title;
    PropertiesWidget* m_properties;
};
} // namespace

void MarkdownView::setTagHandler(std::function<void(const QString&, MarkdownView*)> handler) { g_tagHandler = std::move(handler); }

MarkdownView::MarkdownView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_editor(new EditorView(this))
    , m_searchBar(new EditorSearchBar(m_editor, this))
    , m_title(new InlineTitle)
    , m_properties(new PropertiesWidget)
    , m_header(new DocumentHeader(m_title, m_properties))
    , m_suggest(new EditorSuggest(this, m_editor))
{
    setObjectName(QStringLiteral("ocMarkdownView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_searchBar);
    v->addWidget(m_editor, 1);
    m_editor->setTopWidget(m_header, 0);
    m_editor->setTopWidgetOverhang(kHeaderOverhang);
    setFocusProxy(m_editor);

    connect(m_title, &InlineTitle::renameRequested, this, &MarkdownView::renameFromTitle);
    connect(m_title, &InlineTitle::leaveToEditor, this, [this] {
        m_editor->setFocus();
        if (m_editor->document())
            m_editor->setCursorPosition(0, false);
    });
    connect(m_editor, &EditorView::linkActivated, this, &MarkdownView::openLink);
    connect(m_editor, &EditorView::modeChanged, this, [this](EditorView::Mode m) {
        m_title->setEditable(m != EditorView::Mode::Reading);
        refreshProperties(false);
        updateBacklinksFooter();
        emit modeChanged(m);
        emit stateChanged();
    });
    m_foldSave.setSingleShot(true);
    m_foldSave.setInterval(500);
    connect(&m_foldSave, &QTimer::timeout, this, &MarkdownView::saveFolds);
    connect(m_editor, &EditorView::foldsChanged, this, [this] {
        if (!m_restoringFolds)
            m_foldSave.start();
    });
    if (auto* tm = ThemeManager::instance())
        connect(tm, &ThemeManager::themeChanged, this, [this] {
            static_cast<DocumentHeader*>(m_header)->relayout();
            m_editor->updateTopWidget();
        });

    // Properties: the metadata editor between the inline title and the text.
    {
        PropertyEditorContext ctx;
        ctx.session = [this] { return session(); };
        ctx.sourcePath = [this] { return m_file ? m_file->path() : QString(); };
        ctx.openLink = [this](const QString& target, int kind, bool newLeaf) {
            openLinkText(workspace(), target, kind == 3 ? 3 : 0, m_file ? m_file->path() : QString(), newLeaf);
        };
        ctx.openTag = [this](const QString& tag) {
            if (g_tagHandler)
                g_tagHandler(tag, this);
        };
        ctx.hoverLink = [this](const QString& linktext, const QRect& rect, QWidget* parent) {
            Workspace* ws = workspace();
            PagePreview* pp = PagePreview::of(ws ? ws->window() : nullptr);
            if (!pp)
                return;
            if (linktext.isEmpty())
                pp->onLinkUnhover(parent);
            else
                pp->onLinkHover(mode() == EditorView::Mode::Reading ? QStringLiteral("preview") : QStringLiteral("editor"), linktext,
                                m_file ? m_file->path() : QString(), rect, parent);
        };
        ctx.isResolved = [this](const QString& target) {
            VaultSession* s = session();
            MetadataCache* mc = s ? s->metadata() : nullptr;
            const QString path = LinkResolver::splitSubpath(target).first;
            if (!mc || path.isEmpty())
                return true;
            return !mc->resolver().resolve(path, m_file ? m_file->path() : QString()).isEmpty();
        };
        ctx.notice = [this](const QString& message) {
            if (NoticeManager* n = noticesFor(this))
                n->show(message);
        };
        m_properties->setContext(ctx);
    }
    m_properties->setEditHandler([this](const PropertiesWidget::Edit& edit) { return editFrontmatter(edit); });
    connect(m_properties, &PropertiesWidget::heightChanged, this, [this] {
        static_cast<DocumentHeader*>(m_header)->relayout();
        m_editor->updateTopWidget();
    });
    connect(m_properties, &PropertiesWidget::focusAfterRequested, this, [this] {
        m_editor->setFocus(Qt::OtherFocusReason);
        if (m_doc) {
            const Text& t = m_doc->text();
            const int line = m_frontmatterEnd >= 0 ? std::min(m_frontmatterEnd + 1, t.lineCount() - 1) : 0;
            m_editor->setCursorPosition(t.lineStart(line), false);
            clampSelectionToBody();
        }
    });
    connect(m_properties, &PropertiesWidget::focusBeforeRequested, this, [this] {
        if (m_title->isVisible() && mode() != EditorView::Mode::Reading)
            m_title->beginRename(false);
    });
    connect(m_properties, &PropertiesWidget::undoRequested, this, [this] { m_editor->undo(); });
    connect(m_properties, &PropertiesWidget::redoRequested, this, [this] { m_editor->redo(); });
    connect(m_properties, &PropertiesWidget::showSourceRequested, this, [this] {
        setMode(EditorView::Mode::Source);
        m_editor->setFocus(Qt::OtherFocusReason);
        m_editor->setCursorPosition(0, true);
    });
    // The frontmatter hidden behind the properties (Live Preview / Reading): zero-height lines.
    m_editor->addDecorationLayer([this](int line, LineDecoration& d) {
        if (m_frontmatterEnd >= 0 && line <= m_frontmatterEnd && m_editor->mode() != EditorView::Mode::Source)
            d.hidden = true;
    });
    connect(m_editor, &EditorView::selectionChanged, this, &MarkdownView::clampSelectionToBody);
    connect(m_editor, &EditorView::focusChangedIn, this, [this](bool in) {
        if (in)
            clampSelectionToBody();
    });
    m_editor->installEventFilter(this);

    // Link resolution for rendering: unresolved-link styling and image embeds.
    if (LivePreviewDecorator* lp = m_editor->livePreview()) {
        lp->isResolved = [this](const QString& target) {
            VaultSession* s = session();
            MetadataCache* mc = s ? s->metadata() : nullptr;
            const QString path = LinkResolver::splitSubpath(target).first;
            if (!mc || path.isEmpty())
                return true;
            return !mc->resolver().resolve(path, m_file ? m_file->path() : QString()).isEmpty();
        };
        lp->resolveAttachment = [this](const QString& linkpath) -> QString {
            VaultSession* s = session();
            MetadataCache* mc = s ? s->metadata() : nullptr;
            if (!mc || !s->vault())
                return {};
            const QString dest = mc->resolver().resolve(LinkResolver::splitSubpath(linkpath).first, m_file ? m_file->path() : QString());
            return dest.isEmpty() ? QString() : s->vault()->absolutePath(dest);
        };
    }

    // Spellcheck (editing modes only, like Chromium's contenteditable spellcheck).
    m_editor->addDecorationLayer([this](int line, LineDecoration& d) {
        Document* doc = m_editor->document();
        if (!m_spellcheck || !doc || m_editor->mode() == EditorView::Mode::Reading)
            return;
        int skipCol = -1; // leave the word being typed alone
        if (m_typing) {
            const Text& t = doc->text();
            const qsizetype head = m_editor->cursorPosition();
            if (t.lineAt(head) == line)
                skipCol = int(head - t.lineStart(line));
        }
        SpellChecker::instance().decorateLine(*doc, line, d, skipCol);
    });
    connect(&SpellChecker::instance(), &SpellChecker::dictionaryChanged, this, [this] { m_editor->refreshDecorations(); });
    connect(m_editor, &EditorView::selectionChanged, this, [this] {
        if (m_typing && m_editor->cursorPosition() != m_typingPos) {
            m_typing = false;
            if (Document* doc = m_editor->document()) {
                const int line = doc->text().lineAt(std::min<qsizetype>(m_typingPos, doc->text().length()));
                m_editor->refreshDecorations(line, line + 1);
            }
        }
    });
    connect(m_editor, &EditorView::contextMenuAboutToShow, this, &MarkdownView::addSpellingActions);
    connect(m_editor, &EditorView::contextMenuAboutToShow, this, [this](QMenu* menu, qsizetype pos) {
        QList<MenuEntry> items; // Obsidian's "editor-menu" event
        EditorMenuHooks::collect(items, this, pos);
        if (!items.isEmpty())
            buildSectionedMenu(menu, items);
    });
    attachments::installHooks(); // paste/drop: HTML -> Markdown, URL over selection, attachments
}

void MarkdownView::addSpellingActions(QMenu* menu, qsizetype pos)
{
    Document* doc = m_editor->document();
    SpellChecker& sc = SpellChecker::instance();
    if (!m_spellcheck || !doc || mode() == EditorView::Mode::Reading || !sc.available())
        return;
    const Text& t = doc->text();
    const int line = t.lineAt(pos);
    int from = 0, to = 0;
    if (!SpellChecker::wordAt(t.lineView(line), int(pos - t.lineStart(line)), &from, &to))
        return;
    LineDecoration d;
    sc.decorateLine(*doc, line, d);
    const bool flagged = std::any_of(d.marks.cbegin(), d.marks.cend(), [&](const Mark& m) {
        return m.style == StyleId::SpellError && m.from < to && m.to > from;
    });
    if (!flagged)
        return;
    const QString word = t.line(line).mid(from, to - from);
    const qsizetype base = t.lineStart(line);
    QList<QAction*> actions;
    const QStringList suggestions = sc.suggestions(word, 6);
    for (const QString& s : suggestions) {
        auto* a = new QAction(s, menu);
        connect(a, &QAction::triggered, this, [this, base, from, to, s, word] {
            Document* d = m_editor->document();
            if (!d || d->text().slice(base + from, base + to) != word)
                return;
            Transaction tr;
            tr.changes = ChangeSet::of(ChangeSpec{base + from, base + to, s}, d->text().length());
            tr.selection = Selection::cursor(base + from + s.size());
            tr.userEvent = QStringLiteral("input.spellcheck");
            m_editor->dispatch(tr);
        });
        actions << a;
    }
    if (suggestions.isEmpty()) {
        auto* none = new QAction(tr("No suggestions..."), menu);
        none->setEnabled(false);
        actions << none;
    }
    auto* add = new QAction(tr("Add to dictionary"), menu);
    connect(add, &QAction::triggered, this, [word] { SpellChecker::instance().addToDictionary(word); });
    actions << add;
    auto* sep = new QAction(menu);
    sep->setSeparator(true);
    actions << sep;
    QAction* first = menu->actions().value(0);
    menu->insertActions(first, actions);
}

MarkdownView::~MarkdownView()
{
    saveFolds();
    m_editor->setDocument(nullptr);
    m_doc.reset();
}

bool MarkdownView::canAcceptExtension(const QString& extension) const { return extension.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0; }

Document* MarkdownView::document() const { return m_doc.get(); }

void MarkdownView::applySettings()
{
    VaultSession* s = session();
    VaultConfig* cfg = s ? s->vaultConfig() : nullptr;
    if (!cfg)
        return;
    m_editor->setReadableLineLength(cfg->flag(QStringLiteral("readableLineLength")));
    m_editor->setShowLineNumbers(cfg->flag(QStringLiteral("showLineNumber")));
    FoldOptions fo;
    fo.headings = cfg->flag(QStringLiteral("foldHeading"));
    fo.indent = cfg->flag(QStringLiteral("foldIndent"));
    fo.tabSize = cfg->tabSize();
    m_editor->setFoldOptions(fo);
    const bool showTitle = cfg->flag(QStringLiteral("showInlineTitle"));
    m_title->setVisible(showTitle);
    static_cast<DocumentHeader*>(m_header)->relayout();
    m_editor->updateTopWidget();
    const bool spell = cfg->flag(QStringLiteral("spellcheck"));
    if (spell != m_spellcheck) {
        m_spellcheck = spell;
        m_editor->refreshDecorations();
    }
    setVimMode(cfg->flag(QStringLiteral("vimMode")));
}

void MarkdownView::setVimMode(bool on)
{
    if (on == (m_vim != nullptr))
        return;
    if (!on) {
        delete std::exchange(m_vim, nullptr);
        return;
    }
    m_vim = new VimController(m_editor, this);
    // ":w" / ":q" run Obsidian's commands, after the key that typed them (":q" closes this view)
    m_vim->setCommandRunner([this](const QString& id) {
        CommandRegistry* commands = session() ? session()->commands() : nullptr;
        if (!commands || !commands->find(id))
            return false;
        QMetaObject::invokeMethod(commands, [commands, id] { commands->execute(id); }, Qt::QueuedConnection);
        return true;
    });
}

void MarkdownView::loadFile(TFile* file)
{
    if (file == m_file && m_doc)
        return;
    saveFolds();
    m_foldSave.stop();
    m_editor->setDocument(nullptr);
    m_suggest->documentChanged();
    disconnect(m_propsConn);
    m_doc.reset();
    m_file = file;
    if (!m_backlinksDefaulted) {
        // a new view: the Backlinks option "Show backlinks at the bottom of notes"
        m_backlinksDefaulted = true;
        ConfigStore* c = session() ? session()->config() : nullptr;
        if (c && c->value(QStringLiteral("backlink"), QStringLiteral("backlinkInDocument")).toBool())
            setShowBacklinks(true);
    }
    updateBacklinksFooter();
    if (VaultSession* s = session(); s && s->vaultConfig() && !m_configConn) {
        static const char* keys[] = {"readableLineLength", "showLineNumber", "foldHeading", "foldIndent", "tabSize", "showInlineTitle", "spellcheck", "vimMode"};
        m_configConn = connect(s->vaultConfig(), &VaultConfig::configChanged, this, [this](const QString& key) {
            for (const char* k : keys)
                if (key == QLatin1String(k))
                    applySettings();
            if (key == u"propertiesInDocument")
                refreshProperties(false);
        });
    }
    applySettings();
    if (MetadataCache* mc = session() ? session()->metadata() : nullptr; mc && !m_resolvedConn) {
        // links may have become (un)resolved: re-decorate the visible lines
        m_resolvedConn = connect(mc, &MetadataCache::resolved, this, [this] { m_editor->refreshDecorations(); });
    }
    if (!m_previewAttached) {
        // Page preview: Mod+hover (editing) / hover (reading) over links
        Workspace* ws = workspace();
        if (PagePreview* pp = PagePreview::of(ws ? ws->window() : nullptr)) {
            m_previewAttached = true;
            pp->attachEditor(m_editor, [this] { return m_file ? m_file->path() : QString(); },
                             [this] { return mode() == EditorView::Mode::Reading ? QStringLiteral("preview") : QStringLiteral("editor"); });
        }
    }
    if (file) {
        DocumentManager* dm = session() ? session()->documents() : nullptr;
        QString err;
        Document* d = dm ? dm->acquire(file->path(), &err) : nullptr;
        if (d) {
            QPointer<DocumentManager> guard = dm;
            m_doc = std::shared_ptr<Document>(d, [guard](Document* doc) {
                if (guard)
                    guard->release(doc);
            });
            m_editor->setDocument(m_doc);
            m_suggest->documentChanged();
            if (m_vim)
                m_vim->documentChanged();
            disconnect(m_typingConn);
            m_typing = false;
            m_typingConn = connect(d, &Document::changed, this, [this](const DocUpdate& u) {
                if (u.origin == m_editor && u.tr.isUserEvent(QStringLiteral("input.type"))) {
                    m_typing = true;
                    m_typingPos = m_editor->cursorPosition();
                    if (u.tr.selection)
                        m_typingPos = u.tr.selection->main().head;
                }
            });
            m_propsConn = connect(d, &Document::changed, this, [this](const DocUpdate& u) {
                // only changes at the top of the note can touch the frontmatter
                int first = INT_MAX;
                u.tr.changes.iterChangedRanges([&](qsizetype fa, qsizetype, qsizetype, qsizetype) { first = std::min(first, u.before.lineAt(fa)); });
                const bool mayStart = u.after.lineCount() > 1 && u.after.lineView(0).startsWith(u"---");
                if (first <= std::max(m_frontmatterClose + 1, 1) || (m_frontmatterClose < 0 && mayStart))
                    refreshProperties(false);
            });
            m_frontmatterSynced = false;
            refreshProperties(true);
            restoreFolds();
        } else if (NoticeManager* n = noticesFor(this)) {
            n->show(tr("Failed to open \"%1\": %2").arg(file->path(), err.isEmpty() ? tr("no document manager") : err));
        }
    }
    if (!m_doc)
        refreshProperties(true);
    updateTitle();
    emit titleChanged();
}

// ---------------------------------------------------------------------------------------------
// properties
// ---------------------------------------------------------------------------------------------
void MarkdownView::refreshProperties(bool force)
{
    VaultConfig* cfg = session() ? session()->vaultConfig() : nullptr;
    const QString setting = cfg ? cfg->string(QStringLiteral("propertiesInDocument")) : QStringLiteral("visible");
    const EditorView::Mode m = mode();
    if (!m_doc) {
        m_frontmatterRaw.clear();
        m_frontmatterClose = -1;
        m_frontmatterValid = false;
        m_frontmatterSynced = false;
        m_frontmatterEnd = -1;
        m_properties->synchronize(props::FrontmatterEditor::State::None, {});
    } else {
        const Text& t = m_doc->text();
        const props::FrontmatterSpan span = props::findFrontmatter(t);
        const QString raw = span.exists ? t.slice(0, t.lineEnd(span.closeLine)) : QString();
        if (force || !m_frontmatterSynced || raw != m_frontmatterRaw) {
            const props::FrontmatterEditor ed(t);
            m_properties->synchronize(ed.state(), ed.properties());
            m_frontmatterRaw = raw;
            m_frontmatterClose = span.exists ? span.closeLine : -1;
            m_frontmatterValid = ed.state() != props::FrontmatterEditor::State::Invalid;
            m_frontmatterSynced = true;
        }
    }
    // Obsidian: valid frontmatter is replaced by the widget in Live Preview (unless "source") and never
    // shown as text in Reading view; invalid YAML stays visible as source.
    const bool hide = m_frontmatterClose >= 0 && m_frontmatterValid
                      && ((m == EditorView::Mode::LivePreview && setting != u"source") || m == EditorView::Mode::Reading);
    const int end = hide ? m_frontmatterClose : -1;
    if (end != m_frontmatterEnd) {
        const int last = std::max(end, m_frontmatterEnd);
        m_frontmatterEnd = end;
        m_editor->refreshDecorations(0, last + 1);
    }
    const bool normal = (m == EditorView::Mode::LivePreview && setting == u"visible") || (m == EditorView::Mode::Reading && setting != u"hidden");
    m_properties->setDisplay(normal ? PropertiesWidget::Display::Normal : PropertiesWidget::Display::ErrorOnly);
    // Reading view shows the properties read-only (like the inline title there).
    m_properties->setReadOnly(m == EditorView::Mode::Reading || !m_doc || m_doc->isReadOnly());
    static_cast<DocumentHeader*>(m_header)->relayout();
    m_editor->updateTopWidget();
    clampSelectionToBody();
}

bool MarkdownView::canShowProperties() const
{
    VaultConfig* cfg = session() ? session()->vaultConfig() : nullptr;
    const QString setting = cfg ? cfg->string(QStringLiteral("propertiesInDocument")) : QStringLiteral("visible");
    return m_doc && mode() == EditorView::Mode::LivePreview && setting == u"visible" && !m_doc->isReadOnly();
}

void MarkdownView::addProperty(const QString& key)
{
    if (!canShowProperties())
        return;
    m_editor->setScrollTop(-1e9); // the properties are at the top
    m_properties->addProperty(key);
}

bool MarkdownView::editFrontmatter(const std::function<void(props::FrontmatterEditor&)>& edit)
{
    if (!m_doc || m_doc->isReadOnly())
        return false;
    props::FrontmatterEditor ed(m_doc->text());
    if (!ed.isEditable()) {
        if (NoticeManager* n = noticesFor(this))
            n->show(tr("Failed to read properties."));
        return false;
    }
    edit(ed);
    const QList<ChangeSpec> specs = ed.changes();
    if (specs.isEmpty())
        return true;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, m_doc->text().length());
    tr.userEvent = QStringLiteral("input.properties"); // one undo step per property edit
    tr.scrollIntoView = false;
    m_doc->dispatch(tr, m_editor, m_editor->selection());
    return true;
}

void MarkdownView::clampSelectionToBody()
{
    // Obsidian keeps the cursor out of the frontmatter the properties replace (Live Preview).
    if (m_clamping || m_frontmatterEnd < 0 || !m_doc || mode() != EditorView::Mode::LivePreview)
        return;
    const Text& t = m_doc->text();
    if (m_frontmatterEnd >= t.lineCount())
        return;
    const qsizetype limit = t.lineEnd(m_frontmatterEnd);
    const Selection sel = m_editor->selection();
    bool inside = false;
    for (const SelRange& r : sel.ranges())
        inside = inside || r.from() <= limit;
    if (!inside)
        return;
    m_clamping = true;
    if (m_frontmatterEnd + 1 >= t.lineCount()) {
        // No line after the frontmatter: like Obsidian, add one once the user is in the editor.
        if (m_editor->hasFocus() && !m_editor->isReadOnly()) {
            Transaction tr;
            tr.changes = ChangeSet::of(ChangeSpec{t.length(), t.length(), QStringLiteral("\n")}, t.length());
            tr.selection = Selection::cursor(t.length() + 1);
            tr.userEvent = QStringLiteral("input");
            m_editor->dispatch(tr);
        }
        m_clamping = false;
        return;
    }
    const qsizetype body = t.lineStart(m_frontmatterEnd + 1);
    QList<SelRange> ranges;
    for (const SelRange& r : sel.ranges())
        ranges << SelRange(std::max(r.anchor, body), std::max(r.head, body));
    m_editor->setSelection(Selection::create(ranges, sel.mainIndex()), false, false);
    m_clamping = false;
}

bool MarkdownView::eventFilter(QObject* obj, QEvent* ev)
{
    // ↑ on the first line of the text goes to the properties (their last row / "Add property"),
    // else to the inline title (Obsidian's shiftFocusBefore).
    if (obj == m_editor && ev->type() == QEvent::KeyPress && m_doc && mode() != EditorView::Mode::Reading
        && !m_editor->property("ocSuggestActive").toBool()) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Up && (ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            const Selection& sel = m_editor->selection();
            const Text& t = m_doc->text();
            const int first = m_frontmatterEnd >= 0 ? m_frontmatterEnd + 1 : 0;
            if (sel.size() == 1 && sel.main().empty() && first < t.lineCount() && t.lineAt(sel.main().head) == first
                && std::abs(m_editor->caretRectAt(sel.main().head).top() - m_editor->caretRectAt(t.lineStart(first)).top()) < 1) {
                if (m_properties->isVisible() && !m_properties->isInvalid() && m_properties->focus(true))
                    return true;
                if (m_title->isVisible()) {
                    m_title->beginRename(false);
                    return true;
                }
            }
        }
    }
    return FileView::eventFilter(obj, ev);
}

FoldStore* MarkdownView::foldStore() const
{
    // one per vault session (created by the application as a child of the session)
    VaultSession* s = session();
    return s ? s->findChild<FoldStore*>() : nullptr;
}

void MarkdownView::setShowBacklinks(bool on)
{
    m_backlinksDefaulted = true;
    if (on == m_showBacklinks && on == (m_backlinks != nullptr))
        return;
    m_showBacklinks = on;
    if (on && !m_backlinks) {
        m_backlinks = new BacklinkPane(this);
        m_backlinks->setObjectName(QStringLiteral("ocEmbeddedBacklinks"));
        m_backlinks->setEmbedded(true);
        connect(m_backlinks, &BacklinkPane::stateChanged, this, &View::stateChanged);
        updateBacklinksFooter();
    } else if (!on && m_backlinks) {
        m_editor->setFooterWidget(nullptr);
        m_backlinks->deleteLater();
        m_backlinks = nullptr;
    }
    emit stateChanged();
}

void MarkdownView::updateBacklinksFooter()
{
    if (!m_backlinks)
        return;
    m_backlinks->setFile(m_file ? m_file->path() : QString());
    // .embedded-backlinks: margin-top 3em in the reading view; after the text's own spacing when editing
    const int gap = mode() == EditorView::Mode::Reading ? 48 : 24;
    m_editor->setFooterWidget(m_backlinks, gap);
}

void MarkdownView::onRename(TFile* file, const QString& oldPath)
{
    if (FoldStore* fs = foldStore())
        fs->renamed(oldPath, file->path());
    updateTitle();
    updateBacklinksFooter();
    FileView::onRename(file, oldPath);
}

void MarkdownView::updateTitle() { m_title->setTitle(m_file ? m_file->basename() : QString()); }

void MarkdownView::onClose()
{
    saveFolds();
    m_foldSave.stop();
}

void MarkdownView::focusContent() { m_editor->setFocus(); }

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------
QJsonObject MarkdownView::state() const
{
    QJsonObject o = FileView::state();
    const EditorView::Mode m = m_editor->mode();
    o.insert(QStringLiteral("mode"), m == EditorView::Mode::Reading ? QStringLiteral("preview") : QStringLiteral("source"));
    o.insert(QStringLiteral("source"), m == EditorView::Mode::Source);
    o.insert(QStringLiteral("backlinks"), m_showBacklinks);
    if (m_backlinks)
        o.insert(QStringLiteral("backlinkOpts"), m_backlinks->state());
    return o;
}

void MarkdownView::setModeFromState(const QJsonObject& st)
{
    VaultConfig* cfg = session() ? session()->vaultConfig() : nullptr;
    QString mode = st.value(QStringLiteral("mode")).toString();
    if (mode.isEmpty())
        mode = cfg ? cfg->string(QStringLiteral("defaultViewMode")) : QStringLiteral("source");
    bool source = cfg ? !cfg->flag(QStringLiteral("livePreview")) : false;
    if (st.contains(QStringLiteral("source")))
        source = st.value(QStringLiteral("source")).toBool();
    if (mode == u"preview")
        setMode(EditorView::Mode::Reading);
    else
        setMode(source ? EditorView::Mode::Source : EditorView::Mode::LivePreview);
}

void MarkdownView::setState(const QJsonObject& st)
{
    FileView::setState(st);
    setModeFromState(st);
    if (st.contains(QStringLiteral("backlinks")))
        setShowBacklinks(st.value(QStringLiteral("backlinks")).toBool());
    if (m_backlinks && st.value(QStringLiteral("backlinkOpts")).isObject())
        m_backlinks->setState(st.value(QStringLiteral("backlinkOpts")).toObject());
}

QJsonObject MarkdownView::ephemeralState() const { return m_editor->ephemeralState(); }

void MarkdownView::setEphemeralState(const QJsonObject& e)
{
    if (!m_doc)
        return;
    if (e.contains(QStringLiteral("cursor")) || e.contains(QStringLiteral("scroll")))
        m_editor->setEphemeralState(e);
    if (e.contains(QStringLiteral("subpath")))
        navigateToSubpath(e.value(QStringLiteral("subpath")).toString());
    if (e.contains(QStringLiteral("line"))) {
        const Text& t = m_doc->text();
        const int line = std::clamp(e.value(QStringLiteral("line")).toInt(), 0, t.lineCount() - 1);
        m_editor->setCursorPosition(t.lineStart(line), false);
        m_editor->scrollToPos(t.lineStart(line), true);
    }
    if (e.contains(QStringLiteral("match"))) {
        // From the search view: {"content": ..., "matches": [[from, to], ...]} — select the first one.
        const QJsonArray matches = e.value(QStringLiteral("match")).toObject().value(QStringLiteral("matches")).toArray();
        if (!matches.isEmpty()) {
            const QJsonArray m = matches.first().toArray();
            const Text& t = m_doc->text();
            const qsizetype len = t.length();
            const qsizetype a = std::clamp<qsizetype>(m.at(0).toInteger(), 0, len), b = std::clamp<qsizetype>(m.at(1).toInteger(), 0, len);
            if (m_frontmatterEnd >= 0 && a <= t.lineEnd(m_frontmatterEnd)) {
                // a match in the frontmatter the properties replace: focus that property (Obsidian's propertyMatches)
                m_editor->setScrollTop(-1e9);
                const QString key = props::FrontmatterEditor(t).keyAtLine(t.lineAt(a));
                if (!key.isEmpty())
                    m_properties->focusProperty(key);
            } else {
                m_editor->setSelection(Selection::single(a, b), false);
                m_editor->scrollToPos(a, true);
            }
        }
    }
    if (e.contains(QStringLiteral("rename")) && m_title->isVisible() && mode() != EditorView::Mode::Reading)
        m_title->beginRename(e.value(QStringLiteral("rename")).toString() == u"all");
    else if (e.value(QStringLiteral("focus")).toBool())
        m_editor->setFocus();
}

// ---------------------------------------------------------------------------------------------
// modes, search, save
// ---------------------------------------------------------------------------------------------
void MarkdownView::setMode(EditorView::Mode mode)
{
    if (m_editor->mode() == mode)
        return;
    m_editor->setMode(mode);
}

void MarkdownView::toggleReading()
{
    if (mode() == EditorView::Mode::Reading) {
        VaultConfig* cfg = session() ? session()->vaultConfig() : nullptr;
        setMode(cfg && !cfg->flag(QStringLiteral("livePreview")) ? EditorView::Mode::Source : EditorView::Mode::LivePreview);
    } else {
        setMode(EditorView::Mode::Reading);
    }
}

void MarkdownView::toggleSource()
{
    if (mode() == EditorView::Mode::Reading)
        return;
    setMode(mode() == EditorView::Mode::Source ? EditorView::Mode::LivePreview : EditorView::Mode::Source);
}

void MarkdownView::showSearch(bool replace) { m_searchBar->open(replace && mode() != EditorView::Mode::Reading); }

bool MarkdownView::save() { return m_doc && m_doc->save(); }

QList<QWidget*> MarkdownView::headerActions()
{
    auto* b = new IconButton(QString());
    auto refresh = [this, b] {
        const bool reading = mode() == EditorView::Mode::Reading;
        b->setIconName(reading ? QStringLiteral("pencil") : QStringLiteral("book-open"));
        b->setToolTip((reading ? tr("Current view: reading") + QLatin1Char('\n') + tr("Click to edit")
                               : tr("Current view: editing") + QLatin1Char('\n') + tr("Click to read"))
                      + QLatin1Char('\n') + tr("%1+Click to open to the right").arg(QStringLiteral("Ctrl")));
    };
    refresh();
    connect(this, &MarkdownView::modeChanged, b, refresh);
    connect(b, &QAbstractButton::clicked, this, [this] {
        if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
            // Obsidian: open the other mode to the right
            if (Workspace* ws = workspace(); ws && leaf()) {
                if (WorkspaceLeaf* other = ws->duplicateLeaf(leaf(), Workspace::NewLeaf::SplitRight))
                    if (auto* mv = qobject_cast<MarkdownView*>(other->view()))
                        mv->toggleReading();
            }
            return;
        }
        toggleReading();
    });
    return {b};
}

void MarkdownView::paneMenuEntries(QList<MenuEntry>& items, const QString& source)
{
    // Obsidian's MarkdownView.onPaneMenu, after FileView's items.
    FileView::paneMenuEntries(items, source);
    QPointer<MarkdownView> self = this;
    const bool reading = mode() == EditorView::Mode::Reading;
    MenuEntry readingView{QStringLiteral("pane"), tr("Reading view"), QStringLiteral("book-open"), [self] {
                              if (self)
                                  self->toggleReading();
                          }};
    readingView.checkable = true;
    readingView.checked = reading;
    items << readingView;
    CommandRegistry* commands = session() ? session()->commands() : nullptr;
    MenuEntry addProperty{QStringLiteral("action"), tr("Add file property"), QStringLiteral("plus-circle"), [self, commands] {
                              if (self && self->leaf() && self->workspace())
                                  self->workspace()->setActiveLeaf(self->leaf(), true);
                              if (commands)
                                  commands->execute(QStringLiteral("markdown:add-metadata-property"));
                          }};
    addProperty.enabled = commands && commands->find(QStringLiteral("markdown:add-metadata-property"));
    items << addProperty;
    if (!reading) {
        MenuEntry sourceMode{QStringLiteral("pane"), tr("Source mode"), QStringLiteral("code-2"), [self] {
                                 if (self)
                                     self->toggleSource();
                             }};
        sourceMode.checkable = true;
        sourceMode.checked = mode() == EditorView::Mode::Source;
        items << sourceMode;
    }
    items << MenuEntry{QStringLiteral("find"), tr("Find..."), QStringLiteral("file-search"), [self] {
                           if (self)
                               self->showSearch(false);
                       }};
    MenuEntry replace{QStringLiteral("find"), tr("Replace..."), QStringLiteral("file-search"), [self] {
                          if (self)
                              self->showSearch(true);
                      }};
    replace.enabled = !reading;
    items << replace;
    items << MenuEntry{QStringLiteral("action"), tr("Export to PDF..."), QStringLiteral("file-down"), [self] {
                           if (self)
                               openExportPdfModal(qobject_cast<MainWindow*>(self->window()), self);
                       }};
}

// ---------------------------------------------------------------------------------------------
// links and navigation
// ---------------------------------------------------------------------------------------------
void MarkdownView::openLink(const QString& target, int kind, bool newLeaf)
{
    enum { WikiLink = 0, Embed = 1, MdLink = 2, Url = 3, Tag = 4 };
    if (kind == Tag) {
        if (g_tagHandler)
            g_tagHandler(target, this);
        return;
    }
    const QString t = kind == MdLink ? QUrl::fromPercentEncoding(target.toUtf8()) : target;
    if (kind != Url && LinkResolver::splitSubpath(t).first.isEmpty()) {
        navigateToSubpath(LinkResolver::splitSubpath(t).second);
        return;
    }
    openLinkText(workspace(), target, kind, m_file ? m_file->path() : QString(), newLeaf);
}

void MarkdownView::openLinkText(Workspace* ws, const QString& target, int kind, const QString& source, bool newLeaf)
{
    openLinkText(ws, target, kind, source, newLeaf ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No);
}

void MarkdownView::openLinkText(Workspace* ws, const QString& target, int kind, const QString& source, Workspace::NewLeaf mode)
{
    enum { WikiLink = 0, Embed = 1, MdLink = 2, Url = 3, Tag = 4 };
    QString t = target;
    if (kind == MdLink)
        t = QUrl::fromPercentEncoding(t.toUtf8());
    const bool external = kind == Url || QRegularExpression(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:")).match(t).hasMatch();
    if (external) {
        QDesktopServices::openUrl(QUrl(target));
        return;
    }
    VaultSession* s = ws ? ws->session() : nullptr;
    if (!s || !s->vault())
        return;
    auto [linkpath, subpath] = LinkResolver::splitSubpath(t);
    if (linkpath.isEmpty())
        linkpath = source; // "#Heading" of the source note
    MetadataCache* mc = s->metadata();
    QString dest = mc ? mc->resolver().resolve(linkpath, source, kind == MdLink) : QString();
    TFile* file = dest.isEmpty() ? nullptr : s->vault()->file(dest);
    if (!file) {
        // Unresolved: Obsidian creates the note (in the new-note folder unless the link has a path).
        FileManager* fm = s->fileManager();
        QString path = linkpath;
        if (!path.contains(QLatin1Char('.')) || !path.section(QLatin1Char('/'), -1).contains(QLatin1Char('.')))
            path += QStringLiteral(".md");
        if (!path.contains(QLatin1Char('/')) && fm)
            if (TFolder* parent = fm->newFileParent(source); parent && !parent->isRoot())
                path = parent->path() + QLatin1Char('/') + path;
        QString err;
        file = s->vault()->file(path);
        if (!file)
            file = s->vault()->createFile(path, QByteArray(), &err);
        if (!file) {
            if (NoticeManager* n = ws->window() ? ws->window()->notices() : nullptr)
                n->show(tr("Failed to create \"%1\": %2").arg(path, err));
            return;
        }
    }
    QJsonObject eState;
    if (!subpath.isEmpty())
        eState.insert(QStringLiteral("subpath"), subpath);
    ws->openFile(file, mode, eState);
}

void MarkdownView::navigateToSubpath(const QString& subpath)
{
    if (!m_doc || subpath.isEmpty())
        return;
    const Text& t = m_doc->text();
    const md::SyntaxTree& tree = m_doc->syntax();
    int line = -1;
    QStringList parts = subpath.split(QLatin1Char('#'), Qt::SkipEmptyParts);
    if (parts.size() == 1 && parts.first().startsWith(QLatin1Char('^'))) {
        // block id: "text ^id" at the end of a line
        const QString id = QLatin1Char('^') + parts.first().mid(1).toLower();
        for (int l = 0; l < t.lineCount() && line < 0; ++l) {
            const QString s = t.line(l).trimmed().toLower();
            if (s.endsWith(id) && (s.size() == id.size() || s.at(s.size() - id.size() - 1).isSpace()))
                line = l;
        }
    } else if (!parts.isEmpty()) {
        // nested heading path "A#B": each part matches a deeper heading after the previous one
        m_doc->ensureParsedThrough(t.lineCount() - 1);
        int depth = 0;
        qsizetype part = 0;
        for (int id : tree.headings()) {
            const md::BlockNode& n = tree.node(id);
            const md::HeadingInfo h = tree.heading(id);
            const QString text = t.lineView(n.fromLine).mid(h.textFrom, h.textTo - h.textFrom).toString();
            if (h.level > depth && normalizeHeading(text) == normalizeHeading(parts.at(part))) {
                depth = h.level;
                if (++part == parts.size()) {
                    line = n.fromLine;
                    break;
                }
            }
        }
    }
    if (line < 0)
        return;
    m_editor->setCursorPosition(t.lineStart(line), false);
    // Obsidian brings the target to the top of the view
    m_editor->layout().layoutFor(line);
    m_editor->setScrollTop(std::max(0.0, m_editor->layout().lineTop(line) - 8));
}

void MarkdownView::renameFromTitle(const QString& raw)
{
    VaultSession* s = session();
    if (!m_file || !s || !s->vault())
        return;
    NoticeManager* notices = noticesFor(this);
    const QString name = raw.trimmed();
    static const QString forbidden = QStringLiteral("*\"\\/<>:|?");
    for (QChar c : name) {
        if (forbidden.contains(c)) {
            if (notices)
                notices->show(tr("File name cannot contain any of these characters: ") + QStringLiteral("* \" \\ / < > : | ?"));
            updateTitle();
            return;
        }
    }
    if (name.isEmpty() || name.startsWith(QLatin1Char('.'))) {
        if (notices)
            notices->show(tr("File name cannot be empty."));
        updateTitle();
        return;
    }
    const QString folder = m_file->parent() && !m_file->parent()->isRoot() ? m_file->parent()->path() + QLatin1Char('/') : QString();
    const QString newPath = folder + name + QLatin1Char('.') + m_file->extension();
    if (newPath == m_file->path())
        return;
    if (s->vault()->item(newPath) && newPath.compare(m_file->path(), Qt::CaseInsensitive) != 0) {
        if (notices)
            notices->show(tr("There's already a file with the same name"));
        updateTitle();
        return;
    }
    QString err;
    const bool ok = s->fileManager() ? s->fileManager()->renameFile(m_file, newPath, &err) : s->vault()->rename(m_file, newPath, &err);
    if (!ok) {
        if (notices)
            notices->show(err);
        updateTitle();
    }
}

// ---------------------------------------------------------------------------------------------
// folds
// ---------------------------------------------------------------------------------------------
void MarkdownView::saveFolds()
{
    FoldStore* store = foldStore();
    if (!store || !m_file || !m_doc)
        return;
    QList<QPair<int, int>> folds;
    for (const FoldRange& f : m_editor->folds())
        folds.append({f.line, f.lastLine});
    store->setFolds(m_file->path(), folds, m_doc->text().lineCount());
}

void MarkdownView::restoreFolds()
{
    FoldStore* store = foldStore();
    if (!store || !m_file || !m_doc)
        return;
    const QList<QPair<int, int>> saved = store->folds(m_file->path(), m_doc->text().lineCount());
    if (saved.isEmpty())
        return;
    QList<FoldRange> folds;
    for (const auto& [a, b] : saved)
        folds.append(FoldRange{a, b});
    m_restoringFolds = true;
    m_editor->setFolds(folds);
    m_restoringFolds = false;
}

void registerMarkdownView(ViewRegistry& registry)
{
    registry.registerView(QStringLiteral("markdown"), [](WorkspaceLeaf* leaf) -> View* { return new MarkdownView(leaf); });
    registry.registerExtensions({QStringLiteral("md")}, QStringLiteral("markdown"));
}

} // namespace oc
