#include "app/application.h"

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
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/picker/vault_picker.h"
#include "ui/shell/app_commands.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/views/markdown/markdown_commands.h"
#include "ui/views/markdown/markdown_view.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

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
    if (!m_opts.screenshot.isEmpty() && QGuiApplication::platformName() == u"offscreen")
        QCursor::setPos(-10000, -10000);
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

void Application::applyAppearance(VaultSession* session)
{
    const QJsonObject a = session->config()->object(QStringLiteral("appearance"));
    FontSettings fonts;
    fonts.interfaceFamily = a.value(QStringLiteral("interfaceFontFamily")).toString();
    fonts.textFamily = a.value(QStringLiteral("textFontFamily")).toString();
    fonts.monospaceFamily = a.value(QStringLiteral("monospaceFontFamily")).toString();
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
    if (!m_opts.screenshot.isEmpty())
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
    GreentextGuard::instance().setSettings(GreentextGuard::Settings::fromJson(s->config()->object(QStringLiteral("plugins/greentext-guard"))));
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
    registerMarkdownView(*registry);
    connect(w, &QObject::destroyed, this, [registry] { delete registry; });
    session->setViewRegistry(registry);

    auto* workspace = new Workspace(session, registry, w);
    session->setWorkspace(workspace);
    w->setWorkspace(workspace);

    // Commands and hotkeys.
    CommandRegistry* commands = session->commands();
    registerAppCommands(*commands, w);
    registerMarkdownCommands(*commands, workspace);
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
            QMessageBox box(win);
            box.setWindowTitle(tr("Update links"));
            box.setText(tr("Do you want to update internal links that link to this file?"));
            box.setInformativeText(tr("This will affect %1 in %2.")
                                       .arg(links == 1 ? tr("%1 link").arg(links) : tr("%1 links").arg(links),
                                            filesCount == 1 ? tr("%1 file").arg(filesCount) : tr("%1 files").arg(filesCount)));
            QPushButton* always = box.addButton(tr("Always update"), QMessageBox::AcceptRole);
            QPushButton* once = box.addButton(tr("Just once"), QMessageBox::AcceptRole);
            box.addButton(tr("Do not update"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == always)
                return FileManager::LinkUpdateChoice::AlwaysUpdate;
            if (box.clickedButton() == once)
                return FileManager::LinkUpdateChoice::JustOnce;
            return FileManager::LinkUpdateChoice::DoNotUpdate;
        });
    }

    workspace->loadLayout(); // after the ribbon actions exist (left-ribbon.hiddenItems)
    wireStatusBar(w);

    // --open <note> [--mode ...]: open a note in the active tab (headless screenshots, CLI).
    if (!m_opts.open.isEmpty() && session->vault()) {
        if (TFile* f = session->vault()->file(m_opts.open)) {
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
    auto hookView = [timer, activeMarkdown, w] {
        MarkdownView* mv = activeMarkdown();
        if (!mv)
            return;
        // (re)connect the active view's editor; UniqueConnection keeps this idempotent
        connect(mv->editor(), &EditorView::selectionChanged, timer, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        Q_UNUSED(w);
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
