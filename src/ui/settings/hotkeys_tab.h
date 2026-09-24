#pragma once

#include "core/commands/hotkey.h"
#include "ui/settings/settings_tab.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QLabel;

namespace oc {

class IconButton;
class SearchInput;
class SettingGroup;
class SettingItem;

// A key chip (.setting-hotkey): " Ctrl + , " in the monospace UI font on --background-modifier-hover
// with a delete "×" (.setting-hotkey-icon, red on hover). States: normal, has-conflict (red with
// white text, a tooltip naming the other commands), mod-active ("Press hotkey...", accent) and
// mod-empty ("Blank", no ×). Also used for the spellcheck languages.
class HotkeyChip : public QWidget {
    Q_OBJECT
public:
    enum class State { Normal, Conflict, Active, Empty };

    HotkeyChip(const QString& text, State state = State::Normal, bool deletable = true, QWidget* parent = nullptr);

    QString text() const { return m_text; }
    State state() const { return m_state; }
    bool isDeletable() const { return m_deletable; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void deleteClicked();
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF deleteRect() const;
    QFont chipFont() const;

    QString m_text;
    State m_state;
    bool m_deletable;
    bool m_deleteHover = false;
};

// Settings → Hotkeys: every command (by name) with its hotkeys, a "Filter..." search (name or id,
// Obsidian's simple search) with a keyboard button that filters by a pressed chord, and the filter
// pills All / Conflicts N / Assigned / Assigned by me / Unassigned. Per command: its chips (× removes
// one), "Restore default" when customized, and "Customize this command" (⊕) which records the next
// chord (at least one modifier unless it's an F-key; Shift + letter alone is ignored; Esc cancels).
// Chords used by several commands are marked as conflicts; clicking one filters by it. Changes go
// through CommandRegistry (setCustomHotkeys / resetHotkeys) and are saved to hotkeys.json at once.
class HotkeysTab : public SettingsTab {
    Q_OBJECT
public:
    enum class StatusFilter { All, Conflicts, Assigned, AssignedByMe, Unassigned };

    explicit HotkeysTab(SettingsModal* modal);
    ~HotkeysTab() override;

    QWidget* createContent() override;
    void hidden() override;

    void setQuery(const QString& query);
    QString query() const;
    void setStatusFilter(StatusFilter filter);
    StatusFilter statusFilter() const { return m_status; }
    void setHotkeyFilter(const Hotkey& hotkey); // invalid = none
    Hotkey hotkeyFilter() const { return m_hotkeyFilter; }

    // Recording: the next valid chord is added to `commandId`.
    void startCustomizing(const QString& commandId);
    void cancelCustomizing();
    QString customizingCommand() const { return m_customizing; }
    // Feeds a chord as if pressed while recording / filtering (returns false if it wasn't accepted).
    bool handleChord(const Hotkey& hotkey, bool escape = false);
    void startHotkeyFilter();
    bool isFilteringByHotkey() const { return m_filteringByHotkey; }

    void removeHotkey(const QString& commandId, const Hotkey& hotkey);
    void restoreDefault(const QString& commandId);

    // Commands (by name) whose effective hotkeys clash with another command's.
    QSet<QString> conflicts() const { return m_conflicts; }
    // The rows currently listed (command ids, in order).
    QStringList visibleCommands() const;
    SettingItem* rowFor(const QString& commandId) const;
    SettingGroup* group() const { return m_group; }

    // Obsidian's validity rule for recorded chords.
    static bool isValidRecordedHotkey(const Hotkey& h);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void save();
    void recomputeConflicts();
    void renderRows();
    void applyFilters();
    void updatePills();
    void updateDecorator();
    void grabKeys(bool on);

    QPointer<QWidget> m_content;
    QPointer<SettingGroup> m_group;
    QPointer<SearchInput> m_search;
    QPointer<IconButton> m_keyboardButton;
    QHash<StatusFilter, QPointer<QWidget>> m_pills;
    QHash<QString, QPointer<SettingItem>> m_rows;
    QPointer<SettingItem> m_emptyRow;
    QStringList m_order;
    QHash<QString, QStringList> m_hotkeyToCommands; // Hotkey::toString() -> command names
    QSet<QString> m_conflicts;
    StatusFilter m_status = StatusFilter::All;
    Hotkey m_hotkeyFilter;
    QString m_customizing;
    bool m_filteringByHotkey = false;
    bool m_grabbing = false;
};

} // namespace oc
