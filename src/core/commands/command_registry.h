#pragma once

#include "core/commands/hotkey.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>

namespace oc {

// Context handed to editor commands: the active markdown editor widget and its view (as QObjects so
// core stays independent of the widget libraries; commands qobject_cast them).
struct EditorContext {
    QObject* editor = nullptr; // oc::EditorView
    QObject* view = nullptr;   // oc::MarkdownView
};

// An Obsidian-style command (Command palette entry + hotkey target). Exactly one of the callbacks
// is normally set:
//  - callback: always available;
//  - checkCallback(checking): availability test when checking=true, run when false;
//  - editorCallback / editorCheckCallback: only available when a markdown editor is active.
struct Command {
    QString id;       // "editor:toggle-bold", "app:open-settings", "daily-notes", ...
    QString name;     // palette label, e.g. "Toggle bold"
    QString icon;     // Lucide name
    QList<Hotkey> defaultHotkeys;
    bool repeatable = false;       // run on key auto-repeat
    std::function<void()> callback;
    std::function<bool(bool checking)> checkCallback;
    std::function<void(EditorContext&)> editorCallback;
    std::function<bool(bool checking, EditorContext&)> editorCheckCallback;
};

class CommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit CommandRegistry(QObject* parent = nullptr);

    void add(Command cmd);   // replaces an existing command with the same id
    void remove(const QString& id);
    const Command* find(const QString& id) const;
    QList<const Command*> all() const; // sorted by name

    // Provides the active markdown editor (set by the workspace/app).
    void setEditorProvider(std::function<EditorContext()> provider) { m_editorProvider = std::move(provider); }
    EditorContext activeEditor() const { return m_editorProvider ? m_editorProvider() : EditorContext{}; }

    // Commands the filter rejects are unavailable everywhere (a disabled core plugin's commands).
    void setAvailabilityFilter(std::function<bool(const Command&)> filter) { m_filter = std::move(filter); }
    bool isAvailable(const QString& id) const;
    bool execute(const QString& id); // false if unavailable/unknown

    // Hotkeys: custom (hotkeys.json) override defaults; an empty custom list removes all.
    QList<Hotkey> hotkeys(const QString& id) const;
    bool hasCustomHotkeys(const QString& id) const { return m_custom.contains(id); }
    void setCustomHotkeys(const QString& id, const QList<Hotkey>& keys);
    void resetHotkeys(const QString& id);
    QStringList commandsFor(const Hotkey& hk) const; // in registration order
    void loadHotkeys(const QJsonObject& json);       // Obsidian hotkeys.json shape
    QJsonObject saveHotkeys() const;

    // Recently used command ids (command palette ordering), most recent first.
    QStringList recent() const { return m_recent; }
    void setRecent(const QStringList& ids) { m_recent = ids; }

signals:
    void commandsChanged();
    void hotkeysChanged();
    void executed(const QString& id);

private:
    void rebuildIndex();

    QList<QString> m_order;
    QHash<QString, Command> m_commands;
    QHash<QString, QList<Hotkey>> m_custom;
    QHash<Hotkey, QStringList> m_index;
    std::function<EditorContext()> m_editorProvider;
    std::function<bool(const Command&)> m_filter;
    QStringList m_recent;
};

// Application-wide key dispatcher: maps key presses to commands, honouring focus contexts.
// Modal overlays and suggest popups get keys first (they mark themselves with the dynamic property
// "ocCapturesKeys"); plain text inputs keep standard editing chords.
class HotkeyDispatcher : public QObject {
    Q_OBJECT
public:
    explicit HotkeyDispatcher(CommandRegistry* registry, QObject* parent = nullptr);
    void setEnabled(bool on) { m_enabled = on; }
    // Only key events for widgets inside this top-level window are dispatched.
    void setWindow(QObject* window) { m_window = window; }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    bool tryDispatch(QObject* target, QKeyEvent* ke, bool execute);
    CommandRegistry* m_registry;
    QPointer<QObject> m_window;
    bool m_enabled = true;
};

} // namespace oc
