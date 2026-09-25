#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

namespace oc {

class MainWindow;

// Drives a window from a scenario script (--script), for headless checks of interactive behaviour.
// One command per line; blank lines and "# ..." comments are ignored. After every command the runner
// waits until the app is idle (oc::Busy) and quiet for 100 ms, so debounced work has run.
//
//   open <vault path> [live|source|reading]   open a note in the active tab
//   cursor <line> <col|end> | cursor end       place the cursor in the active editor
//   type <text>                                type characters (\n = Enter, \t = Tab, \\ = \)
//   key <chord>                                press a key: "Enter", "Mod+E", "Shift+ArrowDown", ...
//   command <id>                               run a command ("editor:toggle-bold")
//   click <objectName>                         left-click the center of a visible widget with that name
//   wait <ms>                                  extra wait
//   screenshot <png>                           save the window (relative paths: to the script's folder)
//   expect-line <n> <text>                     fail unless line n (-1 = last) of the active note is <text>
//   try <line>                                 run <line>, ignoring its failure
//   quit                                       stop here (also at the end of the script)
// Lines run from a queued call, so the script keeps going inside nested event loops (menus, dialogs).
// The process exits 0 when everything ran, 3 on an unknown command or a failed expectation.
class ScenarioRunner : public QObject {
    Q_OBJECT
public:
    ScenarioRunner(MainWindow* window, const QString& scriptPath, QObject* parent = nullptr);
    void start();

private:
    void step();
    bool run(const QString& line);
    void finish(int code);
    QWidget* keyTarget() const;
    void sendKey(int key, Qt::KeyboardModifiers mods, const QString& text);

    QPointer<MainWindow> m_window;
    QString m_dir;
    QStringList m_lines;
    int m_next = 0;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_quietSince = -1;
    qint64 m_waitUntil = 0;
    bool m_done = false;
};

} // namespace oc
