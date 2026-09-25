#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

namespace oc {

// Command-line options. The headless ones (--screenshot, --script, --deterministic) exist so the
// app can be driven and inspected without a human (QT_QPA_PLATFORM=offscreen).
struct CliOptions {
    QString vault;          // --vault <dir>
    QString open;           // --open <path[:line[:col]]> (vault-relative)
    QString mode;           // --mode live|source|reading
    QString theme;          // --theme dark|light|system (not persisted)
    QSize size;             // --size WxH
    QString screenshot;     // --screenshot <png>
    int delayMs = 0;        // --delay <ms> extra wait before screenshot
    QString script;         // --script <file>
    QString collapse;       // --collapse left|right|both|none
    bool deterministic = false;
    bool picker = false;    // --picker: show the vault picker
    bool noRegister = false;// --no-register: don't record the vault in the registry
    int frame = -1;         // --native-frame => 0, --frameless => 1, default -1 (setting)
    QString accent;         // --accent #rrggbb
    QString edit;           // --edit <file>: dev harness, a bare editor on one file
    QString uri;            // positional obsidian://open?vault=…&file=… / ?path=…
    QString file;           // positional absolute path of a file (opened in the vault containing it)
    bool newInstance = false; // --new-instance: don't hand the request to a running instance

    // Runs driven by a script or screenshot never talk to (or become) the interactive instance.
    bool headless() const { return !screenshot.isEmpty() || !script.isEmpty() || !edit.isEmpty(); }
};

CliOptions parseCli(const QStringList& args);

} // namespace oc
