#include "ui/views/markdown/markdown_view.h"

#include "ui/views/markdown/attachments.h"
#include "ui/views/markdown/editor_suggest.h"
#include "ui/views/markdown/page_preview.h"

#include "editor/live_preview_decorator.h"
#include "editor/spell/spell_checker.h"

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
#include "ui/shell/main_window.h"
#include "ui/views/markdown/editor_search_bar.h"
#include "ui/views/markdown/inline_title.h"
#include "ui/widgets/icon_button.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
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
} // namespace

void MarkdownView::setTagHandler(std::function<void(const QString&, MarkdownView*)> handler) { g_tagHandler = std::move(handler); }

MarkdownView::MarkdownView(WorkspaceLeaf* leaf, QWidget* parent)
    : FileView(leaf, parent)
    , m_editor(new EditorView(this))
    , m_searchBar(new EditorSearchBar(m_editor, this))
    , m_title(new InlineTitle)
    , m_suggest(new EditorSuggest(this, m_editor))
{
    setObjectName(QStringLiteral("ocMarkdownView"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(m_searchBar);
    v->addWidget(m_editor, 1);
    m_editor->setTopWidget(m_title, m_title->bottomMargin());
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
        connect(tm, &ThemeManager::themeChanged, this, [this] { m_editor->setTopWidget(m_title, m_title->bottomMargin()); });

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
    FoldOptions fo;
    fo.headings = cfg->flag(QStringLiteral("foldHeading"));
    fo.indent = cfg->flag(QStringLiteral("foldIndent"));
    fo.tabSize = cfg->tabSize();
    m_editor->setFoldOptions(fo);
    const bool showTitle = cfg->flag(QStringLiteral("showInlineTitle"));
    m_title->setVisible(showTitle);
    m_editor->updateTopWidget();
    const bool spell = cfg->flag(QStringLiteral("spellcheck"));
    if (spell != m_spellcheck) {
        m_spellcheck = spell;
        m_editor->refreshDecorations();
    }
}

void MarkdownView::loadFile(TFile* file)
{
    if (file == m_file && m_doc)
        return;
    saveFolds();
    m_foldSave.stop();
    m_editor->setDocument(nullptr);
    m_suggest->documentChanged();
    m_doc.reset();
    m_file = file;
    if (VaultSession* s = session(); s && s->vaultConfig() && !m_configConn) {
        static const char* keys[] = {"readableLineLength", "foldHeading", "foldIndent", "tabSize", "showInlineTitle", "spellcheck"};
        m_configConn = connect(s->vaultConfig(), &VaultConfig::configChanged, this, [this](const QString& key) {
            for (const char* k : keys)
                if (key == QLatin1String(k))
                    applySettings();
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
            restoreFolds();
        } else if (NoticeManager* n = noticesFor(this)) {
            n->show(tr("Failed to open \"%1\": %2").arg(file->path(), err.isEmpty() ? tr("no document manager") : err));
        }
    }
    updateTitle();
    emit titleChanged();
}

FoldStore* MarkdownView::foldStore() const
{
    // one per vault session (created by the application as a child of the session)
    VaultSession* s = session();
    return s ? s->findChild<FoldStore*>() : nullptr;
}

void MarkdownView::onRename(TFile* file, const QString& oldPath)
{
    if (FoldStore* fs = foldStore())
        fs->renamed(oldPath, file->path());
    updateTitle();
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
            const qsizetype len = m_doc->text().length();
            const qsizetype a = std::clamp<qsizetype>(m.at(0).toInteger(), 0, len), b = std::clamp<qsizetype>(m.at(1).toInteger(), 0, len);
            m_editor->setSelection(Selection::single(a, b), false);
            m_editor->scrollToPos(a, true);
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

void MarkdownView::populatePaneMenu(QMenu* menu)
{
    menu->addAction(tr("Find..."), this, [this] { showSearch(false); })->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    if (mode() != EditorView::Mode::Reading)
        menu->addAction(tr("Replace..."), this, [this] { showSearch(true); })->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    menu->addSeparator();
    QAction* reading = menu->addAction(tr("Reading view"), this, &MarkdownView::toggleReading);
    reading->setCheckable(true);
    reading->setChecked(mode() == EditorView::Mode::Reading);
    if (mode() != EditorView::Mode::Reading) {
        QAction* source = menu->addAction(tr("Source mode"), this, &MarkdownView::toggleSource);
        source->setCheckable(true);
        source->setChecked(mode() == EditorView::Mode::Source);
    }
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
    ws->openFile(file, newLeaf ? Workspace::NewLeaf::Tab : Workspace::NewLeaf::No, eState);
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
