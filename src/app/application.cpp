#include "app/application.h"

#include "core/config/config_store.h"
#include "core/config/vault_registry.h"
#include "core/util/busy.h"
#include "core/vault_session.h"
#include "render/theme/theme.h"
#include "ui/overlay/overlay_layer.h"
#include "ui/picker/vault_picker.h"
#include "ui/shell/main_window.h"
#include "ui/shell/ribbon.h"
#include "ui/shell/status_bar.h"
#include "ui/workspace/placeholder_views.h"
#include "ui/workspace/view_registry.h"
#include "ui/workspace/workspace.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
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
    m_registry->load();
    if (!m_opts.theme.isEmpty())
        m_themes->setMode(ThemeManager::modeFromObsidianName(m_opts.theme));
    if (!m_opts.accent.isEmpty())
        m_themes->setAccent(QColor(m_opts.accent));
    m_themes->apply();

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
    connect(w, &MainWindow::closing, this, [this, id] {
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
    connect(w, &QObject::destroyed, this, [registry] { delete registry; });
    session->setViewRegistry(registry);

    auto* workspace = new Workspace(session, registry, w);
    session->setWorkspace(workspace);
    w->setWorkspace(workspace);
    workspace->loadLayout(); // after the ribbon actions exist (left-ribbon.hiddenItems)

    w->statusBar()->addItem(QStringLiteral("backlinks"), tr("0 backlinks"));
    w->statusBar()->addItem(QStringLiteral("word-count"), tr("0 words 0 characters"));
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
