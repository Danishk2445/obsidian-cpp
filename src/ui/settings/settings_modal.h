#pragma once

#include "core/config/vault_registry.h"
#include "ui/overlay/modal.h"
#include "ui/shell/main_window.h"
#include "ui/workspace/workspace.h"
#include "ui/settings/settings_pages.h"
#include "ui/settings/settings_tab.h"

#include <QPointer>

class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace oc {

class CommandRegistry;
class MainWindow;
class OverlayLayer;
class SearchInput;
class SettingsController;
class VaultRegistry;
class VaultSession;

// A sidebar entry (.vertical-tab-nav-item): an optional icon (--icon-s, --text-muted) and a 13 px title,
// 4 × 8 px padding, --radius-m; the active tab (.is-active) and hovered items get
// --background-modifier-hover. Search results use the same item with bold matches, an optional
// sublabel (a page's tab) and a 32 px indent for the hits.
class SettingsNavItem : public QWidget {
    Q_OBJECT
public:
    SettingsNavItem(const QString& icon, const QString& title, QWidget* parent = nullptr);

    QString title() const { return m_title; }
    QString tabId() const { return m_tabId; }
    void setTabId(const QString& id) { m_tabId = id; }
    void setActive(bool active);
    bool isActive() const { return m_active; }
    void setSublabel(const QString& text);
    void setIndent(int px);
    void setTitleMatches(const QList<QPair<int, int>>& ranges); // bold [from, to) ranges
    void click() { emit clicked(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void clicked();
    // Keyboard navigation (Obsidian: ↑/↓ or k/j move, Enter/→/l/Tab activate, Shift+Tab or / go back to
    // the search input).
    void moveFocus(int delta);
    void focusSearch();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    bool m_focusVisible = false;
    QString m_icon;
    QString m_title;
    QString m_sublabel;
    QString m_tabId;
    QList<QPair<int, int>> m_matches;
    int m_indent = 0;
    bool m_active = false;
    bool m_hover = false;
};

// Obsidian's settings (app:open-settings, Mod+,): a large in-window modal (.modal.mod-settings.
// mod-sidebar-layout: min(90vw, 1100) × min(85vh, 1000)) with a sidebar — the "Search settings..."
// input and the navigation groups "Options", "Core plugins" (option tabs of the enabled core plugins,
// by name) and "Community plugins" (built-in ports of community plugins) — and the active tab's
// content: setting groups centred at most 700 px wide. Rows can open sub-pages (a back button and the
// page title above their content). Every change applies and persists immediately. Mod+F focuses the
// search; Escape closes a sub-page first, then the modal.
//
// The search matches every tab's setting names, descriptions and aliases (Obsidian's simple search:
// every word must occur) and lists the hits grouped by tab (or page) in the sidebar, three per group
// plus "N more"; choosing one opens its tab / page, scrolls to the row and flashes it.
class SettingsModal : public Modal {
    Q_OBJECT
public:
    SettingsModal(MainWindow* window, SettingsController* controller);
    ~SettingsModal() override;

    MainWindow* window() const { return m_window; }
    VaultSession* session() const;
    SettingsController* controller() const { return m_controller; }
    OverlayLayer* overlay() const;
    void notice(const QString& message) const;

    // Tabs (the modal owns them).
    void addTab(SettingsTab* tab);
    QList<SettingsTab*> tabs() const { return m_tabs; }
    SettingsTab* tab(const QString& id) const;
    SettingsTab* activeTab() const { return m_active; }
    // Only tabs listed in the navigation can be opened (plugin tabs of disabled plugins are not).
    bool isTabListed(const SettingsTab* tab) const;
    SettingsTab* openTab(const QString& id);
    void openTab(SettingsTab* tab);
    void rebuildNavigation(); // plugin tabs appeared / disappeared

    // Sub-pages of the active tab.
    void openPage(const SettingDef& page);
    void closePage();
    int pageDepth() const { return int(m_pages.size()); }
    QString currentTitle() const; // the page's title, else the tab's name

    // The shown tab or page: its rendered rows (null for hand-built tabs) and body widget.
    RenderedSettings* rendered() const;
    QWidget* currentBody() const;
    QScrollArea* currentScrollArea() const;
    // A row of the shown tab / page by name (visible or not).
    SettingItem* findItem(const QString& name) const;
    // Scrolls to the row with this name (in the shown tab / page) and flashes it.
    bool revealItem(const QString& name);

    // Settings search (sidebar).
    SearchInput* searchInput() const { return m_search; }
    void setSearchQuery(const QString& query);
    struct SearchHit {
        QString name;
        double score = 0;
    };
    struct SearchGroup {
        SettingsTab* tab = nullptr;
        QStringList pagePath; // page names from the tab down ("" = the tab itself)
        QString title;        // the page's name or the tab's
        QList<SearchHit> hits;
        double bestScore = 0;
        bool tabNameMatch = false;
    };
    const QList<SearchGroup>& searchResults() const { return m_results; }
    // Opens a result: the group's tab / page, and the hit's row when hitIndex >= 0.
    void navigateToSearchResult(int groupIndex, int hitIndex = -1);

    QList<SettingsNavItem*> navItems() const;
    QWidget* sidebar() const { return m_sidebar; }

signals:
    void tabOpened(const QString& id);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Page {
        QString title;
        SettingDef def;
        QWidget* root = nullptr;
        QScrollArea* scroll = nullptr;
        std::unique_ptr<RenderedSettings> rendered;
    };
    QWidget* buildTabBody(SettingsTab* tab, QScrollArea** scrollOut, RenderedSettings* rendered);
    void renderPage(Page& page);
    void rerenderActive();
    void refreshActive();
    void clearPages();
    void updateSearch();
    void buildSearchResults();
    QList<SettingsNavItem*> focusableNavItems() const; // the navigation, or the search results
    void moveNavFocus(SettingsNavItem* from, int delta);
    void connectNavItem(SettingsNavItem* item);

    QPointer<MainWindow> m_window;
    QPointer<SettingsController> m_controller;
    QPointer<VaultSession> m_session;
    QList<SettingsTab*> m_tabs;
    SettingsTab* m_active = nullptr;

    QWidget* m_body = nullptr;
    QWidget* m_sidebar = nullptr;
    SearchInput* m_search = nullptr;
    QScrollArea* m_navScroll = nullptr;
    QWidget* m_nav = nullptr;
    QVBoxLayout* m_navLayout = nullptr;
    QScrollArea* m_resultsScroll = nullptr;
    QWidget* m_resultsWidget = nullptr;
    QStackedWidget* m_stack = nullptr;

    QWidget* m_tabRoot = nullptr;
    QScrollArea* m_tabScroll = nullptr;
    std::unique_ptr<RenderedSettings> m_tabRendered;
    bool m_tabHandBuilt = false;
    QList<Page*> m_pages;
    QList<SearchGroup> m_results;
};

// Per-window settings state and the hooks the application wires (like PagePreview::of): the vault
// registry (for the global "frame" option), the last opened tab, core / community plugin switches and
// the live appearance. Signals report what the settings can't do on their own.
class SettingsController : public QObject {
    Q_OBJECT
public:
    static SettingsController* of(MainWindow* window); // created on first use, owned by the window
    explicit SettingsController(MainWindow* window);

    MainWindow* window() const { return m_window; }

    void setVaultRegistry(VaultRegistry* registry) { m_registry = registry; }
    VaultRegistry* vaultRegistry() const { return m_registry; }

    SettingsModal* open(const QString& tabId = {});
    SettingsModal* modal() const { return m_modal; }
    QString lastTabId() const { return m_lastTab; }

    // "core-plugins" ({id: bool}, Obsidian's defaults for missing ids). Toggling writes the whole map
    // like Obsidian, re-registers / removes the commands of the note plugins (Daily notes, Templates,
    // Unique note creator, Random note), hides / shows their ribbon actions, and emits
    // corePluginToggled for the rest.
    bool isCorePluginEnabled(const QString& id) const;
    void setCorePluginEnabled(const QString& id, bool enabled);
    // Built-in ports of community plugins ("community-plugins": {id: bool}, enabled by default).
    bool isCommunityPluginEnabled(const QString& id) const;
    void setCommunityPluginEnabled(const QString& id, bool enabled);

    // Applies appearance.json to the ThemeManager: base color scheme, accent, fonts (the first installed
    // family of each comma-separated list), font size and the enabled CSS snippets' custom properties.
    void applyAppearance();

    // Applies the app.json / appearance.json options that live in process-wide or window state rather
    // than being read on use: the ribbon's visibility ("showRibbon"), the editing behaviours of
    // mdcmd::settings() (smart lists, auto-pairing, tabs, tab size) and the spellchecker (on/off, first
    // language). Called on every VaultConfig::configChanged; call it once at startup too.
    void applyOptions();

signals:
    void manageVaultsRequested();  // General → "Manage vaults": show the vault picker
    void openHelpRequested();      // General → Help
    void corePluginToggled(const QString& id, bool enabled);
    void communityPluginToggled(const QString& id, bool enabled);
    void frameStyleChanged(const QString& frame); // "hidden" | "custom" | "native"; takes effect after a restart
    void relaunchRequested();
    void modalOpened(oc::SettingsModal* modal);
    void modalClosed();

private:
    friend class SettingsModal;
    QPointer<MainWindow> m_window;
    QPointer<VaultSession> m_session; // cached: see SettingsModal::session()
    QPointer<VaultRegistry> m_registry;
    QPointer<SettingsModal> m_modal;
    QString m_lastTab;
    css_snippets::Overrides m_overrides; // last applied to the ThemeManager
};

// Registers app:open-settings ("Open settings", Mod+,) for `window`.
void registerSettingsCommands(CommandRegistry& registry, MainWindow* window);
// Opens the settings of `window` (or brings the open ones forward) at `tabId` (e.g. "editor",
// "hotkeys", "daily-notes"); an empty id reopens the last tab (General the first time).
SettingsModal* openSettings(MainWindow* window, const QString& tabId = {});

} // namespace oc
