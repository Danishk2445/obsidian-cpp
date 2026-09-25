#include "app/application.h"

#include "app/scenario_runner.h"

#include "core/commands/command_registry.h"
#include "core/config/clone_paths.h"
#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/config/vault_registry.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/documents/fold_store.h"
#include "core/documents/local_storage.h"
#include "core/files/file_manager.h"
#include "core/fuzzy/fuzzy.h"
#include "core/metadata/metadata_cache.h"
#include "core/recovery/file_recovery.h"
#include "core/recovery/snapshot_store.h"
#include "core/util/busy.h"
#include "core/util/word_count.h"
#include "core/vault/vault.h"
#include "core/vault_bindings.h"
#include "core/vault_session.h"
#include "editor/editor_view.h"
#include "editor/plugins/greentext_guard.h"
#include "render/theme/icons.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/overlay/suggest_modal.h"
#include "ui/graph/graph_view.h"
#include "ui/navigation/navigation.h"
#include "ui/navigation/search_view.h"
#include "ui/plugins/core_file_menu.h"
#include "ui/plugins/core_plugins.h"
#include "ui/plugins/file_recovery_ui.h"
#include "ui/plugins/note_composer_ui.h"
#include "ui/plugins/pdf_export_ui.h"
#include "ui/plugins/rename_subpath.h"
#include "ui/picker/vault_picker.h"
#include "ui/settings/settings_modal.h"
#include "ui/settings/settings_pages.h"
#include "ui/shell/app_commands.h"
#include "ui/shell/main_window.h"
#include "ui/plugins/note_plugins_ui.h"
#include "ui/views/file_explorer/file_actions.h"
#include "ui/views/file_explorer/file_explorer_view.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/views/markdown/markdown_commands.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/views/media/media_views.h"
#include "ui/views/sidebar/sidebar_registration.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/file_menu.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace oc {

Application::Application(CliOptions opts, QObject* parent)
    : QObject(parent)
    , m_opts(std::move(opts))
    , m_themes(new ThemeManager(this))
    , m_registry(new VaultRegistry(this))
{
    if (m_opts.deterministic) {
        QApplication::setCursorFlashTime(0);
        QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    }
}

Application::~Application() = default;

int Application::start()
{
    // Headless screenshots: the offscreen platform's cursor starts at (10, 10), over the ribbon's
    // sidebar toggle. Park it outside before any window is shown so nothing renders hovered.
    if ((!m_opts.screenshot.isEmpty() || !m_opts.script.isEmpty()) && QGuiApplication::platformName() == u"offscreen")
        QCursor::setPos(-10000, -10000);
    // Headless runs never hand files to other applications (PDF viewer, file manager).
    if (!m_opts.screenshot.isEmpty() || !m_opts.script.isEmpty() || QGuiApplication::platformName() == u"offscreen") {
        desktopHandlers().openUrl = [](const QUrl& url) { qInfo("oc: headless: not opening %s", qPrintable(url.toString())); };
        desktopHandlers().showInFolder = [](const QString& path) { qInfo("oc: headless: not showing %s", qPrintable(path)); };
        // No save dialogs either: exports go to the (isolated) cache dir.
        pdfSavePathProvider() = [](MainWindow*, const QString& suggested) {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/obsidian-cpp/exports"_s;
            QDir().mkpath(dir);
            qInfo("oc: headless: exporting to %s/%s", qPrintable(dir), qPrintable(suggested));
            return dir + u'/' + suggested;
        };
    }
    m_registry->load();
    if (!m_opts.theme.isEmpty())
        m_themes->setMode(ThemeManager::modeFromObsidianName(m_opts.theme));
    if (!m_opts.accent.isEmpty())
        m_themes->setAccent(QColor(m_opts.accent));
    m_themes->apply();

    if (!m_opts.edit.isEmpty())
        return startEditHarness();

    QString vault = m_opts.vault;
    if (vault.isEmpty() && !m_opts.picker) {
        for (const VaultEntry& e : m_registry->vaults()) {
            if (e.open && QFileInfo(e.path).isDir()) {
                vault = e.path;
                break;
            }
        }
    }
    if (!vault.isEmpty() && !m_opts.picker) {
        if (!openVault(vault))
            return 1;
        return 0;
    }
    showPicker();
    return 0;
}

int Application::startEditHarness()
{
    const QFileInfo fi(m_opts.edit);
    auto* storage = new LocalFileStorage(fi.absolutePath());
    auto doc = std::make_shared<Document>(storage, fi.fileName());
    QString err;
    if (!doc->load(&err)) {
        qCritical("oc: cannot open %s: %s", qPrintable(fi.absoluteFilePath()), qPrintable(err));
        return 1;
    }
    auto* view = new EditorView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->setWindowTitle(fi.fileName() + QStringLiteral(" - Obsidian C++ (editor harness)"));
    view->setDocument(doc);
    if (m_opts.mode == u"live")
        view->setMode(EditorView::Mode::LivePreview);
    else if (m_opts.mode == u"reading")
        view->setMode(EditorView::Mode::Reading);
    view->resize(m_opts.size.isValid() ? m_opts.size : QSize(1000, 800));
    connect(view, &QObject::destroyed, [storage, doc]() mutable {
        doc->save();
        doc.reset();
        delete storage;
    });
    view->show();
    view->setFocus();
    if (!m_opts.open.isEmpty()) {
        // --open :line[:col] positions the caret
        const QStringList parts = m_opts.open.split(QLatin1Char(':'));
        const int line = std::clamp(parts.value(1).toInt() - 1, 0, doc->text().lineCount() - 1);
        const int col = parts.value(2).toInt();
        view->setCursorPosition(doc->text().lineStart(line) + std::min(col, doc->text().lineLength(line)));
        view->scrollToPos(view->cursorPosition(), true);
    }
    if (!m_opts.screenshot.isEmpty())
        finishHeadless(view);
    return 0;
}

void Application::showPicker()
{
    if (!m_picker) {
        m_picker = new VaultPicker(m_registry);
        m_picker->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_picker, &VaultPicker::openVault, this, [this](const QString& path) {
            if (openVault(path) && m_picker)
                m_picker->close();
        });
    }
    m_picker->show();
    m_picker->raise();
    if (!m_opts.screenshot.isEmpty())
        finishHeadless(m_picker);
}

SuggestModal* Application::openVaultSwitcher(MainWindow* from, bool switchVault)
{
    if (!from)
        return nullptr;
    struct Item {
        QString name, path;
        bool current;
    };
    QList<Item> items;
    const QString currentPath = from->session() ? from->session()->root() : QString();
    for (const VaultEntry& e : m_registry->vaults())
        items.push_back({QFileInfo(e.path).fileName(), e.path, e.path == currentPath});
    auto* m = new SuggestModal(from->overlay());
    m->setPlaceholder(tr("Change vault..."));
    m->setInstructions({{QStringLiteral("↑↓"), tr("to navigate")}, {QStringLiteral("↵"), tr("to use")}, {QStringLiteral("esc"), tr("to dismiss")}});
    m->setProvider([items](const QString& query) {
        QList<std::pair<double, Suggestion>> scored;
        const FuzzyQuery q(query);
        for (const Item& it : items) {
            const std::optional<FuzzyResult> r = q.match(it.name);
            if (!r)
                continue;
            Suggestion s;
            s.title = it.name;
            for (int i : r->matches)
                s.titleMatches.push_back({i, i + 1});
            if (it.current)
                s.auxText = tr("Active"); // Obsidian's "flair mod-pop"
            s.data = it.path;
            scored.push_back({r->score, s});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        QList<Suggestion> out;
        for (const auto& [score, s] : scored)
            out.push_back(s);
        return out;
    });
    QPointer<MainWindow> w = from;
    connect(m, &SuggestModal::chosen, this, [this, w, switchVault, currentPath](const Suggestion& s, Qt::KeyboardModifiers) {
        const QString path = s.data.toString();
        if (path.isEmpty() || path == currentPath)
            return;
        MainWindow* opened = openVault(path);
        if (!opened) {
            if (w && w->notices())
                w->notices()->show(tr("Failed to open."));
            return;
        }
        if (switchVault && w && opened != w)
            w->close();
    });
    m->open();
    return m;
}

void Application::applyAppearance(VaultSession* session)
{
    const QJsonObject a = session->config()->object(QStringLiteral("appearance"));
    FontSettings fonts;
    // Obsidian stores comma-separated lists ("Inter, Arial"): the first installed family applies.
    fonts.interfaceFamily = resolveFontList(a.value(QStringLiteral("interfaceFontFamily")).toString());
    fonts.textFamily = resolveFontList(a.value(QStringLiteral("textFontFamily")).toString());
    fonts.monospaceFamily = resolveFontList(a.value(QStringLiteral("monospaceFontFamily")).toString());
    fonts.baseFontSize = a.value(QStringLiteral("baseFontSize")).toDouble(16);
    if (m_opts.deterministic) {
        fonts.interfaceFamily = Theme::firstInstalled({QStringLiteral("Inter")}, fonts.interfaceFamily);
        fonts.textFamily = Theme::firstInstalled({QStringLiteral("Inter")}, fonts.textFamily);
    }
    m_themes->blockSignals(true);
    m_themes->setFonts(fonts);
    if (m_opts.theme.isEmpty())
        m_themes->setMode(ThemeManager::modeFromObsidianName(a.value(QStringLiteral("theme")).toString(QStringLiteral("system"))));
    if (m_opts.accent.isEmpty()) {
        const QString accent = a.value(QStringLiteral("accentColor")).toString();
        m_themes->setAccent(accent.isEmpty() ? QColor() : QColor(accent));
    }
    const css_snippets::Overrides snippets = css_snippets::collect(session->config()); // enabled CSS snippets' --vars
    m_themes->setOverrides(snippets.body, snippets.light, snippets.dark);
    m_themes->blockSignals(false);
    m_themes->apply();
}

MainWindow* Application::openVault(const QString& rawPath)
{
    const QString path = VaultRegistry::normalizePath(rawPath);
    if (!QFileInfo(path).isDir()) {
        if (m_opts.screenshot.isEmpty())
            QMessageBox::warning(nullptr, QStringLiteral("Obsidian C++"), tr("Folder not found:\n%1").arg(path));
        else
            qCritical("oc: vault folder not found: %s", qPrintable(path));
        return nullptr;
    }
    for (const auto& w : m_windows) {
        if (w && w->session()->root() == path) {
            w->show();
            w->raise();
            w->activateWindow();
            return w;
        }
    }
    QString id;
    if (m_opts.noRegister) {
        id = QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    } else {
        id = m_registry->touch(path, true);
        m_registry->save();
    }
    auto* session = new VaultSession(id, path);
    applyAppearance(session);
    createServices(session);

    bool frameless = m_registry->option(QStringLiteral("frame")).toString(QStringLiteral("hidden")) != u"native";
    if (m_opts.frame >= 0)
        frameless = m_opts.frame == 1;
    auto* w = new MainWindow(session, frameless);
    session->setParent(w);
    populateWindow(w);
    if (m_opts.size.isValid())
        w->resize(m_opts.size);
    else
        w->restoreWindowState();
    if (m_opts.collapse == u"left" || m_opts.collapse == u"both")
        w->setLeftCollapsed(true);
    if (m_opts.collapse == u"right" || m_opts.collapse == u"both")
        w->setRightCollapsed(true);
    if (m_opts.collapse == u"none") {
        w->setLeftCollapsed(false);
        w->setRightCollapsed(false);
    }
    connect(w, &MainWindow::closing, this, [this, id, session] {
        // Obsidian saves pending edits when the window closes.
        if (DocumentManager* dm = session->documents())
            dm->saveAll();
        session->config()->flush();
        if (!m_opts.noRegister) {
            // Keep "open" set so the vault reopens next launch (Obsidian behaviour) unless others remain.
            m_registry->save();
        }
    });
    m_windows.push_back(w);
    w->show();
    if (!m_opts.script.isEmpty())
        (new ScenarioRunner(w, m_opts.script, this))->start(); // takes its own screenshots
    else if (!m_opts.screenshot.isEmpty())
        finishHeadless(w);
    return w;
}

namespace {
// Owns a non-QObject service for the session's lifetime.
template <typename T> class Holder : public QObject {
public:
    Holder(T* p, QObject* parent) : QObject(parent), ptr(p) {}
    ~Holder() override { delete ptr; }
    T* ptr;
};
} // namespace

void Application::createServices(VaultSession* s)
{
    // Children are destroyed in creation order: the vault is parented last so that the document
    // manager (which saves on destruction) and the other services go before it.
    auto* vault = new Vault(s->root());
    auto* files = new FileManager(s, s);
    auto* commands = new CommandRegistry(s);
    auto* meta = new MetadataCache(s);
    const QString dataDir = ClonePaths::ensureDir(ClonePaths::vaultDataDir(s->id()));
    new FoldStore(dataDir + QStringLiteral("/folds.json"), s);
    auto* snapshots = new SnapshotStore(dataDir + QStringLiteral("/file-recovery.sqlite"));
    new Holder<SnapshotStore>(snapshots, s);
    QString err;
    if (!snapshots->open(&err))
        qWarning("oc: file recovery unavailable: %s", qPrintable(err));
    QPointer<Vault> v = vault;
    auto* recovery = new FileRecovery(snapshots, [v](const QString& path) -> std::optional<QByteArray> {
        return v ? v->read(path, nullptr, nullptr) : std::nullopt;
    }, s);
    {
        const QJsonObject o = s->config()->object(QStringLiteral("file-recovery"));
        FileRecovery::Options opts;
        opts.intervalMinutes = o.value(QStringLiteral("intervalMinutes")).toInt(5);
        opts.keepDays = o.value(QStringLiteral("keepDays")).toInt(7);
        recovery->setOptions(opts);
        recovery->startTimers();
    }
    auto* docs = new DocumentManager(vault, s);
    vault->setParent(s);
    s->setVault(vault);
    s->setDocuments(docs);
    s->setMetadata(meta);
    s->setFileManager(files);
    s->setCommands(commands);
    bindVaultServices(s, recovery);
    commands->loadHotkeys(s->config()->object(QStringLiteral("hotkeys")));

    // Greentext Guard (the user's plugin, built in): settings live in the clone's config.
    // Built-in ports of community plugins are on unless turned off in Settings → Community plugins.
    GreentextGuard::instance().setSettings(GreentextGuard::Settings::fromJson(s->config()->object(QStringLiteral("plugins/greentext-guard"))));
    if (s->config()->object(QStringLiteral("community-plugins")).value(QStringLiteral("greentext-guard")).toBool(true))
        GreentextGuard::instance().install();

    vault->load(); // synchronous scan; the metadata index runs in the background
}

void Application::populateWindow(MainWindow* w)
{
    VaultSession* session = w->session();

    Ribbon* r = w->ribbon();
    r->addAction(QStringLiteral("switcher:open"), QStringLiteral("file-search"), tr("Open quick switcher"));
    r->addAction(QStringLiteral("graph:open"), QStringLiteral("git-fork"), tr("Open graph view"));
    r->addAction(QStringLiteral("canvas:new-file"), QStringLiteral("layout-dashboard"), tr("Create new canvas"));
    r->addAction(QStringLiteral("daily-notes"), QStringLiteral("calendar-days"), tr("Open today's daily note"));
    r->addAction(QStringLiteral("insert-template"), QStringLiteral("files"), tr("Insert template"));
    r->addAction(QStringLiteral("command-palette:open"), QStringLiteral("terminal"), tr("Open command palette"));
    r->addAction(QStringLiteral("bases:new-file"), QStringLiteral("layout-list"), tr("Create new base"));
    r->addBottomAction(QStringLiteral("app:open-help"), QStringLiteral("circle-help"), tr("Help"));
    r->addBottomAction(QStringLiteral("app:open-settings"), QStringLiteral("settings"), tr("Settings"));

    // Views provided by the workspace module (empty is built in; sidebar placeholders until the real
    // views land). Other modules register theirs here too.
    auto* registry = new ViewRegistry;
    registerCoreViews(*registry);
    registerSidebarViews(*registry); // backlinks, outgoing links, tags, outline, bookmarks, all properties
    registerMarkdownView(*registry);
    registerFileExplorerView(*registry); // replaces the placeholder
    registerNavigationViews(*registry);  // replaces the "search" placeholder
    registerGraphViews(*registry);       // "graph" + "localgraph"
    registerMediaViews(*registry);       // image, audio, video, pdf views + their embeds
    connect(w, &QObject::destroyed, this, [registry] { delete registry; });
    session->setViewRegistry(registry);

    auto* workspace = new Workspace(session, registry, w);
    session->setWorkspace(workspace);
    // File recovery also snapshots notes when they are opened (Obsidian's "file-open").
    if (FileRecovery* fr = fileRecoveryOf(session))
        connect(workspace, &Workspace::fileOpened, fr, [fr](TFile* f) {
            if (f)
                fr->fileChanged(f->path());
        });
    w->setWorkspace(workspace);

    // Commands and hotkeys.
    CommandRegistry* commands = session->commands();
    registerAppCommands(*commands, w);
    registerMarkdownCommands(*commands, workspace);
    registerFileExplorerCommands(*commands, session);
    registerNotePluginCommands(*commands, w);
    registerNavigationCommands(*commands, session, w->overlay());
    registerGraphCommands(*commands, workspace);
    registerFileRecoveryCommands(*commands, w);
    registerNoteComposerCommands(*commands, w);
    registerSidebarCommands(*commands, w);
    registerSettingsCommands(*commands, w); // app:open-settings (Mod+,), also the ribbon's gear
    registerExportPdfCommand(*commands, w);  // workspace:export-pdf
    rename_subpath::registerCommands(*commands, w); // editor:rename-heading + editor menu "Rename this heading/block ID..."
    installCoreFileMenu();                   // Copy path, Open in default app, Rename..., linked views, ...
    SettingsController* settings = SettingsController::of(w);
    settings->setVaultRegistry(m_registry); // "Window frame style" is a registry option
    settings->applyOptions();               // ribbon visibility, editing behaviours, spellchecker
    connect(settings, &SettingsController::manageVaultsRequested, this, &Application::showPicker);
    {
        // Vaults: "Manage vaults" (the picker), "Change vault..." and "Open vault..." (the switcher).
        QPointer<MainWindow> win = w;
        auto vaultCommand = [](const char* id, const QString& name, std::function<void()> run) {
            Command c;
            c.id = QString::fromLatin1(id);
            c.name = name;
            c.icon = QStringLiteral("vault");
            c.callback = std::move(run);
            return c;
        };
        commands->add(vaultCommand("app:open-vault", tr("Manage vaults"), [this] { showPicker(); }));
        commands->add(vaultCommand("app:switch-vault", tr("Change vault..."), [this, win] {
            if (win)
                openVaultSwitcher(win, true);
        }));
        commands->add(vaultCommand("app:open-another-vault", tr("Open vault..."), [this, win] {
            if (win)
                openVaultSwitcher(win, false);
        }));
    }
    MarkdownView::setTagHandler([](const QString& tag, MarkdownView* from) { openGlobalSearchForTag(from->session(), tag); });
    {
        GraphViewHooks gh;
        gh.openSearch = [](Workspace* ws, const QString& query) { openGlobalSearch(ws->session(), query); };
        gh.deleteFile = [](VaultSession* s, TFile* f) { promptForDeletion(s, f); };
        gh.fileMenu = [](QMenu* menu, TFile* file, WorkspaceLeaf* leaf) {
            QList<MenuEntry> items;
            FileMenuHooks::collect(items, file, QStringLiteral("graph-context-menu"), leaf, leaf ? leaf->view() : nullptr);
            buildSectionedMenu(menu, items);
        };
        setGraphViewHooks(std::move(gh));
    }
    {
        Command c;
        c.id = QStringLiteral("greentext-guard:wrap-selection-in-fence");
        c.name = tr("Greentext Guard: Wrap selection in a code fence");
        c.editorCheckCallback = [w](bool checking, EditorContext& ec) {
            auto* e = qobject_cast<EditorView*>(ec.editor);
            if (!e || e->isReadOnly())
                return false;
            if (!checking && !GreentextGuard::instance().wrapSelectionInFence(*e) && w)
                w->notices()->show(tr("Greentext Guard: nothing selected"));
            return true;
        };
        commands->add(c);
    }
    auto* hotkeys = new HotkeyDispatcher(commands, w);
    hotkeys->setWindow(w);
    qApp->installEventFilter(hotkeys);
    connect(r, &Ribbon::actionTriggered, commands, [commands](const QString& id) { commands->execute(id); });

    // File operations report through notices; link updates ask like Obsidian's "Update links" modal.
    if (FileManager* fm = session->fileManager()) {
        QPointer<MainWindow> win = w;
        connect(fm, &FileManager::notice, w, [win](const QString& msg) {
            if (win)
                win->notices()->show(msg);
        });
        fm->setConfirmLinkUpdate([win](int links, int filesCount) {
            return win ? promptLinkUpdate(win->overlay(), links, filesCount) : FileManager::LinkUpdateChoice::DoNotUpdate;
        });
    }

    workspace->loadLayout(); // after the ribbon actions exist (left-ribbon.hiddenItems)
    wireStatusBar(w);
    installCorePluginSwitches(w); // disabled core plugins: no commands, ribbon actions, status items, views

    // --open <note> [--mode ...]: open a note in the active tab (headless screenshots, CLI).
    if (!m_opts.open.isEmpty() && session->vault()) {
        TFile* f = session->vault()->file(m_opts.open);
        if (f && !f->isMarkdown()) {
            // images, PDFs, media, canvases, ...: the view registered for the extension
            workspace->openFile(f, Workspace::NewLeaf::No);
        } else if (f) {
            QJsonObject state{{QStringLiteral("file"), f->path()}};
            if (m_opts.mode == u"reading")
                state.insert(QStringLiteral("mode"), QStringLiteral("preview"));
            else if (m_opts.mode == u"source" || m_opts.mode == u"live") {
                state.insert(QStringLiteral("mode"), QStringLiteral("source"));
                state.insert(QStringLiteral("source"), m_opts.mode == u"source");
            }
            WorkspaceLeaf* leaf = workspace->getLeaf(Workspace::NewLeaf::No);
            leaf->setViewState(QJsonObject{{QStringLiteral("type"), QStringLiteral("markdown")}, {QStringLiteral("state"), state}});
            workspace->setActiveLeaf(leaf, true);
        } else {
            qWarning("oc: --open: no such file in the vault: %s", qPrintable(m_opts.open));
        }
    }
}

void Application::wireStatusBar(MainWindow* w)
{
    VaultSession* session = w->session();
    Workspace* workspace = w->workspace();
    StatusBar* sb = w->statusBar();
    sb->addItem(QStringLiteral("backlinks"), QString());
    sb->addItem(QStringLiteral("editor-status"), QString());
    sb->addItem(QStringLiteral("word-count"), QString());

    // Obsidian's word count: debounced (200 ms), frontmatter excluded, the selection when there is one.
    auto* timer = new QTimer(w);
    timer->setSingleShot(true);
    timer->setInterval(200);
    QPointer<MainWindow> win = w;
    auto activeMarkdown = [workspace]() -> MarkdownView* {
        WorkspaceLeaf* l = workspace ? workspace->activeRootLeaf() : nullptr;
        return l ? qobject_cast<MarkdownView*>(l->view()) : nullptr;
    };
    connect(timer, &QTimer::timeout, w, [sb, activeMarkdown] {
        MarkdownView* mv = activeMarkdown();
        Document* doc = mv ? mv->document() : nullptr;
        if (!doc) {
            sb->setItemVisible(QStringLiteral("word-count"), false);
            return;
        }
        const QString sel = mv->editor()->selectedText();
        const TextCounts c = sel.isEmpty() ? countNote(doc->text().toString()) : countSelection(sel);
        const QString words = c.words == 1 ? tr("%1 word").arg(c.words) : tr("%1 words").arg(QLocale().toString(c.words));
        const QString chars = c.characters == 1 ? tr("%1 character").arg(c.characters) : tr("%1 characters").arg(QLocale().toString(c.characters));
        sb->setItemText(QStringLiteral("word-count"), words + QStringLiteral("  ") + chars);
        sb->setItemVisible(QStringLiteral("word-count"), true);
    });
    auto updateBacklinks = [sb, session, activeMarkdown] {
        MarkdownView* mv = activeMarkdown();
        MetadataCache* mc = session->metadata();
        if (!mv || !mv->file() || !mc) {
            sb->setItemVisible(QStringLiteral("backlinks"), false);
            return;
        }
        const int n = mc->backlinkCount(mv->file()->path());
        sb->setItemText(QStringLiteral("backlinks"), n == 1 ? tr("%1 backlink").arg(n) : tr("%1 backlinks").arg(n));
        sb->setItemVisible(QStringLiteral("backlinks"), true);
    };
    // Obsidian's "editor-status": the active note's mode as an icon; a click offers the other modes.
    struct ModeItem {
        EditorView::Mode mode;
        const char* icon;
        QString title;
    };
    const QList<ModeItem> modes{{EditorView::Mode::Reading, "book-open", tr("Reading")},
                                {EditorView::Mode::Source, "code-2", tr("Source mode")},
                                {EditorView::Mode::LivePreview, "edit-3", tr("Live Preview")}};
    auto updateEditorStatus = [sb, activeMarkdown, modes] {
        MarkdownView* mv = activeMarkdown();
        QLabel* item = sb->item(QStringLiteral("editor-status"));
        if (!mv || !item) {
            sb->setItemVisible(QStringLiteral("editor-status"), false);
            return;
        }
        const ThemePtr t = currentTheme();
        const QColor color = t->color(QStringLiteral("--status-bar-text-color"));
        const qreal dpr = item->devicePixelRatioF();
        for (const ModeItem& m : modes) {
            if (m.mode != mv->mode())
                continue;
            item->setPixmap(IconProvider::instance().pixmap(QString::fromLatin1(m.icon), 15, color, 1.75, dpr));
            item->setToolTip(m.title);
        }
        sb->setItemVisible(QStringLiteral("editor-status"), true);
    };
    connect(sb, &StatusBar::itemClicked, w, [sb, session, activeMarkdown, modes](const QString& id) {
        if (id == u"backlinks" && session->commands()) {
            session->commands()->execute(QStringLiteral("backlink:open")); // like Obsidian's backlink count
            return;
        }
        MarkdownView* mv = activeMarkdown();
        if (id != u"editor-status" || !mv)
            return;
        QMenu menu;
        QPointer<MarkdownView> view = mv;
        for (const ModeItem& m : modes) {
            QAction* a = menu.addAction(IconProvider::instance().icon(QString::fromLatin1(m.icon), 16, 2.0), m.title);
            a->setCheckable(true);
            a->setChecked(mv->mode() == m.mode);
            const EditorView::Mode mode = m.mode;
            connect(a, &QAction::triggered, &menu, [view, mode] {
                if (view)
                    view->setMode(mode);
            });
        }
        QLabel* item = sb->item(QStringLiteral("editor-status"));
        menu.exec(item ? item->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu.sizeHint().height()) : QCursor::pos());
    });
    auto hookView = [timer, activeMarkdown, w, updateEditorStatus] {
        MarkdownView* mv = activeMarkdown();
        updateEditorStatus();
        if (!mv)
            return;
        // (re)connect the active view's editor; UniqueConnection keeps this idempotent
        connect(mv->editor(), &EditorView::selectionChanged, timer, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        if (!mv->property("ocEditorStatusHooked").toBool()) { // (lambdas can't use UniqueConnection)
            mv->setProperty("ocEditorStatusHooked", true);
            connect(mv, &MarkdownView::modeChanged, w, updateEditorStatus);
        }
    };
    connect(workspace, &Workspace::activeLeafChanged, w, [timer, updateBacklinks, hookView] {
        hookView();
        timer->start();
        updateBacklinks();
    });
    connect(workspace, &Workspace::layoutChanged, w, [timer, updateBacklinks, hookView] {
        hookView();
        timer->start();
        updateBacklinks();
    });
    if (MetadataCache* mc = session->metadata())
        connect(mc, &MetadataCache::resolved, w, updateBacklinks);
    hookView();
    timer->start();
    updateBacklinks();
}

void Application::finishHeadless(QWidget* target)
{
    const QString out = m_opts.screenshot;
    auto* timer = new QTimer(this);
    auto* clock = new QElapsedTimer;
    clock->start();
    auto* quietSince = new qint64(-1);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [=, this]() mutable {
        if (!target)
            return;
        const qint64 now = clock->elapsed();
        if (Busy::idle()) {
            if (*quietSince < 0)
                *quietSince = now;
        } else {
            *quietSince = -1;
        }
        const bool settled = *quietSince >= 0 && now - *quietSince >= 250 + m_opts.delayMs;
        if (!settled && now < 20000)
            return;
        timer->stop();
        target->repaint();
        const QPixmap pm = target->grab();
        const bool ok = pm.save(out);
        if (!ok)
            qCritical("oc: failed to write screenshot %s", qPrintable(out));
        delete clock;
        delete quietSince;
        QApplication::exit(ok ? 0 : 2);
    });
    timer->start();
}

} // namespace oc
