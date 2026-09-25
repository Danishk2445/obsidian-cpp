#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QWidget;

namespace oc {

class MarkdownView;
class TAbstractFile;
class WorkspaceLeaf;

// One item of an Obsidian menu. Items are grouped by section, in Obsidian's section order
// (menuSectionOrder()), with separators between the groups.
struct MenuEntry {
    QString section; // "title", "open", "action-primary", "action", "info", "view", "system", "danger", ...
    QString title;
    QString icon;    // Lucide name
    std::function<void()> run;
    bool warning = false; // red (Delete)
    bool enabled = true;
    bool checkable = false; // Obsidian's setChecked(): a check mark when `checked`
    bool checked = false;
};

// A menu's section order plus the sections Obsidian folds into a submenu (setSectionSubmenu), e.g.
// "info.copy" -> "Copy path", "view.linked" -> "Open linked view".
struct MenuLayout {
    QStringList sections;
    QList<std::pair<QString, std::pair<QString, QString>>> submenus; // section -> {title, icon}
};
// Obsidian's layout for a view's "More options" menu and tab header menu
// (close, pane, open, action, find, info, info.copy, view, view.linked, system, "", danger).
const MenuLayout& paneMenuLayout();

// Obsidian's section order for file and editor menus ("" collects unsectioned items).
const QStringList& menuSectionOrder();
// Fills `menu` with `items` grouped by section.
void buildSectionedMenu(QMenu* menu, const QList<MenuEntry>& items);
void buildSectionedMenu(QMenu* menu, const QList<MenuEntry>& items, const MenuLayout& layout);

// Obsidian's workspace "file-menu" event: views that show a file's context menu ask the providers for
// extra items (bookmarks, note composer's "Merge entire file with...", ...). `source` is Obsidian's:
// "file-explorer-context-menu", "link-context-menu", "graph-context-menu", "tab-header", "more-options",
// "sidebar-context-menu", "pane-more-options", ... `leaf` may be null; `origin` is the widget showing the
// menu (its window() is the MainWindow). Process-wide, GUI thread.
class FileMenuHooks {
public:
    using Provider = std::function<void(QList<MenuEntry>& items, TAbstractFile* file, const QString& source, WorkspaceLeaf* leaf,
                                        QWidget* origin)>;
    static int add(Provider provider); // returns an id for remove()
    static void remove(int id);
    static void collect(QList<MenuEntry>& items, TAbstractFile* file, const QString& source, WorkspaceLeaf* leaf, QWidget* origin);
};

// Obsidian's workspace "editor-menu" event: the markdown editor's context menu. `docPos` is where the
// menu was opened (the selection is the view's).
class EditorMenuHooks {
public:
    using Provider = std::function<void(QList<MenuEntry>& items, MarkdownView* view, qsizetype docPos)>;
    static int add(Provider provider);
    static void remove(int id);
    static void collect(QList<MenuEntry>& items, MarkdownView* view, qsizetype docPos);
};

} // namespace oc
