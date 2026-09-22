#pragma once

#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QString>

class QKeyEvent;

namespace oc {

// A key chord in Obsidian's hotkeys.json form: {"modifiers": ["Mod","Shift"], "key": "B"}.
// "Mod" is Ctrl on Linux/Windows. Keys use JavaScript KeyboardEvent.key names ("A", "ArrowUp",
// "Enter", "F2", ",", "[" ...), letters uppercase.
struct Hotkey {
    bool mod = false;   // Ctrl (platform primary modifier)
    bool shift = false;
    bool alt = false;
    bool meta = false;  // Super / Win
    bool ctrl = false;  // explicit Ctrl (only distinct from Mod on macOS; folded into mod here)
    QString key;        // normalized key name

    bool isValid() const { return !key.isEmpty(); }
    bool operator==(const Hotkey& o) const { return mod == o.mod && shift == o.shift && alt == o.alt && meta == o.meta && key == o.key; }

    static Hotkey fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    static Hotkey fromEvent(const QKeyEvent* e);
    // Parse "Mod+Shift+B" style strings (used for defaults in code).
    static Hotkey parse(const QString& spec);
    QString toString() const;      // "Ctrl + Shift + B" as Obsidian displays it on Linux
    QKeySequence toSequence() const;
};

size_t qHash(const Hotkey& h, size_t seed = 0);

} // namespace oc
